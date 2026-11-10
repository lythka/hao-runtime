/*
 * Return addresses that the callee strips itself (XPACLRI, XPACI, XPACD on
 * x30) before returning, in every kind of guest function that hao enters
 * with its own return address in x30: main, a pthread start routine, a
 * native-called callback (qsort comparator) and a signal handler. Natively
 * the return address is canonical, so stripping keeps it; hao's sentinel
 * must be canonical too (include/hao/runtime.h) now that XPAC* strip as the
 * hardware does. Also prints XPAC* on values a guest can hold, for the diff
 * against native arm64. tests/guest/xpac.sh runs it.
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int realmain(void);
int cmp_xpaclri(const void *, const void *);
int cmp_xpaci(const void *, const void *);
int cmp_xpacd(const void *, const void *);
void *thr_xpaclri(void *);
void on_usr1(int);
uint64_t do_xpaci(uint64_t), do_xpacd(uint64_t), do_xpaclri(uint64_t);
volatile int got_usr1;

/* XPACI x16 = 0xDAC143F0, XPACD x30 = 0xDAC147FE, XPACI x0 = 0xDAC143E0,
 * XPACD x0 = 0xDAC147E0 (as words: the arm64 assembler has no PAuth by default) */
__asm__(".text\n.p2align 2\n"
        ".globl _main\n_main:\n"
        "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n  bl _realmain\n  ldp x29, x30, [sp], #16\n"
        "  hint #7\n  ret\n"                                                   /* XPACLRI */
        ".globl _cmp_xpaclri\n_cmp_xpaclri:\n"
        "  hint #7\n  ldr w8, [x0]\n  ldr w9, [x1]\n  cmp w8, w9\n  cset w0, gt\n  csinv w0, w0, wzr, ge\n  ret\n"
        ".globl _cmp_xpaci\n_cmp_xpaci:\n"
        "  mov x16, x30\n  .long 0xDAC143F0\n"
        "  ldr w8, [x0]\n  ldr w9, [x1]\n  cmp w8, w9\n  cset w0, gt\n  csinv w0, w0, wzr, ge\n  br x16\n"
        ".globl _cmp_xpacd\n_cmp_xpacd:\n"
        "  .long 0xDAC147FE\n  ldr w8, [x0]\n  ldr w9, [x1]\n  cmp w8, w9\n  cset w0, gt\n  csinv w0, w0, wzr, ge\n  ret\n"
        ".globl _thr_xpaclri\n_thr_xpaclri:\n  hint #7\n  add x0, x0, #1\n  ret\n"
        ".globl _on_usr1\n_on_usr1:\n"
        "  adrp x8, _got_usr1@PAGE\n  add x8, x8, _got_usr1@PAGEOFF\n  mov w9, #1\n  str w9, [x8]\n  hint #7\n  ret\n"
        ".globl _do_xpaci\n_do_xpaci:\n  .long 0xDAC143E0\n  ret\n"
        ".globl _do_xpacd\n_do_xpacd:\n  .long 0xDAC147E0\n  ret\n"
        ".globl _do_xpaclri\n_do_xpaclri:\n  mov x17, x30\n  mov x30, x0\n  hint #7\n  mov x0, x30\n  mov x30, x17\n  ret\n");

static int sorted(const int *a, int n)
{
    for (int k = 1; k < n; k++) if (a[k - 1] > a[k]) return 0;
    return 1;
}

int realmain(void)
{
    int (*cmps[3])(const void *, const void *) = {cmp_xpaclri, cmp_xpaci, cmp_xpacd};
    const char *names[3] = {"xpaclri", "xpaci", "xpacd"};
    for (int c = 0; c < 3; c++) {
        int a[64];
        for (int k = 0; k < 64; k++) a[k] = (k * 37 + 11) % 64 - 20;
        qsort(a, 64, sizeof *a, cmps[c]);
        printf("qsort %s: %s\n", names[c], sorted(a, 64) ? "sorted" : "NOT SORTED");
    }
    pthread_t t;
    void *r = 0;
    pthread_create(&t, 0, thr_xpaclri, (void *)41);
    pthread_join(t, &r);
    printf("thread xpaclri: %ld\n", (long)r);
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    printf("signal xpaclri: %d\n", got_usr1);
    /* canonical values a guest holds (heap, stack, image, ObjC-tagged on
     * x86_64 have bit 0) come back unchanged; the others are stripped */
    static const uint64_t v[] = {0x0000000100004000ull, 0x00007ff7bfeff000ull, 0x0000600000c04001ull,
                                 0xff00000100004000ull, 0x8000000000000001ull, 0x3ebce676ed7cc604ull,
                                 0x0000fffffffff000ull, 0x00ff000100004000ull, 0xfffffe0007001234ull};
    uint64_t heap = (uint64_t)(uintptr_t)malloc(16), stack = (uint64_t)(uintptr_t)&t, code = (uint64_t)(uintptr_t)realmain;
    printf("canonical: %d %d %d\n", do_xpaci(heap) == heap && do_xpacd(heap) == heap, do_xpaci(stack) == stack && do_xpaclri(stack) == stack,
           do_xpaci(code) == code && do_xpaclri(code) == code);
    for (size_t k = 0; k < sizeof v / sizeof *v; k++)
        printf("%016llx: xpaci %016llx xpacd %016llx xpaclri %016llx\n", (unsigned long long)v[k],
               (unsigned long long)do_xpaci(v[k]), (unsigned long long)do_xpacd(v[k]), (unsigned long long)do_xpaclri(v[k]));
    fflush(stdout);
    return 0;
}
