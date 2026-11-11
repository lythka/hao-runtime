/* hao — hybrid runtime: guest threads, dispatch, native bridge, traps. */
#ifndef HAO_RUNTIME_H
#define HAO_RUNTIME_H

#include "hao/cpu.h"
#include "hao/macho.h"
#include "hao/code.h"
#include "hao/gmap.h"
#include <setjmp.h>

/* Guest return sentinel: a guest `ret` to this address ends hao_run(). It is
 * a canonical 47-bit address, so a guest that strips its own return address
 * (XPACLRI, XPACI: a64_xpac) keeps it, as it keeps a real one natively; no
 * process can map it (above MACH_VM_MAX_ADDRESS for x86_64 and arm64).
 * bridge_x86_64.S has a copy. */
#define HAO_RETURN_MAGIC 0x00007ffffffff000ull

#include <signal.h>

struct hao_thread;
struct hao_swift_value_scope;

/* One guest segment: a hao_run() invocation, i.e. guest frames entered from
 * native code (hao_call_guest, hao_callback, a signature callback) and run
 * until the guest returns to HAO_RETURN_MAGIC. Segments nest through native
 * frames; the record lives in hao_run's frame (see unwind.c). */
typedef struct hao_seg {
    struct hao_seg *prev;      /* enclosing segment, or NULL */
    struct hao_thread *t;
    const hao_cpu *outer;      /* enclosing segment's guest state, saved by whoever entered this one */
    int depth;                 /* t->depth while this segment runs */
    int fault_at_entry;        /* t->in_fault_handler when the segment was entered */
    int nest_at_entry;         /* t->sig_nest when the segment was entered */
    struct hao_swift_value_scope *swift_values_at_entry;
    jmp_buf jb;                /* re-entry point: a guest longjmp from an inner segment
                                * (hao_longjmp_segment), a signal handler that moved pc (signals.c) */
    /* run_loop -> native_step, for the native call about to run (runtime.c, native-target cache):
     * nc_hit: the cache gave the plan (nc_plan) for native target nc_target; else nc_key, when
     * nonzero, is the address the guest branched to (before shared-cache redirects, which set x1 to
     * nc_x1 when nonzero) and nc_gen the cache generation its classification started in */
    const struct hao_plan *nc_plan;
    uint64_t nc_target, nc_key, nc_x1, nc_gen;
    bool nc_hit;
} hao_seg;

/* An exception whose guest handler the search phase found (unwind.c). */
typedef struct hao_eh_rec {
    void *ue;                  /* struct _Unwind_Exception * */
    hao_seg *seg;
    int depth;
    uint64_t cfa, lp, adj;
    int64_t sel;
    const uint8_t *lsda, *action;
} hao_eh_rec;

#define HAO_EH_RECS 8

#define HAO_JIT_JC_SLOTS 2048   /* a power of two */
#define HAO_NCACHE_SLOTS 256    /* a power of two */
typedef struct hao_thread {
    hao_cpu cpu;               /* must be first (16-byte aligned) */
    uint8_t *stack;
    size_t stack_size;
    int depth;                 /* nested hao_run() levels on this thread */
    uint64_t native_calls, callbacks, traps, steps, blocks;
    unsigned swift_objects_building;
    unsigned swift_results_resolving;
    bool async_contract_resolving;
    struct hao_swift_value_scope *swift_values;
    hao_seg *seg;              /* innermost running segment */
    void *eh_landed;           /* exception that stopped at this segment's native-call guard */
    void *eh_resume;           /* exception to resume natively after the current native step */
    hao_eh_rec eh_rec[HAO_EH_RECS];
    /* signals (signals.c): a handler that moves pc resumes at the innermost
     * segment's jmp_buf (seg->jb) */
    uint32_t sig_pending;      /* arrived inside a hao critical section, not yet redelivered */
    uint32_t sig_redeliver;    /* re-raised by hao_crit_leave: use the saved siginfo */
    int in_fault_handler;      /* guest SIGSEGV/SIGBUS handler running (no SA_NODEFER) */
    int sig_depth;             /* segment depth of the innermost guest signal handler running, or 0 */
    int sig_nest;              /* signal handlers tramp/hao_signal_forward are running on this thread */
    /* a native SIGSEGV/SIGBUS handler hao_signal_forward is running without
     * SA_NODEFER (its signal blocked, as natively): the address of a word in
     * the forwarding frame and the token it holds. A handler that left by
     * longjmp leaves them stale; the frame check tells (signals.c). */
    uint64_t nh_at, nh_tok;
    uint32_t nh_blocked;       /* the SIGSEGV/SIGBUS bits such handlers block */
    uint32_t nh_deferred;      /* SIGSEGV/SIGBUS sent (not faults) while such a handler ran: raised after it */
    uint64_t nh_defer_rip;     /* where the last one was deferred */
    siginfo_t sig_info[NSIG];  /* siginfo of each pending signal */
    /* the thread's signal mask blocks SIGSEGV or SIGBUS (set when the state is
     * created, kept by libhao_identity's pthread_sigmask/sigprocmask
     * interposers): hao_probe_read8 must not fault then */
    int sync_masked;
    /* jit.c: the thread's jump cache, one slot per low part of a branch
     * target, and one miss record (two words) per slot behind them (zero
     * until the thread first runs translated code) */
    uint64_t jit_jc[3 * HAO_JIT_JC_SLOTS];
    /* runtime.c: the thread's native-target cache (guest branch target -> native target, x1, plan),
     * an entry valid while hao_ncache_gen equals its gen; read and written only by this thread */
    struct { uint64_t pc, gen, target, x1; const struct hao_plan *plan; } ncache[HAO_NCACHE_SLOTS];
    /* ncache_owner: 0, or the segment depth (depth) of the cache store in progress on this thread, set and
     * cleared by single stores (a signal handler on top of it neither probes nor stores); ncache_wseq:
     * completed stores, so a probe a handler's store interrupted sees the change */
    int ncache_owner;
    uint32_t ncache_wseq;
} hao_thread;

/* Frames at segment depth `depth` and deeper were abandoned (a nonlocal jump or an exception): a cache
 * store among them never finishes, so its ownership ends. An owner at a smaller depth is a store still
 * suspended below the code that left (runtime.c). */
static inline void hao_ncache_abandon(hao_thread *t, int depth)
{
    int o = __atomic_load_n(&t->ncache_owner, __ATOMIC_RELAXED);
    if (o && o >= depth) __atomic_store_n(&t->ncache_owner, 0, __ATOMIC_RELAXED);
}

/* Where guest execution stands, for a signal that interrupts it. The
 * runtime keeps these in the cpu block's host scratch words. */
#define HAO_SCRATCH_IN_GUEST 1  /* host_scratch[1]: 1 while translated code or the interpreter runs */
#define HAO_SCRATCH_HELPER   2  /* host_scratch[2]: guest pc of the instruction the interpreter runs */

typedef struct {
    uint64_t gpr[6];
    uint64_t target;
    uint64_t *stack_args;
    uint8_t xmm[8][16];
    uint64_t swift[4];    /* r12 error, r13 self, r14 async context, rax indirect result */
    uint64_t sig;         /* r10: hsig*, tagged Swift recipe, HAO_CB_FAULT from NX, else 0 */
    uint64_t ret;         /* return address into the native caller */
    uint64_t rbp;         /* the native caller's rbp at entry */
} hao_cb_frame;
#define HAO_CB_FAULT 1

typedef void (*hao_trap_fn)(hao_thread *t, uint64_t arg);

/* bridge_x86_64.S */
void hao_native_call(uint64_t fn, hao_cpu *cpu, uint64_t nstack);
void hao_callback_entry(void);
/* Call fn(seg) in a frame whose personality is hao_eh_personality; returns
 * 1 when an exception stopped unwinding there, else 0. */
#define HAO_GUARD_NATIVE 1     /* around one native call or trap of a segment */
#define HAO_GUARD_ENTRY  2     /* around a whole segment */
long hao_eh_guard(void (*fn)(void *), hao_seg *seg, long kind);
uint64_t hao_eh_guard_pad(void);
long hao_raw_syscall(long num, const uint64_t args[9], uint64_t *rdx_out, int *err_out);

/* runtime.c */
hao_thread *hao_current(void);       /* this thread's guest state, created on first use; async-signal-safe */
hao_thread *hao_current_peek(void);  /* the same, or NULL; never allocates */
extern uint64_t hao_threads_live, hao_threads_freed;  /* guest thread states alive / freed at thread exit */
void hao_fork_init(void);              /* register the fork child handler; once, before guest code */
/* hao's own critical sections: every hao mutex that guest execution can
 * reach (jit_lock, thunk_lock, sig_lock, invoke_lock, the signal table) and
 * hao's allocations on guest paths. A guest signal that arrives on a thread
 * inside one is recorded and re-raised when the outermost section ends
 * (signals.c), so a guest handler never runs on top of a hao lock or
 * malloc. They nest; hao_crit_depth is async-signal-safe. */
void hao_crit_enter(void);
void hao_crit_leave(void);
int hao_crit_depth(void);
void hao_run(hao_thread *t, const hao_cpu *outer);   /* outer: the caller's saved guest state */
uint64_t hao_call_guest(hao_thread *t, uint64_t fn, const uint64_t *args, int nargs);
uint64_t hao_trap_address(const char *name, hao_trap_fn fn, uint64_t arg);
bool hao_is_trap(uint64_t pc);
void hao_install_fault_handler(void);
bool hao_bootarg_flag(const char *key);   /* boot argument "key=1" present (kern.bootargs) */
/* trust.c: how far the environment is trusted (hao/aot.h); the switches
 * themselves are read once into hao_opt (hao/opts.h, hao_opts_init). */
int hao_env_tier(void);                   /* HAO_ENV_OPEN / GUARDED / SETUGID */
/* hao_opts_init for the CLI's stage 2: flags is the inherited HAO_FLAGS,
 * argv_flags the letters of hao's own command-line switches, each honoured
 * where the environment switch of the same effect is (trust.c). */
void hao_opts_init_cli(const char *flags, const char *argv_flags);
/* guest_sig.c: code-signature policy for a CLI-form guest, checked on the
 * descriptor the loader then maps (hao_load_main_image); path names the file
 * in diagnostics. 0 = run it; -1 = refused, diagnostic printed. */
int hao_guest_sig_enforce(int fd, const char *path);
const char *hao_env_tier_name(int tier);
/* Fatal diagnostics: stderr, or the console with boot argument hao_crash=1
 * (daemons' stderr usually goes nowhere and AMFI denies their core dumps),
 * and with hao_crash=1 also /private/var/log/hao/console.log (HAO_DIAG_DIR:
 * another directory), which hardware keeps when nothing records the console
 * (runtime.c, "diagnostics channel"). */
#include <stdio.h>
#include "hao/opts.h"
extern FILE *hao_diag_file;
#define HAO_DIAG (hao_diag_file ? hao_diag_file : stderr)
void hao_diag_init(void);                 /* once, at startup (hao_install_fault_handler) */
void hao_diag_write(const char *b, size_t n);   /* async-signal-safe: console and log, whole */
extern size_t hao_diag_log_cap;           /* the log's size limit (bytes) */
/* A multi-line report written with one hao_diag_write (hao_diag_block_end):
 * fprintf into hao_diag_block's FILE (HAO_DIAG if no memory). Not in signal handlers. */
typedef struct { FILE *f; char *buf; size_t len; } hao_diag_blk;
FILE *hao_diag_block(hao_diag_blk *b);
void hao_diag_block_end(hao_diag_blk *b);
void hao_diag_prefix(FILE *f);            /* "hao[prog:pid]: " */
/* hao's diagnostic line: HAO_LOG(level, fmt, ...) writes "hao[prog:pid]: "
 * and the message to HAO_DIAG (stderr, or the console and hao_crash's log)
 * in one write, when level <= hao_verbose: 0 always, 1 under -v, 2 under
 * -vv, 3 under -vvv. The arguments are evaluated only then. fmt ends the
 * line itself ("\n"). Not in signal handlers (stdio). */
#define HAO_LOG(level, ...) do { if ((level) <= hao_verbose) hao_log_line(__VA_ARGS__); } while (0)
void hao_log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* hao's own bookkeeping got no memory (`what` names the table): one
 * diagnostic, then abort, never a table that silently lost an entry. Not in
 * signal handlers. hao_alloc_check: p, or hao_oom(what) when p is NULL. */
__attribute__((noreturn, cold)) void hao_oom(const char *what);
static inline void *hao_alloc_check(void *p, const char *what) { if (!p) hao_oom(what); return p; }
void hao_diag_guest_addr(FILE *f, uint64_t a);   /* " (image+off, file vmaddr V)" when a is guest code */
const char *hao_diag_guest_where(uint64_t a, char *buf, size_t n);   /* the same text in buf ("" otherwise) */
uint64_t hao_resolve_import(hao_image *img, int ordinal, const char *name, bool weak, bool *found);
/* Load the main executable from fd, the open descriptor its launcher has
 * checked (main.c); path only names it. The loader owns fd from here on.
 * hao_main_image is set before its imports bind. */
hao_image *hao_load_main_image(const char *path, int fd, char *err, size_t errlen);
/* Load every image's remaining dependencies, then run the initialisation of
 * each image not yet initialised, dependencies first (main.c
 * hao_guest_image_init: TLV, ObjC, Swift, blocks, fnslots, AOT, initializers). */
void hao_run_initializers(void);
/* A guest library hao loads for itself (thunks.c), mapped and bound; NULL with err set. */
hao_image *hao_load_guest_library(const char *path, char *err, size_t n);

/* dsc.c: guest images from the arm64e dyld shared cache */
typedef uint64_t (*hao_dsc_binder)(const char *lib, const char *sym, bool *ok);
bool hao_dsc_has(const char *path);
/* The cache image at path can run as guest code next to native libraries:
 * an arm64 dylib with no ObjC, Swift or thread-local metadata whose every
 * pointer and ADRP+ADD into another cache image names an export (dsc.c).
 * false with the reason in why. */
bool hao_dsc_eligible(const char *path, char *why, size_t n);
hao_image *hao_dsc_load(const char *path, hao_mapped_fn mapped, void *ctx, char *err, size_t errlen);
void hao_dsc_bind(hao_image *img, hao_dsc_binder bind);
/* After hao_dsc_bind: every type_info img exports names native_lib's _ZTS
 * of the same type (one type identity for both copies); the count shared. */
int hao_dsc_share_type_names(hao_image *img, hao_dsc_binder bind, const char *native_lib);
/* A guest branch to cache address pc outside every guest image: *native is
 * where execution continues, and *x1 is nonzero when the target is an
 * objc_msgSend stub, which sets x1 to that native selector first. */
bool hao_dsc_redirect(uint64_t pc, uint64_t *native, uint64_t *x1);
/* pc lies where hao_dsc_redirect looks: a guest cache image is loaded and pc is in the cache */
bool hao_dsc_covers(uint64_t pc);
/* Pre-optimised ObjC metadata of cache images (objc.c): the runtime address
 * of the selector buffer that direct relative method lists are offsets
 * from (0 without one); a pointer-sized field anywhere in the cache, decoded
 * from the slide info when no guest image owns (and so rewrote) it; whether
 * `a` lies in the cache but in no guest image; and making [a, a+n) of a
 * guest cache image writable. */
uint64_t hao_dsc_selector_base(void);
/* The arm64e cache's selector strings (slid; span 0 without a cache): a
 * selector built in code that hao could not patch is such a string, which is
 * no native SEL. hao_dsc_sel_map returns the native selector for x1 in the
 * range (counted in hao_dsc_sels_mapped), else x1. */
extern uint64_t hao_dsc_sel_lo, hao_dsc_sel_span, hao_dsc_sels_mapped;
uint64_t hao_dsc_sel_map(uint64_t x1);
uint64_t hao_dsc_load_ptr(uint64_t a);
bool hao_dsc_foreign(uint64_t a);
void hao_dsc_writable(uint64_t a, uint64_t n);
/* executable room for Swift metadata island entries within rel32 reach of [lo, hi), carved out of the arm64e
 * cache's reservation (dsc.c); NULL if none */
void *hao_dsc_island(uint64_t lo, uint64_t hi, uint64_t size);
/* What hao_dsc_bind wrote to the pointer-sized cache slot at a (a uniqued
 * GOT slot an image's code or Swift metadata reaches); 0 if it rewrote none. */
uint64_t hao_dsc_bound_ptr(uint64_t a);
/* Bind the cache slot at a as hao_dsc_bind binds a GOT slot (named and bound
 * natively, or slid); true if it is bound now (swiftmeta.c: key path patterns). */
bool hao_dsc_bind_slot(uint64_t a);
extern hao_image *hao_main_image;
extern int hao_verbose;
extern int hao_use_jit;
/* Set by host test drivers that load and register an image without running
 * any of its code: hao_run aborts instead of entering the guest. */
extern bool hao_no_guest_code;

/* objc.c: hao_objc_register_image = hao_objc_read_image (selectors,
 * protocols, classes, categories) + hao_objc_run_loads (+load methods). */
void hao_objc_register_image(hao_image *img);
void hao_objc_read_image(hao_image *img);
void hao_objc_run_loads(hao_image *img);

/* interpose.c */
bool hao_interpose_lookup(const char *name, uint64_t *addr);
int hao_interpose_selfcheck(char *why, size_t n);   /* tests: table order and name indexes; 0 = sound */
/* name is an ABI repair of a native C++ function (applies to native binds only) */
bool hao_interpose_abi_repair(const char *name);
void hao_tlv_setup(hao_image *img);

/* Native entry points the register bridge cannot call exactly have plans
 * (hao/bridge.h, src/bridge.c): imports keep their native address, so
 * native callers and function-pointer comparisons are unaffected; the
 * generic native-call path (hao_run) looks the target's plan up and either
 * extends a narrow result or makes a signature-driven call:
 *   'B' bool, 'c'/'C' signed/unsigned char, 's'/'S' signed/unsigned short:
 *       Apple arm64 callers rely on the callee extending such a result to
 *       32 bits; x86_64 defines only al/ax. Extend x0 after the call.
 *   'A': call through hao_call_native_sig with the plan's encoding
 *       (aggregates by value, arm64 stack arguments packed at their natural
 *       size where SysV uses 8-byte slots, ...). */
const char *hao_stackargs_enc(const char *name);                 /* stackargs_gen.inc signature, or NULL */
/* varsniff.c: bind a variadic native function that no interposer table names */
bool hao_varsniff_bind(const char *name, uint64_t native, uint64_t *addr);
bool hao_varsniff_prologue(const uint8_t *code, int *ngp, int *nfp);
bool hao_varsniff_native(uint64_t fn, int *ngp, int *nfp);   /* checks fn is in an image's __TEXT first */
/* the same, also taking a prologue that saves no GPR as *ngp = 6 (six or more fixed: ObjC IMPs, whose
 * encoding gives the count) */
bool hao_varsniff_native_imp(uint64_t fn, int *ngp, int *nfp);
/* the varsniff bridge for a native variadic fn: fmt = index of the format among the ngp fixed arguments
 * (-1: none), fkind = its SDK family ('p', 'C', ...; 0: typed only as a guest-image constant); returns
 * whether the variadic slots were typed */
bool hao_varsniff_call(struct hao_thread *t, uint64_t fn, int ngp, int nfp, char narrow, int fmt, char fkind);
/* the prologue verdict of a native ObjC IMP, kept per IMP (lock-free reads); decode false in a guest
 * signal handler; *fresh when this call decoded a variadic one */
bool hao_varsniff_imp(uint64_t imp, bool decode, int *ngp, int *nfp, bool *fresh);
bool hao_varsniff_imp_note(uint64_t imp, unsigned bit);   /* one-time reports per IMP (bits 0-5) */
void hao_varsniff_fork_child(void);
/* objc.c: the plan for a native variadic IMP (prologue counts g, f) of a method (enc, sel): false when
 * the encoding disagrees (declined); else the fixed GPR count, the format's index and SDK family */
bool hao_objcva_plan(int g, int f, const char *enc, const char *sel, int *ngp, int *fmt, char *fk);
char hao_narrowret_kind_for_name(const char *name);   /* imported symbol (with '_'); 0 if wide */
char hao_narrowret_kind_for_enc(const char *enc);     /* ObjC/block type encoding's return type */
/* x0 as an Apple arm64 callee leaves it: extended to 32 bits, upper half zero */
static inline uint64_t hao_narrowret_extend(uint64_t v, char kind)
{
    switch (kind) {
    case 'B': case 'C': return (uint8_t)v;
    case 'c': return (uint32_t)(int32_t)(int8_t)v;
    case 's': return (uint32_t)(int32_t)(int16_t)v;
    case 'S': return (uint16_t)v;
    default: return v;
    }
}

/* unwind.c: exceptions and longjmp across native frames */
void hao_eh_landed(hao_thread *t);
void hao_eh_resume_native(hao_thread *t);
void hao_eh_trap_resume(hao_thread *t, uint64_t arg);
void hao_eh_forget(hao_thread *t, int above_depth);
void hao_longjmp_segment(hao_thread *t, int depth);   /* noreturn: re-enter segment `depth` */
/* on_fault: a libunwind personality call to a non-canonical address (the x86_64 libdispatch callout CIE)
 * returns _URC_FATAL_PHASE1_ERROR, as the arm64e noexcept personality does; init before on_fault is installed */
void hao_eh_fault_init(void);
bool hao_eh_fault(ucontext_t *uc);
/* rip is the instruction after a `callq *%rax` in libunwind's __TEXT: where hao_eh_fault leaves a context */
bool hao_eh_fault_target(uint64_t rip);
extern uint64_t hao_eh_badpers_hits;

/* syscall.c */
void hao_guest_svc(hao_thread *t);

/* signals.c */
void hao_signals_init(void);
int hao_signal_raw_sigaction(int sig, const void *nsa, void *osa);   /* a guest svc sigaction(2) */
void hao_signals_stats(void);
bool hao_signal_forward(int sig, siginfo_t *info, void *uc);
/* on_fault gives up on a fault: SIG_DFL, the signal and SIGILL/SIGTRAP unblocked (also in the
 * context the kernel restores), a sent signal raised again: the default action is certain */
void hao_signal_giveup(int sig, const siginfo_t *info, ucontext_t *uc);
/* the kernel's sigaction, past libhao_identity's interposers (hao's own dispositions) */
int hao_sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
void hao_signals_hook(void);           /* native sigaction of SIGSEGV/SIGBUS -> the shadow; after on_fault is installed */
extern uint64_t hao_sig_native_installs;   /* native installs of SIGSEGV/SIGBUS kept in the shadow */
/* bridge_x86_64.S: fn(sig, info, uctx); bit mask of callee-saved registers it changed (1 rbx, 4 r12, ...) */
int hao_sig_call_native(uint64_t fn, int sig, siginfo_t *info, void *uctx);
void hao_signal_redeliver(hao_thread *t, uint32_t pending);
void hao_signals_block_async(void);    /* on hao's own threads: no asynchronous signal is delivered there */
/* Leave on_fault without __sigreturn and without any system call: restore every register ctx's mcontext holds
 * and jump to its rip (never returns; the mask is already the ucontext's), or return when it must go back
 * through the kernel (switch off, Rosetta, alternate stack, a state or rip outside what it resumes).
 * path: what on_fault did, for the counts. */
enum { HAO_RESUME_NX, HAO_RESUME_TRAP, HAO_RESUME_COMMPAGE, HAO_RESUME_STALE, HAO_RESUME_GIVEUP, HAO_RESUME_EHPERS, HAO_RESUME_PROBE, HAO_RESUME_NPATHS };
void hao_sig_resume(void *ctx, int path);
/* bridge_x86_64.S: *out = the 8 bytes at a, or false when the read faults (on_fault resumes at the fail
 * label). Only where hao_probe_safe(t) holds. */
bool hao_probe_read8(uint64_t a, uint64_t *out);
extern char hao_probe_read8_load[], hao_probe_read8_fail[];
/* t may take a fault in hao_probe_read8: not inside a signal handler hao runs (tramp, hao_signal_forward)
 * and SIGSEGV/SIGBUS not blocked, as far as libhao_identity's mask interposers tell */
bool hao_probe_safe(const hao_thread *t);
/* a may be read by hao_probe_read8: not in the main thread's stack reservation below its stack, where
 * xnu turns a protection fault into SIGSEGV with SIG_DFL forced (handle_ux_exception) */
bool hao_probe_addr_ok(uint64_t a);
/* Nonempty, nonoverflowing word spans: aligned, or one unaligned word wholly
 * within one page. Same guarded reader and VM-generation-aware unreadable-page
 * cache as block argument scans; both unaligned endpoints check stack guards. */
bool hao_probe_read_words(const hao_thread *t, uint64_t a, uint64_t *out, unsigned n);
extern uint64_t hao_probe_faults;     /* hao_probe_read8 reads that faulted */
/* libhao_identity's count of calls that can map memory or make it readable (src/blocks.c read_words), or NULL */
extern const uint64_t *hao_vm_gen;
/* a fetch fault at addr whose context stands at _sigtramp's entry (another signal's frame came first) */
bool hao_sig_stale(const ucontext_t *uc, uint64_t addr);
/* a data fault at addr whose context stands at _sigtramp's entry and whose interrupted context stands at pc */
bool hao_sig_stale_at(const ucontext_t *uc, uint64_t addr, uint64_t pc);
void hao_sig_resume_print(FILE *f);    /* the "hao: signal returns:" line (--stats, hao_nx_stats) */
size_t hao_sig_resume_line(char *b, size_t cap);   /* the same line into b (async-signal-safe); 0: none */
/* SIG's default action now, from inside a handler: SIG_DFL, SIG unblocked, raised; never returns */
__attribute__((noreturn)) void hao_signal_die(int sig);
/* the shadow handler of SIGSEGV/SIGBUS (what hao_signal_forward would call), else 0 */
uint64_t hao_signal_shadow_handler(int sig);
/* The last action hao's signal handlers (on_fault, tramp) took on this thread, kept for the failed-__sigreturn
 * report (runtime.c on_abort). uctx/tok: the delivery's ucontext and sigreturn token (tok 0: not known). */
enum { HAO_ACT_NX = 1, HAO_ACT_NX_TRAP, HAO_ACT_COMMPAGE, HAO_ACT_FORWARD, HAO_ACT_GIVEUP, HAO_ACT_TRAMP, HAO_ACT_STALE, HAO_ACT_EHPERS, HAO_ACT_PROBE };
void hao_fault_act_note(int kind, int sig, uint64_t a, uint64_t b, const void *uctx, uint64_t tok);
/* bridge_x86_64.S: the kernel's handlers for on_fault and on_abort, fn(sig, info, uctx), which call
 * hao_on_fault/hao_on_abort(sig, info, uctx, r12): r12 is the sigreturn token _sigtramp keeps there */
void hao_fault_entry(int sig, siginfo_t *info, void *uctx);
void hao_abort_entry(int sig, siginfo_t *info, void *uctx);

/* jit.c */
int hao_jit_run_block(hao_thread *t);   /* returns exit reason */
/* The guest instruction a host pc inside translated code belongs to:
 * false if host_pc is not in a translated block. *flags_in_host: the guest
 * NZCV lives in host RFLAGS there, not in cpu.flags. */
bool hao_jit_locate(uint64_t host_pc, uint64_t *guest_pc, bool *flags_in_host);
bool hao_jit_init(void);      /* false: no executable memory (interpreter, no thunks) */
bool hao_have_thunks(void);
bool hao_thunk_range(uint64_t *lo, uint64_t *hi); /* entire reserved plain-thunk area */
void hao_jit_stats(void);
/* `hao --aot [--cache DIR] FILE`: write FILE's AOT cache into cache_dir (absolute), or with NULL into
 * the directory this process writes to; 0 or -1 (reported) */
int hao_aot_generate_file(const char *path, bool verbose, const char *cache_dir);
uint64_t hao_thunk_for(uint64_t guest);
uint64_t hao_thunk_target(uint64_t thunk);
bool hao_in_code_cache(uint64_t a);
void hao_jit_alias(uint64_t entry, uint64_t guest);
uint64_t hao_sig_thunk(uint64_t guest, void *sig);   /* sig: an hsig* (abi.c); 0 if none can be made */
uint64_t hao_sig_thunk_target(uint64_t thunk);
void hao_jit_fork_child(void);         /* fork child: re-initialise the module's mutexes */
/* blocks.c */
void hao_thunk_native_args(hao_thread *t);
void hao_thunk_native_blocks(hao_thread *t);
void hao_thunk_native_code_slots(hao_thread *t, unsigned slots);   /* typed slots (hao_sig_msgsend) */
void hao_blocks_register_image(hao_image *img);
bool hao_blocks_heal(uint64_t a0, uint64_t target);
void hao_blocks_stats(void);
void hao_blocks_fork_child(void);
void hao_dsc_fork_child(void);
/* gss.c: GSS-API calls and data with x86_64 pack(2) layouts (gss_gen.inc) */
bool hao_gss_interpose(const char *name, uint64_t *addr);
void hao_gss_fork_child(void);
void hao_objc_fork_child(void);      /* objc.c: the deferred-category lock */
/* objc.c: categories deferred to Swift-runtime realization, attached since, classes realized so */
void hao_objc_late_stats(uint64_t *deferred, uint64_t *attached, uint64_t *classes);
/* swiftcc.c: x86_64 swifttailcc argument area (callee pop) of a guest async
 * function from its own code, or -1 when it cannot be analysed */
int64_t hao_swift_async_arg_area(uint64_t fn, const char **why);
/* its x86_64 stack-slot count (-1: not analysable), whether the arm64 pop at
 * an exit set it, and the instruction that did (offline checks) */
int hao_swift_async_slots(uint64_t fn, int *from_pop, uint64_t *at);
/* Narrow results of native Swift async functions reach the guest as an
 * argument of its continuation: kinds 'b' Bool, 'h'/'H' Int8/UInt8,
 * 't'/'T' Int16/UInt16 (the kind of the native entry's plan). */
char hao_swift_async_result_kind(const char *sym);           /* "_$s...": 0 if not async or wide */
char hao_swift_async_kind_for_demangled(const char *d);
void hao_swift_async_note_call(uint64_t ctx, char kind);     /* guest calls such a function, x22 = its context */
uint64_t hao_swift_async_extend_result(uint64_t target, uint64_t x0);   /* native resumes guest `target` */
void hao_swiftcc_fork_child(void);
/* interpose.c: a page the guest mapped or made executable itself (mmap,
 * mprotect): arm64 code generated at run time, which hao does not run */
bool hao_guest_exec_mapped(uint64_t a);
void hao_guest_vm_note(uint64_t a, uint64_t n, bool exec);   /* the guest mapped [a, a+n): executable or not */
void hao_interpose_fork_child(void);
static inline bool hao_swift_async_kind(char k) { return k == 'b' || k == 'h' || k == 'H' || k == 't' || k == 'T'; }
/* fnslots.c */
void hao_fnslots_image(hao_image *img);
void hao_fnslots_stats(void);
/* taken[k] = 1 for each fn[k] (sorted, distinct) whose address the image's code materializes */
void hao_fnslots_address_taken(hao_image *img, const uint64_t *fn, size_t nfn, uint8_t *taken);
/* swiftmeta.c: relative pointers in Swift metadata to guest functions ->
 * thunks in islands within rel32 range of the image */
/* a layout string a guest metadata instantiation function hands to swift_cvw_instantiateLayoutString: its
 * enum-tag and resilient-accessor functions get island entries (once per string) */
void hao_swiftmeta_layout_template(uint64_t ls);
void hao_swiftmeta_image(hao_image *img);
void hao_swiftmeta_stats(void);
/* The relative indirect(able) references of an image's Swift metadata whose
 * slot lies outside the image, by kind (hao_swiftmeta_slots, swiftmeta.c). */
enum {
    HAO_SK_TYPEREF_CTX, HAO_SK_TYPEREF_CONF, HAO_SK_TYPEREF_ASSOC, HAO_SK_TYPEREF_OBJCPROTO,
    HAO_SK_TYPE_RECORD, HAO_SK_PROTO_RECORD, HAO_SK_PARENT,
    HAO_SK_REQ_PROTO, HAO_SK_REQ_OBJCPROTO, HAO_SK_REQ_CONF,
    HAO_SK_CONF_PROTO, HAO_SK_CONF_TYPEREF, HAO_SK_CONF_OBJCCLASS, HAO_SK_CONF_RETRO, HAO_SK_CONF_WITNESS,
    HAO_SK_CONF_GLOBALACTOR, HAO_SK_SUPERCLASS, HAO_SK_OVERRIDE_CLASS, HAO_SK_OVERRIDE_METHOD,
    HAO_SK_DEFOVER_REPL, HAO_SK_DEFOVER_ORIG, HAO_SK_PATTERN_VWT, HAO_SK_SHAPE_VWT,
    HAO_SK_REPLACE_KEY, HAO_SK_REPLACE2_ORIG, HAO_SK_N
};
typedef void (*hao_swift_slot_fn)(void *ctx, int kind, uint64_t field, uint64_t slot);
size_t hao_swiftmeta_slots(hao_image *img, hao_swift_slot_fn fn, void *ctx);
const char *hao_swiftmeta_slot_kind(int kind);
bool hao_swiftmeta_is_island(uint64_t a);
uint64_t hao_swiftmeta_island_target(uint64_t entry);   /* guest function of an island entry, else 0 */
bool hao_swiftmeta_interpose(const char *name, uint64_t *addr);   /* swift_continuation_init, swift_task_create, ... */
void hao_swiftmeta_resume_parent(uint64_t ctx);
void hao_async_contract_prepare(hao_cpu *, uint64_t target, const char *symbol);
void hao_async_contract_fork_child(void);
void hao_swiftmeta_async_args(hao_cpu *c);   /* async function records in x0-x7 for a native Swift function (HAO_PLAN_ASYNCARG) */
bool hao_swift_async_param(const char *sym);   /* a Swift function with a parameter of async function type (swiftcc.c) */
/* "_$s...": a type metadata accessor or a value witness, whose signature the metadata ABI fixes (swiftcc.c) */
bool hao_swift_fixed_abi(const char *sym);
bool hao_swift_async_param_demangled(const char *d);
void hao_swiftmeta_fork_child(void);   /* an AsyncContext handed to native code: thunk its ResumeParent */
/* callbacks.c */
bool hao_callbacks_interpose(const char *name, uint64_t *addr);
/* libm.c: <fenv.h> with the arm64 values; long double functions bound to their double twins */
bool hao_libm_interpose(const char *name, uint64_t *addr);
void hao_callbacks_stats(void);
/* abi.c */
bool hao_sig_simple(const char *enc);
/* Stable parsed identity by encoding content (or NULL), never descriptor address. */
const void *hao_sig_identity(const char *enc);
/* every argument of enc an integer, pointer, float or double and the result one too (or void): their
 * GPR and FP counts (self and _cmd included); false otherwise */
bool hao_sig_scalar_args(const char *enc, int *ngp, int *nfp);
bool hao_call_native_sig(hao_cpu *c, uint64_t fn, const char *enc);
/* an ObjC method encoding for objc_msgSend to a native IMP: true when the register bridge is exact
 * (hao_sig_simple); *codeslots: the arm64 argument slots holding code ('^?' function pointers, '@?'
 * blocks, such fields of by-value aggregates; hao/bridge.h argmask bits), *fnret: a '^?' result */
bool hao_sig_msgsend(const char *enc, unsigned *codeslots, bool *fnret);
uint64_t hao_guest_entry_for_sig(uint64_t guest, const char *enc);
/* Explicit API callback contract: only supported unambiguous simple void
 * signatures receive a typed entry; everything else keeps a plain thunk. */
uint64_t hao_guest_entry_for_void_sig(uint64_t guest, const char *enc);
void hao_abi_fork_child(void);
void *hao_guest_sig(uint64_t guest);   /* signature noted for a guest function that has no signature thunk */
/* native entry point (plain or signature thunk, Swift metadata island entry) -> guest function, else 0 */
static inline uint64_t hao_guest_of_entry(uint64_t a)
{
    uint64_t g = hao_thunk_target(a);
    if (!g) g = hao_sig_thunk_target(a);
    return g ? g : hao_swiftmeta_island_target(a);
}
bool hao_aot_load(hao_image *im);
/* phys_footprint and its limit (launchd's jetsam limit), bytes; false: no limit or unreadable (jit.c) */
bool hao_footprint_limit(uint64_t *footprint, uint64_t *limit);

/* Closure fields discovered from Swift allocation metadata. */
void hao_swift_object_prepare(uint64_t object);
void hao_objc_prepare_generic_class(uint64_t metadata);
void hao_swift_objects_args(hao_thread *t);
void hao_swift_objects_result(hao_thread *t, uint64_t target, uint64_t indirect);
bool hao_swift_objects_interpose(const char *name, uint64_t *addr);
void hao_swift_objects_fork_child(void);
void hao_swift_objects_stats(void);

#endif
