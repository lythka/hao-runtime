/*
 * hao — guest signal dispositions and delivery.
 *
 * The kernel delivers signals to the native process and calls whatever
 * handler address is installed. Guest handlers are arm64 code, so they are
 * never installed directly: the native trampoline `tramp` runs them through
 * the translator on the interrupted thread. SIGSEGV/SIGBUS stay owned by hao
 * (they implement native->guest calls); faults that are not hao's own are
 * forwarded here by the fault handler (runtime.c on_fault).
 *
 * Delivery:
 *   - A signal that arrives while the thread is inside one of hao's own
 *     critical sections (hao_crit_enter: jit_lock, thunk_lock, sig_lock,
 *     the block-invoke table, hao's allocations) is only recorded. The
 *     outermost hao_crit_leave re-raises it on the same thread, with its
 *     siginfo kept, so the handler runs right after the section and never
 *     on top of a hao lock or malloc. Synchronous faults cannot wait: one
 *     inside a critical section takes the default action.
 *   - Otherwise the handler runs at once (asynchronously to the guest, as
 *     natively; a SA_RESTART read() it interrupts still sees it run).
 *   - The handler gets an arm64 frame on the guest stack (or on the guest's
 *     sigaltstack with SA_ONSTACK): siginfo, ucontext and an arm64
 *     mcontext built from the guest cpu. Inside translated code the pc is
 *     the exact guest instruction (hao_jit_locate), and NZCV is read from
 *     host RFLAGS where the block keeps it there.
 *   - When it returns, register, NZCV, FP and signal-mask changes it made to
 *     the context are written back. A changed pc takes effect when the
 *     signal interrupted guest execution (translated code, its interpreter
 *     helper or the interpreter): the dispatcher resumes at the new pc. A
 *     pc change while the thread was in native or hao code is ignored.
 *
 * Bookkeeping: the kernel stays the source of truth. A guest query asks the
 * kernel first and reports the guest's own handler only while the
 * trampoline is still what the kernel has installed (so SA_RESETHAND and
 * failed installs read back as natively); the shadow entry is committed
 * only when the native sigaction succeeds. SIGSEGV/SIGBUS are emulated in
 * full: their flags are reported as the kernel reports them, sa_mask,
 * SA_NODEFER and SA_RESETHAND are honoured, and a native handler (a
 * libSystem function, say) is called natively.
 *
 * Native code shares the SIGSEGV/SIGBUS shadow. libhao_identity interposes
 * libsystem_c's sigaction, signal, bsd_signal, sigset, sigvec and sigignore
 * (src/process_identity.c) and hands these two signals here
 * (hao_native_sigaction), so a native library's handler never replaces
 * on_fault in the kernel: native->guest calls keep working on every thread,
 * the library reads back what it installed, and on_fault calls its handler
 * for the faults that are not hao's (hao_signal_forward), with the signal
 * blocked for the handler unless SA_NODEFER, sa_mask, SA_RESETHAND and
 * signals sent meanwhile held until it returns, as the kernel would. A
 * handler may leave by longjmp (LLVM's CrashRecoveryContext): the next fault
 * finds its frame gone. One that returns with rbx or r12 changed would fail
 * __sigreturn natively (_sigtramp keeps the ucontext and the token there):
 * hao reports it and takes SIGTRAP's default action, as an arm64 _sigtramp's
 * brk after a failed sigreturn does. hao's own dispositions bypass the
 * interposers (hao_sys_sigaction).
 *
 * When on_fault gives up (hao_signal_giveup) the default action is made
 * certain: SIG_DFL, the signal, SIGILL and SIGTRAP unblocked in the thread
 * and in the context sigreturn restores, SIGILL/SIGTRAP not ignored, and a
 * signal that was sent rather than raised by a fault is raised again. A
 * fault is not raised: the faulting context is resumed in user space
 * (hao_sig_resume, path "giveup", any rip), so the instruction faults again
 * and the kernel takes the default action of the real fault, with its own
 * exception type, si_code and address, as natively (a raise would be an
 * EXC_CRASH, a sent signal). Nothing then depends on __sigreturn, which a
 * sandbox profile may deny (27.0 WindowManager, sharingd: its EPERM made
 * every given-up fault a failed-sigreturn SIGTRAP). Where the resume does
 * not apply (switch, Rosetta's default, alternate stack, state) the handler
 * returns through the kernel as before. A blocked or ignored synchronous
 * signal is never delivered: xnu re-executes the instruction forever, and a
 * later failed __sigreturn (whose ud2 raises SIGILL) would spin silently.
 * Under the diagnostics log a failed __sigreturn after any handler is
 * reported and fatal (runtime.c hao_on_abort, hao_signal_die).
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* ------------------------------------------------ arm64 signal frame
 * Layouts of Darwin arm64's <mach/arm/_structs.h> (measured with
 * sizeof/offsetof in an arm64 build). ucontext_t and siginfo_t are the same
 * on x86_64 and arm64. */
typedef struct { uint64_t far; uint32_t esr, exception; } a64_es;
typedef struct { uint64_t x[29], fp, lr, sp, pc; uint32_t cpsr, flags; } a64_ss;
typedef struct { unsigned __int128 v[32]; uint32_t fpsr, fpcr; } a64_ns;
typedef struct { a64_es es; a64_ss ss; a64_ns ns; } a64_mcontext;
_Static_assert(sizeof(a64_es) == 16 && sizeof(a64_ss) == 272 && sizeof(a64_ns) == 528, "arm64 state layout");
_Static_assert(sizeof(a64_mcontext) == 816 && __builtin_offsetof(a64_mcontext, ns) == 288, "arm64 mcontext layout");
_Static_assert(sizeof(siginfo_t) == 104 && sizeof(ucontext_t) == 56, "LP64 signal frame layout");
#define A64_TS_NO_PTRAUTH 0x1   /* __DARWIN_ARM_THREAD_STATE64_FLAGS_NO_PTRAUTH: pc/lr/sp/fp are raw */

typedef struct {
    siginfo_t si;
    ucontext_t uc;
    uint64_t pad;
    a64_mcontext mc;
} __attribute__((aligned(16))) a64_frame;

/* ------------------------------------------------ disposition table */

typedef struct { uint64_t handler; int flags; sigset_t mask; } gsig_t;
static gsig_t gsig[NSIG];                     /* fields read atomically by tramp */
static uint64_t st_delivered, st_deferred, st_moved;
uint64_t hao_sig_native_installs;
static pthread_mutex_t disp_lock = PTHREAD_MUTEX_INITIALIZER;

#define OWNED(s) ((s) == SIGSEGV || (s) == SIGBUS)
/* the flags Darwin's sigaction reports back (kern_sig.c): never SA_RESETHAND */
#define REPORTED_FLAGS (SA_ONSTACK | SA_RESTART | SA_SIGINFO | SA_NODEFER | SA_NOCLDSTOP | SA_NOCLDWAIT)

static uint64_t ld_handler(int s) { return __atomic_load_n(&gsig[s].handler, __ATOMIC_ACQUIRE); }
static int ld_flags(int s) { return __atomic_load_n(&gsig[s].flags, __ATOMIC_ACQUIRE); }
static sigset_t ld_mask(int s) { return __atomic_load_n(&gsig[s].mask, __ATOMIC_ACQUIRE); }
static void st_gsig(int s, uint64_t h, int fl, sigset_t m)
{
    __atomic_store_n(&gsig[s].mask, m, __ATOMIC_RELEASE);
    __atomic_store_n(&gsig[s].flags, fl, __ATOMIC_RELEASE);
    __atomic_store_n(&gsig[s].handler, h, __ATOMIC_RELEASE);
}

static bool is_guest_fn(uint64_t h)
{
    if (h > 1 && hao_thunk_target(h)) return true;
    return h > 1 && hao_image_for_code(h) != NULL;
}

static uint64_t guest_entry(uint64_t h)
{
    uint64_t g = hao_thunk_target(h);
    return g ? g : h;
}

static int (*real_sa)(int, const struct sigaction *, struct sigaction *);
static void resume_init(void);

int hao_sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    return (real_sa ? real_sa : sigaction)(sig, act, oact);
}

/* The kernel's dispositions of SIGSEGV/SIGBUS before hao took them: the
 * guest's view until it installs its own. Handlers native initializers
 * installed are among them: on_fault calls them from now on. */
void hao_signals_init(void)
{
    /* libhao_identity's, when the front end links it (hao, hao-stub) */
    real_sa = (int (*)(int, const struct sigaction *, struct sigaction *))dlsym(RTLD_DEFAULT, "hao_identity_real_sigaction");
    resume_init();
    for (int s = 1; s < NSIG; s++) {
        if (!OWNED(s)) continue;
        struct sigaction k;
        if (hao_sys_sigaction(s, NULL, &k) == 0) st_gsig(s, (uint64_t)(uintptr_t)k.sa_handler, k.sa_flags & REPORTED_FLAGS, k.sa_mask);
    }
}

/* ------------------------------------------- resuming without __sigreturn
 * When on_fault leaves the thread in a state it has built (the NX and trap
 * redirects to hao_callback_entry, an emulated commpage read, libunwind
 * after an uncallable personality, unwind.c hao_eh_fault) or leaves a
 * stale fault alone (runtime.c), hao_sig_resume resumes that state in user
 * space instead of returning to _sigtramp and its __sigreturn: every
 * register the mcontext holds (hao_sig_resume_ctx: FXSAVE image, ymm upper
 * halves, AVX-512 opmasks and zmm state, GPRs, rflags, rsp, rip). It makes
 * no system call at all: a sandbox profile may deny SYS_sigreturn (27.0
 * WindowManager's does: EPERM before xnu reads the frame) and
 * __pthread_sigmask too (WindowManager's does; its allowed sigprocmask sets
 * every thread's mask, set_procsigmask). None is needed for the mask:
 * on_fault is SA_NODEFER with an empty sa_mask, so xnu runs it with the
 * interrupted thread's mask, the ucontext's uc_sigmask, and these paths do
 * not change it. Guest and native signal handlers' returns keep
 * __sigreturn, as natively.
 *
 * The kernel's bookkeeping (xnu bsd/dev/i386/unix_signal.c) then stays as
 * after any handler that never returns (deliver's _longjmp below already
 * leaves that way): the thread's pending-sigreturn count stays one higher,
 * which only stops sendsig from drawing a new sigreturn token (one is drawn
 * only when the count is 0; the thread keeps its token, so an enclosing
 * frame's __sigreturn still validates), and sigreturn's other work is not
 * needed: on_fault is not SA_ONSTACK, and a thread already on its alternate
 * stack (uc_onstack) returns through the kernel; the restartable-range pc
 * reset sigreturn arms (act_set_ast_reset_pcs: objc_msgSend's cache scan,
 * for one) cannot apply to a target rip in hao's own code (libhao, the code
 * cache, Swift islands) or _sigtramp's entry (a stale fault), and any other
 * rip returns through the kernel. So do a full-state (LDT) mcontext, an
 * AVX-512 one on a CPU without AVX512BW, a code segment other than this
 * thread's, a set trap flag and a target rsp that is not just above this
 * handler's stack.
 *
 * Under Rosetta the x86 signal frame is not on the thread's stack and
 * Rosetta's __sigreturn is not subject to the sandbox (measured: the WindowManager
 * profile denies an arm64 process's sigreturn, not an x86_64 one's). Rosetta keeps
 * one 4 KiB frame per delivery until its __sigreturn, so a handler that
 * never returns leaks it (200,000 faults resumed in user space: 822 MB
 * resident), and a __sigreturn of a frame after a frame above it was left
 * without one fails (SIGILL). There the kernel return stays the default;
 * HAO_SIGRETURN_USER=1 (tests) resumes in user space anyway, but only a
 * fault no other handler of hao's encloses and not a stale one.
 * HAO_SIGRETURN_KERNEL=1 / hao_sigreturn_kernel=1 always returns through
 * the kernel.
 *
 * A given-up fault (HAO_RESUME_GIVEUP) resumes at any rip: the instruction
 * faults again under SIG_DFL and the process dies, as it would natively
 * with no handler at all (where a restartable range's pc reset is not
 * armed either). An uncallable personality (HAO_RESUME_EHPERS, unwind.c
 * hao_eh_fault) resumes libunwind just after its `callq *%rax`: libunwind
 * registers no restartable ranges, so the pc reset __sigreturn would arm
 * cannot apply there either. */
typedef struct {
    _Alignas(64) uint8_t fx[512];    /* FXSAVE image: x87, MXCSR, xmm0-15 */
    uint8_t ymmh[16][16];             /* upper halves of ymm0-15 */
    uint64_t vec, pad;                /* 0 xmm only, 1 + ymm upper halves, 2 + AVX-512 state */
    uint64_t gpr[15];                 /* pop order: r15 r14 r13 r12 r11 r10 r9 r8 rbp rsi rdi rdx rcx rbx rax */
    uint64_t rflags;
    uint64_t rsp_slot;                /* target rsp - 136: holds the target rip */
    _Alignas(64) uint64_t k[8];       /* AVX-512: opmask registers */
    uint8_t zmmh[16][32];             /* upper halves of zmm0-15 */
    uint8_t zmm[16][64];              /* zmm16-31 */
} hao_resume_blk;
_Static_assert(__builtin_offsetof(hao_resume_blk, ymmh) == 0x200 && __builtin_offsetof(hao_resume_blk, vec) == 0x300 &&
               __builtin_offsetof(hao_resume_blk, gpr) == 0x310 && __builtin_offsetof(hao_resume_blk, rflags) == 0x388 &&
               __builtin_offsetof(hao_resume_blk, rsp_slot) == 0x390 && __builtin_offsetof(hao_resume_blk, k) == 0x3C0 &&
               __builtin_offsetof(hao_resume_blk, zmmh) == 0x400 && __builtin_offsetof(hao_resume_blk, zmm) == 0x600,
               "bridge_x86_64.S hao_sig_resume_ctx layout");
_Static_assert(__builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_ymmh0) == __builtin_offsetof(_STRUCT_X86_AVX_STATE64, __fpu_ymmh0) &&
               sizeof(((_STRUCT_X86_AVX512_STATE64 *)0)->__fpu_k0) == 8 &&
               __builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_zmmh15) - __builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_zmmh0) == 15 * 32 &&
               __builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_zmm31) - __builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_zmm16) == 15 * 64 &&
               __builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_k7) - __builtin_offsetof(_STRUCT_X86_AVX512_STATE64, __fpu_k0) == 7 * 8,
               "AVX-512 mcontext: k0-7, zmmh0-15 and zmm16-31 contiguous");
_Static_assert(__builtin_offsetof(_STRUCT_X86_AVX_STATE64, __fpu_ymmh0) - __builtin_offsetof(_STRUCT_X86_AVX_STATE64, __fpu_fcw) >= 512 &&
               __builtin_offsetof(_STRUCT_X86_AVX_STATE64, __fpu_fcw) == __builtin_offsetof(_STRUCT_X86_FLOAT_STATE64, __fpu_fcw),
               "mcontext float state: an FXSAVE image at __fpu_fcw");
__attribute__((noreturn)) void hao_sig_resume_ctx(const hao_resume_blk *b);

enum { RES_OFF_SWITCH = 1, RES_OFF_ROSETTA, RES_ONSTACK, RES_STATE, RES_RIP, RES_STACK, RES_NESTED, RES_NREASONS };
static int res_mode;                       /* 0 user space, else RES_OFF_SWITCH / RES_OFF_ROSETTA */
static bool res_rosetta;                   /* user space under Rosetta (HAO_SIGRETURN_USER) */
static uint64_t res_sigtramp;              /* _sigtramp: a stale fault's rip */
static uint64_t res_text_lo, res_text_hi;  /* libhao's __TEXT */
static uint64_t res_cs;
static bool res_avx512bw;                  /* kmovq (AVX512BW): every Mac with AVX-512 has it */
static uint64_t st_res_user[HAO_RESUME_NPATHS], st_res_kernel[HAO_RESUME_NPATHS], st_res_why[RES_NREASONS];

static void resume_init(void)
{
    int tr = 0;
    size_t n = sizeof tr;
    bool rosetta = sysctlbyname("sysctl.proc_translated", &tr, &n, NULL, 0) == 0 && tr == 1;
    if (hao_opt.sigreturn_kernel) res_mode = RES_OFF_SWITCH;
    else if (rosetta && !hao_opt.sigreturn_user) res_mode = RES_OFF_ROSETTA;
    res_rosetta = rosetta;
    res_sigtramp = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, "_sigtramp");
    uint16_t cs;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    res_cs = cs;
    unsigned a, b, c, d7;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d7)) res_avx512bw = (b >> 30) & 1;
    Dl_info d;
    if (!dladdr((void *)(uintptr_t)hao_sig_resume_ctx, &d) || !d.dli_fbase) return;
    const struct mach_header_64 *mh = d.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (strncmp(sg->segname, "__TEXT", sizeof sg->segname)) continue;
        res_text_lo = (uint64_t)(uintptr_t)mh;   /* __TEXT starts at the header */
        res_text_hi = res_text_lo + sg->vmsize;
        return;
    }
}

static bool res_hao_code(uint64_t rip)
{
    return (rip >= res_text_lo && rip < res_text_hi) || hao_in_code_cache(rip) || hao_swiftmeta_is_island(rip);
}

/* 0, or why this context returns through the kernel */
static int resume_refused(const ucontext_t *uc, uint64_t here, int path)
{
    if (res_mode) return res_mode;
    if (res_rosetta) {
        hao_thread *t = hao_current_peek();
        if (path == HAO_RESUME_STALE || (t && t->sig_nest)) return RES_NESTED;
    }
    if (!uc || !uc->uc_mcontext) return RES_STATE;
    if (uc->uc_onstack & 1) return RES_ONSTACK;
    if (uc->uc_mcsize != sizeof(_STRUCT_MCONTEXT64) && uc->uc_mcsize != sizeof(_STRUCT_MCONTEXT_AVX64) &&
        !(uc->uc_mcsize == sizeof(_STRUCT_MCONTEXT_AVX512_64) && res_avx512bw))
        return RES_STATE;   /* the full (LDT) thread state */
    const _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    if (ss->__cs != res_cs || (ss->__rflags & 0x100)) return RES_STATE;   /* TF */
    /* stale: _sigtramp's entry (hao_sig_stale); give-up: the faulting instruction, to fault again; ehpers:
     * libunwind after the personality call hao_eh_fault answered */
    if (!res_hao_code(ss->__rip) && path != HAO_RESUME_STALE && path != HAO_RESUME_GIVEUP &&
        !(path == HAO_RESUME_EHPERS && hao_eh_fault_target(ss->__rip)))
        return RES_RIP;
    /* the slot (and the stack up to the target) lies above everything this handler still uses */
    uint64_t slot = ss->__rsp - 136;
    if (ss->__rsp < 4096 + 136 || slot <= here + 64 || slot - here > (1u << 20)) return RES_STACK;
    return 0;
}

static const char *const res_why_name[RES_NREASONS] = {"", "switch", "rosetta", "onstack", "state", "rip", "stack", "nested"};

/* The give-up's line (one write): how the default action of sig is taken */
static void giveup_line(int sig, const siginfo_t *si, int why)
{
    char m[320];
    int n = why ? snprintf(m, sizeof m,
                           "hao[%s:%d]: signal %d (code %d, addr %#llx) given up: SIG_DFL, returning through __sigreturn "
                           "(kernel: %s) to fault again\n",
                           getprogname(), (int)getpid(), sig, si ? si->si_code : 0,
                           (unsigned long long)(uintptr_t)(si ? si->si_addr : 0), res_why_name[why])
                : snprintf(m, sizeof m,
                           "hao[%s:%d]: signal %d (code %d, addr %#llx) given up: SIG_DFL, the faulting instruction "
                           "resumed in user space to fault again (no __sigreturn)\n",
                           getprogname(), (int)getpid(), sig, si ? si->si_code : 0,
                           (unsigned long long)(uintptr_t)(si ? si->si_addr : 0));
    if (n > 0) hao_diag_write(m, (size_t)n < sizeof m ? (size_t)n : sizeof m - 1);
}

/* hao_sig_resume; a give-up (giveup_sig, path HAO_RESUME_GIVEUP) also
 * writes its line first */
static __attribute__((noinline)) void sig_resume(ucontext_t *uc, int path, int giveup_sig, const siginfo_t *si)
{
    hao_resume_blk b;
    /* this frame (b, then hao_sig_resume_ctx's call below it) is all the handler still uses */
    if (path < 0 || path >= HAO_RESUME_NPATHS) path = 0;
    int why = resume_refused(uc, (uint64_t)(uintptr_t)__builtin_frame_address(0), path);
    if (giveup_sig) giveup_line(giveup_sig, si, why);
    if (why) {
        __atomic_fetch_add(&st_res_kernel[path], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&st_res_why[why], 1, __ATOMIC_RELAXED);
        return;
    }
    const _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    const _STRUCT_X86_AVX_STATE64 *fs = &((const _STRUCT_MCONTEXT_AVX64 *)(const void *)uc->uc_mcontext)->__fs;
    memcpy(b.fx, &fs->__fpu_fcw, sizeof b.fx);
    /* MXCSR as the kernel's fpu_set_fxstate masks it (FXRSTOR faults on a reserved bit) */
    uint32_t mx, mxmask;
    memcpy(&mx, b.fx + 24, 4);
    memcpy(&mxmask, b.fx + 28, 4);
    mx &= mxmask ? mxmask : 0xFFBF;
    memcpy(b.fx + 24, &mx, 4);
    b.vec = uc->uc_mcsize == sizeof(_STRUCT_MCONTEXT_AVX512_64) ? 2 : uc->uc_mcsize == sizeof(_STRUCT_MCONTEXT_AVX64) ? 1 : 0;
    if (b.vec) memcpy(b.ymmh, &fs->__fpu_ymmh0, sizeof b.ymmh);
    if (b.vec == 2) {
        const _STRUCT_X86_AVX512_STATE64 *zs = &((const _STRUCT_MCONTEXT_AVX512_64 *)(const void *)uc->uc_mcontext)->__fs;
        memcpy(b.k, &zs->__fpu_k0, sizeof b.k);
        memcpy(b.zmmh, &zs->__fpu_zmmh0, sizeof b.zmmh);
        memcpy(b.zmm, &zs->__fpu_zmm16, sizeof b.zmm);
    }
    const uint64_t g[15] = {ss->__r15, ss->__r14, ss->__r13, ss->__r12, ss->__r11, ss->__r10, ss->__r9, ss->__r8,
                            ss->__rbp, ss->__rsi, ss->__rdi, ss->__rdx, ss->__rcx, ss->__rbx, ss->__rax};
    memcpy(b.gpr, g, sizeof g);
    b.rflags = ss->__rflags;
    b.rsp_slot = ss->__rsp - 136;
    uint64_t rip = ss->__rip;
    __atomic_fetch_add(&st_res_user[path], 1, __ATOMIC_RELAXED);
    /* the ucontext may hold the slot (xnu: uc_mcontext64, uc + 48): read before this store */
    *(volatile uint64_t *)(uintptr_t)b.rsp_slot = rip;
    hao_sig_resume_ctx(&b);   /* the signal mask is already the ucontext's (see above) */
}

/* Resume ctx's state in user space (never returns), or return when it must
 * go back through the kernel (the caller returns to _sigtramp). */
void hao_sig_resume(void *ctx, int path) { sig_resume(ctx, path, 0, NULL); }

/* on_fault: a fetch fault at addr (error code I/D bit, fault address addr)
 * whose context stands at _sigtramp's entry: another signal's frame was set
 * up between the fault and this delivery */
bool hao_sig_stale(const ucontext_t *uc, uint64_t addr)
{
    const _STRUCT_MCONTEXT64 *m = uc ? (const _STRUCT_MCONTEXT64 *)uc->uc_mcontext : NULL;
    return m && res_sigtramp && m->__ss.__rip == res_sigtramp && m->__ss.__rip != addr && (m->__es.__err & 0x10) &&
           m->__es.__faultvaddr == addr;
}

/* on_fault: a data fault at addr whose context stands at _sigtramp's entry (another signal's frame was set
 * up first, as hao_sig_stale) and whose interrupted context, the frame _sigtramp is about to run
 * (_sigtramp(catcher, style, sig, info, uctx, token): uctx in r8), stands at pc. The frame is read through
 * Mach: this runs only for a context at _sigtramp's entry, and must not fault itself. */
bool hao_sig_stale_at(const ucontext_t *uc, uint64_t addr, uint64_t pc)
{
    const _STRUCT_MCONTEXT64 *m = uc ? (const _STRUCT_MCONTEXT64 *)uc->uc_mcontext : NULL;
    if (!m || !res_sigtramp || m->__ss.__rip != res_sigtramp || m->__es.__faultvaddr != addr || (m->__es.__err & 0x10))
        return false;
    ucontext_t in;
    _STRUCT_X86_THREAD_STATE64 ss;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), m->__ss.__r8, sizeof in, (mach_vm_address_t)(uintptr_t)&in, &got) != KERN_SUCCESS ||
        got != sizeof in || !in.uc_mcontext)
        return false;
    got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)in.uc_mcontext + offsetof(_STRUCT_MCONTEXT64, __ss),
                                  sizeof ss, (mach_vm_address_t)(uintptr_t)&ss, &got) == KERN_SUCCESS &&
           got == sizeof ss && ss.__rip == pc;
}

/* The line into b (no stdio stream: on_abort writes it from a handler); 0 when no handler has ended */
size_t hao_sig_resume_line(char *b, size_t cap)
{
    static const char *const path[HAO_RESUME_NPATHS] = {"nx", "trap", "commpage", "stale", "giveup", "ehpers", "probe"};
    uint64_t u = 0, k = 0;
    for (int i = 0; i < HAO_RESUME_NPATHS; i++) {
        u += __atomic_load_n(&st_res_user[i], __ATOMIC_RELAXED);
        k += __atomic_load_n(&st_res_kernel[i], __ATOMIC_RELAXED);
    }
    if (!u && !k) return 0;
    size_t n = 0;
#define ADD(...) do { int r_ = snprintf(b + n, cap - n, __VA_ARGS__); if (r_ > 0) n += (size_t)r_ < cap - n ? (size_t)r_ : cap - n - 1; } while (0)
    ADD("hao: signal returns: %llu user space, %llu kernel (", (unsigned long long)u, (unsigned long long)k);
    for (int i = 0; i < HAO_RESUME_NPATHS; i++)
        ADD("%s%s %llu/%llu", i ? ", " : "", path[i], (unsigned long long)st_res_user[i], (unsigned long long)st_res_kernel[i]);
    ADD("; kernel: switch=%llu rosetta=%llu onstack=%llu state=%llu rip=%llu stack=%llu nested=%llu)\n",
        (unsigned long long)st_res_why[RES_OFF_SWITCH], (unsigned long long)st_res_why[RES_OFF_ROSETTA],
        (unsigned long long)st_res_why[RES_ONSTACK], (unsigned long long)st_res_why[RES_STATE],
        (unsigned long long)st_res_why[RES_RIP], (unsigned long long)st_res_why[RES_STACK],
        (unsigned long long)st_res_why[RES_NESTED]);
#undef ADD
    if (n && b[n - 1] != '\n') b[n - 1] = '\n';
    return n;
}

/* --stats / hao_nx_stats: one line, when a handler has ended at all */
void hao_sig_resume_print(FILE *f)
{
    char b[512];
    size_t n = hao_sig_resume_line(b, sizeof b);
    if (n) fwrite(b, 1, n, f);
}

/* ------------------------------------------------------ delivery */

static uint64_t rflags_to_image(uint64_t rf)
{
    return HAO_RF_BASE | (rf & (HAO_RF_CF | HAO_RF_ZF | HAO_RF_SF | HAO_RF_OF));
}

static void set_default(int sig)
{
    struct sigaction d;
    memset(&d, 0, sizeof d);
    d.sa_handler = SIG_DFL;
    hao_sys_sigaction(sig, &d, NULL);
}

/* Run guest handler h for sig on thread t, which the signal interrupted in
 * the state described by the native context nuc. */
static void deliver(hao_thread *t, int sig, uint64_t h, int flags, const siginfo_t *si, ucontext_t *nuc)
{
    hao_cpu *c = &t->cpu;
    _STRUCT_X86_THREAD_STATE64 *hs = &nuc->uc_mcontext->__ss;
    /* where the guest stands */
    bool in_guest = c->host_scratch[HAO_SCRATCH_IN_GUEST] != 0, flags_in_host = false;
    uint64_t pc = c->pc;
    if (in_guest && !hao_jit_locate(hs->__rip, &pc, &flags_in_host)) {
        if (c->host_scratch[HAO_SCRATCH_HELPER]) pc = c->host_scratch[HAO_SCRATCH_HELPER];
    }
    uint64_t fl_image = flags_in_host ? rflags_to_image(hs->__rflags) : c->flags;

    /* the frame: below the red zone of the interrupted guest code, or at the
     * top of the guest's alternate stack */
    uint64_t sp = c->x[31] - 256;
    stack_t alt;
    memset(&alt, 0, sizeof alt);
    bool onstack = false;
    if ((flags & SA_ONSTACK) && sigaltstack(NULL, &alt) == 0 && !(alt.ss_flags & SS_DISABLE) && alt.ss_size) {
        uint64_t lo = (uint64_t)(uintptr_t)alt.ss_sp, hi = lo + alt.ss_size;
        if (c->x[31] >= lo && c->x[31] <= hi) onstack = true;        /* already on it */
        else { sp = hi; onstack = true; }
        alt.ss_flags = SS_ONSTACK;
    }
    a64_frame *fr = (a64_frame *)(uintptr_t)((sp - sizeof(a64_frame)) & ~15ull);
    memset(fr, 0, sizeof *fr);
    fr->si = *si;
    fr->uc.uc_onstack = onstack;
    fr->uc.uc_sigmask = nuc->uc_sigmask;
    fr->uc.uc_stack = alt;
    fr->uc.uc_mcsize = sizeof(a64_mcontext);
    fr->uc.uc_mcontext = (mcontext_t)(void *)&fr->mc;
    a64_mcontext *m = &fr->mc;
    if (OWNED(sig) && si->si_code > 0 && si->si_code < 0x10000) {
        /* data abort from EL0: ISS WnR from the x86 error code, translation
         * or permission fault at level 3 */
        uint32_t err = nuc->uc_mcontext->__es.__err;
        m->es.far = (uint64_t)(uintptr_t)si->si_addr;
        m->es.esr = 0x24u << 26 | 1u << 25 | ((err & 2) ? 1u << 6 : 0) | ((err & 1) ? 0x0Fu : 0x07u);
    }
    for (int k = 0; k < 29; k++) m->ss.x[k] = c->x[k];
    m->ss.fp = c->x[29];
    m->ss.lr = c->x[30];
    m->ss.sp = c->x[31];
    m->ss.pc = pc;
    hao_cpu tmp;
    tmp.flags = fl_image;
    m->ss.cpsr = hao_nzcv(&tmp) << 28;
    m->ss.flags = A64_TS_NO_PTRAUTH;
    memcpy(m->ns.v, c->v, sizeof m->ns.v);
    m->ns.fpsr = c->fpsr;
    m->ns.fpcr = c->fpcr;
    a64_mcontext orig = *m;
    sigset_t orig_mask = fr->uc.uc_sigmask;

    __atomic_fetch_add(&st_delivered, 1, __ATOMIC_RELAXED);
    hao_cpu saved = *c;
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
    c->x[0] = (uint64_t)sig;
    c->x[1] = (uint64_t)(uintptr_t)&fr->si;
    c->x[2] = (uint64_t)(uintptr_t)&fr->uc;
    c->x[31] = (uint64_t)(uintptr_t)fr;
    c->x[30] = HAO_RETURN_MAGIC;
    c->pc = guest_entry(h);
    int sig_depth = t->sig_depth;
    t->sig_depth = t->depth + 1;      /* the handler's segment: no first-call binding (dladdr) inside it */
    hao_run(t, &saved);
    t->sig_depth = sig_depth;
    *c = saved;

    /* write back what the handler changed */
    for (int k = 0; k < 29; k++) if (m->ss.x[k] != orig.ss.x[k]) c->x[k] = m->ss.x[k];
    if (m->ss.fp != orig.ss.fp) c->x[29] = m->ss.fp;
    if (m->ss.lr != orig.ss.lr) c->x[30] = m->ss.lr;
    if (m->ss.sp != orig.ss.sp) c->x[31] = m->ss.sp;
    for (int k = 0; k < 32; k++) if (m->ns.v[k] != orig.ns.v[k]) memcpy(&c->v[k], &m->ns.v[k], 16);
    if (m->ns.fpsr != orig.ns.fpsr) c->fpsr = m->ns.fpsr;
    if (m->ns.fpcr != orig.ns.fpcr) c->fpcr = m->ns.fpcr;
    if (memcmp(&fr->uc.uc_sigmask, &orig_mask, sizeof orig_mask)) nuc->uc_sigmask = fr->uc.uc_sigmask;
    bool moved = m->ss.pc != orig.ss.pc && in_guest && t->seg;
    if ((m->ss.cpsr ^ orig.ss.cpsr) >> 28) {
        hao_set_nzcv(&tmp, m->ss.cpsr >> 28);
        if (flags_in_host && !moved)
            hs->__rflags = (hs->__rflags & ~(uint64_t)(HAO_RF_CF | HAO_RF_ZF | HAO_RF_SF | HAO_RF_OF)) |
                           (tmp.flags & (HAO_RF_CF | HAO_RF_ZF | HAO_RF_SF | HAO_RF_OF));
        else c->flags = tmp.flags;
    } else if (flags_in_host && moved) c->flags = fl_image;
    if (m->ss.pc != orig.ss.pc && !moved && hao_verbose)
        fprintf(stderr, "hao: signal %d handler moved pc outside guest execution; ignored\n", sig);
    if (moved) {
        /* Resume at the new pc from the dispatcher that was running the
         * interrupted guest code; the frames above it (translated block,
         * helper or interpreter step, this handler) are abandoned. */
        __atomic_fetch_add(&st_moved, 1, __ATOMIC_RELAXED);
        c->pc = m->ss.pc;
        c->host_scratch[0] = 0;
        c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
        t->in_fault_handler = 0;
        if (t->sig_nest > 0) t->sig_nest--;   /* tramp's or hao_signal_forward's, which never see this return */
        pthread_sigmask(SIG_SETMASK, &nuc->uc_sigmask, NULL);
        _longjmp(t->seg->jb, 1);   /* the interrupted segment's hao_run (runtime.c) */
    }
}

/* hao_probe_safe: the mask the thread returns to from a handler is its ucontext's */
static void sync_mask_from(hao_thread *t, const void *ctx)
{
    const ucontext_t *uc = ctx;
    if (uc) t->sync_masked = sigismember(&uc->uc_sigmask, SIGSEGV) == 1 || sigismember(&uc->uc_sigmask, SIGBUS) == 1;
}

static bool synchronous(int sig, const siginfo_t *si)
{
    switch (sig) {
    case SIGILL: case SIGFPE: case SIGTRAP: case SIGEMT: case SIGSYS: case SIGSEGV: case SIGBUS:
        return si && si->si_code > 0 && si->si_code < 0x10000;   /* kernel codes; SI_USER is 0x10001 */
    default:
        return false;
    }
}

static void tramp(int sig, siginfo_t *info, void *ctx)
{
    uint32_t bit = 1u << sig;
    hao_fault_act_note(HAO_ACT_TRAMP, sig, ld_handler(sig), ((ucontext_t *)ctx)->uc_mcontext->__ss.__rip, ctx, 0);
    if (hao_crit_depth() > 0) {
        if (synchronous(sig, info)) {
            static const char m[] = "hao: synchronous signal inside a hao critical section\n";
            write(2, m, sizeof m - 1);
            set_default(sig);
            pthread_kill(pthread_self(), sig);   /* taken when this handler returns */
            return;
        }
        hao_thread *t = hao_current();   /* async-signal-safe */
        __atomic_fetch_add(&st_deferred, 1, __ATOMIC_RELAXED);
        t->sig_info[sig] = *info;
        __atomic_fetch_or(&t->sig_pending, bit, __ATOMIC_RELEASE);
        return;
    }
    hao_thread *t = hao_current();
    siginfo_t si = *info;
    if (__atomic_load_n(&t->sig_redeliver, __ATOMIC_ACQUIRE) & bit) {
        __atomic_fetch_and(&t->sig_redeliver, ~bit, __ATOMIC_ACQ_REL);
        if (info->si_code == SI_USER && info->si_pid == getpid()) si = t->sig_info[sig];
    }
    uint64_t h = ld_handler(sig);
    int fl = ld_flags(sig);
    /* the kernel already reset its disposition; the shadow follows */
    if (fl & SA_RESETHAND) st_gsig(sig, 0, 0, 0);
    if (!is_guest_fn(h)) {
        /* the disposition changed while this signal was on its way: the shadow
         * of a signal that is not OWNED holds only guest handlers, so a native
         * one, SIG_IGN or SIG_DFL is what the kernel has now */
        struct sigaction k;
        if (hao_sys_sigaction(sig, NULL, &k) == 0 && k.sa_sigaction != tramp) h = (uint64_t)(uintptr_t)k.sa_handler;
        if (h == (uint64_t)(uintptr_t)SIG_IGN) return;
        if (h > 1) {
            t->sig_nest++;   /* the kernel's mask holds this handler's sa_mask meanwhile */
            ((void (*)(int, siginfo_t *, void *))(uintptr_t)h)(sig, &si, ctx);
            sync_mask_from(t, ctx);
            t->sig_nest--;
            return;
        }
        set_default(sig);
        pthread_kill(pthread_self(), sig);
        return;
    }
    t->sig_nest++;
    deliver(t, sig, h, fl, &si, ctx);
    sync_mask_from(t, ctx);   /* what __sigreturn restores (the handler may have edited it) */
    t->sig_nest--;
}

void hao_signals_stats(void)
{
    if (st_delivered || st_deferred || hao_sig_native_installs)
        fprintf(stderr, "hao-signals: delivered=%llu deferred=%llu pc_moved=%llu native_fault_installs=%llu\n",
                (unsigned long long)st_delivered, (unsigned long long)st_deferred, (unsigned long long)st_moved,
                (unsigned long long)hao_sig_native_installs);
}

/* hao's own threads (the periodic NX and bridge reports) take no
 * asynchronous signal: the guest's go to its threads, as natively, and a
 * report's sleep is not cut short. Faults stay deliverable. */
void hao_signals_block_async(void)
{
    sigset_t m;
    sigfillset(&m);
    static const int sync_sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGEMT, SIGSYS};
    for (size_t k = 0; k < sizeof sync_sigs / sizeof *sync_sigs; k++) sigdelset(&m, sync_sigs[k]);
    pthread_sigmask(SIG_BLOCK, &m, NULL);
}

/* hao_crit_leave on t's thread: re-raise what arrived inside the section */
void hao_signal_redeliver(hao_thread *t, uint32_t pending)
{
    for (int s = 1; s < NSIG; s++) {
        if (!(pending & (1u << s))) continue;
        __atomic_fetch_or(&t->sig_redeliver, 1u << s, __ATOMIC_RELEASE);
        pthread_kill(pthread_self(), s);
    }
}

/* A SIGSEGV/SIGBUS that was sent (raise, pthread_kill, kill), not raised
 * by a fault. xnu gives a sent one the thread's last fault code and address
 * (si_code SEGV_*, si_pid 0), so the siginfo cannot tell; a signal this
 * thread sent arrives on the return from its kill or __pthread_kill system
 * call, so rip follows a syscall instruction. (A fault at an instruction
 * that follows one is taken for a send: give-up raises it again, which ends
 * the same way, and hao_signal_forward defers it once only.) A send from
 * another thread or process is taken for a fault. */
static bool sent_signal(const siginfo_t *si, const ucontext_t *uc)
{
    if (si->si_code <= 0 || si->si_code >= 0x10000) return true;   /* SI_USER, SI_QUEUE, ... */
    uint64_t rip = uc ? uc->uc_mcontext->__ss.__rip : 0;
    if (rip < 4096 || (rip & 0xfff) < 2) return false;   /* rip - 2 on rip's own (mapped) page */
    const uint8_t *p = (const uint8_t *)(uintptr_t)(rip - 2);
    return p[0] == 0x0f && p[1] == 0x05;
}

/* SIGSEGV/SIGBUS bits of a set */
static uint32_t owned_bits(const sigset_t *m)
{
    return (sigismember(m, SIGSEGV) ? 1u << SIGSEGV : 0) | (sigismember(m, SIGBUS) ? 1u << SIGBUS : 0);
}

/* The native handlers hao_signal_forward is running on t block these of
 * SIGSEGV/SIGBUS (their own signal unless SA_NODEFER, and sa_mask's): the
 * innermost one's frame (the word at nh_at, holding nh_tok) is above HERE
 * and intact. A handler that left by longjmp (LLVM's CrashRecoveryContext
 * does, after unblocking the signal) leaves them stale: its frame is gone,
 * its word reused, or this frame is where its was. */
static uint32_t nh_live(hao_thread *t, const volatile uint64_t *here, uint32_t *blocked)
{
    uint64_t at = t->nh_at;
    if (at && at > (uint64_t)(uintptr_t)here && *(const volatile uint64_t *)(uintptr_t)at == t->nh_tok) return *blocked;
    t->nh_at = 0;
    *blocked = 0;
    return 0;
}

/* Take SIG's default action now, from inside a handler: the process dies
 * before the handler returns, so nothing depends on its __sigreturn. */
void hao_signal_die(int sig)
{
    struct sigaction d;
    memset(&d, 0, sizeof d);
    d.sa_handler = SIG_DFL;
    hao_sys_sigaction(sig, &d, NULL);
    sigset_t u;
    sigemptyset(&u);
    sigaddset(&u, sig);
    pthread_sigmask(SIG_UNBLOCK, &u, NULL);
    pthread_kill(pthread_self(), sig);
    _exit(128 + sig);
}

/* A native handler returned with the registers _sigtramp keeps the
 * ucontext and the sigreturn token in (rbx, r12) changed: natively its
 * __sigreturn fails and _sigtramp traps (an arm64 brk: SIGTRAP; x86 ud2:
 * SIGILL, which spins forever when blocked). */
__attribute__((noreturn)) static void bad_sigreturn(int sig, uint64_t h, int changed)
{
    char b[512];
    Dl_info d;
    bool named = dladdr((void *)(uintptr_t)h, &d) && d.dli_fname;
    int n = snprintf(b, sizeof b,
                     "hao[%s:%d]: native signal %d handler %#llx [%s`%s] returned with%s%s changed: natively its "
                     "__sigreturn fails (ucontext and token are kept there); taking SIGTRAP's default action\n",
                     getprogname(), (int)getpid(), sig, (unsigned long long)h,
                     named ? (strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname) : "?",
                     named && d.dli_sname ? d.dli_sname : "?", changed & 1 ? " rbx" : "", changed & 4 ? " r12" : "");
    if (n > 0) hao_diag_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
    hao_signal_die(SIGTRAP);
    __builtin_unreachable();
}

uint64_t hao_signal_shadow_handler(int sig) { return sig > 0 && sig < NSIG && OWNED(sig) ? ld_handler(sig) : 0; }

/* Called by the fault handler for faults hao does not own (never inside a
 * hao critical section); false: the default action. */
bool hao_signal_forward(int sig, siginfo_t *info, void *ctx)
{
    if (sig <= 0 || sig >= NSIG) return false;
    /* a native handler that blocks the signal is running on this thread
     * (whatever the disposition has become since) */
    volatile uint64_t mark = 0;
    hao_thread *pt = hao_current_peek();
    uint32_t outer = pt ? nh_live(pt, &mark, &pt->nh_blocked) : 0;
    if (outer & (1u << sig)) {
        /* a fault spins forever natively (xnu re-executes the instruction):
         * the default action; a sent signal waits for the handler (once: one
         * that comes back at the same pc is a fault) */
        uint64_t rip = ((ucontext_t *)ctx)->uc_mcontext->__ss.__rip;
        if (!sent_signal(info, ctx) || ((pt->nh_deferred & (1u << sig)) && pt->nh_defer_rip == rip)) return false;
        pt->nh_defer_rip = rip;
        __atomic_fetch_or(&pt->nh_deferred, 1u << sig, __ATOMIC_RELAXED);
        return true;
    }
    uint64_t h = ld_handler(sig);
    int fl = ld_flags(sig);
    sigset_t mask = ld_mask(sig);
    if (h <= 1) return false;   /* SIG_DFL; SIG_IGN of a fault is fatal too */
    hao_thread *t = hao_current();
    /* natively the signal is blocked while its handler runs, and a fault
     * with it blocked is fatal; hao cannot block it (it carries native->guest
     * calls), so it emulates that */
    if (t->in_fault_handler) return false;
    if (fl & SA_RESETHAND) st_gsig(sig, 0, 0, 0);
    uint32_t blocks = owned_bits(&mask) | (fl & SA_NODEFER ? 0 : 1u << sig);
    sigdelset(&mask, SIGSEGV);
    sigdelset(&mask, SIGBUS);
    sigset_t old;
    pthread_sigmask(SIG_BLOCK, &mask, &old);
    t->sig_nest++;
    if (is_guest_fn(h)) {
        int prev = t->in_fault_handler;
        if (!(fl & SA_NODEFER)) t->in_fault_handler = 1;
        deliver(t, sig, h, fl, info, ctx);
        t->in_fault_handler = prev;
    } else {
        static uint64_t seq;
        uint64_t at = t->nh_at, tok = t->nh_tok;
        if (blocks) {
            mark = ((uint64_t)(uintptr_t)&mark * 0x9E3779B97F4A7C15ull) ^ __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
            if (!mark) mark = 1;
            t->nh_tok = mark;
            t->nh_blocked = outer | blocks;
            __atomic_store_n(&t->nh_at, (uint64_t)(uintptr_t)&mark, __ATOMIC_RELEASE);
        }
        int changed = hao_sig_call_native(h, sig, info, ctx);
        t->nh_at = at;
        t->nh_tok = tok;
        t->nh_blocked = outer;
        mark = 0;
        if (changed & 5) bad_sigreturn(sig, h, changed);
    }
    t->sig_nest--;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    sync_mask_from(t, ctx);   /* what __sigreturn restores */
    /* what was sent while the handler had it blocked, now that it is not */
    uint32_t due = __atomic_load_n(&t->nh_deferred, __ATOMIC_RELAXED) & ~outer;
    if (due) {
        __atomic_fetch_and(&t->nh_deferred, ~due, __ATOMIC_RELAXED);
        for (int s = 1; s < NSIG; s++)
            if (due & (1u << s)) pthread_kill(pthread_self(), s);
    }
    return true;
}

void hao_signal_giveup(int sig, const siginfo_t *info, ucontext_t *uc)
{
    struct sigaction d, o;
    memset(&d, 0, sizeof d);
    d.sa_handler = SIG_DFL;
    int dfl = hao_sys_sigaction(sig, &d, NULL);
    if (OWNED(sig)) st_gsig(sig, 0, 0, 0);
    static const int traps[] = {SIGILL, SIGTRAP};
    for (int k = 0; k < 2; k++)
        if (hao_sys_sigaction(traps[k], NULL, &o) == 0 && o.sa_handler == SIG_IGN) hao_sys_sigaction(traps[k], &d, NULL);
    /* A fault: the instruction runs again and takes the default action of
     * the real fault. In user space when it can (never returns): the signal
     * is unblocked there already (it was delivered, and on_fault is
     * SA_NODEFER with an empty sa_mask), so no system call is needed but the
     * sigaction above: not __pthread_sigmask or __sigreturn, which a sandbox
     * may deny (27.0 WindowManager's denies both). Without SIG_DFL the fault
     * would come back here: through the kernel then. */
    bool sent = sent_signal(info, uc);
    if (!sent && uc && dfl == 0) sig_resume(uc, HAO_RESUME_GIVEUP, sig, info);
    sigset_t u;
    sigemptyset(&u);
    sigaddset(&u, sig);
    sigaddset(&u, SIGILL);
    sigaddset(&u, SIGTRAP);
    pthread_sigmask(SIG_UNBLOCK, &u, NULL);
    if (uc) uc->uc_sigmask &= ~u;   /* what sigreturn restores */
    if (sent) pthread_kill(pthread_self(), sig);   /* delivered now: the default action */
}

/* ------------------------------------------------------ sigaction */

/* Guest sigaction (trap_sigaction) and native sigaction of SIGSEGV/SIGBUS
 * (hao_native_sigaction). sigaction is async-signal-safe: a native handler
 * may call it on top of this thread's own call, so asynchronous signals wait
 * while disp_lock is held. */
static int do_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    if (sig <= 0 || sig >= NSIG) return hao_sys_sigaction(sig, act, oact);   /* EINVAL, as natively */
    int r = 0;
    struct sigaction cur;
    memset(&cur, 0, sizeof cur);
    sigset_t async, omask;
    sigfillset(&async);
    static const int sync_sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGEMT, SIGSYS};
    for (size_t k = 0; k < sizeof sync_sigs / sizeof *sync_sigs; k++) sigdelset(&async, sync_sigs[k]);
    pthread_sigmask(SIG_BLOCK, &async, &omask);
    hao_crit_enter();
    pthread_mutex_lock(&disp_lock);
    if (OWNED(sig)) {
        cur.sa_handler = (void (*)(int))(uintptr_t)ld_handler(sig);
        cur.sa_flags = ld_flags(sig) & REPORTED_FLAGS;
        cur.sa_mask = ld_mask(sig);
        if (act) {
            uint64_t h = (uint64_t)(uintptr_t)act->sa_handler;
            sigset_t m = act->sa_mask;
            sigdelset(&m, SIGKILL);
            sigdelset(&m, SIGSTOP);
            st_gsig(sig, h, h > 1 ? act->sa_flags : act->sa_flags & ~SA_SIGINFO, m);
        }
    } else {
        if (hao_sys_sigaction(sig, NULL, &cur) < 0) { r = -1; goto out; }   /* SIGKILL/SIGSTOP: EINVAL, as natively */
        if (cur.sa_sigaction == tramp) {
            int gf = ld_flags(sig);
            cur.sa_handler = (void (*)(int))(uintptr_t)ld_handler(sig);
            cur.sa_flags = (cur.sa_flags & ~(SA_SIGINFO | SA_ONSTACK)) | (gf & (SA_SIGINFO | SA_ONSTACK));
        }
        if (act) {
            uint64_t h = (uint64_t)(uintptr_t)act->sa_handler;
            struct sigaction na = *act;
            gsig_t was = {ld_handler(sig), ld_flags(sig), ld_mask(sig)};
            if (is_guest_fn(h)) {
                /* committed first: a signal delivered as soon as the kernel
                 * has the trampoline must find the handler */
                st_gsig(sig, h, act->sa_flags, act->sa_mask);
                na.sa_sigaction = tramp;
                /* the guest's sigaltstack is emulated on the guest side;
                 * the trampoline itself runs on the thread's native stack */
                na.sa_flags = (act->sa_flags | SA_SIGINFO) & ~SA_ONSTACK;
            }
            if (hao_sys_sigaction(sig, &na, NULL) < 0) {
                st_gsig(sig, was.handler, was.flags, was.mask);
                r = -1;
                goto out;
            }
            if (!is_guest_fn(h)) st_gsig(sig, 0, 0, 0);   /* SIG_DFL/SIG_IGN/native: the kernel's */
        }
    }
    if (oact) *oact = cur;
out:
    pthread_mutex_unlock(&disp_lock);
    {
        int e = errno;
        hao_crit_leave();
        pthread_sigmask(SIG_SETMASK, &omask, NULL);
        errno = e;
    }
    return r;
}

static void trap_sigaction(hao_thread *t, uint64_t a)
{
    (void)a;
    hao_cpu *c = &t->cpu;
    c->x[0] = (uint64_t)(int64_t)do_sigaction((int)c->x[0], (const struct sigaction *)(uintptr_t)c->x[1],
                                              (struct sigaction *)(uintptr_t)c->x[2]);
}

static void trap_signal(hao_thread *t, uint64_t a)
{
    (void)a;
    hao_cpu *c = &t->cpu;
    struct sigaction na, oa;
    memset(&na, 0, sizeof na);
    na.sa_handler = (void (*)(int))(uintptr_t)c->x[1];
    na.sa_flags = SA_RESTART;
    if (do_sigaction((int)c->x[0], &na, &oa) < 0) { c->x[0] = (uint64_t)(intptr_t)SIG_ERR; return; }
    c->x[0] = (uint64_t)(uintptr_t)oa.sa_handler;
}

static const char *const sig_names[] = {"_sigaction", "___sigaction", "_signal", "_bsd_signal", "_sigset"};

void hao_signal_interpose_names(hao_name_fn cb, void *ctx)
{
    for (size_t k = 0; k < sizeof sig_names / sizeof *sig_names; k++) cb(sig_names[k], ctx);
}

bool hao_signal_interpose(const char *name, uint64_t *addr)
{
    if (!strcmp(name, "_sigaction") || !strcmp(name, "___sigaction")) { *addr = hao_trap_address(name, trap_sigaction, 0); return true; }
    if (!strcmp(name, "_signal") || !strcmp(name, "_bsd_signal") || !strcmp(name, "_sigset")) {
        *addr = hao_trap_address(name, trap_signal, 0);
        return true;
    }
    return false;
}

/* A raw `svc` sigaction (syscall.c): the kernel's struct __sigaction also
 * names the libc trampoline that calls the handler, which hao's own
 * delivery (tramp) stands in for, as for a libc sigaction. oact is the
 * user struct sigaction. */
int hao_signal_raw_sigaction(int sig, const void *nsa, void *osa)
{
    const struct { uint64_t handler, tramp; sigset_t mask; int flags; } *k = nsa;
    struct sigaction na, oa;
    if (k) {
        memset(&na, 0, sizeof na);
        na.sa_handler = (void (*)(int))(uintptr_t)k->handler;
        na.sa_mask = k->mask;
        na.sa_flags = k->flags;
    }
    int r = do_sigaction(sig, k ? &na : NULL, osa ? &oa : NULL);
    if (!r && osa) memcpy(osa, &oa, sizeof oa);
    return r;
}

/* libhao_identity's sigaction family for native callers, SIGSEGV/SIGBUS only */
static int hao_native_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    if (act) __atomic_fetch_add(&hao_sig_native_installs, 1, __ATOMIC_RELAXED);
    return do_sigaction(sig, act, oact);
}

/* ---------------------------------------------- fault-tolerant reads
 * hao_probe_read8 (bridge_x86_64.S) may fault; on_fault resumes it at its
 * fail label. xnu re-executes a synchronous fault whose signal is blocked
 * forever, so it is used only where SIGSEGV and SIGBUS are known to be
 * deliverable: libhao_identity's pthread_sigmask and sigprocmask
 * interposers report each change of a thread's mask (mask_changed), a
 * thread's state records its mask when it is created (hao_current), and
 * the signal handlers hao runs (tramp, hao_signal_forward: the kernel adds
 * sa_mask, which may hold them) never probe. A native handler of another
 * signal whose sa_mask holds SIGSEGV or SIGBUS (libhao_identity's sigaction
 * reports it) turns the reads off for the process, and so does a process
 * without libhao_identity's hooks or with HAO_NO_PROBE=1. Callers then use
 * mach_vm_read_overwrite, which never faults. */
static int probe_off = 1;   /* until the hooks are registered */
uint64_t hao_probe_faults;
const uint64_t *hao_vm_gen;

static void mask_changed(int blocked)
{
    hao_thread *t = hao_current_peek();
    if (t) t->sync_masked = blocked;
}

static void handler_mask_sync(void) { __atomic_store_n(&probe_off, 1, __ATOMIC_RELAXED); }

bool hao_probe_safe(const hao_thread *t)
{
    return t && !t->sig_nest && !t->sync_masked && !__atomic_load_n(&probe_off, __ATOMIC_RELAXED);
}

/* xnu's handle_ux_exception (bsd/uxkern/ux_exception.c): a KERN_PROTECTION_FAILURE whose address lies in
 * [p->user_stack - MAXSSIZ, p->user_stack) is taken for a stack overflow, and unless SIGSEGV is handled on
 * an alternate stack (on_fault is not SA_ONSTACK) the kernel sets SIGSEGV to SIG_DFL and unblocks it before
 * delivery: on_fault never runs and the process dies ("Thread stack size exceeded"). That range is the main
 * thread's stack and the guard reservation below it (8 MB + 56 MB): a scanned value pointing into the guard
 * (a stale frame address in a register) is read through Mach instead. MAXSSIZ is 64 MB on x86_64 and arm64
 * macOS (bsd/i386/vmparam.h, bsd/arm/vmparam.h). */
#define HAO_MAXSSIZ (64ull << 20)
static uint64_t main_stack_lo, main_stack_hi;   /* the guard part: [user_stack - MAXSSIZ, stack bottom) */
static pthread_once_t main_stack_once = PTHREAD_ONCE_INIT;
extern pthread_t pthread_main_thread_np(void);   /* libpthread private/pthread/private.h */

static void main_stack_bounds(void)
{
    pthread_t m = pthread_main_thread_np();
    uint64_t top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(m), size = pthread_get_stacksize_np(m);
    if (top < HAO_MAXSSIZ) return;
    main_stack_lo = top - HAO_MAXSSIZ;
    main_stack_hi = size < HAO_MAXSSIZ ? top - size : main_stack_lo;
}

bool hao_probe_addr_ok(uint64_t a)
{
    pthread_once(&main_stack_once, main_stack_bounds);
    return !(a >= main_stack_lo && a < main_stack_hi);
}

void hao_signals_hook(void)
{
    void (*reg)(int (*)(int, const struct sigaction *, struct sigaction *)) =
        (void (*)(int (*)(int, const struct sigaction *, struct sigaction *)))dlsym(RTLD_DEFAULT, "hao_identity_sig_hook");
    if (reg) reg(hao_native_sigaction);
    /* returns whether a native handler with SIGSEGV/SIGBUS in its sa_mask was installed before */
    int (*mreg)(void (*)(int), void (*)(void)) = (int (*)(void (*)(int), void (*)(void)))dlsym(RTLD_DEFAULT, "hao_identity_mask_hook");
    const uint64_t *(*vg)(void) = (const uint64_t *(*)(void))dlsym(RTLD_DEFAULT, "hao_identity_vm_gen");
    if (vg) hao_vm_gen = vg();
    if (mreg && !mreg(mask_changed, handler_mask_sync) && !hao_opt.no_probe) __atomic_store_n(&probe_off, 0, __ATOMIC_RELAXED);
}
