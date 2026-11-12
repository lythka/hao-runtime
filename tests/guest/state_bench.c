/* Table dispatch through BLR/BR to eight targets, returning to alternating
 * call sites; compact and 8-KiB-spaced targets; short blocks around a cheap
 * XPACI helper. Timing is informational, results must match native arm64. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ptrauth.h>

#define FN(n, alignment) \
    __attribute__((naked, noinline, aligned(alignment))) \
    static uint64_t f##n(uint64_t x) { \
        __asm__("add x0, x0, #" #n "\neor x0, x0, x0, lsr #7\nret"); \
    }
FN(0,64) FN(1,64) FN(2,64) FN(3,64) FN(4,64) FN(5,64) FN(6,64) FN(7,64)
FN(8,8192) FN(9,8192) FN(10,8192) FN(11,8192) FN(12,8192) FN(13,8192) FN(14,8192) FN(15,8192)

static uint64_t ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

#define BODY(call1, call2) \
    "stp x19, x20, [sp, #-32]!\nstp x21, x30, [sp, #16]\n" \
    "mov w19, w0\nmov x20, x1\nmov x21, #17\n" \
    "1: and x9, x19, #7\nldr x16, [x20, x9, lsl #3]\nmov x0, x21\n" \
    "tbz w19, #0, 3f\n" call1 "\nb 2f\n3:\n" call2 "\n" \
    "2: mov x21, x0\nsubs w19, w19, #1\nb.ne 1b\n" \
    "ldp x21, x30, [sp, #16]\nldp x19, x20, [sp], #32\nret"
__attribute__((naked, noinline)) static uint64_t calls(unsigned n, void **table)
{
    __asm__(BODY("blr x16", "blr x16"));
}
__attribute__((naked, noinline)) static uint64_t tails(unsigned n, void **table)
{
    __asm__(BODY("adr x30, 4f\nbr x16\n4:", "adr x30, 5f\nbr x16\n5:"));
}
/* One return site isolates the recurring cache-base load from the extra
 * branches in calls/tails. Keep both shapes: their measured costs differ. */
__attribute__((naked, noinline)) static uint64_t single(unsigned n, void **table)
{
    __asm__("stp x19, x20, [sp, #-32]!\nstp x21, x30, [sp, #16]\n"
            "mov w19, w0\nmov x20, x1\nmov x21, #17\n"
            "1: and x9, x19, #7\nldr x16, [x20, x9, lsl #3]\nmov x0, x21\nblr x16\n"
            "mov x21, x0\nsubs w19, w19, #1\nb.ne 1b\n"
            "ldp x21, x30, [sp, #16]\nldp x19, x20, [sp], #32\nret");
}
__attribute__((naked, noinline)) static uint64_t helpers(unsigned n)
{
    __asm__("mov w10, w0\nmov x0, #17\nmov x1, #31\n"
            "1: add x0, x0, x1\neor x1, x1, x0, lsr #7\n"
            "mov x9, #0\nxpaci x9\nadd x0, x0, x9\n"
            "subs w10, w10, #1\nb.ne 1b\nret");
}
int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 100000;
    unsigned mode = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    if (!n || n > 1000000000 || mode > 4) return 2;
    uint64_t (*fs[16])(uint64_t) = {f0,f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15};
    void *table[8];
    for (unsigned k = 0; k < 8; k++) table[k] = ptrauth_strip(fs[k + 8 * (mode == 1)], ptrauth_key_function_pointer);
    uint64_t start = ns();
    uint64_t result = mode == 4 ? single(n, table) : mode == 3 ? helpers(n) : mode == 2 ? tails(n, table) : calls(n, table);
    uint64_t elapsed = ns() - start;
    printf("%u %016llx\n", mode, (unsigned long long)result);
    fprintf(stderr, "%u %.3f ns/iteration\n", mode, (double)elapsed / n);
    return 0;
}
