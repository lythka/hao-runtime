/* Native helper for tests/guest/nativesig.c, built universal (x86_64, arm64,
 * arm64e). Under hao the guest binds to the x86_64 slice: everything here
 * is native code that installs SIGSEGV/SIGBUS handlers with the native API,
 * as libraries the guest loads do (LLVM's CrashRecoveryContext and signal
 * handlers in libBNNS, JIT runtimes, crash reporters). */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* a native function that calls a function pointer it finds in memory: a
 * guest function reaches it only through the NX-fault path */
typedef int (*nsh_fn)(int);
int nsh_call_slot(nsh_fn *slot, int v) { return (*slot)(v); }

static void say(const char *s) { write(1, s, strlen(s)); }

/* ---- llvm::CrashRecoveryContext as libBNNS carries it: six signals with
 * sa_flags 0 (the signal blocked in the handler), the previous handlers
 * saved, and the current context in a process-wide global (libBNNS's
 * CurrentContext is in __bss, not thread-local) */
static const int crc_sigs[] = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP};
static struct sigaction crc_prev[6];
static int crc_on;
typedef struct { jmp_buf jb; volatile int sig; } crc_ctx;
static crc_ctx *volatile crc_current;

static void crc_disable(void)
{
    if (!crc_on) return;
    crc_on = 0;
    for (int k = 0; k < 6; k++) sigaction(crc_sigs[k], &crc_prev[k], NULL);
}

void nsh_crc_handler(int sig)
{
    crc_ctx *c = crc_current;
    if (!c) {
        /* no recovery context: put the old handlers back and raise again;
         * natively the signal waits until this handler returns */
        crc_disable();
        raise(sig);
        say("crc: handler returns after raise\n");
        return;
    }
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, sig);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    c->sig = sig;
    longjmp(c->jb, 1);
}

void nsh_crc_enable(void)
{
    if (crc_on) return;
    crc_on = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = nsh_crc_handler;
    sigemptyset(&sa.sa_mask);
    for (int k = 0; k < 6; k++) sigaction(crc_sigs[k], &sa, &crc_prev[k]);
}

/* RunSafely: 0, or the signal it recovered from */
int nsh_crc_run(void (*fn)(void *), void *arg)
{
    crc_ctx c;
    c.sig = 0;
    if (setjmp(c.jb) == 0) {
        crc_current = &c;
        fn(arg);
        crc_current = NULL;
        return 0;
    }
    crc_current = NULL;
    return c.sig;
}

/* thread B: a recovery context stays active while thread A (the guest's)
 * calls guest code through the NX path; then B crashes inside it twice */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stage;
static int b_result[3];
static pthread_t b_thread;

static void wait_stage(int s)
{
    pthread_mutex_lock(&mu);
    while (stage < s) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void set_stage(int s)
{
    pthread_mutex_lock(&mu);
    stage = s;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void b_wait_then_fault(void *p)
{
    set_stage(1);
    wait_stage(2);
    *(volatile int *)p = 1;
}

static void b_fault(void *p) { *(volatile long *)p = 2; }

static void b_bus(void *p)
{
    (void)p;
    raise(SIGBUS);   /* a sent fault signal: recovered as well */
}

static void *b_main(void *x)
{
    (void)x;
    b_result[0] = nsh_crc_run(b_wait_then_fault, NULL);
    b_result[1] = nsh_crc_run(b_fault, (void *)8);
    b_result[2] = nsh_crc_run(b_bus, NULL);
    return NULL;
}

void nsh_crc_thread_start(void)
{
    pthread_create(&b_thread, NULL, b_main, NULL);
    wait_stage(1);   /* B is inside RunSafely */
}

void nsh_crc_thread_finish(int out[3])
{
    set_stage(2);
    pthread_join(b_thread, NULL);
    memcpy(out, b_result, sizeof b_result);
}

/* ---- a handler that returns (JIT runtimes): a fault in nsh_probe resumes
 * at nsh_probe_fail, which returns -1; any other fault goes to the handler
 * that was there before (SIG_DFL: restored, and the fault taken again) */
long nsh_probe(const long *p);
long nsh_probe_fail(void);
#if defined(__x86_64__)
__asm__(".globl _nsh_probe\n_nsh_probe:\n  movq (%rdi), %rax\n  ret\n"
        ".globl _nsh_probe_fail\n_nsh_probe_fail:\n  movq $-1, %rax\n  ret\n");
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__rip)
#define SET_PC(uc, a) ((uc)->uc_mcontext->__ss.__rip = (uint64_t)(a))
#else
__asm__(".globl _nsh_probe\n.p2align 2\n_nsh_probe:\n  ldr x0, [x0]\n  ret\n"
        ".globl _nsh_probe_fail\n_nsh_probe_fail:\n  mov x0, #-1\n  ret\n");
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define UC_PC(uc) ((uint64_t)ptrauth_strip((void *)__darwin_arm_thread_state64_get_pc_fptr((uc)->uc_mcontext->__ss), ptrauth_key_function_pointer))
#define SET_PC(uc, a) __darwin_arm_thread_state64_set_pc_fptr((uc)->uc_mcontext->__ss, (void *)(a))
#else
#define UC_PC(uc) ((uint64_t)__darwin_arm_thread_state64_get_pc((uc)->uc_mcontext->__ss))
#define SET_PC(uc, a) __darwin_arm_thread_state64_set_pc_fptr((uc)->uc_mcontext->__ss, (void *)(a))
#endif
#endif

static struct sigaction ret_prev;
static volatile int ret_hits, ret_other;

void nsh_ret_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uint64_t pc = UC_PC(uc);
#if __has_feature(ptrauth_calls)
    uint64_t probe = (uint64_t)ptrauth_strip((void *)nsh_probe, ptrauth_key_function_pointer);
#else
    uint64_t probe = (uint64_t)(uintptr_t)nsh_probe;
#endif
    if (pc == probe) {
        ret_hits++;
        SET_PC(uc, nsh_probe_fail);
        return;
    }
    ret_other++;
    if (ret_prev.sa_handler == SIG_DFL || ret_prev.sa_handler == SIG_IGN) {
        sigaction(sig, &ret_prev, NULL);   /* the fault is taken again */
        return;
    }
    if (ret_prev.sa_flags & SA_SIGINFO) ret_prev.sa_sigaction(sig, si, ctx);
    else ret_prev.sa_handler(sig);
}

void nsh_ret_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = nsh_ret_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &ret_prev);
}

int nsh_ret_hits(void) { return ret_hits; }
int nsh_ret_other(void) { return ret_other; }

/* ---- the native API's view of what native code installed */
void nsh_h1(int sig) { (void)sig; }
void nsh_h2(int sig) { (void)sig; }

static const char *hname(void (*h)(int))
{
    if (h == SIG_DFL) return "SIG_DFL";
    if (h == SIG_IGN) return "SIG_IGN";
    if (h == SIG_ERR) return "SIG_ERR";
    if (h == SIG_HOLD) return "SIG_HOLD";
    if (h == nsh_h1) return "h1";
    if (h == nsh_h2) return "h2";
    if (h == nsh_crc_handler) return "crc";
    if (h == (void (*)(int))nsh_ret_handler) return "ret";
    return "other";
}

static void show(const char *what, int sig)
{
    struct sigaction cur;
    char b[256];
    if (sigaction(sig, NULL, &cur) != 0) { snprintf(b, sizeof b, "%s: sigaction failed\n", what); say(b); return; }
    snprintf(b, sizeof b, "%s: %s flags=%#x mask=%#x\n", what, hname(cur.sa_handler), cur.sa_flags, (unsigned)cur.sa_mask);
    say(b);
}

const char *nsh_name(void (*h)(int)) { return hname(h); }

void nsh_readback(int sig)
{
    char b[256];
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = nsh_h1;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    int r = sigaction(sig, &sa, &old);
    snprintf(b, sizeof b, "sigaction -> %d, old %s\n", r, hname(old.sa_handler));
    say(b);
    show("after sigaction", sig);
    void (*o)(int) = signal(sig, nsh_h2);
    snprintf(b, sizeof b, "signal(h2) -> %s\n", hname(o));
    say(b);
    show("after signal", sig);
    o = bsd_signal(sig, nsh_h1);
    snprintf(b, sizeof b, "bsd_signal(h1) -> %s\n", hname(o));
    say(b);
    show("after bsd_signal", sig);
    o = sigset(sig, nsh_h2);
    snprintf(b, sizeof b, "sigset(h2) -> %s\n", hname(o));
    say(b);
    show("after sigset", sig);
    o = sigset(sig, SIG_HOLD);
    snprintf(b, sizeof b, "sigset(SIG_HOLD) -> %s\n", hname(o));
    say(b);
    o = sigset(sig, nsh_h1);
    snprintf(b, sizeof b, "sigset(h1) while held -> %s\n", hname(o));
    say(b);
    struct sigvec sv, osv;
    memset(&sv, 0, sizeof sv);
    memset(&osv, 0, sizeof osv);
    sv.sv_handler = nsh_h2;
    sv.sv_mask = sigmask(SIGUSR2);
    sv.sv_flags = SV_INTERRUPT | SV_ONSTACK;
    r = sigvec(sig, &sv, &osv);
    snprintf(b, sizeof b, "sigvec(h2) -> %d, old %s flags=%#x mask=%#x\n", r, hname(osv.sv_handler), osv.sv_flags,
             (unsigned)osv.sv_mask);
    say(b);
    show("after sigvec", sig);
    r = sigvec(sig, NULL, &osv);
    snprintf(b, sizeof b, "sigvec(NULL) -> %d, %s flags=%#x mask=%#x\n", r, hname(osv.sv_handler), osv.sv_flags,
             (unsigned)osv.sv_mask);
    say(b);
    r = sigignore(sig);
    snprintf(b, sizeof b, "sigignore -> %d\n", r);
    say(b);
    show("after sigignore", sig);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = nsh_h1;
    sigaction(sig, &sa, NULL);   /* left installed: the guest reads it back */
}

/* ---- a SIGSEGV handler that returns with the registers _sigtramp keeps
 * the ucontext and the sigreturn token in changed (x86_64 rbx/r12; arm64
 * x19/x20): natively its __sigreturn fails and _sigtramp traps */
void nsh_bad_handler(int, siginfo_t *, void *);
#if defined(__x86_64__)
__asm__(".globl _nsh_bad_handler\n_nsh_bad_handler:\n  xorl %ebx, %ebx\n  xorl %r12d, %r12d\n  ret\n");
#else
__asm__(".globl _nsh_bad_handler\n.p2align 2\n_nsh_bad_handler:\n  mov x19, #0\n  mov x20, #0\n  ret\n");
#endif

void nsh_bad_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = nsh_bad_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

/* a fault in native code */
void nsh_null_write(void) { *(volatile int *)(uintptr_t)16 = 1; }
/* a write to a page mapped read-only: a protection fault (SIGBUS) */
void nsh_ro_write(int *p) { *(volatile int *)p = 1; }

int nsh_raise(int sig) { return raise(sig); }

/* ---- a failed __sigreturn, as a thread finds itself after one: at the trap
 * _sigtramp executes after its call of __sigreturn (x86_64 ud2, arm64 brk),
 * with the frame's ucontext and token where _sigtramp keeps them (x86_64
 * rbx/r12, arm64 x20/x19) and errno set by the failed call. The host runs
 * x86_64 under Rosetta, whose sigreturn never returns an error (a changed
 * token or ucontext pointer hangs the thread or is ignored), so the state is
 * built here and the trap is jumped to. Natively (arm64) the brk raises
 * SIGTRAP; under hao the ud2 raises SIGILL, which hao's diagnostics handler
 * takes (src/runtime.c hao_on_abort).
 *   how 0  a native SIGUSR1 handler's frame, its own token, EINVAL
 *   how 1  an unreadable ucontext (PROT_NONE), a wrong token, EFAULT
 *   how 2  a native SIGSEGV handler's frame: under hao that is on_fault's
 *          (hao_signal_forward passes its ucontext), whose token the handler
 *          learns from a nested SIGUSR1 (xnu: uctx ^ the thread's token, the
 *          same while a sigreturn is pending; Rosetta passes the thread's
 *          token itself); natively _sigtramp's own, EINVAL
 *   how 3  the same valid frame/token as how 0, but EPERM: the state left
 *          by a syscall policy denial (real policy tested by host/sigpolicy) */
#include <errno.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
typedef void (*nsh_sa)(int, siginfo_t *, void *);
void nsh_tok_usr1(int, siginfo_t *, void *), nsh_tok_segv(int, siginfo_t *, void *);
void nsh_jump_trap(uint64_t at, uint64_t uctx, uint64_t token);
/* entries: the C handler gets the token and the return address (into _sigtramp when the kernel called it) */
#if defined(__x86_64__)
__asm__(".globl _nsh_tok_usr1\n_nsh_tok_usr1:\n  movq %r12, %rcx\n  movq (%rsp), %r8\n  jmp _nsh_usr1_c\n"
        ".globl _nsh_tok_segv\n_nsh_tok_segv:\n  movq %r12, %rcx\n  movq (%rsp), %r8\n  jmp _nsh_segv_c\n"
        ".globl _nsh_jump_trap\n_nsh_jump_trap:\n  movq %rsi, %rbx\n  movq %rdx, %r12\n  jmp *%rdi\n");
#else
__asm__(".globl _nsh_tok_usr1\n.p2align 2\n_nsh_tok_usr1:\n  mov x3, x19\n  mov x4, x30\n  b _nsh_usr1_c\n"
        ".globl _nsh_tok_segv\n.p2align 2\n_nsh_tok_segv:\n  mov x3, x19\n  mov x4, x30\n  b _nsh_segv_c\n"
        ".globl _nsh_jump_trap\n.p2align 2\n_nsh_jump_trap:\n  mov x20, x1\n  mov x19, x2\n  br x0\n");
#endif

static int sr_how;
static uint64_t sr_trap, sr_ret, sr_ctx2, sr_tok2;
static volatile int sr_probe;

/* the trap after _sigtramp's call of __sigreturn, from the handler's return address */
static uint64_t find_trap(uint64_t ret)
{
#if defined(__x86_64__)
    const uint8_t *p = (const uint8_t *)ret;
    for (int k = 0; k < 64; k++)
        if (p[k] == 0xe8 && p[k + 5] == 0x0f && p[k + 6] == 0x0b) return ret + k + 5;
#else
    ret &= 0x0000000fffffffffull;   /* a signed return address */
    const uint32_t *w = (const uint32_t *)ret;
    for (int k = 0; k < 16; k++)
        if ((w[k] & 0xfc000000u) == 0x94000000u && (w[k + 1] & 0xffe0001fu) == 0xd4200000u) return ret + 4 * (k + 1);
#endif
    return 0;
}

static int translated(void)
{
    int t = 0;
    size_t n = sizeof t;
    return sysctlbyname("sysctl.proc_translated", &t, &n, NULL, 0) == 0 && t == 1;
}

void nsh_usr1_c(int sig, siginfo_t *si, void *ctx, uint64_t tok, uint64_t ret)
{
    (void)sig; (void)si;
    if (sr_probe) {   /* learning where the trap is, or the thread's token */
        sr_trap = find_trap(ret);
        sr_ret = ret;
        sr_ctx2 = (uint64_t)ctx;
        sr_tok2 = tok;
        return;
    }
    if (sr_how == 0 || sr_how == 3) {
        errno = sr_how == 3 ? EPERM : EINVAL;
        nsh_jump_trap(sr_trap, (uint64_t)ctx, tok);
    }
    void *p = mmap(NULL, 4096, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);   /* kept: nothing else lands there */
    errno = EFAULT;
    nsh_jump_trap(sr_trap, (uint64_t)p, 0x5678);
}

void nsh_segv_c(int sig, siginfo_t *si, void *ctx, uint64_t tok, uint64_t ret)
{
    (void)sig; (void)si;
    if (ret != sr_ret) {   /* not called by _sigtramp (hao's on_fault forwarded it): learn the token */
        sr_probe = 1;
        raise(SIGUSR1);
        sr_probe = 0;
        tok = translated() ? sr_tok2 : (uint64_t)ctx ^ sr_tok2 ^ sr_ctx2;
    }
    errno = EINVAL;
    nsh_jump_trap(sr_trap, (uint64_t)ctx, tok);
}

void nsh_sigret(int how)
{
    sr_how = how;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = nsh_tok_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sr_probe = 1;
    raise(SIGUSR1);
    sr_probe = 0;
    if (!sr_trap) {
        say("no trap after __sigreturn found in _sigtramp\n");
        return;
    }
    if (how < 2 || how == 3) {
        raise(SIGUSR1);
        return;
    }
    sa.sa_sigaction = nsh_tok_segv;
    sigaction(SIGSEGV, &sa, NULL);
    nsh_null_write();
}
