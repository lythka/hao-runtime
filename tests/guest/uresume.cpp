/* hao resumes the contexts its own fault handler builds: an NX fault of a
 * native call into guest code is redirected to hao_callback_entry, a native
 * call of a trap address the same way (runtime.c on_fault). This program
 * checks that the redirected context is the faulting one, exactly, as
 * tests/guest/uresume.sh runs it natively (arm64) and under hao.
 * Every guest function pointer reaches native code through memory built at
 * run time (uresumehelper.cpp reads it from a struct or a slot), so hao
 * cannot thunk it ahead: each call is one NX fault.
 *   uresume MODE
 *   state     native code holds known values in every argument, callee-saved
 *             and other register and sets the FP control (rounding up,
 *             flush-to-zero, inexact raised; x87 control word), then calls a
 *             guest function; the guest checks its arguments (x0-x9, v0-v7)
 *             and x20-x22 (r13 r12 r14 under hao), its FPCR/FPSR and the
 *             native view (a native call), divides by zero; after the
 *             return the native side checks what the ABI says survives
 *   trap      the same through the guest's dladdr import (a trap address)
 *   mask      SIGUSR1, SIGUSR2 and SIGALRM blocked by native code around the
 *             call: the mask inside the callee and after the return
 *   alarm     SIGALRM every 100 us with a guest handler while thousands of
 *             checked NX callbacks run (each checks the mask too)
 *   alarmnx   the same, the guest handler itself making an NX callback
 *   alarmh    SIGALRM every 100 us interrupting a guest loop that makes no
 *             native call; the guest handler makes an NX callback (no NX
 *             fault of the interrupted code can race the signal)
 *   nest      an NX callback whose body makes a checked NX callback, depth 3
 *   eh        C++ exceptions across redirected frames: guest throw to guest
 *             catch, to native catch, from depth 3 to depth 1, native throw
 *             through a guest frame to native catch; then a checked call
 *   threads   8 guest threads, each 400 checked NX callbacks
 *   nthreads  8 native threads, each 400 checked NX callbacks
 *   altstack  a sigaltstack installed by native code on this thread, then
 *             state, a loop, a forwarded fault and the altstack still there
 *   fwd       every register (all GP, all vector, flags with DF set, FP
 *             control and status) across a fault a native handler fixes */
#include "uresume.h"
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define RAW(f) ((uint64_t)(uintptr_t)ptrauth_strip((void *)(f), ptrauth_key_function_pointer))
#else
#define RAW(f) ((uint64_t)(uintptr_t)(void *)(f))
#endif

typedef uint32_t v4u __attribute__((vector_size(16)));

extern "C" {
uint64_t g_seen[5];          /* x20 x21 x22 fpcr fpsr at g_entry */
long g_entry(long, long, long, long, long, long, long, long, long, long, v4u, v4u, v4u, v4u, v4u, v4u, v4u, v4u);
long g_body(long, long, long, long, long, long, long, long, long, long, v4u, v4u, v4u, v4u, v4u, v4u, v4u, v4u);
}
/* g_entry records what the caller left in x20-x22, FPCR and FPSR before any
 * guest instruction touches them, then is g_body */
__asm__(".text\n.p2align 2\n.globl _g_entry\n_g_entry:\n"
        "adrp x16, _g_seen@PAGE\nadd x16, x16, _g_seen@PAGEOFF\n"
        "stp x20, x21, [x16]\nstr x22, [x16, #16]\n"
        "mrs x17, fpcr\nstr x17, [x16, #24]\nmrs x17, fpsr\nstr x17, [x16, #32]\n"
        "b _g_body\n");

static uint64_t g_entry_raw;   /* RAW(g_entry), set in main */
static int g_state;          /* state mode: print what g_body saw */
static int g_maskview;       /* mask mode: print the mask from inside */
static int g_nest, g_level;
static volatile double g_zero = 0.0, g_inf;

static void p(const char *s) { fputs(s, stdout); fflush(stdout); }

extern "C" long g_body(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, v4u v0, v4u v1, v4u v2, v4u v3,
                       v4u v4, v4u v5, v4u v6, v4u v7)
{
    uint64_t gp[10] = {(uint64_t)a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4,
                       (uint64_t)a5, (uint64_t)a6, (uint64_t)a7, (uint64_t)a8, (uint64_t)a9};
    uint8_t vec[8][16];
    v4u vs[8] = {v0, v1, v2, v3, v4, v5, v6, v7};
    memcpy(vec, vs, sizeof vec);
    uint64_t seen[5];
    memcpy(seen, g_seen, sizeof seen);
    if (g_state) {
        uh_view("guest entry");
        int ok = 1;
        for (int k = 0; k < 10; k++) ok &= gp[k] == uh_val(k);
        int vok = 1;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 16; j++) vok &= vec[k][j] == (uint8_t)((k * 16 + j) * 7 + 1);
        /* uh_fill: keep[1..3] are x20-x22 on arm64; r13 r12 r14 under hao, which maps them there (swiftcc) */
        int sok = seen[0] == uh_val(41) && seen[1] == uh_val(42) && seen[2] == uh_val(43);
        char b[512];
        snprintf(b, sizeof b, "guest entry: x0-x9 %s, v0-v7 %s, x20-x22 %s\n", ok ? "ok" : "BAD", vok ? "ok" : "BAD", sok ? "ok" : "BAD");
        p(b);
        if (!ok || !vok || !sok) {
            snprintf(b, sizeof b, "guest entry: a0=%#lx a6=%#lx a8=%#lx a9=%#lx x20=%#llx x21=%#llx x22=%#llx\n", a0, a6, a8, a9,
                     (unsigned long long)seen[0], (unsigned long long)seen[1], (unsigned long long)seen[2]);
            p(b);
        }
        static const char *rm[] = {"nearest", "up", "down", "zero"};
        snprintf(b, sizeof b, "guest entry: fpcr rounding %s, fz %d; fpsr flags %#llx\n", rm[seen[3] >> 22 & 3], (int)(seen[3] >> 24 & 1),
                 (unsigned long long)(seen[4] & 0x1f));
        p(b);
        g_inf = 1.0 / g_zero;   /* divbyzero, seen by the native caller after the return */
    }
    if (g_maskview) {
        g_maskview = 0;   /* the first call prints; the loop after it checks quietly */
        uh_mask_print("mask inside the guest callee (native view)");
        sigset_t m;
        sigprocmask(SIG_BLOCK, NULL, &m);
        char b[128];
        snprintf(b, sizeof b, "mask inside the guest callee (guest view): USR1 %d USR2 %d ALRM %d SEGV %d BUS %d\n", sigismember(&m, SIGUSR1),
                 sigismember(&m, SIGUSR2), sigismember(&m, SIGALRM), sigismember(&m, SIGSEGV), sigismember(&m, SIGBUS));
        p(b);
    }
    if (g_nest && g_level < 3) {
        int lv = ++g_level;
        uh_ctx c;
        uh_fill(&c, lv);
        c.fn = RAW(g_entry);
        char tag[32];
        snprintf(tag, sizeof tag, "depth %d", lv);
        uh_check(&c, tag, 1, 0);
        g_level--;
    }
    return (long)uh_sum(gp, vec);
}

/* ---- signals */
static volatile int g_alarms, g_hnx_bad;
static uh_fn g_hslot;
static int g_hnx;
static void on_alarm(int sig)
{
    (void)sig;
    int n = ++g_alarms;
    if (g_hnx && uh_call_slot(&g_hslot, n) != 2 * (long)n) g_hnx_bad++;
}
static long twice(long v) { return 2 * v; }

static void alarm_mode(int nx)
{
    g_hnx = nx;
    g_hslot = (uh_fn)RAW(twice);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 100}, {0, 100}};
    setitimer(ITIMER_REAL, &it, NULL);
    int calls = 0, fails = 0;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    for (;;) {
        fails += uh_loop(&g_entry_raw, 100, 1);
        calls += 100;
        gettimeofday(&t1, NULL);
        double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_usec - t0.tv_usec) / 1e6;
        if ((calls >= 5000 && g_alarms >= 50) || s > 15) break;
    }
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    signal(SIGALRM, SIG_IGN);
    char b[256];
    snprintf(b, sizeof b, "alarm%s: calls>=5000 %s, alarms>=50 %s, failures %d\n", nx ? "nx" : "", calls >= 5000 ? "yes" : "NO",
             g_alarms >= 50 ? "yes" : "NO", fails);
    p(b);
    if (nx) { snprintf(b, sizeof b, "alarmnx: NX callbacks in the handler wrong: %d\n", g_hnx_bad); p(b); }
}

/* the guest spins, the handler calls guest code through native code */
static void alarmh_mode(void)
{
    g_hnx = 1;
    g_hslot = (uh_fn)RAW(twice);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 100}, {0, 100}};
    setitimer(ITIMER_REAL, &it, NULL);
    volatile uint64_t x = 0;
    time_t t0 = time(NULL);
    while (g_alarms < 2000 && time(NULL) - t0 < 15)
        for (int k = 0; k < 10000; k++) x = x * 3 + (uint64_t)k;
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    signal(SIGALRM, SIG_IGN);
    char b[160];
    snprintf(b, sizeof b, "alarmh: alarms>=2000 %s, NX callbacks in the handler wrong: %d\n", g_alarms >= 2000 ? "yes" : "NO", g_hnx_bad);
    p(b);
}

/* ---- exceptions */
static uh_fn g_throw_slot, g_nest_slot, g_nthrow_slot;
static long thrower(long v) { throw (int)v; }
static long native_thrower(long v) { uh_throw((int)v + 1); return 0; }
static long eh_nest(long v)
{
    if (v == 3) throw 33;
    if (v == 1) {
        try {
            uh_call_slot(&g_nest_slot, v + 1);
        } catch (int e) {
            char b[96];
            snprintf(b, sizeof b, "eh: depth 3 throw %d caught at depth 1\n", e);
            p(b);
            return 100;
        }
        return -1;
    }
    return uh_call_slot(&g_nest_slot, v + 1);
}

static void eh_mode(void)
{
    char b[128];
    g_throw_slot = (uh_fn)RAW(thrower);
    try {
        uh_call_slot(&g_throw_slot, 7);
        p("eh: guest throw not caught\n");
    } catch (int e) {
        snprintf(b, sizeof b, "eh: guest throw %d caught by the guest\n", e);
        p(b);
    }
    snprintf(b, sizeof b, "eh: guest throw caught natively: %ld\n", uh_catch_slot(&g_throw_slot, 8));
    p(b);
    g_nest_slot = (uh_fn)RAW(eh_nest);
    snprintf(b, sizeof b, "eh: nest returned %ld\n", uh_call_slot(&g_nest_slot, 1));
    p(b);
    g_nthrow_slot = (uh_fn)RAW(native_thrower);
    snprintf(b, sizeof b, "eh: native throw through a guest frame caught natively: %ld\n", uh_catch_slot(&g_nthrow_slot, 20));
    p(b);
    uh_ctx c;
    uh_fill(&c, 0);
    c.fn = RAW(g_entry);
    uh_check(&c, "eh: after the unwinds", 1, 0);
    snprintf(b, sizeof b, "eh: loop failures %d\n", uh_loop(&g_entry_raw, 200, 1));
    p(b);
}

/* ---- threads */
static void *th_main(void *x)
{
    (void)x;
    return (void *)(intptr_t)uh_loop(&g_entry_raw, 400, 1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *m = argc > 1 ? argv[1] : "state";
    g_entry_raw = RAW(g_entry);
    uh_ctx c;
    char b[256];
    if (!strcmp(m, "state")) {
        g_state = 1;
        uh_fill(&c, 0);
        c.fn = RAW(g_entry);
        uh_check(&c, "state", 2, 0);
        g_state = 0;
        snprintf(b, sizeof b, "state loop: 500 calls, %d failures\n", uh_loop(&g_entry_raw, 500, 1));
        p(b);
    } else if (!strcmp(m, "trap")) {
        { static uint64_t d; d = RAW(&dladdr); uh_trap_case(&d); }
    } else if (!strcmp(m, "mask")) {
        g_maskview = 1;
        uh_mask_case(&g_entry_raw);
        g_maskview = 0;
    } else if (!strcmp(m, "alarm")) {
        alarm_mode(0);
    } else if (!strcmp(m, "alarmnx")) {
        alarm_mode(1);
    } else if (!strcmp(m, "alarmh")) {
        alarmh_mode();
    } else if (!strcmp(m, "nest")) {
        g_nest = 1;
        g_level = 0;
        uh_fill(&c, 0);
        c.fn = RAW(g_entry);
        uh_check(&c, "depth 0 (native caller of the first)", 1, 0);
        g_nest = 0;
    } else if (!strcmp(m, "eh")) {
        eh_mode();
    } else if (!strcmp(m, "threads")) {
        pthread_t th[8];
        for (int k = 0; k < 8; k++) pthread_create(&th[k], NULL, th_main, NULL);
        long f = 0;
        for (int k = 0; k < 8; k++) { void *r; pthread_join(th[k], &r); f += (long)(intptr_t)r; }
        snprintf(b, sizeof b, "threads: 8 x 400 calls, %ld failures\n", f);
        p(b);
    } else if (!strcmp(m, "nthreads")) {
        snprintf(b, sizeof b, "nthreads: 8 x 400 calls, %d failures\n", uh_nthreads(&g_entry_raw, 8, 400));
        p(b);
    } else if (!strcmp(m, "altstack")) {
        uh_altstack_on();
        uh_fill(&c, 0);
        c.fn = RAW(g_entry);
        uh_check(&c, "altstack call", 1, 0);
        uh_mask_print("altstack mask");
        snprintf(b, sizeof b, "altstack loop: 300 calls, %d failures\n", uh_loop(&g_entry_raw, 300, 1));
        p(b);
        uh_fwd(20);
        uh_altstack_check();
    } else if (!strcmp(m, "fwd")) {
        uh_fwd(200);
    } else {
        fprintf(stderr, "unknown mode %s\n", m);
        return 2;
    }
    p("done\n");
    return 0;
}
