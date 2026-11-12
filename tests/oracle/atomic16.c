/*
 * x86_64 checks of what the per-instruction oracle cannot express:
 *
 *  1. FEAT_LSE2 single-copy atomicity of 16-byte aligned LDP/STP (X pairs)
 *     and LDXP: a writer thread stores {k, k} with STP while a reader loads
 *     with LDP (or LDXP) and checks that both halves are equal. Run through
 *     the interpreter and the JIT, with a register base and with SP as the
 *     base. A control run that splits the access into two 8-byte halves
 *     must tear, which shows the race is real on this machine. Each case
 *     reads only once the writer is running, and repeats its ITERATIONS
 *     loads until the writer has stored at least as often during them (the
 *     control: until it tears), for at most ATOMIC16_SECONDS (30) each,
 *     the writer's start included.
 *     Under heavy host load the two threads may never overlap enough: the
 *     case then prints SKIP (the race was not established, so it proves
 *     nothing either way) and does not fail; a torn atomic load always fails.
 *  2. LDXP/LDAXP and LDP on a read-only page load the data without a fault
 *     (an exclusive load must not write), and LDXP + STXP still update a
 *     writable cell.
 *
 *   [ATOMIC16_SECONDS=30] oracle-atomic [ITERATIONS]
 */
#include "hao/cpu.h"
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
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
static double limit_s = 30;
static _Atomic long wstores;   /* the writer's stores so far */
static _Atomic int wstarted;
static int skips;
static _Atomic int stop;
static int fails;

typedef struct { int jit, sp, split; uint32_t ld; long loads, torn, overlap; } race;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

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
    atomic_store(&wstarted, 1);
    for (uint64_t k = 1; !atomic_load_explicit(&stop, memory_order_relaxed); k++) {
        if (r->split) { cell[0] = k; __asm__ volatile("" ::: "memory"); cell[1] = k; }
        else {
            c.x[0] = c.x[1] = k * 0x0101010101010101ull;
            run(&c, r->sp ? STP_X0_X1_SP : STP_X0_X1_X2, r->jit);
        }
        atomic_store_explicit(&wstores, (long)k, memory_order_relaxed);
    }
    return NULL;
}

static void do_race(const char *what, race *r)
{
    pthread_t t;
    atomic_store(&stop, 0);
    atomic_store(&wstarted, 0);
    atomic_store(&wstores, 0);
    cell[0] = cell[1] = 0;
    if (pthread_create(&t, NULL, writer, r) != 0) {
        printf("%-44s FAIL (pthread_create)\n", what);
        fails++;
        return;
    }
    hao_cpu c;
    memset(&c, 0, sizeof c);
    c.x[2] = c.x[31] = (uint64_t)(uintptr_t)cell;
    /* the deadline covers the writer's start and is checked inside a round */
    double end = now_s() + limit_s;
    while (!atomic_load(&wstarted) && now_s() <= end) sched_yield();
    /* rounds of ITERATIONS loads until the race is established or time is up */
    while (atomic_load(&wstarted)) {
    long w0 = atomic_load_explicit(&wstores, memory_order_relaxed);
    for (long k = 0; k < iters; k++) {
        if ((k & 4095) == 4095 && now_s() > end) break;
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
    r->overlap += atomic_load_explicit(&wstores, memory_order_relaxed) - w0;
    bool established = r->split ? r->torn != 0 : r->loads > 0 && r->overlap >= r->loads;
    if (established || (!r->split && r->torn) || now_s() > end) break;
    }
    atomic_store(&stop, 1);
    pthread_join(t, NULL);
    bool established = r->split ? r->torn != 0 : r->loads > 0 && r->overlap >= r->loads;
    const char *res = !r->split && r->torn ? "FAIL"
                    : established ? "ok"
                    : r->split ? "SKIP (the control did not tear: the race was not established)"
                    : "SKIP (the writer stored less often than the reader loaded: the race was not established)";
    printf("%-44s %9ld loads, %8ld torn, %10ld stores  %s\n", what, r->loads, r->torn, r->overlap, res);
    if (res[0] == 'F') fails++;
    if (res[0] == 'S') skips++;
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
    if (getenv("ATOMIC16_SECONDS")) limit_s = atof(getenv("ATOMIC16_SECONDS"));
    signal(SIGSEGV, on_sig);
    signal(SIGBUS, on_sig);
    { hao_cpu c; memset(&c, 0, sizeof c); run(&c, CLREX, 1); }   /* JIT init before any thread */
    race r[] = {
        {0, 0, 1, 0, 0, 0, 0},
        {0, 0, 0, LDP_X3_X4_X2, 0, 0, 0}, {0, 1, 0, LDP_X3_X4_SP, 0, 0, 0}, {0, 0, 0, LDXP_X3_X4_X2, 0, 0, 0},
        {1, 0, 0, LDP_X3_X4_X2, 0, 0, 0}, {1, 1, 0, LDP_X3_X4_SP, 0, 0, 0}, {1, 0, 0, LDXP_X3_X4_X2, 0, 0, 0},
    };
    static const char *names[] = {
        "control: two 8-byte halves (must tear)",
        "interp stp/ldp [x2]", "interp stp/ldp [sp]", "interp stp/ldxp [x2]",
        "jit    stp/ldp [x2]", "jit    stp/ldp [sp]", "jit    stp/ldxp [x2]",
    };
    for (unsigned k = 0; k < sizeof r / sizeof *r; k++) do_race(names[k], &r[k]);
    readonly_checks(0);
    readonly_checks(1);
    printf("atomic16: %s (%d failures, %d races not established)\n", fails ? "FAILED" : "OK", fails, skips);
    return fails != 0;
}
