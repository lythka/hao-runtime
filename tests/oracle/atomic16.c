/*
 * x86_64 checks of what the per-instruction oracle cannot express:
 *
 *  1. FEAT_LSE2 single-copy atomicity of 16-byte aligned LDP/STP (X pairs)
 *     and LDXP: a writer thread stores {k, k} with STP while a reader loads
 *     with LDP (or LDXP) and checks that both halves are equal. Run through
 *     the interpreter and the JIT, with a register base and with SP as the
 *     base. A control run that splits the access into two 8-byte halves
 *     must tear, which shows the race is real on this machine.
 *  2. LDXP/LDAXP and LDP on a read-only page load the data without a fault
 *     (an exclusive load must not write), and LDXP + STXP still update a
 *     writable cell.
 *
 *   oracle-atomic [ITERATIONS]
 */
#include "hao/cpu.h"
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int hao_jit_exec_code(hao_cpu *c, const uint32_t *code, int n, uint64_t base);

#define STP_X0_X1_X2 0xA9000440u   /* stp x0, x1, [x2] */
#define LDP_X3_X4_X2 0xA9401043u   /* ldp x3, x4, [x2] */
#define STP_X0_X1_SP 0xA90003E0u   /* stp x0, x1, [sp] */
#define LDP_X3_X4_SP 0xA94013E3u   /* ldp x3, x4, [sp] */
#define LDXP_X3_X4_X2 0xC87F1043u  /* ldxp x3, x4, [x2] */
#define LDAXP_X3_X4_X2 0xC87F9043u /* ldaxp x3, x4, [x2] */
#define STXP_W5_X0_X1_X2 0xC8250440u /* stxp w5, x0, x1, [x2] */
#define CLREX 0xD503305Fu

static volatile uint64_t cell[4] __attribute__((aligned(64)));
static long iters = 300000;
static volatile int stop;
static int fails;

typedef struct { int jit, sp, split; uint32_t ld; long loads, torn; } race;

static int run(hao_cpu *c, uint32_t insn, int jit)
{
    if (jit) return hao_jit_exec_code(c, &insn, 1, 0x100000);
    return a64_exec(c, insn, 0x100000);
}

static void *writer(void *arg)
{
    race *r = arg;
    static __thread hao_cpu c;
    memset(&c, 0, sizeof c);
    c.x[2] = c.x[31] = (uint64_t)(uintptr_t)cell;
    for (uint64_t k = 1; !stop; k++) {
        if (r->split) { cell[0] = k; __asm__ volatile("" ::: "memory"); cell[1] = k; continue; }
        c.x[0] = c.x[1] = k * 0x0101010101010101ull;
        run(&c, r->sp ? STP_X0_X1_SP : STP_X0_X1_X2, r->jit);
    }
    return NULL;
}

static void do_race(const char *what, race *r)
{
    pthread_t t;
    stop = 0;
    cell[0] = cell[1] = 0;
    pthread_create(&t, NULL, writer, r);
    hao_cpu c;
    memset(&c, 0, sizeof c);
    c.x[2] = c.x[31] = (uint64_t)(uintptr_t)cell;
    for (long k = 0; k < iters; k++) {
        uint64_t a, b;
        if (r->split) { a = cell[1]; __asm__ volatile("" ::: "memory"); b = cell[0]; }
        else {
            run(&c, r->ld, r->jit);
            a = c.x[3]; b = c.x[4];
            if (r->ld == LDXP_X3_X4_X2) run(&c, CLREX, r->jit);
        }
        r->loads++;
        if (a != b) r->torn++;
    }
    stop = 1;
    pthread_join(t, NULL);
    bool bad = r->split ? r->torn == 0 : r->torn != 0;
    printf("%-44s %9ld loads, %8ld torn  %s\n", what, r->loads, r->torn,
           bad ? (r->split ? "FAIL (the control did not tear: the race is not exercised)" : "FAIL") : "ok");
    if (bad) fails++;
}

static sigjmp_buf jb;
static void on_sig(int s) { (void)s; siglongjmp(jb, 1); }

static void readonly_checks(int jit)
{
    uint64_t *ro = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (int k = 0; k < 8; k++) ro[k] = 0x1111111111111111ull * (uint64_t)(k + 1);
    mprotect(ro, 0x4000, PROT_READ);
    static const struct { uint32_t insn; const char *name; } t[] = {
        {LDXP_X3_X4_X2, "ldxp"}, {LDAXP_X3_X4_X2, "ldaxp"}, {LDP_X3_X4_X2, "ldp"},
    };
    for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
        hao_cpu c;
        memset(&c, 0, sizeof c);
        c.x[2] = (uint64_t)(uintptr_t)(ro + 2);
        volatile int faulted = 0;
        if (sigsetjmp(jb, 1) == 0) run(&c, t[k].insn, jit);
        else faulted = 1;
        bool ok = !faulted && c.x[3] == ro[2] && c.x[4] == ro[3];
        printf("%s %-6s on a read-only page: %s\n", jit ? "jit   " : "interp", t[k].name,
               faulted ? "FAULT (FAIL)" : ok ? "ok" : "wrong value (FAIL)");
        if (!ok) fails++;
    }
    munmap(ro, 0x4000);
    /* exclusive pair round trip on writable memory */
    hao_cpu c;
    memset(&c, 0, sizeof c);
    cell[0] = 5; cell[1] = 6;
    c.x[2] = (uint64_t)(uintptr_t)cell;
    c.x[0] = 7; c.x[1] = 8;
    run(&c, LDXP_X3_X4_X2, jit);
    run(&c, STXP_W5_X0_X1_X2, jit);
    bool ok = c.x[3] == 5 && c.x[4] == 6 && c.x[5] == 0 && cell[0] == 7 && cell[1] == 8;
    c.x[0] = 9;
    run(&c, STXP_W5_X0_X1_X2, jit);    /* monitor now clear: must fail */
    ok = ok && c.x[5] == 1 && cell[0] == 7;
    printf("%s ldxp+stxp on writable memory: %s\n", jit ? "jit   " : "interp", ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) iters = atol(argv[1]);
    signal(SIGSEGV, on_sig);
    signal(SIGBUS, on_sig);
    { hao_cpu c; memset(&c, 0, sizeof c); run(&c, CLREX, 1); }   /* JIT init before any thread */
    race r[] = {
        {0, 0, 1, 0, 0, 0},
        {0, 0, 0, LDP_X3_X4_X2, 0, 0}, {0, 1, 0, LDP_X3_X4_SP, 0, 0}, {0, 0, 0, LDXP_X3_X4_X2, 0, 0},
        {1, 0, 0, LDP_X3_X4_X2, 0, 0}, {1, 1, 0, LDP_X3_X4_SP, 0, 0}, {1, 0, 0, LDXP_X3_X4_X2, 0, 0},
    };
    static const char *names[] = {
        "control: two 8-byte halves (must tear)",
        "interp stp/ldp [x2]", "interp stp/ldp [sp]", "interp stp/ldxp [x2]",
        "jit    stp/ldp [x2]", "jit    stp/ldp [sp]", "jit    stp/ldxp [x2]",
    };
    for (unsigned k = 0; k < sizeof r / sizeof *r; k++) do_race(names[k], &r[k]);
    readonly_checks(0);
    readonly_checks(1);
    printf("atomic16: %s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
