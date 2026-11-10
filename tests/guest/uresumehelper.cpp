/* Native helper for tests/guest/uresume.cpp, built x86_64 + arm64e (the
 * guest's: under hao the guest binds to the x86_64 slice) and arm64 (the
 * native reference's). Everything here is native code that calls guest
 * function pointers it finds in memory, so under hao every such call takes
 * the NX-fault path: on_fault redirects it to hao_callback_entry and
 * resumes the redirected context. The register halves are in
 * uresumehelper.S. Each check prints one line per register class; the
 * classes are the same on both architectures, the registers behind them
 * are each architecture's own (uresume.h). */
#include "uresume.h"
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif

extern "C" void uh_call_full(uh_ctx *c);
extern "C" void uh_fault_full(uh_full *f);

static_assert(offsetof(uh_ctx, fn) == 0x180, "uh_ctx");
static_assert(offsetof(uh_ctx, keep) == 0x1d8, "uh_ctx");
static_assert(offsetof(uh_ctx, fpctl) == 0x270, "uh_ctx");
static_assert(offsetof(uh_ctx, ret) == 0x280, "uh_ctx");
static_assert(offsetof(uh_ctx, sp_before) == 0x320, "uh_ctx");
static_assert(offsetof(uh_ctx, flags_after) == 0x33c, "uh_ctx");
static_assert(offsetof(uh_full, gin) == 0x600, "uh_full");
static_assert(offsetof(uh_full, addr) == 0x800, "uh_full");
static_assert(offsetof(uh_full, fpctl_in) == 0x828, "uh_full");
static_assert(offsetof(uh_full, x87_out) == 0x840, "uh_full");

#if defined(__x86_64__)
/* MXCSR: masks, RC up, FTZ, PE raised; x87: 53-bit precision, RC up */
#define FPCTL_SET 0xDFA0u
#define FPCTL_CTL 0xFFC0u          /* DAZ, masks, RC, FTZ */
#define X87_SET 0x0A7Fu
static int have_avx(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if (!(c & bit_OSXSAVE) || !(c & bit_AVX)) return 0;
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}
static uint32_t fpctl_now(void) { uint32_t m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static uint32_t fpstat_now(void) { return fpctl_now(); }
static uint16_t x87_now(void) { uint16_t w; __asm__ volatile("fnstcw %0" : "=m"(w)); return w; }
static int df_now(void) { uint64_t f; __asm__ volatile("pushfq; popq %0" : "=r"(f)); return (int)(f >> 10 & 1); }
static const char *rmode_name(uint32_t m) { static const char *n[] = {"nearest", "down", "up", "zero"}; return n[m >> 13 & 3]; }
static int fz_bit(uint32_t m) { return (m >> 15) & 1; }
/* raised IEEE flags in arm64 order: invalid divbyzero overflow underflow inexact */
static unsigned flags_of(uint32_t m) { return (m & 1) | (m >> 1 & 0x1e); }
#else
/* FPCR: RMode RP, FZ; FPSR: IXC */
#define FPCTL_SET 0x01400000u
#define FPSTAT_SET 0x10u
static int have_avx(void) { return 0; }
static uint32_t fpctl_now(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return (uint32_t)v; }
static uint32_t fpstat_now(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return (uint32_t)v; }
static int df_now(void) { return 0; }
static const char *rmode_name(uint32_t c) { static const char *n[] = {"nearest", "up", "down", "zero"}; return n[c >> 22 & 3]; }
static int fz_bit(uint32_t c) { return (c >> 24) & 1; }
static unsigned flags_of(uint32_t s) { return s & 0x1f; }
#endif

static void flag_names(unsigned f, char *b, size_t n)
{
    static const char *nm[] = {"invalid", "divbyzero", "overflow", "underflow", "inexact"};
    b[0] = 0;
    for (int k = 0; k < 5; k++)
        if (f & (1u << k)) { strlcat(b, b[0] ? " " : "", n); strlcat(b, nm[k], n); }
    if (!b[0]) strlcpy(b, "none", n);
}

static void out(const char *s) { fputs(s, stdout); fflush(stdout); }

extern "C" void uh_fill(uh_ctx *c, int salt)
{
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 10; k++) c->gp[k] = uh_val(k + salt);
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 16; j++) c->vec[k][j] = (uint8_t)((k * 16 + j) * 7 + 1 + salt);
    for (int k = 0; k < 16; k++)
        for (int j = 0; j < 16; j++) c->yhi[k][j] = (uint8_t)((k * 16 + j) * 5 + 3 + salt);
    for (int k = 0; k < 11; k++) c->keep[k] = uh_val(40 + k + salt);
    for (int k = 0; k < 8; k++) c->fpkeep[k] = uh_val(60 + k + salt);
    c->fpctl = FPCTL_SET;
#if defined(__x86_64__)
    c->x87cw = X87_SET;
#else
    c->fpstat = FPSTAT_SET;
#endif
    c->avx = (uint32_t)have_avx();
}

/* the classes, each architecture's registers behind them; 0 = all good */
enum { K_RET = 1, K_GP = 2, K_SP = 4, K_FPCTL = 8, K_DF = 16, K_VEC = 32 };
static int verify(const uh_ctx *c, uint64_t want_ret, char *why, size_t n)
{
    int bad = 0;
    why[0] = 0;
    if (c->ret != want_ret) bad |= K_RET;
#if defined(__x86_64__)
    static const int kept[] = {0, 1, 2, 3, 4, 10};   /* rbx r13 r12 r14 r15 rbp */
    for (int k : kept)
        if (c->keep_after[k] != c->keep[k]) { bad |= K_GP; snprintf(why + strlen(why), n - strlen(why), " keep[%d]=%#llx", k, (unsigned long long)c->keep_after[k]); }
    if ((c->fpctl_after & FPCTL_CTL) != (c->fpctl & FPCTL_CTL) || c->x87cw_after != c->x87cw) {
        bad |= K_FPCTL;
        snprintf(why + strlen(why), n - strlen(why), " mxcsr=%#x x87cw=%#x", c->fpctl_after, c->x87cw_after);
    }
    if (c->flags_after & 0x400) bad |= K_DF;
#else
    for (int k = 0; k < 11; k++)
        if (c->keep_after[k] != c->keep[k]) { bad |= K_GP; snprintf(why + strlen(why), n - strlen(why), " x%d=%#llx", k == 10 ? 29 : 19 + k, (unsigned long long)c->keep_after[k]); }
    if (c->fpctl_after != c->fpctl) { bad |= K_FPCTL; snprintf(why + strlen(why), n - strlen(why), " fpcr=%#x", c->fpctl_after); }
    for (int k = 0; k < 8; k++)
        if (c->fpkeep_after[k] != c->fpkeep[k]) { bad |= K_VEC; snprintf(why + strlen(why), n - strlen(why), " d%d", 8 + k); }
#endif
    if (c->sp_after != c->sp_before) bad |= K_SP;
    return bad;
}

/* one call through uh_call_full, then the FP environment as it was */
static int call_checked(uh_ctx *c, uint64_t want, char *why, size_t n)
{
    fenv_t env;
    fegetenv(&env);
    uh_call_full(c);
    fesetenv(&env);
    return verify(c, want, why, n);
}

/* print: 1 the classes, 2 also the FP status flags after the return */
extern "C" int uh_check(uh_ctx *c, const char *tag, int print, uint64_t want_ret)
{
    if (!want_ret) want_ret = uh_sum(c->gp, c->vec);
    char why[512];
    fenv_t env;
    fegetenv(&env);
    uh_call_full(c);
    uint32_t st = fpstat_now();
    fesetenv(&env);
#if defined(__x86_64__)
    c->fpstat_after = st;
#endif
    int bad = verify(c, want_ret, why, sizeof why);
    if (print) {
        char b[1024];
        snprintf(b, sizeof b,
                 "%s: return %s\n%s: callee-saved gp %s\n%s: stack pointer %s\n%s: fp control %s\n%s: direction flag %s\n%s: callee-saved fp %s\n",
                 tag, bad & K_RET ? "BAD" : "ok", tag, bad & K_GP ? "BAD" : "ok", tag, bad & K_SP ? "BAD" : "ok", tag,
                 bad & K_FPCTL ? "BAD" : "ok", tag, bad & K_DF ? "set (BAD)" : "clear", tag, bad & K_VEC ? "BAD" : "ok");
        out(b);
        if (bad) { snprintf(b, sizeof b, "%s: detail:%s ret=%#llx\n", tag, why, (unsigned long long)c->ret); out(b); }
        if (print == 2) {
            char f[128];
            flag_names(flags_of(c->fpstat_after), f, sizeof f);
            snprintf(b, sizeof b, "%s: fp flags after the return: %s\n", tag, f);
            out(b);
        }
    }
    return bad;
}

static void mask_names(const sigset_t *m, char *b, size_t n)
{
    static const struct { int s; const char *nm; } sg[] = {
        {SIGHUP, "HUP"}, {SIGINT, "INT"}, {SIGQUIT, "QUIT"}, {SIGILL, "ILL"}, {SIGTRAP, "TRAP"}, {SIGABRT, "ABRT"}, {SIGEMT, "EMT"},
        {SIGFPE, "FPE"}, {SIGBUS, "BUS"}, {SIGSEGV, "SEGV"}, {SIGSYS, "SYS"}, {SIGPIPE, "PIPE"}, {SIGALRM, "ALRM"}, {SIGTERM, "TERM"},
        {SIGURG, "URG"}, {SIGTSTP, "TSTP"}, {SIGCONT, "CONT"}, {SIGCHLD, "CHLD"}, {SIGTTIN, "TTIN"}, {SIGTTOU, "TTOU"}, {SIGIO, "IO"},
        {SIGXCPU, "XCPU"}, {SIGXFSZ, "XFSZ"}, {SIGVTALRM, "VTALRM"}, {SIGPROF, "PROF"}, {SIGWINCH, "WINCH"}, {SIGINFO, "INFO"},
        {SIGUSR1, "USR1"}, {SIGUSR2, "USR2"}};
    b[0] = 0;
    for (auto &e : sg)
        if (sigismember(m, e.s) == 1) { strlcat(b, " ", n); strlcat(b, e.nm, n); }
    if (!b[0]) strlcpy(b, " none", n);
}

extern "C" void uh_mask_print(const char *tag)
{
    sigset_t m;
    pthread_sigmask(SIG_BLOCK, NULL, &m);
    char b[256], l[320];
    mask_names(&m, b, sizeof b);
    snprintf(l, sizeof l, "%s: blocked:%s\n", tag, b);
    out(l);
}

/* the native view from inside a guest function */
extern "C" void uh_view(const char *tag)
{
    uint32_t c = fpctl_now(), s = fpstat_now();
    char f[128], b[512];
    flag_names(flags_of(s), f, sizeof f);
#if defined(__x86_64__)
    const char *x87 = x87_now() == X87_SET ? "as set" : "CHANGED";
#else
    const char *x87 = "as set";
#endif
    snprintf(b, sizeof b, "%s: native view: rounding %s, flush-to-zero %d, flags %s, direction flag %s, x87 control %s\n", tag, rmode_name(c),
             fz_bit(c), f, df_now() ? "set" : "clear", x87);
    out(b);
}

extern "C" __attribute__((disable_tail_calls, noinline)) long uh_call_slot(uh_fn *slot, long v)
{
    return (*slot)(v);
}

extern "C" __attribute__((disable_tail_calls, noinline)) long uh_catch_slot(uh_fn *slot, long v)
{
    try {
        return (*slot)(v);
    } catch (int e) {
        return 1000 + e;
    }
}

extern "C" __attribute__((noinline)) void uh_throw(int v) { throw v; }

extern "C" __attribute__((noinline)) void uh_marker(void) { __asm__ volatile(""); }

/* n calls, each verified; maskcheck: the signal mask before and after each */
extern "C" int uh_loop(const uint64_t *fn, int n, int maskcheck)
{
    int fails = 0;
    char why[512];
    for (int k = 0; k < n; k++) {
        uh_ctx c;
        uh_fill(&c, k & 15);
        c.fn = *fn;
        sigset_t a, b;
        if (maskcheck) pthread_sigmask(SIG_BLOCK, NULL, &a);
        if (call_checked(&c, uh_sum(c.gp, c.vec), why, sizeof why)) {
            if (fails++ < 3) { char l[640]; snprintf(l, sizeof l, "loop: call %d: bad:%s ret=%#llx\n", k, why, (unsigned long long)c.ret); out(l); }
        }
        if (maskcheck) {
            pthread_sigmask(SIG_BLOCK, NULL, &b);
            if (memcmp(&a, &b, sizeof a)) {
                if (fails++ < 3) { char l[640], x[256], y[256]; mask_names(&a, x, sizeof x); mask_names(&b, y, sizeof y);
                    snprintf(l, sizeof l, "loop: call %d: mask before:%s after:%s\n", k, x, y); out(l); }
            }
        }
    }
    return fails;
}

struct nt { const uint64_t *fn; int n; int fails; };
static void *nt_main(void *p)
{
    nt *a = (nt *)p;
    a->fails = uh_loop(a->fn, a->n, 1);
    return NULL;
}

extern "C" int uh_nthreads(const uint64_t *fn, int nthreads, int n)
{
    pthread_t th[64];
    nt a[64];
    if (nthreads > 64) nthreads = 64;
    for (int k = 0; k < nthreads; k++) { a[k] = nt{fn, n, 0}; pthread_create(&th[k], NULL, nt_main, &a[k]); }
    int fails = 0;
    for (int k = 0; k < nthreads; k++) { pthread_join(th[k], NULL); fails += a[k].fails; }
    return fails;
}

/* the mask around the redirect: SIGUSR1, SIGUSR2 and SIGALRM blocked by
 * native code; the guest callee prints it from inside */
extern "C" void uh_mask_case(const uint64_t *fn)
{
    sigset_t set, old, before, after;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    pthread_sigmask(SIG_BLOCK, NULL, &before);
    uh_mask_print("mask before the call");
    uh_ctx c;
    uh_fill(&c, 0);
    c.fn = *fn;
    uh_check(&c, "mask call", 1, 0);
    pthread_sigmask(SIG_BLOCK, NULL, &after);
    uh_mask_print("mask after the return");
    out(memcmp(&before, &after, sizeof before) ? "mask unchanged: NO\n" : "mask unchanged: yes\n");
    int f = uh_loop(fn, 300, 1);
    char b[128];
    snprintf(b, sizeof b, "mask loop: 300 calls, %d failures\n", f);
    out(b);
    /* nothing pending got lost or delivered: raise one, still pending */
    raise(SIGUSR2);
    sigset_t pend;
    sigpending(&pend);
    out(sigismember(&pend, SIGUSR2) == 1 ? "mask: SIGUSR2 pending while blocked\n" : "mask: SIGUSR2 NOT pending\n");
    signal(SIGUSR2, SIG_IGN);     /* discard it on unblock */
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    signal(SIGUSR2, SIG_DFL);
}

/* a native call of the guest's dladdr: under hao a trap address, which takes
 * the NX path to hao's trap (runtime.c on_fault, native->trap) */
extern "C" void uh_trap_case(const uint64_t *dladdr_fn)
{
    Dl_info info;
    memset(&info, 0, sizeof info);
    uh_ctx c;
    uh_fill(&c, 3);
    c.fn = *dladdr_fn;
    c.gp[0] = (uint64_t)(uintptr_t)(void *)uh_marker;
    c.gp[1] = (uint64_t)(uintptr_t)&info;
    uh_check(&c, "trap", 1, 1);
    const char *f = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
    char b[256];
    snprintf(b, sizeof b, "trap: dladdr image %s symbol %s\n", f ? f + 1 : "?", info.dli_sname ? info.dli_sname : "?");
    out(b);
    int fails = 0;
    char why[512];
    for (int k = 0; k < 200; k++) {
        memset(&info, 0, sizeof info);
        uh_fill(&c, k & 15);
        c.fn = *dladdr_fn;
        c.gp[0] = (uint64_t)(uintptr_t)(void *)uh_marker;
        c.gp[1] = (uint64_t)(uintptr_t)&info;
        if (call_checked(&c, 1, why, sizeof why) || info.dli_saddr != (void *)uh_marker) fails++;
    }
    snprintf(b, sizeof b, "trap loop: 200 calls, %d failures\n", fails);
    out(b);
}

/* ---- sigaltstack installed by native code on this thread */
static stack_t alt;
extern "C" void uh_altstack_on(void)
{
    alt.ss_size = 1 << 17;
    alt.ss_sp = mmap(NULL, alt.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    alt.ss_flags = 0;
    out(sigaltstack(&alt, NULL) == 0 ? "altstack: installed\n" : "altstack: install failed\n");
}

extern "C" void uh_altstack_check(void)
{
    stack_t cur;
    if (sigaltstack(NULL, &cur) != 0) { out("altstack: query failed\n"); return; }
    char b[256];
    snprintf(b, sizeof b, "altstack: still installed %s, size %s, on it now %s\n", cur.ss_sp == alt.ss_sp && !(cur.ss_flags & SS_DISABLE) ? "yes" : "NO",
             cur.ss_size == alt.ss_size ? "same" : "CHANGED", cur.ss_flags & SS_ONSTACK ? "YES" : "no");
    out(b);
    stack_t off;
    memset(&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, NULL);
}

/* ---- every register across a fault that a native handler fixes and
 * returns from (under hao: on_fault forwards it, then resumes) */
static void *fw_page;
static size_t fw_size;
static volatile int fw_hits;
static struct sigaction fw_prev[2];
static void fw_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    if ((char *)si->si_addr >= (char *)fw_page && (char *)si->si_addr < (char *)fw_page + fw_size) {
        fw_hits++;
        mprotect(fw_page, fw_size, PROT_READ);
        /* the handler's own FP and flag use must not leak into the resumed context */
        volatile double z = 0.0, r = 1.0 / z;
        (void)r;
        return;
    }
    _exit(99);
}

static int fw_once(int salt, int avx, char *why, size_t n)
{
    uh_full f;
    memset(&f, 0, sizeof f);
    for (int k = 0; k < 32; k++)
        for (int j = 0; j < 16; j++) f.vin[k][j] = (uint8_t)((k * 16 + j) * 3 + 11 + salt);
    for (int k = 0; k < 16; k++)
        for (int j = 0; j < 16; j++) f.yhin[k][j] = (uint8_t)((k * 16 + j) * 13 + 5 + salt);
    for (int k = 0; k < 32; k++) f.gin[k] = uh_val(80 + k + salt);
    f.addr = (uint64_t)(uintptr_t)fw_page;
    f.avx = (uint32_t)avx;
#if defined(__x86_64__)
    f.flags_in = 0xCD7;              /* CF PF AF ZF SF DF OF (+ the reserved bit 1) */
    f.fpctl_in = 0xDFA0 | 0x10;      /* RC up, FTZ, masks; PE and UE raised */
    f.x87_in = X87_SET;
    static const int nregs = 16, skip_a = 4, skip_b = 7;   /* rsp; rdi holds the address */
#else
    f.flags_in = 0xF0000000;         /* N Z C V */
    f.fpctl_in = FPCTL_SET;
    f.fpstat_in = 0x18;              /* IXC UFC */
    static const int nregs = 31, skip_a = 18, skip_b = 9;  /* x18; x9 holds the address */
#endif
    mprotect(fw_page, fw_size, PROT_NONE);
    fenv_t env;
    fegetenv(&env);
    uh_fault_full(&f);
    fesetenv(&env);
    int bad = 0;
    why[0] = 0;
    for (int k = 0; k < nregs; k++) {
        if (k == skip_a) continue;
        uint64_t want = k == skip_b ? 0x1122334455667788ull : f.gin[k];
        if (f.gout[k] != want) { bad |= 1; snprintf(why + strlen(why), n - strlen(why), " gp%d=%#llx", k, (unsigned long long)f.gout[k]); }
    }
    if (f.sp_out != f.sp_in) bad |= 2;
#if defined(__x86_64__)
    if ((f.flags_out & 0xCD5) != (f.flags_in & 0xCD5)) { bad |= 4; snprintf(why + strlen(why), n - strlen(why), " rflags=%#llx", (unsigned long long)f.flags_out); }
    if (memcmp(f.vin, f.vout, 16 * 16)) bad |= 8;
    if (avx && memcmp(f.yhin, f.yhout, sizeof f.yhin)) bad |= 16;
    if (f.fpctl_out != f.fpctl_in || f.x87_out != f.x87_in) { bad |= 32; snprintf(why + strlen(why), n - strlen(why), " mxcsr=%#x x87=%#x", f.fpctl_out, f.x87_out); }
#else
    if ((f.flags_out & 0xF0000000) != f.flags_in) bad |= 4;
    if (memcmp(f.vin, f.vout, sizeof f.vin)) bad |= 8;
    if (f.fpctl_out != f.fpctl_in || f.fpstat_out != f.fpstat_in) { bad |= 32; snprintf(why + strlen(why), n - strlen(why), " fpcr=%#x fpsr=%#x", f.fpctl_out, f.fpstat_out); }
#endif
    return bad;
}

extern "C" void uh_fwd(int n)
{
    fw_size = (size_t)getpagesize();
    fw_page = mmap(NULL, fw_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    *(volatile uint64_t *)fw_page = 0x1122334455667788ull;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fw_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &fw_prev[0]);
    sigaction(SIGBUS, &sa, &fw_prev[1]);
    sigset_t before, after;
    pthread_sigmask(SIG_BLOCK, NULL, &before);
    int avx = have_avx();
    int acc = 0, fails = 0;
    char why[512], first[512] = "";
    fw_hits = 0;
    for (int k = 0; k < n; k++) {
        int b = fw_once(k & 15, avx, why, sizeof why);
        if (b && !first[0]) strlcpy(first, why, sizeof first);
        acc |= b;
        fails += b != 0;
    }
    pthread_sigmask(SIG_BLOCK, NULL, &after);
    sigaction(SIGSEGV, &fw_prev[0], NULL);
    sigaction(SIGBUS, &fw_prev[1], NULL);
    char b[1024];
    snprintf(b, sizeof b,
             "fwd: %d faults, handler ran %s\nfwd: all gp %s\nfwd: stack pointer %s\nfwd: flags %s\nfwd: vector registers %s\nfwd: ymm upper halves %s\n"
             "fwd: fp control and status %s\nfwd: mask after %s\n",
             n, fw_hits == n ? "each time" : "NOT each time", acc & 1 ? "BAD" : "ok", acc & 2 ? "BAD" : "ok", acc & 4 ? "BAD" : "ok",
             acc & 8 ? "BAD" : "ok", acc & 16 ? "BAD" : "ok", acc & 32 ? "BAD" : "ok", memcmp(&before, &after, sizeof before) ? "CHANGED" : "unchanged");
    out(b);
    if (fails) { snprintf(b, sizeof b, "fwd: %d bad, first:%s\n", fails, first); out(b); }
    munmap(fw_page, fw_size);
}
