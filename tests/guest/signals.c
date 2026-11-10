/* Guest signal semantics, one mode per run (tests/guest/signals.sh runs each
 * natively and under hao and compares stdout and the exit status):
 *   disp      disposition bookkeeping: sigaction/signal round trips, flags
 *             and masks, SA_RESETHAND, failed installs, owned signals
 *   uctx      the arm64 ucontext of SA_SIGINFO handlers: registers, pc, far,
 *             and a SIGSEGV handler that edits x1 and pc to recover
 *   prof      ITIMER_PROF samples land in the running guest function
 *   jitlock   SIGALRM every 300 us while code is being translated, with a
 *             handler that runs code nobody ran before
 *   native    a guest handler on native threads that never ran guest code
 *             and are inside malloc
 *   mlocked   the same on native threads that hold every malloc zone lock
 *             when the signal arrives: hao's delivery path must not allocate
 *   noalloc   the allocations made while a signal is delivered, counted
 *             with malloc_logger (all zones, nano too): none, as natively
 *   restart   a SA_RESTART handler interrupting a blocked read()
 *   segvnative  a native function (_exit) as the SIGSEGV handler
 *   segvreset   a SA_RESETHAND SIGSEGV handler: recovers once, then the
 *               second fault takes the default action
 *   segvrecurse a SIGSEGV handler that faults again (no SA_NODEFER): the
 *               default action, not unbounded recursion
 *   segvjmp     a SIGSEGV/SIGBUS handler that leaves by siglongjmp, three
 *               faults in a row (signal() and SA_SIGINFO): each recovers */
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define STRIP(f) ((uintptr_t)ptrauth_strip((void *)(f), ptrauth_key_function_pointer))
#define SET_PC_RAW(uc, a) do { void (*f_)(void) = ptrauth_sign_unauthenticated((void (*)(void))(a), ptrauth_key_function_pointer, 0); \
        __darwin_arm_thread_state64_set_pc_fptr((uc)->uc_mcontext->__ss, f_); } while (0)
#else
#define SET_PC_RAW(uc, a) ((uc)->uc_mcontext->__ss.__pc = (a))
#define STRIP(f) ((uintptr_t)(void *)(f))
#endif

#define SS(uc) ((uc)->uc_mcontext->__ss)
#define GET_PC(uc) ((uintptr_t)__darwin_arm_thread_state64_get_pc(SS(uc)))
#define GET_SP(uc) ((uintptr_t)__darwin_arm_thread_state64_get_sp(SS(uc)))

static void say(const char *s) { write(1, s, strlen(s)); }

/* ------------------------------------------------------------ disp */
static volatile int hits;
static void h1(int s) { (void)s; hits++; }
static void h2(int s, siginfo_t *i, void *u) { (void)s; (void)i; (void)u; hits++; }
static void h3(int s) { (void)s; }

static const char *name(void *h)
{
    if (h == (void *)SIG_DFL) return "SIG_DFL";
    if (h == (void *)SIG_IGN) return "SIG_IGN";
    if (h == (void *)h1) return "h1";
    if (h == (void *)h2) return "h2";
    if (h == (void *)h3) return "h3";
    if (h == (void *)abort) return "abort";
    return "other";
}

static int disp(void)
{
    struct sigaction sa, oa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h1;
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    sigaction(SIGUSR1, NULL, &oa);
    printf("resethand: hits=%d now=%s\n", hits, name((void *)oa.sa_handler));

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h2;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaddset(&sa.sa_mask, SIGHUP);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR1, NULL, &oa);
    printf("roundtrip: handler=%s flags=%#x usr2=%d hup=%d int=%d\n", name((void *)oa.sa_sigaction), oa.sa_flags,
           sigismember(&oa.sa_mask, SIGUSR2), sigismember(&oa.sa_mask, SIGHUP), sigismember(&oa.sa_mask, SIGINT));
    raise(SIGUSR1);
    printf("roundtrip: hits=%d\n", hits);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h1;
    errno = 0;
    int r = sigaction(SIGKILL, &sa, NULL);
    printf("sigkill install: r=%d errno=%d\n", r, errno);
    sigaction(SIGKILL, NULL, &oa);
    printf("sigkill now=%s\n", name((void *)oa.sa_handler));
    errno = 0;
    r = sigaction(SIGSTOP, &sa, &oa);
    printf("sigstop install: r=%d errno=%d old=%s\n", r, errno, name((void *)oa.sa_handler));
    errno = 0;
    r = sigaction(0, &sa, NULL);
    printf("sig 0: r=%d errno=%d\n", r, errno);
    errno = 0;
    r = sigaction(NSIG, &sa, NULL);
    printf("sig NSIG: r=%d errno=%d\n", r, errno);

    void (*p)(int) = signal(SIGUSR2, h3);
    printf("signal: %s", name((void *)p));
    p = signal(SIGUSR2, SIG_IGN);
    printf(" %s", name((void *)p));
    p = signal(SIGUSR2, SIG_DFL);
    printf(" %s\n", name((void *)p));
    sigaction(SIGUSR2, NULL, &oa);
    printf("signal flags=%#x\n", oa.sa_flags);
    errno = 0;
    p = signal(SIGKILL, h3);
    printf("signal(SIGKILL): err=%d errno=%d\n", p == SIG_ERR, errno);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = (void (*)(int))abort;
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGUSR2, NULL, &oa);
    printf("native handler: %s\n", name((void *)oa.sa_handler));
    signal(SIGUSR2, SIG_DFL);

    sigaction(SIGBUS, NULL, &oa);
    printf("sigbus default: %s flags=%#x\n", name((void *)oa.sa_handler), oa.sa_flags);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h2;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGSEGV, NULL, &oa);
    printf("sigsegv: %s flags=%#x usr2=%d\n", name((void *)oa.sa_sigaction), oa.sa_flags, sigismember(&oa.sa_mask, SIGUSR2));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &oa);
    printf("sigsegv old: %s\n", name((void *)oa.sa_sigaction));
    sigaction(SIGSEGV, NULL, &oa);
    printf("sigsegv now: %s\n", name((void *)oa.sa_handler));
    signal(SIGSEGV, SIG_DFL);
    return 0;
}

/* ------------------------------------------------------------ uctx */
__attribute__((noinline)) long probe(volatile long *p, long b) { return *p + b; }
__attribute__((noinline)) long probe_fail(long a, long b) { (void)a; return b + 1000; }
static long (*volatile probe_p)(volatile long *, long) = probe;
static uintptr_t main_sp;
static volatile long *bad = (volatile long *)(uintptr_t)0x10;

static void segv_fix(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uintptr_t pc = GET_PC(uc), sp = GET_SP(uc);
    printf("segv: sig=%d addr-ok=%d far-ok=%d mcsize-ok=%d pc-in-probe=%d x0-ok=%d x1=%llu sp-near=%d\n", sig,
           si->si_addr == (void *)bad, uc->uc_mcontext->__es.__far == (uintptr_t)bad,
           uc->uc_mcsize == sizeof *uc->uc_mcontext, pc >= STRIP(probe) && pc < STRIP(probe) + 16,
           SS(uc).__x[0] == (uintptr_t)bad, (unsigned long long)SS(uc).__x[1],
           sp < main_sp && main_sp - sp < 65536);
    fflush(stdout);
    SS(uc).__x[1] = 7;
    long (*fix)(long, long) = probe_fail;
    __darwin_arm_thread_state64_set_pc_fptr(SS(uc), fix);
}

/* reached by a direct bl, with work before the faulting load: the pc must
 * be the load itself, not the block or function the guest entered last */
__attribute__((noinline)) long probe2(volatile long *p, long b)
{
    long c = b * 7 + 3;
    __asm__ volatile("" : "+r"(c));
    c ^= b >> 2;
    __asm__ volatile("" : "+r"(c));
    return *p + c;
}

static void segv_skip(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    uintptr_t pc = GET_PC(uc), lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(SS(uc));
    uint32_t insn = pc >= STRIP(probe2) && pc < STRIP(probe2) + 64 ? *(const uint32_t *)pc : 0;
    printf("segv2: pc-in-probe2=%d past-entry=%d insn-is-load=%d\n", insn != 0, pc > STRIP(probe2),
           (insn & 0x0A000000) == 0x08000000 && (insn & (1u << 22)));
    fflush(stdout);
    /* leaf function: "return" 4242 to the caller */
    SS(uc).__x[0] = 4242;
    SET_PC_RAW(uc, lr);
}

static void usr1_ctx(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uintptr_t sp = GET_SP(uc);
    printf("usr1: sig=%d code=%d pid-ok=%d mcsize-ok=%d sp-near=%d\n", sig, si->si_code, si->si_pid == getpid(),
           uc->uc_mcsize == sizeof *uc->uc_mcontext, sp < main_sp && main_sp - sp < 65536);
}

static int uctx(void)
{
    volatile int here = 0;
    main_sp = (uintptr_t)&here;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = usr1_ctx;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    sa.sa_sigaction = segv_fix;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    long r = probe_p(bad, 5);
    printf("probe recovered: %ld\n", r);
    r = probe_p(bad, 6);
    printf("probe recovered again: %ld\n", r);
    sa.sa_sigaction = segv_skip;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    r = probe2(bad, 3);
    printf("probe2 skipped: %ld\n", r);
    return 0;
}

/* ------------------------------------------------------------ prof */
static volatile int samples, in_spin;
static void prof_h(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    uintptr_t pc = GET_PC(uc);
    extern long spin(long);
    if (pc >= STRIP(spin) && pc < STRIP(spin) + 512) in_spin++;
    samples++;
}

__attribute__((noinline)) long spin(long x)
{
    while (samples < 40) x = x * 6364136223846793005L + 1442695040888963407L;
    return x;
}

static int prof(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_h;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 2000}, {0, 2000}};
    setitimer(ITIMER_PROF, &it, NULL);
    long x = spin(1);
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_PROF, &it, NULL);
    int s = samples, in = in_spin;
    printf("prof: samples>=40 %d, in spin >= 90%%: %d\n", s >= 40, in * 10 >= s * 9);
    return x == 0;
}

/* --------------------------------------------------------- jitlock */
#define F(n) __attribute__((noinline)) static long f##n(long x) { return (x * (n + 3)) ^ (x >> ((n % 5) + 1)) ^ n; }
#define F10(n) F(n##0) F(n##1) F(n##2) F(n##3) F(n##4) F(n##5) F(n##6) F(n##7) F(n##8) F(n##9)
#define F100(n) F10(n##0) F10(n##1) F10(n##2) F10(n##3) F10(n##4) F10(n##5) F10(n##6) F10(n##7) F10(n##8) F10(n##9)
F100(1) F100(2) F100(3) F100(4) F100(5) F100(6) F100(7) F100(8)
#define R(n) f##n,
#define R10(n) R(n##0) R(n##1) R(n##2) R(n##3) R(n##4) R(n##5) R(n##6) R(n##7) R(n##8) R(n##9)
#define R100(n) R10(n##0) R10(n##1) R10(n##2) R10(n##3) R10(n##4) R10(n##5) R10(n##6) R10(n##7) R10(n##8) R10(n##9)
static long (*const main_fns[])(long) = {R100(1) R100(2) R100(3) R100(4) R100(5) R100(6)};
static long (*const handler_fns[])(long) = {R100(7) R100(8)};
static volatile long ticks, hsum;

static void alrm(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si; (void)ctx;
    long k = ticks++;
    hsum += handler_fns[k % (long)(sizeof handler_fns / sizeof handler_fns[0])](k);
}

static int jitlock(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = alrm;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 300}, {0, 300}};
    setitimer(ITIMER_REAL, &it, NULL);
    long sum = 0;
    for (size_t k = 0; k < sizeof main_fns / sizeof main_fns[0]; k++) sum += main_fns[k]((long)k + 1);
    while (ticks < 400) sum += main_fns[ticks % 600](3) & 1;
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
    printf("jitlock: ticks>=400 %d sum-ok=%d\n", ticks >= 400, sum != 0);
    return 0;
}

/* ---------------------------------------------------------- native */
void *sighelper_malloc_loop(void *stop);
static volatile int handled;
static void usr1_count(int sig) { (void)sig; __atomic_fetch_add(&handled, 1, __ATOMIC_RELAXED); }

static int native(void)
{
    signal(SIGUSR1, usr1_count);
    int ok = 0;
    for (int k = 0; k < 16; k++) {
        static int stop;
        stop = 0;
        pthread_t th;
        pthread_create(&th, NULL, sighelper_malloc_loop, &stop);
        usleep(2000);
        int before = handled;
        pthread_kill(th, SIGUSR1);
        for (int w = 0; w < 2000 && handled == before; w++) usleep(1000);
        if (handled != before) ok++;
        __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
        pthread_join(th, NULL);
    }
    printf("native threads: handled %d of 16\n", ok);
    return 0;
}

/* Each thread takes the default zone's locks, signals itself and releases
 * them (sighelper.c): the handler runs with the locks held, so anything
 * hao allocates on the way to it deadlocks or traps, every time. */
void *sighelper_locked_signal(void *sig);
static int mlocked(void)
{
    signal(SIGUSR1, usr1_count);
    int ok = 0;
    for (int k = 0; k < 8; k++) {
        int before = handled;
        pthread_t th;
        pthread_create(&th, NULL, sighelper_locked_signal, (void *)(intptr_t)SIGUSR1);
        pthread_join(th, NULL);
        if (handled != before) ok++;
    }
    printf("malloc-locked native threads: handled %d of 8\n", ok);
    return 0;
}

/* --------------------------------------------------------- noalloc
 * Count the allocations made while a signal is delivered (sighelper.c sets
 * malloc_logger, which sees every zone, the nano zone too): on native
 * threads that never ran guest code, on the main thread, and on a
 * sigaltstack. Each first delivery runs a handler nobody ran before, which
 * calls a native function. */
long sighelper_count_signal(int sig);
void *sighelper_count_thread(void *sig);
static volatile int na_hits;
static volatile long na_pid;
#define NA_H(n) static void na_h##n(int s, siginfo_t *i, void *u) { (void)s; (void)i; (void)u; na_pid ^= getpid() + n; na_hits++; }
NA_H(0) NA_H(1) NA_H(2) NA_H(3) NA_H(4) NA_H(5)
static void (*const na_fns[])(int, siginfo_t *, void *) = {na_h0, na_h1, na_h2, na_h3, na_h4, na_h5};

static void na_install(int k, int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = na_fns[k];
    sa.sa_flags = SA_SIGINFO | flags;
    sigaction(SIGUSR2, &sa, NULL);
}

static int noalloc(void)
{
    for (int k = 0; k < 4; k++) {
        na_install(k, 0);
        int before = na_hits;
        pthread_t th;
        void *n = NULL;
        pthread_create(&th, NULL, sighelper_count_thread, (void *)(intptr_t)SIGUSR2);
        pthread_join(th, &n);
        printf("noalloc native thread %d: handled=%d allocs=%ld\n", k, na_hits - before, (long)n);
    }
    na_install(4, 0);
    for (int k = 0; k < 2; k++) {
        int before = na_hits;
        long n = sighelper_count_signal(SIGUSR2);
        printf("noalloc main thread %s: handled=%d allocs=%ld\n", k ? "warm" : "cold", na_hits - before, n);
    }
    static char alt[1 << 16];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    na_install(5, SA_ONSTACK);
    int before = na_hits;
    long n = sighelper_count_signal(SIGUSR2);
    printf("noalloc sigaltstack: handled=%d allocs=%ld\n", na_hits - before, n);
    return 0;
}

/* --------------------------------------------------------- restart */
static int pfd[2];
static void alrm_write(int sig) { (void)sig; write(pfd[1], "x", 1); }

static int restart(void)
{
    pipe(pfd);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = alrm_write;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 0}, {0, 50000}};
    setitimer(ITIMER_REAL, &it, NULL);
    char c = 0;
    ssize_t n = read(pfd[0], &c, 1);
    printf("restart: read=%zd c=%c\n", n, c);
    return 0;
}

/* ------------------------------------------------------------ segv */
static void segv_once(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    long (*fix)(long, long) = probe_fail;
    __darwin_arm_thread_state64_set_pc_fptr(SS(uc), fix);
}

static void segv_again(int sig)
{
    (void)sig;
    say("segv handler\n");
    probe_p(bad, 0);   /* faults with SIGSEGV blocked */
    say("handler fault returned\n");
}

static sigjmp_buf segv_jb;
static void segv_jmp(int sig) { siglongjmp(segv_jb, sig); }
static void segv_jmp_info(int sig, siginfo_t *si, void *ctx) { (void)si; (void)ctx; siglongjmp(segv_jb, sig); }

static int segv(const char *how)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    if (!strcmp(how, "segvjmp")) {
        for (int round = 0; round < 2; round++) {
            if (!round) { signal(SIGSEGV, segv_jmp); signal(SIGBUS, segv_jmp); }
            else {
                sa.sa_sigaction = segv_jmp_info;
                sa.sa_flags = SA_SIGINFO;
                sigaction(SIGSEGV, &sa, NULL);
                sigaction(SIGBUS, &sa, NULL);
            }
            for (int k = 0; k < 3; k++) {
                int sig = sigsetjmp(segv_jb, 1);
                if (!sig) { probe_p(bad, k); say("not reached\n"); return 1; }
                printf("%s fault %d recovered by siglongjmp (signal %s)\n", round ? "SA_SIGINFO" : "signal()", k,
                       sig == SIGSEGV ? "SEGV" : sig == SIGBUS ? "BUS" : "?");
            }
        }
        return 0;
    }
    if (!strcmp(how, "segvnative")) {
        sa.sa_handler = (void (*)(int))_exit;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        say("faulting\n");
        probe_p(bad, 0);
        say("not reached\n");
        return 1;
    }
    if (!strcmp(how, "segvreset")) {
        sa.sa_sigaction = segv_once;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        long r = probe_p(bad, 1);
        struct sigaction oa;
        sigaction(SIGSEGV, NULL, &oa);
        printf("first fault recovered: %ld now=%s\n", r, name((void *)oa.sa_handler));
        fflush(stdout);
        probe_p(bad, 2);
        say("not reached\n");
        return 1;
    }
    sa.sa_handler = segv_again;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    probe_p(bad, 0);
    say("not reached\n");
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "disp")) return disp();
    if (!strcmp(m, "uctx")) return uctx();
    if (!strcmp(m, "prof")) return prof();
    if (!strcmp(m, "jitlock")) return jitlock();
    if (!strcmp(m, "native")) return native();
    if (!strcmp(m, "mlocked")) return mlocked();
    if (!strcmp(m, "noalloc")) return noalloc();
    if (!strcmp(m, "restart")) return restart();
    if (!strncmp(m, "segv", 4)) return segv(m);
    fprintf(stderr, "usage: signals disp|uctx|prof|jitlock|native|mlocked|noalloc|restart|segvnative|segvreset|segvrecurse|segvjmp\n");
    return 2;
}
