/*
 * hao — hybrid runtime.
 *
 * One address space, two instruction sets:
 *   - guest arm64e code is mapped non-executable and runs through the
 *     translator (JIT) or the reference interpreter;
 *   - everything else (dyld, libSystem, frameworks) is native x86_64.
 * A guest branch to an address outside guest code is a native call made
 * through hao_native_call. A native call into guest code faults on the
 * non-executable page; the fault handler redirects it to
 * hao_callback_entry, which runs the guest function on this thread.
 */
#include "hao/runtime.h"
#include "hao/gen_stamp.h"
#include "hao/identity.h"
#include "hao/swiftvalue.h"
#include "hao/bridge.h"
#include "hao/native_bundle.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <pthread/introspection.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/semaphore.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <os/log.h>
#include <mach-o/loader.h>

hao_image *hao_main_image;
static hao_real_dlopen_fn native_open;
static void *runtime_dlopen(const char *path, int flags)
{
    return native_open ? native_open(path, flags) : dlopen(path, flags);
}
int hao_verbose;
int hao_use_jit = 1;

/* Per-thread state is reached through two TSD keys: the hao_thread, and the
 * thread's critical-section depth (a plain integer, so entering a section
 * never needs a hao_thread). Both keys exist before any guest code runs
 * (constructor), so signal handlers only read them.
 *
 * Guest thread state lives from the thread's first guest call until
 * libpthread's THREAD_TERMINATE introspection event, which it sends on the
 * dying thread after every TSD destructor pass. The key's destructor only
 * puts the value back (libpthread clears the slot before each call), so
 * guest code that runs in later destructors (guest keys, the ObjC
 * autorelease-pool drain, TLV and C++ thread_local destructors) finds this
 * thread's state instead of mapping a fresh guest stack in every pass. */
static pthread_key_t thread_key, crit_key;
static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
uint64_t hao_threads_live, hao_threads_freed;

/* One anonymous mapping per thread: a guard, the guest stack, and the
 * hao_thread above it (thread_new). */
#define GUEST_STACK_GUARD (64u << 10)  /* PROT_NONE below the stack */
static size_t thread_state_size(void) { size_t page = (size_t)getpagesize(); return (sizeof(hao_thread) + page - 1) & ~(page - 1); }

static void thread_key_dtor(void *v) { pthread_setspecific(thread_key, v); }

/* Saved guest states of nested native-to-guest calls (hao_thread.cpu_saves).
 * Each level of guest -> native -> guest recursion (a chain of objects whose
 * deinits release the next, a native sort whose comparator sorts) costs
 * native stack for hao's frames; the 1 KB copy of the guest state was the
 * largest part, so a 512 KB dispatch worker ran out at ~125 levels where
 * native arm64 needs ~200 bytes a level. The first CPU_SAVES_ON_STACK
 * levels keep the copy on the stack (most threads never nest deeper, so
 * they never map anything); deeper ones take a slot here. A mapping, not
 * malloc: callbacks also run in signal handlers. */
#define CPU_SAVES 4096
#define CPU_SAVES_ON_STACK 4
static hao_cpu *cpu_save_push(hao_thread *t, unsigned *mark)
{
    unsigned n = t->cpu_saves_n;
    if (t->depth < CPU_SAVES_ON_STACK || n >= CPU_SAVES) return NULL;
    if (!t->cpu_saves) {
        void *m = mmap(NULL, CPU_SAVES * sizeof(hao_cpu), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) return NULL;
        t->cpu_saves = m;
    }
    *mark = n;
    t->cpu_saves_n = n + 1;
    return &t->cpu_saves[n];
}

static pthread_introspection_hook_t prev_hook;

static void thread_hook(unsigned int ev, pthread_t th, void *addr, size_t size)
{
    if (prev_hook) prev_hook(ev, th, addr, size);
    if (ev != PTHREAD_INTROSPECTION_THREAD_TERMINATE) return;
    hao_thread *t = pthread_getspecific(thread_key);
    if (!t) return;  /* the thread never ran guest code */
    pthread_setspecific(thread_key, NULL);
    __atomic_fetch_sub(&hao_threads_live, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&hao_threads_freed, 1, __ATOMIC_RELAXED);
    if (t->cpu_saves) munmap(t->cpu_saves, CPU_SAVES * sizeof(hao_cpu));
    /* Safe with guest frames still on this stack (pthread_exit from guest
     * code): Darwin's pthread_exit never returns into them. The mapping
     * holds t itself, so nothing reads t after this. */
    munmap(t->stack - GUEST_STACK_GUARD, GUEST_STACK_GUARD + t->stack_size + thread_state_size());
}

/* A fork child keeps the other threads' guest stacks mapped, as it keeps
 * their native stacks: child code may still read data that lives on them. */
static void threads_child(void) { hao_threads_live = pthread_getspecific(thread_key) ? 1 : 0; }

static void thread_key_init(void)
{
    pthread_key_create(&thread_key, thread_key_dtor);
    pthread_key_create(&crit_key, NULL);
    prev_hook = pthread_introspection_hook_install(thread_hook);
    pthread_atfork(NULL, NULL, threads_child);
}
__attribute__((constructor)) static void keys_ctor(void) { pthread_once(&thread_once, thread_key_init); }

/* This thread's guest state, or NULL; never allocates (crash and watchdog paths). */
hao_thread *hao_current_peek(void)
{
    pthread_once(&thread_once, thread_key_init);
    return pthread_getspecific(thread_key);
}

/* Size of a new guest stack. A guest thread runs on its own native thread,
 * so it gets that thread's stack size (pthread_attr_setstacksize reaches
 * the native pthread_create); the main thread gets the larger of
 * RLIMIT_STACK and the program's LC_MAIN stacksize (if the main image is
 * loaded by then). Never less than 8 MB, which hao gave every thread
 * before (hao's frames between guest frames need room too), never more
 * than 1 GB. The mapping is lazy, so the size costs address space only.
 * Only system calls: thread_new may run in a signal handler. */
#define GUEST_STACK_MIN (8ull << 20)
#define GUEST_STACK_MAX (1ull << 30)
static size_t guest_stack_size(void)
{
    uint64_t want = pthread_get_stacksize_np(pthread_self());
    if (pthread_main_np()) {
        struct rlimit rl;
        if (!getrlimit(RLIMIT_STACK, &rl) && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > want) want = rl.rlim_cur;
        if (hao_main_image && hao_main_image->stack_size > want) want = hao_main_image->stack_size;
    }
    if (want < GUEST_STACK_MIN) want = GUEST_STACK_MIN;
    if (want > GUEST_STACK_MAX) want = GUEST_STACK_MAX;
    return (size_t)((want + 0xFFFF) & ~0xFFFFull);
}

/* One anonymous mapping holds the guard, the guest stack and the
 * hao_thread above it: only mmap/mprotect, so a guest signal handler can
 * create the state of a native thread that never ran guest code. */
static hao_thread *thread_new(void)
{
    size_t ss = guest_stack_size(), hs = thread_state_size();
    uint8_t *m = mmap(NULL, GUEST_STACK_GUARD + ss + hs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        static const char msg[] = "hao: guest stack: out of memory\n";
        write(2, msg, sizeof msg - 1);
        abort();
    }
    mprotect(m, GUEST_STACK_GUARD, PROT_NONE);
    hao_thread *t = (hao_thread *)(m + GUEST_STACK_GUARD + ss);   /* zero-filled */
    t->stack = m + GUEST_STACK_GUARD;
    t->stack_size = ss;
    t->cpu.x[31] = ((uint64_t)(uintptr_t)t->stack + t->stack_size - 64) & ~15ull;
    t->cpu.thread = t;
    hao_set_nzcv(&t->cpu, 0);
    /* Darwin arm64 keeps the TSD base in TPIDRRO_EL0. In hybrid mode the
     * guest shares the native thread, so expose the native TSD: x86_64
     * Darwin points %gs at it; pthread_self() is its base. */
    t->cpu.tpidrro_el0 = (uint64_t)(uintptr_t)pthread_self() + 0xE0; /* _PTHREAD_TSD_OFFSET */
    return t;
}

hao_thread *hao_current(void)
{
    hao_thread *t = hao_current_peek();
    if (t) return t;
    /* a guest handler interrupting this would create a second state */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    t = pthread_getspecific(thread_key);
    if (!t) {
        t = thread_new();
        t->sync_masked = sigismember(&old, SIGSEGV) == 1 || sigismember(&old, SIGBUS) == 1;   /* hao_probe_safe */
        __atomic_fetch_add(&hao_threads_live, 1, __ATOMIC_RELAXED);
        pthread_setspecific(thread_key, t);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return t;
}

/* ------------------------------------------------------------- fork
 * A fork child gets only the forking thread, but every hao mutex keeps the
 * state it had in the parent, so a lock another thread held at the fork
 * would stay held forever and the child would hang at its first new block,
 * thunk, signature or block invoke. The child handler re-initialises them
 * (unlocking is wrong: the owner and any waiter state belong to threads
 * the child does not have). There is deliberately no prepare handler; the
 * lock comment in jit.c says why and what that requires of every critical
 * section. Registered before any guest code runs, so it runs after
 * libSystem's own child handlers and before every guest child handler,
 * which may translate. */
static void traps_fork_child(void);
static void loads_fork_child(void);
static void fork_child(void)
{
    hao_ncache_invalidate();   /* a plan another thread published may not have bumped it before the fork */
    traps_fork_child();
    loads_fork_child();
    hao_jit_fork_child();
    hao_abi_fork_child();
    hao_blocks_fork_child();
    hao_bridge_fork_child();
    hao_dsc_fork_child();
    hao_gss_fork_child();
    hao_objc_fork_child();
    hao_swiftmeta_fork_child();
    hao_swift_objects_fork_child();
    hao_swift_result_fork_child();
    hao_swiftcc_fork_child();
    hao_interpose_fork_child();
    hao_swift_value_fork_child();
    hao_async_contract_fork_child();
    hao_varsniff_fork_child();
}

static pthread_once_t fork_once = PTHREAD_ONCE_INIT;
static void fork_register(void) { pthread_atfork(NULL, NULL, fork_child); }
void hao_fork_init(void) { pthread_once(&fork_once, fork_register); }

/* ------------------------------------------------ critical sections */

int hao_crit_depth(void) { return (int)(intptr_t)pthread_getspecific(crit_key); }

void hao_crit_enter(void) { pthread_setspecific(crit_key, (void *)(intptr_t)(hao_crit_depth() + 1)); }

void hao_crit_leave(void)
{
    int d = hao_crit_depth() - 1;
    pthread_setspecific(crit_key, (void *)(intptr_t)d);
    if (d) return;
    hao_thread *t = pthread_getspecific(thread_key);
    if (t && __atomic_load_n(&t->sig_pending, __ATOMIC_RELAXED))
        hao_signal_redeliver(t, __atomic_exchange_n(&t->sig_pending, 0, __ATOMIC_ACQ_REL));
}

/* ------------------------------------------------------------- traps */

/* A trap is a 16-byte slot of a PROT_NONE reservation: guest code that
 * branches there runs traps[k].fn(t, traps[k].arg) instead. Slots are
 * made at image load, on any thread, and found again by (fn, arg) through
 * a growable index (hao/gmap.h; traps[] is filled in order, so only the
 * pages in use are touched). The last slot is the overflow trap: once the
 * table is full, new traps get it (with one warning), so a program with
 * too many imports still loads and fails only if it calls one of them. */
#define TRAP_SLOTS 65536
static uint8_t *trap_base;
static struct { const char *name; hao_trap_fn fn; uint64_t arg; } traps[TRAP_SLOTS];
static hao_gmap trap_index = HAO_GMAP_INIT(3, 2, 7, 17);   /* (fn, arg) -> slot */
static uint32_t ntraps;
static pthread_mutex_t trap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t unres_lock = PTHREAD_MUTEX_INITIALIZER;   /* unresolved_name, below */
static pthread_once_t trap_once = PTHREAD_ONCE_INIT;

static void trap_overflow(hao_thread *t, uint64_t arg)
{
    fprintf(stderr, "hao: call to an import that got no trap: the trap table (%d) was full at load (lr %#llx)\n", TRAP_SLOTS,
            (unsigned long long)t->cpu.x[30]);
    abort();
}

static void trap_init(void)
{
    trap_base = mmap(NULL, TRAP_SLOTS * 16, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (trap_base == MAP_FAILED) { perror("hao: trap area"); abort(); }
    traps[TRAP_SLOTS - 1].name = "(trap table full)";
    traps[TRAP_SLOTS - 1].fn = trap_overflow;
}

/* fork child: see the lock comment in jit.c. Both locks are held only for
 * a lookup or an insert, inside a hao critical section. */
static void traps_fork_child(void)
{
    trap_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    unres_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

uint64_t hao_trap_address(const char *name, hao_trap_fn fn, uint64_t arg)
{
    pthread_once(&trap_once, trap_init);
    uint64_t key[2] = {(uint64_t)(uintptr_t)fn, arg};
    uint32_t slot = TRAP_SLOTS - 1;
    bool warn = false, fresh;
    hao_crit_enter();
    pthread_mutex_lock(&trap_lock);
    uint64_t *e = hao_gmap_claim(&trap_index, key, &fresh);
    if (e && !fresh) slot = (uint32_t)e[2];
    else if (!e || ntraps >= TRAP_SLOTS - 1) warn = true;
    else {
        slot = ntraps++;
        /* an own copy: an import's name lies in its image's fixups, which the loader reads in the
         * slice and gives back when the load ends (macho.c) */
        const char *own = strdup(name);
        traps[slot].name = own ? own : "(trap)"; traps[slot].arg = arg;
        __atomic_store_n(&traps[slot].fn, fn, __ATOMIC_RELEASE);
        e[2] = slot;
        hao_gmap_publish(&trap_index, e, key);
    }
    pthread_mutex_unlock(&trap_lock);
    hao_crit_leave();
    static int warned;
    if (warn && !__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hao: trap table full (%d): %s and later imports that need a trap abort if called\n", TRAP_SLOTS, name);
    return (uint64_t)(uintptr_t)(trap_base + 16 * slot);
}

bool hao_is_trap(uint64_t pc)
{
    return trap_base && pc >= (uint64_t)(uintptr_t)trap_base && pc < (uint64_t)(uintptr_t)trap_base + TRAP_SLOTS * 16;
}

static void run_trap(hao_thread *t, uint64_t pc)
{
    int k = (int)((pc - (uint64_t)(uintptr_t)trap_base) / 16);
    t->traps++;
    if (hao_verbose > 1) fprintf(stderr, "hao: trap %s\n", traps[k].name);
    t->cpu.pc = t->cpu.x[30];  /* default: return to caller; handlers may override */
    traps[k].fn(t, traps[k].arg);
}

/* -------------------------------------------------------- dispatch */

__attribute__((noinline, cold)) static void guest_fault(hao_thread *t, int st)
{
    hao_cpu *c = &t->cpu;
    const char *what = st == HAO_EXIT_UNDEF ? "unimplemented/undefined instruction" :
                       st == HAO_EXIT_BRK ? "brk" : st == HAO_EXIT_UDF ? "udf" : "hlt";
    FILE *f = HAO_DIAG;
    hao_diag_prefix(f);
    fprintf(f, "guest %s at pc=%#llx", what, (unsigned long long)c->pc);
    hao_diag_guest_addr(f, c->pc);
    fprintf(f, " insn=%08x lr=%#llx", st == HAO_EXIT_UNDEF ? c->exit_insn : hao_code_read32(c->pc), (unsigned long long)c->x[30]);
    hao_diag_guest_addr(f, c->x[30]);
    fprintf(f, "\n");
    fflush(f);
    if (hao_opt.bridge_stats) hao_bridge_stats();   /* the last report of a process that stops here */
    if (st == HAO_EXIT_BRK) {
        /* __builtin_trap / assertion: behave like the native process would */
        raise(SIGTRAP);
    }
    abort();
}

bool hao_no_guest_code;

/* Guest -> native call of `pc` with the guest's registers and stack, as its
 * plan says (hao/bridge.h; NULL: UNKNOWN). */
/* noinline: its 320 bytes of arrays would otherwise be in every native call's frame, also across the
 * native callee's callbacks into guest code */
__attribute__((noinline)) static void native_call_stackpack(hao_cpu *c, uint64_t pc, const hao_plan *p)
{
    /* Swift packed stack arguments (interface-unit record kind K): Apple arm64 put the arguments
     * past x7 at their natural size, SysV wants one 8-byte slot each. hao_native_call copies
     * guest stack words to slots 2 onward, so build those words below the guest's frame (the
     * arguments zero-extended, the rest 0) and let it copy them from there. Native code
     * calling back into guest code runs below them (hao_callback's red-zone margin); the
     * guest sp is restored after the call. */
    uint8_t off[HAO_IF_MAX_PACK], w[HAO_IF_MAX_PACK];
    int n = hao_ifunit_stackpack(p->enc, off, w);
    uint64_t sp = c->x[31], words[HAO_IF_MAX_PACK] = {0};
    for (int i = 0; i < n; i++) {
        const uint8_t *a = (const uint8_t *)(uintptr_t)(sp + off[i]);
        words[i] = w[i] == 1 ? *a : w[i] == 2 ? *(const uint16_t *)a : w[i] == 4 ? *(const uint32_t *)a : *(const uint64_t *)a;
    }
    uint64_t nsp = (sp - 256 - sizeof words) & ~15ull;
    memcpy((void *)(uintptr_t)nsp, words, sizeof words);
    c->x[31] = nsp;
    hao_native_call(pc, c, (uint64_t)n);
    c->x[31] = sp;
}

static inline void native_call(hao_cpu *c, uint64_t pc, const hao_plan *p)
{
    char kind = p ? p->kind : 0;
    if (kind == 'A' && hao_call_native_sig(c, pc, p->enc)) return;
    /* Foundation.Data? arguments and result (hao/bridge.h kind 'D'): only the nil differs */
    const char *optdata = NULL;
    if (kind == 'D') {
        optdata = p->enc;
        for (int i = 0; optdata[i + 1] && i < 8; i++)
            if (optdata[i + 1] == 'd' && c->x[i] == 0 && c->x[i + 1] == HAO_OPTDATA_NIL_ARM) c->x[i + 1] = HAO_OPTDATA_NIL_X86;
        kind = 0;
    }
    if (kind == 'Z') { hao_cxxstr_call(c, pc, p->enc); return; }   /* std::string arguments (cxxstr.c) */
    if (kind == 'P') {   /* a C++ constructor or destructor: arm64 returns this, x86_64 nothing */
        uint64_t self = c->x[0];
        hao_native_call(pc, c, 16);
        c->x[0] = self;
        return;
    }
    /* a native Swift async function with a narrow result passes it to the
     * guest continuation (swiftcc.c): note that, x0 here is not the result */
    if (hao_swift_async_kind(kind)) { hao_swift_async_note_call(c->x[22], kind); kind = 0; }
    /* the continuation a native async callee returns to (swiftmeta.c) */
    if (p && (p->flags & HAO_PLAN_SWIFTASYNC)) {
        hao_swiftmeta_resume_parent(c->x[22]);
        hao_async_contract_prepare(c,pc,p->name);
    }
    /* a native generic result G? (swiftcc.c): async ones are converted at their resume */
    const void *genres_type = NULL;
    uint64_t genres_at = p && (p->flags & HAO_PLAN_GENRESULT) ? hao_swift_genres_call(c, p->native, &genres_type) : 0;
    /* async function values for a native Swift function's async parameters (swiftmeta.c) */
    if (p && (p->flags & HAO_PLAN_ASYNCARG)) hao_swiftmeta_async_args(c);
    if (kind == HAO_IF_KIND_STACKPACK) { native_call_stackpack(c, pc, p); return; }
    hao_native_call(pc, c, 16);
    if (genres_at) hao_swift_genres_result(genres_type, genres_at);
    if (optdata && optdata[0] == 'R' && c->x[0] == 0 && c->x[1] == HAO_OPTDATA_NIL_X86) c->x[1] = HAO_OPTDATA_NIL_ARM;
    if (kind && kind != 'A') c->x[0] = hao_narrowret_extend(c->x[0], kind);   /* bool/char/short result */
}

/* The first word at a, or 0 when a is not readable: a register the -vv
 * trace prints may hold any 64-bit value (a Swift small string's bytes look
 * like an aligned address). */
static uint64_t trace_word(uint64_t a)
{
    uint64_t w = 0;
    mach_vm_size_t got = 0;
    if (a > 0x100000000ull && a < 0x800000000000ull && !(a & 7) &&
        mach_vm_read_overwrite(mach_task_self(), a, 8, (mach_vm_address_t)(uintptr_t)&w, &got) != KERN_SUCCESS)
        w = 0;
    return w;
}

/* ------------------------------------------------ native-target cache
 *
 * A guest branch to an address outside guest code is classified by
 * run_loop (trap, guest code, a native entry of a guest function, a
 * shared-cache redirect) and then by native_step (its bridge plan, bound on
 * the first call of a bare address). For an ordinary native function the
 * answer depends on the address only: the native target (the address, or
 * where a shared-cache stub or island leads), the x1 a redirected
 * objc_msgSend stub sets, and the plan. Each thread remembers it in a
 * direct-mapped table in its hao_thread, so a repeated call costs one probe.
 *
 * Validity. An entry carries the generation (hao_ncache_gen) its
 * classification started in and is used only while the generation is
 * unchanged. Everything that can change the answer bumps it once the change
 * is visible: image registration and release (macho.c: guest code ranges,
 * the shared cache's loaded images) and every plan table insert or
 * replacement (bridge.c tab_put: first-call binds, ObjC probes that found a
 * type, typed upgrades, async marks). A classification that raced a bump
 * is stored under the older generation, which no later probe accepts. What
 * is remembered is only what the slow path would compute again: a trap, a
 * guest function's entry, an address the shared-cache redirect looked at
 * without a result, an address in hao's code cache or a Swift island, and a
 * plan whose lookup still has side effects (no plan yet, an ObjC probe plan
 * with probes left) are never stored; the shared-cache redirect table never
 * forgets an entry. The per-call checks after the plan (ObjC selector
 * recheck, interposer redirect, REFUSED, counting, Swift value and witness
 * hooks, argument scan) run on every call, as before.
 *
 * Concurrency. Only the owning thread reads or writes its table, so there is
 * no sharing; but a guest signal handler may run on top of a probe or a
 * store on the same thread and use the table itself. A store owns the
 * thread's table for its whole duration (ncache_owner: its segment depth,
 * set and cleared by single stores, so a handler sees either no owner or
 * the real one) and counts itself (ncache_wseq) when done: a handler that interrupts a store misses on every probe and skips
 * its own stores, so no entry is ever written by two stores at once; a
 * probe that a handler's completed store interrupted sees the count change
 * and misses. Either way the slow path runs instead. A handler that leaves
 * by a nonlocal jump (a guest siglongjmp, or a pc it moved) re-enters some
 * segment's hao_run and abandons every frame above it there: a store running
 * at that segment's depth or deeper never finishes, so the re-entry clears
 * its ownership; so does an exception that unwinds through it (unwind.c: the
 * NATIVE guard's landing, and leaving a segment). A jump or an exception
 * into a handler above the store leaves the suspended store's ownership
 * alone. Nothing here allocates or locks. The fork child invalidates every
 * table (fork_child): a plan published just before the fork may not have
 * bumped the generation yet. */

static inline unsigned ncache_slot(uint64_t pc)
{
    return (unsigned)(((pc >> 2) * 0x9E3779B97F4A7C15ull) >> 56) & (HAO_NCACHE_SLOTS - 1);
}

static inline bool ncache_get(hao_thread *t, uint64_t pc, uint64_t *target, uint64_t *x1, const hao_plan **plan)
{
    __typeof__(&t->ncache[0]) e = &t->ncache[ncache_slot(pc)];
    uint32_t seq = __atomic_load_n(&t->ncache_wseq, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t->ncache_owner, __ATOMIC_RELAXED) || e->pc != pc) return false;
    uint64_t gen = e->gen;
    *target = e->target;
    *x1 = e->x1;
    *plan = e->plan;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&t->ncache_wseq, __ATOMIC_RELAXED) == seq && !__atomic_load_n(&t->ncache_owner, __ATOMIC_RELAXED) &&
           __atomic_load_n(&e->pc, __ATOMIC_RELAXED) == pc && gen == __atomic_load_n(&hao_ncache_gen, __ATOMIC_ACQUIRE);
}

static void ncache_put(hao_thread *t, uint64_t gen, uint64_t pc, uint64_t target, uint64_t x1, const hao_plan *plan)
{
    if (!pc || gen != __atomic_load_n(&hao_ncache_gen, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&t->ncache_owner, __ATOMIC_RELAXED)) return;   /* a handler on top of an unfinished store */
    __atomic_store_n(&t->ncache_owner, t->depth > 0 ? t->depth : 1, __ATOMIC_RELAXED);   /* ownership and depth in one store */
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __typeof__(&t->ncache[0]) e = &t->ncache[ncache_slot(pc)];
    __atomic_store_n(&e->pc, 0, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    e->gen = gen;
    e->target = target;
    e->x1 = x1;
    e->plan = plan;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&e->pc, pc, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&t->ncache_wseq, t->ncache_wseq + 1, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&t->ncache_owner, 0, __ATOMIC_RELAXED);
}

/* One native call or trap of segment `arg`. Runs inside a NATIVE guard
 * (hao_eh_guard), so an exception unwinding out of it meets the guest's
 * catch handlers and cleanups (unwind.c). */
static void native_step(void *arg)
{
    hao_seg *seg = arg;
    hao_thread *t = seg->t;
    hao_cpu *c = &t->cpu;
    uint64_t pc = c->pc;
    const hao_plan *p;
    if (seg->nc_hit && pc == seg->nc_target) p = seg->nc_plan;   /* run_loop's cache probe: not a trap, the settled plan */
    else {
        if (hao_is_trap(pc)) { run_trap(t, pc); return; }
        p = hao_bridge_plan_for_call(t, pc);
        if (seg->nc_key && hao_bridge_plan_settled(p)) ncache_put(t, seg->nc_gen, seg->nc_key, pc, seg->nc_x1, p);
    }
    hao_plan objc_call;   /* a method IMP called with another selector than its plan's: this call's type */
    if (p && p->src == HAO_BRS_OBJCIMP && c->x[1] != p->sel) p = hao_bridge_objc_recheck(c, p, &objc_call);
    if (p && p->redirect) { run_trap(t, p->redirect); return; }   /* an interposed function by its bare address */
    if (p && p->cls == HAO_BR_REFUSED) hao_bridge_refused(t, p);
    if (__builtin_expect(hao_bridge_counting, 0)) {
        hao_bridge_count(t, p, pc);
        if (hao_opt.bridge_check && p && (p->flags & HAO_PLAN_MASKED)) hao_bridge_check_mask(t, p);
    }
    if (t->swift_values && hao_swift_value_native(t,pc)) return;
    if (pc >= __atomic_load_n(&hao_swift_witness_lo,__ATOMIC_ACQUIRE) && pc <= hao_swift_witness_hi &&
        hao_swift_witness_native(t,pc)) return;
    if (p && p->thunk_guest) {
        hao_swift_objects_args(t);   /* swiftself need not appear in the pair's code-argument mask */
        /* a generated thunk pair (hao/bridge.h): the guest half runs with
         * the guest's own registers, stack and lr; x18 = &p->tp */
        c->x[18] = (uint64_t)(uintptr_t)p->tp;
        c->pc = (uint64_t)(uintptr_t)p->thunk_guest;
        return;
    }
    /* guest function pointers and blocks passed to native code get thunks:
     * in the argument slots the plan names, or in all of them */
    hao_thunk_native_args_mask(t, p && (p->flags & HAO_PLAN_MASKED) ? p->argmask : HAO_BR_SCAN_ALL);
    t->native_calls++;
    unsigned objc_enum_result = p && p->src == HAO_BRS_OBJCIMP ? hao_objc_enum_args(c, pc, false) : 0;
    if (hao_verbose > 2) {
        Dl_info di;
        const char *sn = dladdr((void *)(uintptr_t)pc, &di) && di.dli_sname ? di.dli_sname : "?";
        const char *sel = "";
        if (strstr(sn, "objc_msgSend") && c->x[1]) sel = sel_getName((SEL)(uintptr_t)c->x[1]);
        uint64_t i0 = trace_word(c->x[0]);
        fprintf(stderr, "hao: -> %s(%#llx [isa %#llx], %#llx %s, %#llx) lr=%#llx\n", sn, (unsigned long long)c->x[0], (unsigned long long)i0,
                (unsigned long long)c->x[1], sel, (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
        native_call(c, pc, p);
        uint64_t r0 = c->x[0], w0 = trace_word(r0);
        fprintf(stderr, "hao: <- %s = %#llx [isa %#llx]\n", sn, (unsigned long long)r0, (unsigned long long)w0);
    } else
    native_call(c, pc, p);
    if (objc_enum_result) hao_objc_enum_result(c, objc_enum_result);
    c->pc = c->x[30];
}

/* The dispatch loop of segment `arg`; runs inside its ENTRY guard. */
static void run_loop(void *arg)
{
    hao_seg *seg = arg;
    hao_thread *t = seg->t;
    hao_cpu *c = &t->cpu;
    /* a chain of shared-cache redirects being classified: the address the guest branched to, the
     * last nonzero x1 a redirect set, the cache generation the chain started in */
    uint64_t chain_from = 0, chain_x1 = 0, chain_gen = 0;
    for (;;) {
        uint64_t pc = c->pc;
        if (pc == HAO_RETURN_MAGIC) break;
        uint64_t target, x1;
        const hao_plan *plan;
        bool native = true;
        seg->nc_hit = false;
        seg->nc_key = 0;
        if (ncache_get(t, pc, &target, &x1, &plan)) {   /* what the classification below found last time */
            if (x1) c->x[1] = x1;
            c->pc = target;
            seg->nc_hit = true;
            seg->nc_plan = plan;
            seg->nc_target = target;
        } else if (hao_is_trap(pc) || !hao_image_for_code(pc)) {
            if (!hao_is_trap(pc)) {
                if (!chain_from) chain_gen = __atomic_load_n(&hao_ncache_gen, __ATOMIC_ACQUIRE);   /* before any lookup */
                uint64_t g = hao_guest_of_entry(pc);
                if (g) {                              /* guest calling a guest function via its thunk */
                    hao_jit_alias(pc, g);
                    c->pc = g;
                    chain_from = 0;
                    continue;
                }
                /* a shared-cache guest image branching into another cache
                 * library, or into an objc_msgSend stub (which sets x1) */
                if (hao_dsc_redirect(pc, &g, &x1)) {
                    if (x1) c->x[1] = chain_x1 = x1;
                    else if (!chain_from) chain_x1 = 0;
                    if (!chain_from) chain_from = pc;
                    c->pc = g;
                    continue;
                }
                /* remembered unless the redirect could still find a target later, or pc is hao's own code */
                if (!hao_dsc_covers(pc) && !hao_in_code_cache(pc) && !hao_swiftmeta_is_island(pc)) {
                    seg->nc_key = chain_from ? chain_from : pc;
                    seg->nc_x1 = chain_from ? chain_x1 : 0;
                    seg->nc_gen = chain_gen;
                }
            }
        } else native = false;
        chain_from = 0;
        if (native) {
            if (hao_eh_guard(native_step, seg, HAO_GUARD_NATIVE)) hao_eh_landed(t);
            t->cpu_saves_n = seg->cpu_saves_at_entry;   /* also when an exception skipped a callback's pop */
            if (t->eh_resume) hao_eh_resume_native(t);
            if (t->seg != seg) {
                /* a native longjmp skipped inner segments: their saved state is gone */
                hao_diag_prefix(HAO_DIAG);
                fprintf(HAO_DIAG, "native longjmp across translated frames (segment depth %d) is not supported\n", seg->depth);
                abort();
            }
            continue;
        }
        int st;
        if (hao_use_jit) {
            st = hao_jit_run_block(t);
            t->blocks++;
        } else {
            /* a64_step advances cpu.pc before the instruction's memory
             * accesses: a signal handler gets this pc instead */
            c->host_scratch[HAO_SCRATCH_HELPER] = pc;
            c->host_scratch[HAO_SCRATCH_IN_GUEST] = 1;
            st = a64_step(c);
            c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
            t->steps++;
        }
        switch (st) {
        case HAO_EXIT_NONE: case HAO_EXIT_BRANCH: break;
        case HAO_EXIT_SVC: hao_guest_svc(t); break;
        default: guest_fault(t, st);
        }
    }
}

/* Run guest code from t->cpu until it returns to HAO_RETURN_MAGIC. `outer`
 * is the enclosing segment's state, which the caller saved and restores
 * afterwards; the unwinder restores it when an exception or a longjmp
 * leaves this segment early. */
void hao_run(hao_thread *t, const hao_cpu *outer)
{
    if (hao_no_guest_code) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "guest code entered at pc=%#llx with hao_no_guest_code set\n", (unsigned long long)t->cpu.pc);
        abort();
    }
    hao_seg seg;
    seg.prev = t->seg;
    seg.t = t;
    seg.outer = outer;
    seg.depth = ++t->depth;
    seg.fault_at_entry = t->in_fault_handler;
    seg.nest_at_entry = t->sig_nest;
    seg.swift_values_at_entry = t->swift_values;
    seg.cpu_saves_at_entry = t->cpu_saves_n;
    t->seg = &seg;
    /* Re-entry point for a guest longjmp from an inner segment
     * (hao_longjmp_segment) and for a guest signal handler that moved pc
     * while this segment ran guest code (signals.c); cpu.pc is set. */
    if (_setjmp(seg.jb)) {
        t->swift_values = seg.swift_values_at_entry;
        t->cpu_saves_n = seg.cpu_saves_at_entry;   /* saves of the skipped native-to-guest calls */
        /* frames above this hao_run are gone, among them an interrupted cache store at this depth or deeper */
        hao_ncache_abandon(t, seg.depth);
    }
    hao_eh_guard(run_loop, &seg, HAO_GUARD_ENTRY);
    t->seg = seg.prev;
    t->depth = seg.depth - 1;
}

/* bridge_x86_64.S: a native callee returned with an rsp/rbp that differs
 * from the call site (e.g. a swifttailcc frame popped stack arguments, or a
 * native async function resumed somewhere else). Debug report
 * (HAO_BRIDGE_CHECK=1 or HAO_VERBOSE>=2); execution goes on with the call
 * site's rsp/rbp restored. Expected: native swifttailcc callees (Swift
 * concurrency: swift_task_switch, async functions that suspend) `retq $8`;
 * when the guest tail-called them, hao_callback forwards the pop (cpu
 * native_pop) to its own native caller. */
void hao_bridge_unbalanced(hao_cpu *c, uint64_t rsp, uint64_t rbp)
{
    static int on = -1;
    if (on < 0) on = hao_opt.bridge_check || hao_verbose > 1;
    if (!on) return;
    uint64_t want_rsp = c->bridge_rsp, want_rbp = c->bridge_rbp, fn = c->bridge_fn;
    Dl_info di;
    const char *sn = dladdr((void *)(uintptr_t)fn, &di) && di.dli_sname ? di.dli_sname : "?";
    fprintf(stderr, "hao: UNBALANCED native call %s (%#llx): rsp %#llx -> %#llx (%+lld), rbp %#llx -> %#llx; x22=%#llx lr=%#llx%s\n", sn,
            (unsigned long long)fn, (unsigned long long)want_rsp, (unsigned long long)rsp, (long long)(rsp - want_rsp),
            (unsigned long long)want_rbp, (unsigned long long)rbp, (unsigned long long)c->x[22], (unsigned long long)c->x[30],
            c->native_pop ? " (guest tail call)" : "");
    if (hao_opt.bridge_abort) abort();
}

/* Call a guest function with integer arguments from C. */
uint64_t hao_call_guest(hao_thread *t, uint64_t fn, const uint64_t *args, int nargs)
{
    hao_cpu saved = t->cpu;
    hao_cpu *c = &t->cpu;
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
    for (int k = 0; k < nargs && k < 8; k++) c->x[k] = args[k];
    c->x[31] = (saved.x[31] - 256) & ~15ull;  /* below the suspended guest frame (red zone) */
    c->x[30] = HAO_RETURN_MAGIC;
    c->pc = fn;
    hao_run(t, &saved);
    uint64_t r = c->x[0];
    t->cpu = saved;   /* the result is returned, not left in the suspended frame's x0 */
    return r;
}

/* Native code called a guest function (redirected by the fault handler).
 * hao_callback returns the number of bytes hao_callback_entry pops beyond
 * the return address: 0 for ordinary callbacks, the x86_64 swifttailcc
 * callee pop for Swift async functions.
 *
 * Which callbacks are swifttailcc: a guest async function always ends by
 * tail-calling its continuation; when that is native, hao_native_call
 * measured that the callee popped (cpu native_pop != 0; a C function
 * tail-called by the guest pops nothing, and every x86_64 swifttailcc
 * callee pops at least 8).
 *
 * How much: x86_64 swifttailcc callees pop their own argument area. Native
 * Swift concurrency calls resume functions as `callq *56(%r13); subq $8,
 * %rsp`; a callee with more stack arguments is reached by a tail call that
 * first grows the argument area and moves the return address down, so the
 * pop is the guest function's own area, from its signature
 * (hao_swift_async_arg_area, swiftcc.c), whatever the call site states or
 * the guest tail-calls at the end.
 *
 * The code gives a lower bound: an argument it never reads (a generic
 * witness's trailing Self metadata and witness table, an unused parameter
 * at -O) is not seen, and stripped binaries carry no mangled names. The
 * caller's frame gives the exact number when its geometry is known, and
 * then it is the pop, whatever the signature says (a signature above it
 * pops into the caller's frame: WallpaperAgent's pool token);
 * the signature is used when there is no such frame, the frame's own
 * fallbacks when the code cannot be analysed either. The chain of
 * tail calls returns to the native caller with rsp = (rsp at its call
 * instruction) + P, P being what the call site's `subq $P, %rsp`
 * re-establishes, so the frame's pop is
 *     S0 + P - (entry rsp) - 8
 * with S0 = the caller's rsp at the call = caller rbp - its fixed frame size
 * (prologue `push %rbp; mov %rsp,%rbp; push...; sub $N,%rsp`; every native
 * swifttailcc tail call restores rbp before jumping, so rbp at our entry is
 * the caller's). Its own fallbacks: no recognisable frame -> P; no `subq`
 * at the call site -> the measured pop. A callback reached by a native
 * function that hao_native_call called (and that tail-called into the
 * guest) returns into hao_native_call, which absorbs any pop; it pops as
 * if that native function popped 8 (bridge rsp - entry rsp).
 * HAO_BRIDGE_CHECK=1 reports a caller-frame result that differs from the
 * signature, once per guest function, to the diagnostics log;
 * HAO_SWIFT_POP=signature or =frame uses one source only. */
void hao_callback_with_sig(hao_cb_frame *f, void *sig);
extern const uint64_t hao_native_call_ret;   /* bridge_x86_64.S: address after `callq *%r15` */

/* Bytes between rbp and rsp in the body of the native function containing
 * `ret`, from its prologue; -1 when not a plain rbp frame. Cached. */
static int64_t native_frame_size(uint64_t ret)
{
    enum { N = 256 };
    static _Atomic uint64_t cache[N];   /* ret << 16 | (size + 1); 0 = empty */
    uint64_t h = (ret ^ (ret >> 9)) % N;
    uint64_t e = cache[h];
    if (e && (e >> 16) == ret) return (int64_t)(e & 0xffff) - 1;
    int64_t sz = -1;
    Dl_info di;
    if (dladdr((void *)(uintptr_t)ret, &di) && di.dli_saddr && ret - (uint64_t)(uintptr_t)di.dli_saddr < (1u << 20)) {
        const uint8_t *p = di.dli_saddr;
        if (p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89 && p[3] == 0xe5) {   /* push %rbp; mov %rsp,%rbp */
            int64_t s = 0;
            p += 4;
            for (;;) {   /* push %rax..%rdi (not %rsp/%rbp), push %r8..%r15 */
                if (*p >= 0x50 && *p <= 0x57 && *p != 0x54 && *p != 0x55) { s += 8; p += 1; }
                else if (p[0] == 0x41 && p[1] >= 0x50 && p[1] <= 0x57) { s += 8; p += 2; }
                else break;
            }
            if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xec && p[3] < 0x80) { s += p[3]; p += 4; }
            else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xec) { uint32_t v; memcpy(&v, p + 3, 4); s += v; p += 7; }
            /* stack realignment (and $-N,%rsp) or a probed frame (mov $N,%eax;
             * call ___chkstk_darwin) is not a fixed frame */
            int realign = p[0] == 0x48 && (p[1] == 0x83 || p[1] == 0x81) && p[2] == 0xe4;
            int probe = p[0] == 0xb8;
            if (!realign && !probe && s < 0xfff0) sz = s;
        }
    }
    if (ret < (1ull << 47)) cache[h] = ret << 16 | (uint64_t)(sz + 1);
    return sz;
}

static int swifttailcc_pop_ok(int64_t pop) { return pop >= 8 && pop <= 4096 && (pop & 15) == 8; }

static uint64_t frame_pop(const hao_cb_frame *f, const hao_cpu *outer, int64_t measured, const char **why)
{
    uint64_t entry = (uint64_t)(uintptr_t)f->stack_args - 8;   /* rsp at our entry: return address */
    if (f->ret == hao_native_call_ret) {
        int64_t pop = (int64_t)(outer->bridge_rsp - entry);   /* S0 + 8 - entry - 8 */
        *why = "into hao_native_call";
        return swifttailcc_pop_ok(pop) ? (uint64_t)pop : 8;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)f->ret;
    int64_t site = -1;
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xec && p[3] < 0x80) site = p[3];   /* subq $imm8, %rsp */
    else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xec) { uint32_t v; memcpy(&v, p + 3, 4); site = v; }   /* subq $imm32 */
    if (!swifttailcc_pop_ok(site)) {
        *why = "measured";
        return swifttailcc_pop_ok(measured) ? (uint64_t)measured : 0;
    }
    int64_t fs = native_frame_size(f->ret);
    if (fs >= 0) {
        int64_t pop = (int64_t)(f->rbp - (uint64_t)fs) + site - (int64_t)entry - 8;
        if (swifttailcc_pop_ok(pop)) { *why = "caller frame"; return (uint64_t)pop; }
    }
    *why = "call site";
    return (uint64_t)site;
}

/* A disagreement between the caller's frame and the signature, reported
 * once per guest function (HAO_BRIDGE_CHECK=1; in a HAO_TEST_SWITCHES build
 * also boot argument hao_bridge_check=1) to the diagnostics log, where
 * hardware keeps it. */
static void pop_disagrees(const hao_cb_frame *f, int64_t sig, const char *swhy, uint64_t fr)
{
    enum { N = 512 };
    static uint64_t told[N];
    uint64_t h = ((f->target >> 2) * 0x9E3779B97F4A7C15ull) >> 55;
    for (unsigned k = 0; k < N; k++) {
        uint64_t *e = &told[(h + k) % N];
        uint64_t cur = __atomic_load_n(e, __ATOMIC_ACQUIRE);
        if (cur == f->target) return;
        if (cur) continue;
        uint64_t zero = 0;
        if (__atomic_compare_exchange_n(e, &zero, f->target, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
        if (zero == f->target) return;
    }
    Dl_info di;
    const char *sn = dladdr((void *)(uintptr_t)f->ret, &di) && di.dli_sname ? di.dli_sname : "?";
    hao_image *im = hao_image_for_code(f->target);
    const char *in = im ? (strrchr(im->path, '/') ? strrchr(im->path, '/') + 1 : im->path) : "?";
    char b[512];
    int n = snprintf(b, sizeof b, "hao[%s:%d]: swifttailcc pop of guest %#llx (%s+%#llx, file vmaddr %#llx): %s %lld, caller frame %llu (returning to %s); "
                     "popping the caller frame's\n", getprogname(), (int)getpid(), (unsigned long long)f->target, in,
                     (unsigned long long)(im ? f->target - im->base : 0), (unsigned long long)(im ? f->target - im->slide : 0), swhy, (long long)sig,
                     (unsigned long long)fr, sn);
    if (n > 0) hao_diag_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

/* noinline: its diagnostics' buffer would otherwise sit in callback_plain's frame for the whole guest call */
__attribute__((noinline)) static uint64_t callee_pop(const hao_cb_frame *f, const hao_cpu *outer, int64_t measured, const char **why)
{
    const char *m = hao_opt.swift_pop;
    int mode = !strcmp(m, "signature") ? 1 : !strcmp(m, "frame") ? 2 : 0;   /* 0: both */
    *why = "";
    if (!measured) return 0;                        /* not a swifttailcc function */
    const char *swhy = "", *fwhy = "";
    if (mode == 1) {
        int64_t sig = hao_swift_async_arg_area(f->target, &swhy);
        *why = sig >= 8 ? swhy : "signature unknown, default";
        return sig >= 8 ? (uint64_t)sig : 8;
    }
    uint64_t fr = frame_pop(f, outer, measured, &fwhy);
    *why = fwhy;
    if (mode == 2) return fr;
    /* the caller's frame, when its geometry is known, is exact: it is what
     * the native caller re-establishes after the call. The signature is
     * the guest code's reading of its own arguments, needed only when there
     * is no such frame. */
    bool exact = !strcmp(fwhy, "caller frame");
    bool check = hao_opt.bridge_check || hao_verbose > 1;
    if (exact && !check) return fr;
    int64_t sig = hao_swift_async_arg_area(f->target, &swhy);
    if (exact) {
        if (sig >= 8 && (uint64_t)sig != fr) pop_disagrees(f, sig, swhy, fr);
        return fr;
    }
    if (sig < 8) return fr;
    *why = swhy;
    return (uint64_t)sig;
}

/* Without a signature the number of stack arguments is unknown, so a
 * fixed window of the native caller's stack is copied: SysV stack slots
 * 0 and 1 become x6 and x7, slots 2.. the guest's stack arguments. That
 * covers 6 + 2 + CB_STACK_COPY = 22 integer-class arguments; a callee
 * with more needs a signature thunk (hao_callback_with_sig copies by its
 * signature). The window reaches 128 bytes past the caller's argument
 * area, into its own and its callers' frames (at a thread's first frame,
 * Darwin's pthread structure lies above the stack), so it is mapped. */
#define CB_STACK_COPY 14
#define CB_STACK_AREA (8 * 16)   /* guest stack reserved for them */
_Static_assert(8 * CB_STACK_COPY <= CB_STACK_AREA, "callback stack window");

static void *swift_arm_location(hao_cpu *c, const hao_swift_move *m)
{
    if (m->arm_kind == HAO_SR_GP) return &c->x[m->arm];
    if (m->arm_kind == HAO_SR_FP) return &c->v[m->arm];
    return (void *)(uintptr_t)(c->x[31] + m->arm);
}

static void *swift_native_location(hao_cb_frame *f, const hao_swift_move *m)
{
    if (m->native_kind == HAO_SR_GP) return &f->gpr[m->native];
    if (m->native_kind == HAO_SR_FP) return f->xmm[m->native];
    if (m->native_kind == HAO_SR_SPECIAL) return &f->swift[m->native];
    return (uint8_t *)f->stack_args + m->native;
}

__attribute__((noinline)) static void swift_result_prepare_args(hao_cpu *c, const hao_swift_reverse *s,
    const hao_swift_result_context *context, uint64_t target, hao_swift_value_scope *values)
{
    uint64_t arguments[128] = {0};
    for (unsigned k = 0; k < s->nargs; k++)
        if (s->moves[k].size <= 8) memcpy(arguments + k, swift_arm_location(c, &s->moves[k]), s->moves[k].size);
    hao_swift_result_prepare(context, arguments, target, values);
}

static void callback_swift_run(hao_cb_frame *f, const hao_swift_reverse *s, const hao_swift_result_context *context,
                               hao_thread *t, hao_cpu *saved)
{
    hao_cpu *c = &t->cpu;
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
    c->x[31] = (saved->x[31] - 256 - s->stack_bytes) & ~15ull;
    memset((void *)(uintptr_t)c->x[31], 0, s->stack_bytes);
    /* Clear high bits of narrow register values; only declared bytes are read
     * from the native frame. No speculative stack window or C sret placement. */
    for (int k = 0; k < 9; k++) c->x[k] = 0;
    for (int k = 0; k < 8; k++) memset(&c->v[k], 0, 16);
    c->x[20] = c->x[21] = c->x[22] = 0;
    for (unsigned k = 0; k < s->nargs; k++) {
        const hao_swift_move *m = &s->moves[k];
        memcpy(swift_arm_location(c, m), swift_native_location(f, m), m->size);
    }
    hao_swift_value_scope values = {0};
    if (context) swift_result_prepare_args(c, s, context, f->target, &values);
    uint64_t indirect = c->x[8];
    unsigned made = t->swift_made_n;
    c->x[30] = HAO_RETURN_MAGIC;
    c->pc = f->target;
    c->native_pop = 0;
    t->callbacks++;
    hao_run(t, saved);
    hao_swift_objects_result(t, f->target, indirect);
    hao_swift_witness_result(t, made, indirect);
    hao_swift_value_end(t, &values);
    for (unsigned k = s->nargs; k < s->nargs + s->nrets; k++) {
        const hao_swift_move *m = &s->moves[k];
        memcpy(swift_native_location(f, m), swift_arm_location(c, m), m->size);
    }
    t->cpu = *saved;
}

__attribute__((noinline)) static void callback_swift_local(hao_cb_frame *f, const hao_swift_reverse *s,
    const hao_swift_result_context *context, hao_thread *t)
{
    hao_cpu saved = t->cpu;
    callback_swift_run(f, s, context, t, &saved);
}

static void callback_swift(hao_cb_frame *f, const hao_swift_reverse *s, const hao_swift_result_context *context)
{
    hao_thread *t = hao_current();
    unsigned mark;
    hao_cpu *saved = cpu_save_push(t, &mark);
    if (!saved) { callback_swift_local(f, s, context, t); return; }
    *saved = t->cpu;
    callback_swift_run(f, s, context, t, saved);
    t->cpu_saves_n = mark;
}

static uint64_t callback_plain(hao_cb_frame *f, hao_thread *t, hao_cpu *saved);
static uint64_t callback_plain_local(hao_cb_frame *f, hao_thread *t);

uint64_t hao_callback(hao_cb_frame *f)
{
    /* f->sig on entry: a signature thunk's hsig* or tagged Swift recipe,
     * HAO_CB_FAULT from NX, or 0; on return 1 asks the entry to load st0, so it is
     * cleared here (hao_callback_with_sig sets it for long double). */
    uint64_t in = f->sig;
    f->sig = 0;
    if ((in & 7) == HAO_CB_SWIFT_RESULT_TAG) {
        const hao_swift_result_context *context = (const void *)(uintptr_t)(in & ~7ull);
        callback_swift(f, hao_swift_result_reverse(context), context);
        return 0;
    }
    if ((in & 7) == HAO_CB_SWIFT_TAG) {
        callback_swift(f, (const hao_swift_reverse *)(uintptr_t)(in & ~7ull), NULL);
        return 0;
    }
    /* arrived by an NX fault: a block whose helper this is gets thunks for next time */
    if (in == HAO_CB_FAULT) hao_blocks_heal(f->gpr[0], f->target);
    /* a call without a signature thunk (NX fault, plain thunk) may still
     * have a signature noted for its target (abi.c) */
    void *sig = in > HAO_CB_FAULT ? (void *)(uintptr_t)in : hao_guest_sig(f->target);
    if (sig) { hao_callback_with_sig(f, sig); return 0; }
    hao_thread *t = hao_current();
    unsigned mark;
    hao_cpu *saved = cpu_save_push(t, &mark);
    if (!saved) return callback_plain_local(f, t);
    *saved = t->cpu;
    uint64_t pop = callback_plain(f, t, saved);
    t->cpu_saves_n = mark;
    return pop;
}

/* HAO_VERBOSE > 1 lines, out of line so that their locals are not on the native stack across the guest call */
__attribute__((noinline, cold)) static void callback_trace_entry(const hao_cb_frame *f, const hao_thread *t)
{
    Dl_info di;
    const char *sn = dladdr((void *)(uintptr_t)f->ret, &di) && di.dli_sname ? di.dli_sname : "?";
    fprintf(stderr, "hao: callback into guest %#llx from %s+%#llx (ret %#llx rsp %#llx rbp %#llx, thread %p)\n", (unsigned long long)f->target, sn,
            (unsigned long long)(f->ret - (uint64_t)(uintptr_t)di.dli_saddr), (unsigned long long)f->ret,
            (unsigned long long)(uintptr_t)f->stack_args - 8, (unsigned long long)f->rbp, (const void *)t);
}
__attribute__((noinline, cold)) static void callback_trace_pop(const hao_cb_frame *f, uint64_t pop, const char *why, int64_t native_pop)
{
    Dl_info di;
    const char *sn = dladdr((void *)(uintptr_t)f->ret, &di) && di.dli_sname ? di.dli_sname : "?";
    fprintf(stderr, "hao: callback %#llx returns to %s+%#llx popping %llu (%s; native tail call popped %lld)\n",
            (unsigned long long)f->target, sn, (unsigned long long)(f->ret - (uint64_t)(uintptr_t)di.dli_saddr), (unsigned long long)pop, why,
            (long long)native_pop);
}

/* A native-to-guest call without a signature (NX fault, plain thunk). */
static uint64_t callback_plain(hao_cb_frame *f, hao_thread *t, hao_cpu *saved)
{
    hao_cpu *c = &t->cpu;
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
    t->callbacks++;
    if (hao_verbose > 1) callback_trace_entry(f, t);
    for (int k = 0; k < 6; k++) c->x[k] = f->gpr[k];
    /* SysV stack args 0,1 -> x6,x7; the rest are copied to the guest stack */
    c->x[6] = f->stack_args[0];
    c->x[7] = f->stack_args[1];
    for (int k = 0; k < 8; k++) memcpy(&c->v[k], f->xmm[k], 16);
    c->x[21] = f->swift[0];
    c->x[20] = f->swift[1];
    c->x[22] = f->swift[2];
    c->x[0] = hao_swift_async_extend_result(f->target, c->x[0]);
    /* Guest stack: the SysV stack arguments from slot 2 at sp, above them a
     * scratch area for an indirect result x8 must not point at (below). */
    enum { SCRATCH = 1024 };
    uint64_t top = (saved->x[31] - 256) & ~15ull;
    uint64_t sp = (top - SCRATCH - CB_STACK_AREA) & ~15ull;
    memcpy((void *)(uintptr_t)sp, &f->stack_args[2], 8 * CB_STACK_COPY);
    /* x8 is the indirect-result register: native swiftcc passes it in rax.
     * A C caller leaves anything there, often the function pointer it just
     * called (the thunk) or al = the vector-register count of a variadic
     * call; a guest function returning a large struct would then write its
     * result into hao's code. So only a value that can be a data pointer is
     * passed through; otherwise x8 points at the scratch area. */
    uint64_t r = f->swift[3];
    bool data = r >= 0x10000 && r < 0x800000000000ull && r != f->target && !hao_in_code_cache(r) && !hao_swiftmeta_is_island(r) && !hao_image_for_code(r) &&
                !hao_is_trap(r);
    c->x[8] = data ? r : sp + CB_STACK_AREA;
    c->x[31] = sp;
    c->x[30] = HAO_RETURN_MAGIC;
    c->pc = f->target;
    c->native_pop = 0;       /* per activation; nested callbacks restore it */
    unsigned objc_enum_result = hao_objc_enum_args(c, f->target, true);
    unsigned made = t->swift_made_n;
    hao_run(t, saved);
    hao_objc_enum_result(c, objc_enum_result);
    hao_swift_objects_result(t, f->target, data ? r : 0);
    hao_swift_witness_result(t, made, data ? r : 0);
    for (int k = 0; k < 4; k++) { f->gpr[k] = c->x[k]; memcpy(f->xmm[k], &c->v[k], 16); }
    f->swift[0] = c->x[21];
    const char *why;
    uint64_t pop = callee_pop(f, saved, (int64_t)c->native_pop, &why);
    if (pop && hao_verbose > 1) callback_trace_pop(f, pop, why, c->native_pop);
    t->cpu = *saved;
    return pop;
}

__attribute__((noinline)) static uint64_t callback_plain_local(hao_cb_frame *f, hao_thread *t)
{
    hao_cpu saved = t->cpu;
    return callback_plain(f, t, &saved);
}

/* ---------------------------------------------------- fault handler */

/* ------------------------------------------------- diagnostics channel
 *
 * hao's diagnostics go to the console and, when one is configured, to a log
 * file, each line (or report, hao_diag_block) with one write(2):
 *   console  stderr; with boot argument hao_crash=1, /dev/console on a high
 *            close-on-exec descriptor (PID 1's stderr is the console);
 *   log      with hao_crash=1, /private/var/log/hao/console.log; HAO_DIAG_DIR
 *            (an absolute directory, ordinary processes) names another.
 * On hardware nothing records /dev/console; the file is read afterwards over
 * ssh or from the Data volume. Opening for O_APPEND makes every write(2)
 * land whole at the file's end, so the lines of concurrent processes do not
 * tear. The file holds one boot: its first line names the boot session, and
 * a process that finds another boot's file renames it to console.log.0
 * (under an flock on DIR/.lock, a 0600 file only the log's writer can open)
 * and starts the new one. Root processes create the directory (0755), the
 * file (0600: it holds register dumps and image bases) and DIR/user (1777,
 * sticky, as /tmp). A process that may not write DIR/console.log (every non-root
 * one: GUI-session processes such as WindowManager, uid 248, whose
 * /dev/console is refused too) keeps its own log in DIR/user/<euid>/
 * console.log, with the same boot header, rotation and cap: it creates
 * DIR/user/<euid> (0700) and uses it only when it owns it and nobody else
 * can write it, and DIR/user only when it is sticky and DIR's owner's.
 * PID 1 starts before launchd's boot tasks mount the Data volume: an open
 * that finds no directory yet (or the System volume's own /private/var) is
 * retried at the next write. Writes stop at hao_diag_log_cap bytes (64 MB a
 * boot; console.log.0 keeps the boot before), after one line that says so.
 * The write path makes system calls on descriptors opened here and formats
 * into stack buffers, nothing else (crash diagnostics come from signal
 * handlers); a log descriptor the program closed and reused is dropped.
 *
 * The unified-log fallback. An App-Sandboxed process may write neither
 * /dev/console nor any console.log (libSystem's initializer applies the
 * sandbox before hao starts), and its stderr goes nowhere. When a log is
 * configured, every write the file does not take (no file could be opened,
 * the write failed, the program closed the descriptor) goes to the unified
 * log instead, subsystem "com.hao", category "diag": one public record per
 * line, a line longer than UNI_PIECE bytes cut into pieces, each but the last
 * ending in "\x1f" (log show --predicate 'subsystem == "com.hao"';
 * the bridge worklist tool, not part of this repository, reads them). Lines the file took are not repeated
 * there. Not while PID 1 waits for the Data volume (diag_retry), and not past
 * the file's cap. os_log is not async-signal-safe (its first use allocates; a
 * full buffer or a preference refresh takes libtrace's locks), so no writer
 * calls it: the writer copies its bytes into a static ring (no allocation; a
 * nested write on the same thread is dropped) and a drain thread turns them
 * into records. The drain starts at hao_diag_init when no file could be
 * opened, else at the first fallback write from a context that may create a
 * thread (stdio, hao_diag_block_end). The writer then waits for the drain to
 * take its bytes, at most 2 s (a crash report is followed by the process's
 * death); after one timeout writers stop waiting until the drain makes
 * progress again. A fork child drops the parent's undrained bytes and starts
 * its own drain at its first stdio or report write; its signal-handler
 * writes before that stay in the ring. */
FILE *hao_diag_file;
size_t hao_diag_log_cap = (size_t)64 << 20;
static int diag_con = 2;          /* the console */
static int diag_log = -1;         /* the log, -1: none (yet) */
static int diag_retry;            /* 1: open the log at the next write */
static dev_t diag_log_dev;        /* the log's identity: a program that closes descriptors it did not open may */
static ino_t diag_log_ino;        /* reuse the number for a file of its own, which hao must not write to */
static char diag_dir[1024];
static char diag_buf[16384];      /* hao_diag_file's line buffer */
static bool diag_stamp;           /* HAO_DIAG_TIME: each line starts with "@<seconds of the mach clock> " */
static mach_timebase_info_data_t diag_tb;

/* Nanoseconds of the mach clock, the clock of the kernel's and the unified
 * log's timestamps (mach_absolute_time and the timebase are async-signal-safe). */
static uint64_t diag_now_ns(void)
{
    uint64_t t = mach_absolute_time();
    if (!diag_tb.denom) mach_timebase_info(&diag_tb);
    return diag_tb.numer == diag_tb.denom ? t : (uint64_t)((__uint128_t)t * diag_tb.numer / diag_tb.denom);
}

/* "@S.UUUUUU " into out (at least 32 bytes) without stdio: also from signal handlers. */
static size_t diag_stamp_fmt(char *out, uint64_t ns)
{
    char d[24];
    int k = 0;
    uint64_t s = ns / 1000000000u, us = ns % 1000000000u / 1000u;
    do { d[k++] = (char)('0' + s % 10); s /= 10; } while (s && k < 20);
    size_t n = 0;
    out[n++] = '@';
    while (k) out[n++] = d[--k];
    out[n++] = '.';
    for (uint64_t p = 100000; p; p /= 10) out[n++] = (char)('0' + us / p % 10);
    out[n++] = ' ';
    return n;
}

/* Lines written while PID 1 (or another root process started by a boot task)
 * waits for the Data volume, which the file lives on: the console has no
 * recorder on hardware, so they are kept here and written to the file first
 * when it opens (diag_log_fd). Reservations only grow: the first one that
 * does not fit ends the kept bytes (pend_full), later ones are counted lost. */
#define DIAG_PEND (256u << 10)
static char diag_pend[DIAG_PEND];
static uint64_t pend_res, pend_done, pend_full = DIAG_PEND, pend_lost;
static int pend_closed;

static void pend_put(const char *b, size_t n)
{
    if (__atomic_load_n(&pend_closed, __ATOMIC_ACQUIRE)) { __atomic_fetch_add(&pend_lost, n, __ATOMIC_RELAXED); return; }
    uint64_t at = __atomic_fetch_add(&pend_res, n, __ATOMIC_ACQ_REL);
    if (at + n <= DIAG_PEND) memcpy(diag_pend + at, b, n);
    else {
        uint64_t f = __atomic_load_n(&pend_full, __ATOMIC_RELAXED);
        while (at < f && !__atomic_compare_exchange_n(&pend_full, &f, at, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
        __atomic_fetch_add(&pend_lost, n, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&pend_done, n, __ATOMIC_RELEASE);
}

void hao_diag_prefix(FILE *f) { fprintf(f, "hao[%s:%d]: ", getprogname(), (int)getpid()); }

/* HAO_LOG's line: prefix and message formatted together and written with one
 * fputs, so that it reaches an unbuffered stderr, the console or the log in
 * one piece; a longer line is written as prefix, then message. */
void hao_log_line(const char *fmt, ...)
{
    FILE *f = HAO_DIAG;
    char buf[1024];
    int p = snprintf(buf, sizeof buf, "hao[%s:%d]: ", getprogname(), (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    int n = p > 0 && (size_t)p < sizeof buf ? vsnprintf(buf + p, sizeof buf - (size_t)p, fmt, ap) : -1;
    va_end(ap);
    if (n >= 0 && (size_t)p + (size_t)n < sizeof buf) { fputs(buf, f); return; }
    hao_diag_prefix(f);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
}

/* Startup phases under HAO_DIAG_TIME (boot argument hao_diag_time=1): one
 * line each, "phase NAME at S s (+MS ms)" on the mach clock, so a process's
 * start can be laid against the kernel's log. hao_phase_mark records a time
 * before the diagnostics channel exists (the front end); hao_phase_begin
 * prints the exec (the kernel's start of this process, from its start time)
 * and the marks. */
static uint64_t phase_last, phase_front;

void hao_phase_mark(void) { if (!phase_front) phase_front = diag_now_ns(); }

/* After each phase from "diag" on, "usage NAME cpu_user_ms=U cpu_sys_ms=S
 * pageins=P read_kb=R": this process's CPU time (getrusage), page-ins and
 * bytes read from disk (proc_pid_rusage) since the previous usage line, and
 * the first since the exec. The wall time a phase took less its CPU time is
 * time spent waiting: for a page-in, a lock or the scheduler. (CPU time is
 * taken from getrusage, in microseconds: proc_pid_rusage's is in the kernel's
 * ticks, which under Rosetta are not the timebase this process is given.) */
static struct rusage_info_v4 phase_ru;
static uint64_t phase_cpu_u, phase_cpu_s;   /* ns */

static void phase_usage(const char *what)
{
    struct rusage_info_v4 ru;
    struct rusage r;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ru) != 0 || getrusage(RUSAGE_SELF, &r) != 0) return;
    uint64_t cu = ((uint64_t)r.ru_utime.tv_sec * 1000000u + (uint64_t)r.ru_utime.tv_usec) * 1000u,
             cs = ((uint64_t)r.ru_stime.tv_sec * 1000000u + (uint64_t)r.ru_stime.tv_usec) * 1000u;
    uint64_t u = cu - phase_cpu_u, k = cs - phase_cpu_s;
    phase_cpu_u = cu;
    phase_cpu_s = cs;
    hao_log_line("usage %s cpu_user_ms=%llu.%03llu cpu_sys_ms=%llu.%03llu pageins=%llu read_kb=%llu\n", what,
                 (unsigned long long)(u / 1000000u), (unsigned long long)(u % 1000000u / 1000u),
                 (unsigned long long)(k / 1000000u), (unsigned long long)(k % 1000000u / 1000u),
                 (unsigned long long)(ru.ri_pageins - phase_ru.ri_pageins),
                 (unsigned long long)((ru.ri_diskio_bytesread - phase_ru.ri_diskio_bytesread) / 1024u));
    phase_ru = ru;
}

static void phase_line(const char *what, uint64_t ns)
{
    if (phase_last && ns >= phase_last)
        hao_log_line("phase %s at %llu.%06llu s (+%llu.%03llu ms)\n", what, (unsigned long long)(ns / 1000000000u),
                     (unsigned long long)(ns % 1000000000u / 1000u), (unsigned long long)((ns - phase_last) / 1000000u),
                     (unsigned long long)((ns - phase_last) % 1000000u / 1000u));
    else
        hao_log_line("phase %s at %llu.%06llu s\n", what, (unsigned long long)(ns / 1000000000u),
                     (unsigned long long)(ns % 1000000000u / 1000u));
    phase_last = ns;
}

void hao_phase(const char *what)
{
    if (!hao_opt.diag_time) return;
    phase_line(what, diag_now_ns());
    phase_usage(what);
}

void hao_phase_begin(void)
{
    if (!hao_opt.diag_time) return;
    uint64_t now = diag_now_ns();
    struct proc_bsdinfo bi;
    struct timeval tv;
    if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi && gettimeofday(&tv, NULL) == 0) {
        int64_t ago = ((int64_t)tv.tv_sec - (int64_t)bi.pbi_start_tvsec) * 1000000000 +
                      ((int64_t)tv.tv_usec - (int64_t)bi.pbi_start_tvusec) * 1000;
        if (ago >= 0 && (uint64_t)ago <= now) phase_line("exec", now - (uint64_t)ago);
    }
    if (phase_front) phase_line("front", phase_front);
    phase_line("diag", now);
    phase_usage("diag");
}

void hao_oom(const char *what)
{
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG, "%s: out of memory\n", what);
    abort();
}

const char *hao_diag_guest_where(uint64_t a, char *buf, size_t n)
{
    hao_image *im = hao_image_for_code(a);
    if (!n) return buf;
    buf[0] = 0;
    if (im) snprintf(buf, n, " (%s+%#llx, file vmaddr %#llx)", strrchr(im->path, '/') ? strrchr(im->path, '/') + 1 : im->path,
                     (unsigned long long)(a - im->base), (unsigned long long)(a - im->slide));
    return buf;
}

void hao_diag_guest_addr(FILE *f, uint64_t a)
{
    char where[1100];
    fputs(hao_diag_guest_where(a, where, sizeof where), f);
}

static bool diag_write_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

/* ------------------------------ the unified-log fallback (see above) */
#define UNI_RING (256u << 10)
#define UNI_PIECE 1000            /* os_log keeps 1018 bytes of a string */
#define UNI_WAIT 20000            /* 100 us steps: 2 s */
static char uni_ring[UNI_RING];
static uint64_t uni_head;         /* bytes published to the drain (only the owner advances it) */
static uint64_t uni_tail;         /* bytes the drain has logged */
static pthread_t uni_owner;       /* the writer copying into the ring */
static pthread_t uni_thr;         /* the drain thread; NULL: none (yet, or in a fork child) */
static int uni_starting;
static int uni_wedged;            /* a writer timed out waiting for the drain */
static semaphore_t uni_sem;       /* 0: none (a fork child has none of its parent's) */
static os_log_t uni_log;
static uint64_t uni_dropped;      /* bytes not logged: nested, lock timeout, ring full with no drain */

static void uni_nap(void) { nanosleep(&(struct timespec){0, 100000}, NULL); }

static void *uni_drain(void *arg)
{
    (void)arg;
    char l[UNI_PIECE + 1];
    for (;;) {
        semaphore_wait(uni_sem);
        uint64_t t = __atomic_load_n(&uni_tail, __ATOMIC_ACQUIRE), h = __atomic_load_n(&uni_head, __ATOMIC_ACQUIRE);
        while (t < h) {
            size_t n = 0;
            bool end = false;
            while (t < h && n < UNI_PIECE) {
                char c = uni_ring[t++ % UNI_RING];
                if (c == '\n') { end = true; break; }
                l[n++] = c ? c : ' ';
            }
            if (!end && t < h && uni_ring[t % UNI_RING] == '\n') { end = true; t++; }   /* UNI_PIECE bytes, then the newline */
            l[n] = 0;
            if (end || t >= h) {
                if (n) os_log_with_type(uni_log, OS_LOG_TYPE_DEFAULT, "%{public}s", l);
            } else
                os_log_with_type(uni_log, OS_LOG_TYPE_DEFAULT, "%{public}s\x1f", l);
            __atomic_store_n(&uni_tail, t, __ATOMIC_RELEASE);
            __atomic_store_n(&uni_wedged, 0, __ATOMIC_RELAXED);
            if (t >= h) h = __atomic_load_n(&uni_head, __ATOMIC_ACQUIRE);
        }
    }
    return NULL;
}

/* Not from a signal handler: allocates (os_log_create, the thread). */
static void uni_start(void)
{
    int no = 0;
    if (__atomic_load_n(&uni_thr, __ATOMIC_ACQUIRE) ||
        !__atomic_compare_exchange_n(&uni_starting, &no, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    if (!uni_log) uni_log = os_log_create("com.hao", "diag");
    (void)os_log_type_enabled(uni_log, OS_LOG_TYPE_DEFAULT);   /* its preferences, read now */
    semaphore_t s = uni_sem;
    if (!s && semaphore_create(mach_task_self(), &s, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) s = 0;
    uni_sem = s;
    pthread_t th = NULL;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 << 10);
    if (s && uni_log && pthread_create(&th, &a, uni_drain, NULL) != 0) th = NULL;
    pthread_attr_destroy(&a);
    if (th) {
        __atomic_store_n(&uni_thr, th, __ATOMIC_RELEASE);
        semaphore_signal(uni_sem);   /* bytes a signal handler left in the ring */
    }
    __atomic_store_n(&uni_starting, 0, __ATOMIC_RELEASE);
}

static void uni_fork_child(void)
{
    uni_thr = NULL;
    uni_owner = NULL;
    uni_sem = 0;
    uni_log = NULL;   /* the parent's faults in the child (os_log_type_enabled: KERN_PROTECTION_FAILURE) */
    uni_starting = 0;
    uni_wedged = 0;
    uni_tail = uni_head;   /* the parent logs its own */
}

/* Async-signal-safe without may_start: memcpy, nanosleep and semaphore_signal. */
static void uni_put(const char *b, size_t n, bool may_start)
{
    if (may_start && !__atomic_load_n(&uni_thr, __ATOMIC_ACQUIRE)) uni_start();
    pthread_t self = pthread_self(), none = NULL, drain = __atomic_load_n(&uni_thr, __ATOMIC_ACQUIRE);
    for (int k = 0; !__atomic_compare_exchange_n(&uni_owner, &none, self, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); k++) {
        if (pthread_equal(none, self) || k >= UNI_WAIT) {   /* a handler on top of this thread's own copy; 2 s */
            __atomic_fetch_add(&uni_dropped, n, __ATOMIC_RELAXED);
            return;
        }
        none = NULL;
        uni_nap();
    }
    semaphore_t sem = uni_sem;
    uint64_t h = __atomic_load_n(&uni_head, __ATOMIC_RELAXED), pub = h;
    size_t i = 0;
    for (int waits = 0; i < n;) {
        uint64_t room = UNI_RING - (h - __atomic_load_n(&uni_tail, __ATOMIC_ACQUIRE));
        if (!room) {
            if (pub < h) __atomic_store_n(&uni_head, pub = h, __ATOMIC_RELEASE);   /* a line as long as the ring */
            if (!drain || !sem || ++waits > UNI_WAIT || __atomic_load_n(&uni_wedged, __ATOMIC_RELAXED)) break;
            semaphore_signal(sem);
            uni_nap();
            continue;
        }
        size_t k = n - i < room ? n - i : (size_t)room;
        for (size_t j = 0; j < k;) {   /* up to the ring's end, then from its start */
            size_t at = (size_t)((h + j) % UNI_RING), c = UNI_RING - at < k - j ? UNI_RING - at : k - j;
            memcpy(uni_ring + at, b + i + j, c);
            j += c;
        }
        const char *nl = NULL;
        for (size_t j = k; j-- > 0 && !nl;) if (b[i + j] == '\n') nl = b + i + j;
        h += k;
        i += k;
        if (nl) __atomic_store_n(&uni_head, pub = h - (uint64_t)(b + i - nl - 1), __ATOMIC_RELEASE);   /* whole lines */
    }
    if (i < n) __atomic_fetch_add(&uni_dropped, n - i, __ATOMIC_RELAXED);
    __atomic_store_n(&uni_head, h, __ATOMIC_RELEASE);
    __atomic_store_n(&uni_owner, NULL, __ATOMIC_RELEASE);
    if (!drain || !sem) return;
    semaphore_signal(sem);
    if (pthread_equal(self, drain)) return;   /* a fault inside os_log: nobody else drains */
    /* logged before the writer goes on (a crash report: before the process dies) */
    for (int k = 0; __atomic_load_n(&uni_tail, __ATOMIC_ACQUIRE) < h; k++) {
        if (__atomic_load_n(&uni_wedged, __ATOMIC_RELAXED)) return;
        if (k >= UNI_WAIT) { __atomic_store_n(&uni_wedged, 1, __ATOMIC_RELAXED); return; }
        uni_nap();
    }
}

static int diag_high_fd(int fd)
{
    int hi = fcntl(fd, F_DUPFD_CLOEXEC, 201);
    if (hi < 0) return fd;
    close(fd);
    return hi;
}

/* "hao: console.log of boot <kern.bootsessionuuid>\n" (kern.boottime when there is no session UUID) */
static int diag_boot_header(char *out, size_t n)
{
    char id[64];
    size_t l = sizeof id;
    if (sysctlbyname("kern.bootsessionuuid", id, &l, NULL, 0) != 0 || l < 2) {
        struct timeval tv = {0};
        size_t tl = sizeof tv;
        int mib[2] = {CTL_KERN, KERN_BOOTTIME};
        sysctl(mib, 2, &tv, &tl, NULL, 0);
        snprintf(id, sizeof id, "time %ld.%06d", (long)tv.tv_sec, (int)tv.tv_usec);
    }
    id[sizeof id - 1] = 0;
    int r = snprintf(out, n, "hao: console.log of boot %s\n", id);
    return r < 0 || (size_t)r >= n ? 0 : r;
}

/* The directory lies on the root volume while /System/Volumes/Data is an
 * unmounted directory of that volume: early boot, before launchd's
 * mount-phase-1 task mounts the Data volume that /private/var resolves to
 * (st_dev cannot tell them apart: a volume group's System and Data volumes
 * report one device). A single-volume system has no such directory. */
static bool diag_before_data_volume(int dfd)
{
    struct statfs d, m;
    return fstatfs(dfd, &d) == 0 && !strcmp(d.f_mntonname, "/") && statfs("/System/Volumes/Data", &m) == 0 &&
           !strcmp(m.f_mntonname, "/");
}

/* DIR/.lock open, or -1 with errno: the file whose flock serialises the
 * check-rotate-create below. Only the log's writer can open it (0600, and
 * it must be this process's own), so no other user can hold the lock: a
 * lock on DIR itself (0755) could be taken by anyone, which delayed every
 * process's start and lost its log. A process that may not write DIR gets
 * EACCES here, as it did from console.log. */
static int diag_lock_open(int dfd)
{
    int fd = -1;
    for (int k = 0; k < 2 && fd < 0; k++) {
        fd = openat(dfd, ".lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT) fd = openat(dfd, ".lock", O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) return -1;
    }
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))) {
        close(fd);
        fd = -1;
        errno = EACCES;
    }
    return fd;
}

/* DIR/console.log open for appending, this boot's; -1 with errno. mk: create the directory if absent. */
static int diag_log_open(const char *dir, bool mk)
{
    struct stat ds;
    if (mk && stat(dir, &ds) != 0 && errno == ENOENT) mkdir(dir, 0755);   /* no write check (a sandbox report) when present */
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    if (diag_before_data_volume(dfd)) { close(dfd); errno = ENOENT; return -1; }
    char hdr[160], got[160];
    int hl = diag_boot_header(hdr, sizeof hdr);
    int lfd = diag_lock_open(dfd);
    if (lfd < 0) { int le = errno; close(dfd); errno = le; return -1; }
    /* the lock is held for a few system calls; a holder that is stopped (one of this user's own processes) must
     * not stop every process's startup */
    bool locked = false;
    for (int k = 0; k < 200 && !(locked = flock(lfd, LOCK_EX | LOCK_NB) == 0); k++) nanosleep(&(struct timespec){0, 1000000}, NULL);
    if (!locked) { close(lfd); close(dfd); errno = EAGAIN; return -1; }
    int fd = openat(dfd, "console.log", O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC), e = errno;
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))) { close(fd); fd = -1; e = EINVAL; }
    /* register dumps and image bases: the owner's alone (a file an earlier hao made 0644 too, before it rotates) */
    if (fd >= 0 && st.st_uid == geteuid() && (st.st_mode & 077)) fchmod(fd, 0600);
    if (fd >= 0 && (pread(fd, got, (size_t)hl, 0) != hl || memcmp(got, hdr, (size_t)hl))) {
        /* another boot's file */
        close(fd);
        fd = -1;
        e = renameat(dfd, "console.log", dfd, "console.log.0") == 0 ? ENOENT : errno;
    }
    if (fd < 0 && e == ENOENT) {
        fd = openat(dfd, "console.log", O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        e = errno;
        if (fd >= 0) { fchmod(fd, 0600); diag_write_all(fd, hdr, (size_t)hl); }
    }
    if (fd >= 0 && fstat(fd, &st) == 0) { diag_log_dev = st.st_dev; diag_log_ino = st.st_ino; }
    if (fd >= 0 && mk && mkdirat(dfd, "user", 0700) == 0) {
        /* the per-user logs' parent; set after mkdir, past the umask */
        int ufd = openat(dfd, "user", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (ufd >= 0) { fchmod(ufd, 01777); close(ufd); }
    }
    close(lfd);   /* releases the lock */
    close(dfd);
    errno = e;
    return fd;
}

/* The kept lines, once, to the file just opened: writers that still see the
 * retry state finish their copies first (at most 2 s). */
static void pend_flush(int fd)
{
    int no = 0;
    if (!__atomic_compare_exchange_n(&pend_closed, &no, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    for (int k = 0; k < 20000 && __atomic_load_n(&pend_done, __ATOMIC_ACQUIRE) != __atomic_load_n(&pend_res, __ATOMIC_ACQUIRE); k++)
        nanosleep(&(struct timespec){0, 100000}, NULL);
    uint64_t r = __atomic_load_n(&pend_res, __ATOMIC_ACQUIRE), f = __atomic_load_n(&pend_full, __ATOMIC_ACQUIRE);
    uint64_t n = r < f ? r : f, lost = __atomic_load_n(&pend_lost, __ATOMIC_ACQUIRE);
    if (!n && !lost) return;
    char m[240];
    int k = snprintf(m, sizeof m, "hao[%s:%d]: %llu bytes of lines kept from before the Data volume was mounted follow%s",
                     getprogname(), (int)getpid(), (unsigned long long)n, lost ? "" : "\n");
    if (lost && k > 0 && (size_t)k < sizeof m)
        k += snprintf(m + k, sizeof m - (size_t)k, " (%llu bytes did not fit and are lost)\n", (unsigned long long)lost);
    if (k > 0) diag_write_all(fd, m, (size_t)k < sizeof m ? (size_t)k : sizeof m - 1);
    if (n) diag_write_all(fd, diag_pend, (size_t)n);
}

static int diag_log_fd(void)
{
    int fd = __atomic_load_n(&diag_log, __ATOMIC_ACQUIRE);
    if (fd >= 0 || !__atomic_load_n(&diag_retry, __ATOMIC_ACQUIRE)) return fd;
    fd = diag_log_open(diag_dir, true);
    if (fd < 0) {
        if (errno != ENOENT && errno != EROFS) __atomic_store_n(&diag_retry, 0, __ATOMIC_RELEASE);
        return -1;
    }
    fd = diag_high_fd(fd);
    int none = -1;
    if (!__atomic_compare_exchange_n(&diag_log, &none, fd, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        close(fd);   /* another thread opened it first */
        fd = none;
    } else
        pend_flush(fd);
    __atomic_store_n(&diag_retry, 0, __ATOMIC_RELEASE);
    return fd;
}

/* The console, then the log (one write(2) for the whole buffer), else the
 * unified log. may_start: the caller may create the drain thread (it is not
 * a signal handler). While the log waits for the Data volume the bytes are
 * kept (pend_put); with HAO_DIAG_TIME a process other than PID 1 also sends
 * them to the unified log, which keeps a boot task's lines (it exits before
 * the volume mounts) with the kernel's timestamps. */
static void diag_emit(const char *b, size_t n, bool may_start)
{
    if (!n) return;
    int e = errno;
    diag_write_all(diag_con, b, n);
    int fd = diag_log_fd();
    if (fd < 0 && __atomic_load_n(&diag_retry, __ATOMIC_ACQUIRE)) {
        pend_put(b, n);
        if (diag_stamp && getpid() != 1) uni_put(b, n, may_start);
        errno = e;
        return;
    }
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) != 0 || st.st_dev != diag_log_dev || st.st_ino != diag_log_ino)) {
        __atomic_store_n(&diag_log, -1, __ATOMIC_RELEASE);   /* closed by the program: no file from now on */
        fd = -1;
    }
    if (fd < 0) {
        if (diag_dir[0] && !__atomic_load_n(&diag_retry, __ATOMIC_ACQUIRE)) uni_put(b, n, may_start);
    } else if ((uint64_t)st.st_size < hao_diag_log_cap) {   /* past the cap: dropped on purpose */
        if (!diag_write_all(fd, b, n)) {
            uni_put(b, n, may_start);   /* a sandbox's write check, a full volume, an I/O error */
            errno = e;
            return;
        }
        if ((uint64_t)st.st_size + n >= hao_diag_log_cap) {
            char m[200];
            int k = snprintf(m, sizeof m, "hao[%s:%d]: console.log reached its cap of %zu KB: later lines go to the console only\n",
                             getprogname(), (int)getpid(), hao_diag_log_cap >> 10);
            if (k > 0) diag_write_all(fd, m, (size_t)k < sizeof m ? (size_t)k : sizeof m - 1);
        }
    }
    errno = e;
}

/* diag_emit, each line starting with its time stamp under HAO_DIAG_TIME. A
 * write is taken to start a line (stdio's line buffer and the report blocks
 * write whole lines); a long one goes out in pieces that end at a newline. */
static void diag_write(const char *b, size_t n, bool may_start)
{
    if (!diag_stamp) { diag_emit(b, n, may_start); return; }
    char out[4096], st[32];
    size_t sl = diag_stamp_fmt(st, diag_now_ns()), o = 0;
    bool at_start = true;
    for (size_t i = 0; i < n; i++) {
        if (at_start) {
            if (o + sl + 1 > sizeof out) { diag_emit(out, o, may_start); o = 0; }
            memcpy(out + o, st, sl);
            o += sl;
            at_start = false;
        }
        if (o == sizeof out) { diag_emit(out, o, may_start); o = 0; }
        out[o++] = b[i];
        if (b[i] == '\n') {
            at_start = true;
            if (o > sizeof out - 256) { diag_emit(out, o, may_start); o = 0; }
        }
    }
    diag_emit(out, o, may_start);
}

/* Async-signal-safe. */
void hao_diag_write(const char *b, size_t n) { diag_write(b, n, false); }

/* After a crash report: the process dies next (PID 1: the kernel panics). */
static void diag_sync(void)
{
    int fd = __atomic_load_n(&diag_log, __ATOMIC_ACQUIRE);
    if (fd >= 0) fsync(fd);
}

static int diag_fwrite(void *cookie, const char *b, int n)
{
    (void)cookie;
    if (n > 0) diag_write(b, (size_t)n, true);
    return n;
}

FILE *hao_diag_block(hao_diag_blk *b)
{
    b->buf = NULL;
    b->len = 0;
    b->f = open_memstream(&b->buf, &b->len);
    return b->f ? b->f : HAO_DIAG;
}

void hao_diag_block_end(hao_diag_blk *b)
{
    if (!b->f) { fflush(HAO_DIAG); return; }
    fclose(b->f);
    if (b->buf) {
        fflush(HAO_DIAG);
        diag_write(b->buf, b->len, true);
        free(b->buf);
    }
}

/* BASE/user/<euid> for a process that may not write BASE/console.log, or
 * false: BASE/user is a sticky directory of BASE's owner, and <euid> a
 * directory this process owns that no one else can write (another user may
 * have made one under that name first: then there is no log). */
static bool diag_user_dir(const char *base, char *out, size_t n)
{
    struct stat bs, us, ds;
    if (stat(base, &bs) != 0) return false;
    char u[1100];
    snprintf(u, sizeof u, "%s/user", base);
    int ufd = open(u, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (ufd < 0) return false;
    bool ok = fstat(ufd, &us) == 0 && S_ISDIR(us.st_mode) && (us.st_mode & S_ISVTX) && us.st_uid == bs.st_uid;
    char id[16];
    snprintf(id, sizeof id, "%u", (unsigned)geteuid());
    if (ok && mkdirat(ufd, id, 0700) != 0 && errno != EEXIST) ok = false;
    int dfd = ok ? openat(ufd, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    ok = dfd >= 0 && fstat(dfd, &ds) == 0 && ds.st_uid == geteuid() && !(ds.st_mode & (S_IWGRP | S_IWOTH));
    if (dfd >= 0) close(dfd);
    close(ufd);
    if (!ok) return false;
    int r = snprintf(out, n, "%s/%s", u, id);
    return r > 0 && (size_t)r < n;
}

void hao_diag_init(void)
{
    static int done;
    if (done++) return;
    bool crash = hao_bootarg_flag("hao_crash"), mk = false;
    diag_stamp = hao_opt.diag_time;
    if (diag_stamp) diag_now_ns();   /* the timebase, read outside any signal handler */
    if (crash && getpid() != 1) {
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (fd >= 0) diag_con = diag_high_fd(fd);
    }
    if (hao_opt.diag_dir[0] == '/') { snprintf(diag_dir, sizeof diag_dir, "%s", hao_opt.diag_dir); mk = true; }
    else if (crash) { snprintf(diag_dir, sizeof diag_dir, "/private/var/log/hao"); mk = geteuid() == 0; }
    if (diag_dir[0]) {
        int fd = diag_log_open(diag_dir, mk), e = errno;
        char ud[sizeof diag_dir];
        if (fd < 0 && (e == EACCES || e == EPERM) && geteuid() != 0 && diag_user_dir(diag_dir, ud, sizeof ud)) {
            memcpy(diag_dir, ud, sizeof diag_dir);   /* retries (diag_log_fd) open the same */
            mk = false;
            fd = diag_log_open(diag_dir, false);
            e = errno;
        }
        /* e, not errno: diag_user_dir's failed lookups (ENOENT) are no reason to retry */
        if (fd >= 0) diag_log = diag_high_fd(fd);
        else if (mk && (e == ENOENT || e == EROFS)) diag_retry = 1;
        pthread_atfork(NULL, NULL, uni_fork_child);
        if (diag_log < 0 && !diag_retry) uni_start();   /* a sandboxed process: the unified log from the start */
    }
    if (diag_con == 2 && !diag_dir[0] && !diag_stamp) return;   /* stderr alone, as before */
    FILE *f = funopen(NULL, NULL, diag_fwrite, NULL, NULL);
    if (!f) return;
    setvbuf(f, diag_buf, _IOLBF, sizeof diag_buf);
    hao_diag_file = f;
}

/* Exact, space-delimited "key=1" in the kernel's boot arguments. Lets a
 * whole boot (PID 1 has no environment) switch hao features without an
 * image rebuild. kern.bootargs is readable without privileges. */
bool hao_bootarg_flag(const char *key)
{
    char ba[1024];
    size_t n = sizeof ba - 1, k = strlen(key);
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return false;
    ba[n] = 0;
    for (char *p = ba; (p = strstr(p, key)); p += k)
        if ((p == ba || p[-1] == ' ') && p[k] == '=' && p[k + 1] == '1') return true;
    return false;
}

/* Native -> guest calls that arrived by faulting on a non-executable guest
 * page (each one is a kernel "execution not permitted" event). With
 * HAO_NX_STATS=1 (or boot argument hao_nx_stats=1) the faults are also
 * grouped by native caller (the return address the call pushed) and guest
 * target, and printed at exit; PID 1, which never exits, prints them every
 * two minutes. */
uint64_t hao_nx_faults, hao_nx_trap_faults;
#define NX_SITES 4096
static struct { uint64_t key, ret, target, n, via; } nx_sites[NX_SITES];
static bool nx_stats;

/* via: when the native caller is hao_native_call itself (a native function
 * the guest called tail-called the guest target: a Swift protocol dispatch
 * thunk, a native async function returning to its continuation), the
 * native function the guest called (its cpu.pc), else 0 */
static void nx_record(uint64_t ret, uint64_t target, uint64_t via)
{
    uint64_t key = (ret * 0x9E3779B97F4A7C15ull) ^ target ^ (via * 0xC2B2AE3D27D4EB4Full);
    if (!key) key = 1;
    for (uint32_t k = 0, h = (uint32_t)(key >> 40) & (NX_SITES - 1); k < NX_SITES; k++) {
        uint32_t i = (h + k) & (NX_SITES - 1);
        uint64_t cur = __atomic_load_n(&nx_sites[i].key, __ATOMIC_ACQUIRE), zero = 0;
        if (cur == 0 && __atomic_compare_exchange_n(&nx_sites[i].key, &zero, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            nx_sites[i].ret = ret; nx_sites[i].target = target; nx_sites[i].via = via; cur = key;
        } else if (cur == 0) cur = zero;
        if (cur == key) { __atomic_fetch_add(&nx_sites[i].n, 1, __ATOMIC_RELAXED); return; }
    }
}

static int nx_cmp(const void *a, const void *b)
{
    uint64_t x = ((const uint64_t *)a)[3], y = ((const uint64_t *)b)[3];
    return x < y ? 1 : x > y ? -1 : 0;
}

/* After the NX rows: native installs of SIGSEGV/SIGBUS that the shadow took
 * (src/signals.c), and a kernel disposition that is not on_fault (set past
 * libhao_identity's interposers: __sigaction, a raw system call) */
static void sig_owner_print(FILE *f)
{
    uint64_t n = __atomic_load_n(&hao_sig_native_installs, __ATOMIC_RELAXED);
    if (n) fprintf(f, "hao: native SIGSEGV/SIGBUS handler installs kept behind on_fault: %llu\n", (unsigned long long)n);
    static const int sigs[] = {SIGSEGV, SIGBUS};
    for (int k = 0; k < 2; k++) {
        struct sigaction cur;
        if (hao_sys_sigaction(sigs[k], NULL, &cur) != 0 || cur.sa_sigaction == hao_fault_entry) continue;
        Dl_info d = {0};
        bool found = dladdr((void *)(uintptr_t)cur.sa_handler, &d) && d.dli_fname;
        fprintf(f, "hao: signal %d handler in the kernel is %p (%s`%s), not on_fault: native->guest calls reach it\n", sigs[k],
                (void *)(uintptr_t)cur.sa_handler, found ? (strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname) : "?",
                found && d.dli_sname ? d.dli_sname : "?");
    }
}

/* One write (hao_diag_block): the rows stay under their header in the log. */
static void nx_print(void)
{
    /* the periodic report and the exit report share the sort buffer: one at a time (a fork child that inherits a
     * report in progress skips its own) */
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    hao_diag_blk blk;
    FILE *f = hao_diag_block(&blk);
    fprintf(f, "hao: nx faults: %llu native->guest, %llu native->trap\n", (unsigned long long)hao_nx_faults,
            (unsigned long long)hao_nx_trap_faults);
    if (!nx_stats) { sig_owner_print(f); hao_sig_resume_print(f); hao_diag_block_end(&blk); __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); return; }
    fprintf(f, "hao: nx fault sites in %s[%d] (count, native caller, guest image, guest file vmaddr):\n", getprogname(),
            (int)getpid());
    static struct { uint64_t key, ret, target, n, via; } s[NX_SITES];
    memcpy(s, nx_sites, sizeof s);
    qsort(s, NX_SITES, sizeof s[0], nx_cmp);
    for (int k = 0; k < NX_SITES && k < 60 && s[k].n; k++) {
        Dl_info d = {0};
        bool found = dladdr((void *)(uintptr_t)s[k].ret, &d);
        const char *sn = found && d.dli_sname ? d.dli_sname : "?";
        const char *fn = found && d.dli_fname ? (strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname) : "?";
        uint64_t off = d.dli_saddr ? s[k].ret - (uint64_t)(uintptr_t)d.dli_saddr : 0;
        hao_image *im = hao_image_for_code(s[k].target);
        /* a stripped library's nearest exported symbol is often far away: the image name says whose code it is */
        fprintf(f, "  %8llu  %s`%s+%#llx -> %s %#llx", (unsigned long long)s[k].n, fn, sn, (unsigned long long)off,
                im ? (strrchr(im->path, '/') ? strrchr(im->path, '/') + 1 : im->path) : "trap",
                (unsigned long long)(im ? s[k].target - im->slide : s[k].target));
        Dl_info v = {0};
        if (s[k].via && dladdr((void *)(uintptr_t)s[k].via, &v) && v.dli_fname)
            fprintf(f, " via %s`%s+%#llx", strrchr(v.dli_fname, '/') ? strrchr(v.dli_fname, '/') + 1 : v.dli_fname,
                    v.dli_sname ? v.dli_sname : "?", (unsigned long long)(s[k].via - (uint64_t)(uintptr_t)(v.dli_sname ? v.dli_saddr : v.dli_fbase)));
        else if (s[k].via)
            fprintf(f, " via %#llx", (unsigned long long)s[k].via);
        fprintf(f, "\n");
    }
    sig_owner_print(f);
    hao_sig_resume_print(f);
    hao_diag_block_end(&blk);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* At exit: stderr, or the console descriptor of hao_crash=1 */
static void nx_report(void) { nx_print(); }

void hao_nx_stats_atexit(void) { static int once; if (!once++) atexit(nx_report); }

/* Daemons never exit and their stderr is usually /dev/null: with
 * hao_nx_stats=1 every process reports every two minutes, to the console
 * descriptor of hao_crash=1 (PID 1's stderr is the console), when its
 * counts have changed. */
static void *nx_periodic(void *arg)
{
    (void)arg;
    hao_signals_block_async();
    uint64_t last = 0;
    /* the first report is printed even with no faults: a process that never
     * faulted is then told apart from one that died before reporting */
    for (bool first = true;; first = false) {
        sleep(120);
        uint64_t now = __atomic_load_n(&hao_nx_faults, __ATOMIC_RELAXED) + __atomic_load_n(&hao_nx_trap_faults, __ATOMIC_RELAXED);
        if (now == last && !first) continue;
        last = now;
        nx_print();
    }
    return NULL;
}

/* Fatal diagnostics from a signal handler: each line is formatted into a
 * stack buffer and written with write(2), so no stdio lock or allocation is
 * involved (the interrupted thread may hold either). The format is the one
 * hao_diag_prefix/hao_diag_guest_addr print elsewhere. */
typedef struct { char b[1024]; size_t n; } dline;

__attribute__((format(printf, 2, 3))) static void dl_add(dline *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = sizeof l->b - l->n;
    int r = vsnprintf(l->b + l->n, room, fmt, ap);
    va_end(ap);
    if (r > 0) l->n += (size_t)r < room ? (size_t)r : room - 1;
}

static void dl_prefix(dline *l) { dl_add(l, "hao[%s:%d]: ", getprogname(), (int)getpid()); }

static void dl_guest_addr(dline *l, uint64_t a)
{
    hao_image *im = hao_image_for_code(a);
    if (im) dl_add(l, " (%s+%#llx, file vmaddr %#llx)", strrchr(im->path, '/') ? strrchr(im->path, '/') + 1 : im->path,
                   (unsigned long long)(a - im->base), (unsigned long long)(a - im->slide));
}

static void dl_flush(dline *l)
{
    hao_diag_write(l->b, l->n);
    l->n = 0;
}

/* The segment and section of the native image D (dladdr's) that hold A,
 * from the image's own load commands in memory: no allocation, no lock.
 * Shared-cache images have one slide for all their segments, as files do. */
static bool native_section(const Dl_info *d, uint64_t a, char seg[17], char sect[17], int *prot)
{
    const struct mach_header_64 *mh = d->dli_fbase;
    if (!mh || mh->magic != MH_MAGIC_64 || mh->sizeofcmds > (1u << 20)) return false;
    const uint8_t *lc = (const uint8_t *)(mh + 1), *end = lc + mh->sizeofcmds;
    uint64_t slide = 0;
    bool text = false;
    for (const uint8_t *p = lc; !text && p + 8 <= end; p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *sc = (const void *)p;
        if (sc->cmdsize < 8) return false;
        if (sc->cmd == LC_SEGMENT_64 && !strncmp(sc->segname, "__TEXT", 16)) {
            slide = (uint64_t)(uintptr_t)mh - sc->vmaddr;
            text = true;
        }
    }
    if (!text) return false;
    for (const uint8_t *p = lc; p + sizeof(struct segment_command_64) <= end; p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *sc = (const void *)p;
        if (sc->cmdsize < 8) return false;
        if (sc->cmd != LC_SEGMENT_64 || a - slide < sc->vmaddr || a - slide - sc->vmaddr >= sc->vmsize) continue;
        memcpy(seg, sc->segname, 16);
        seg[16] = 0;
        sect[0] = 0;
        *prot = sc->initprot;
        const struct section_64 *s = (const void *)(sc + 1);
        for (uint32_t k = 0; k < sc->nsects && (const uint8_t *)(s + k + 1) <= p + sc->cmdsize; k++)
            if (a - slide >= s[k].addr && a - slide - s[k].addr < s[k].size) {
                memcpy(sect, s[k].sectname, 16);
                sect[16] = 0;
            }
        return true;
    }
    return false;
}

/* A for a report line: " image`symbol+off", " image+off" or "" */
static void dl_native_addr(dline *l, uint64_t a)
{
    Dl_info d;
    if (!a || !dladdr((void *)(uintptr_t)a, &d) || !d.dli_fname) return;
    const char *fn = strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname;
    if (d.dli_sname && d.dli_saddr)
        dl_add(l, " %s`%s+%#llx", fn, d.dli_sname, (unsigned long long)(a - (uint64_t)(uintptr_t)d.dli_saddr));
    else
        dl_add(l, " %s+%#llx", fn, (unsigned long long)(a - (uint64_t)(uintptr_t)d.dli_fbase));
}

/* [A, A+N) lies in this thread's native stack or its guest stack */
static bool diag_stack_word(hao_thread *t, uint64_t a, uint64_t n)
{
    pthread_t self = pthread_self();
    uint64_t hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(self), lo = hi - pthread_get_stacksize_np(self);
    if (a >= lo && a + n <= hi && a + n > a) return true;
    if (t && t->stack) {
        uint64_t glo = (uint64_t)(uintptr_t)t->stack, ghi = glo + t->stack_size;
        if (a >= glo && a + n <= ghi && a + n > a) return true;
    }
    return false;
}

/* dladdr reads dyld's image list; the rest reads only registers, hao's
 * own tables, the faulting image's load commands and stack words inside
 * this thread's stacks. The report: the first line (sig, addr, rip, guest
 * pc/lr of the innermost guest segment), the guest's x0-x7, every host
 * register, the native image and section of rip, the four words at rsp
 * (at rip 0 or in data, the first is the return address of the call that
 * went there: who called it), and the native frame-pointer backtrace with
 * image, symbol and offset. */
static void fault_diag(int sig, uint64_t addr, ucontext_t *uc, int crit)
{
    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    hao_thread *t = hao_current_peek();
    dline l = {.n = 0};
    Dl_info d1;
    bool named = dladdr((void *)(uintptr_t)rip, &d1) && d1.dli_sname && d1.dli_saddr;
    dl_prefix(&l);
    dl_add(&l, "host fault sig=%d addr=%#llx rip=%#llx", sig, (unsigned long long)addr, (unsigned long long)rip);
    if (named) dl_add(&l, " [%s+%#llx]", d1.dli_sname, (unsigned long long)(rip - (uint64_t)(uintptr_t)d1.dli_saddr));
    else dl_add(&l, " [translated code or unknown]");
    if (t) {
        uint64_t pc = t->cpu.pc;
        bool fh;
        if (t->cpu.host_scratch[HAO_SCRATCH_IN_GUEST] && !hao_jit_locate(rip, &pc, &fh) && t->cpu.host_scratch[HAO_SCRATCH_HELPER])
            pc = t->cpu.host_scratch[HAO_SCRATCH_HELPER];
        dl_add(&l, " guest pc=%#llx", (unsigned long long)pc);
        dl_guest_addr(&l, pc);
        dl_add(&l, " lr=%#llx", (unsigned long long)t->cpu.x[30]);
        dl_guest_addr(&l, t->cpu.x[30]);
        dl_add(&l, "\n ");
        for (int k = 0; k < 8; k++) dl_add(&l, " x%d=%#llx", k, (unsigned long long)t->cpu.x[k]);
    } else
        dl_add(&l, " (no guest state on this thread)");
    if (t) dl_add(&l, " depth=%d", t->depth);
    if (crit) dl_add(&l, "\n  inside a hao critical section (depth %d)", crit);
    const _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
#define R(x) (unsigned long long)ss->__##x
    dl_add(&l, "\n  host regs: rdi=%#llx rsi=%#llx rdx=%#llx rax=%#llx rcx=%#llx rbx=%#llx rbp=%#llx rsp=%#llx\n", R(rdi), R(rsi),
           R(rdx), R(rax), R(rcx), R(rbx), R(rbp), R(rsp));
    dl_add(&l, "             r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx rflags=%#llx\n", R(r8),
           R(r9), R(r10), R(r11), R(r12), R(r13), R(r14), R(r15), R(rflags));
#undef R
    dl_flush(&l);
    uint64_t rsp = uc->uc_mcontext->__ss.__rsp;
    if (diag_stack_word(t, rsp, 32)) {
        const uint64_t *w = (const uint64_t *)(uintptr_t)rsp;
        dl_add(&l, "  [rsp]: %#llx %#llx %#llx %#llx\n", (unsigned long long)w[0], (unsigned long long)w[1],
               (unsigned long long)w[2], (unsigned long long)w[3]);
        dl_add(&l, "  [rsp] as a return address:");
        size_t n0 = l.n;
        dl_native_addr(&l, w[0]);
        if (l.n == n0) {
            if (hao_image_for_code(w[0])) dl_guest_addr(&l, w[0]);
            else dl_add(&l, " (none)");
        }
        dl_add(&l, "\n");
    } else
        dl_add(&l, "  [rsp]: %#llx is outside this thread's stacks\n", (unsigned long long)rsp);
    /* where rip is in its native image (a line of its own: the first line's
     * format is the bridge worklist tool's, not part of this repository): a call to an import that the
     * native library defines as data (a placeholder exported under a
     * function's name) executes that data, as it would natively */
    char seg[17], sect[17];
    int prot = 0;
    if (named && d1.dli_fname && native_section(&d1, rip, seg, sect, &prot)) {
        dl_add(&l, "  native image: %s (%s%s%s)", strrchr(d1.dli_fname, '/') ? strrchr(d1.dli_fname, '/') + 1 : d1.dli_fname,
               seg, sect[0] ? "," : "", sect);
        if (rip == addr && !(prot & VM_PROT_EXECUTE)) dl_add(&l, "; not code: a native data symbol was called");
        dl_add(&l, "\n");
    }
    dl_flush(&l);
    /* frame-pointer walk, every read checked against this thread's stack
     * (translated code and leaf routines need not keep rbp a frame pointer) */
    pthread_t self = pthread_self();
    uint64_t hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(self), lo = hi - pthread_get_stacksize_np(self);
    uint64_t fp = uc->uc_mcontext->__ss.__rbp;
    for (int k = 0; k < 32 && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
        const uint64_t *f = (const uint64_t *)(uintptr_t)fp;
        uint64_t ret = f[1];
        dl_add(&l, "  #%d %#llx", k, (unsigned long long)ret);
        size_t n0 = l.n;
        dl_native_addr(&l, ret);
        if (l.n == n0) {
            if (hao_image_for_code(ret)) dl_guest_addr(&l, ret);
            else dl_add(&l, " ?");
        }
        dl_add(&l, "\n");
        dl_flush(&l);
        if (f[0] <= fp) break;
        fp = f[0];
    }
    diag_sync();
}

/* ------------------------------------------- the last handler action
 * What on_fault (and signals.c's tramp) last did on each thread, for the
 * failed-__sigreturn report: a failed sigreturn names the frame it failed
 * on (_sigtramp keeps its ucontext in rbx, its token in r12), and this says
 * whose frame that was. Direct-mapped by pthread_self, no allocation, no
 * lock: a slot is the thread's while its key is (two threads that share a
 * slot overwrite each other; the reader checks the key around its copy). */
typedef struct { uint64_t key, kind, sig, a, b, uctx, tok, n; } fault_act;
#define FAULT_ACTS 256
static fault_act fault_acts[FAULT_ACTS];

static fault_act *fault_act_slot(uint64_t self)
{
    return &fault_acts[((self >> 12) * 0x9E3779B97F4A7C15ull) >> 56];
}

void hao_fault_act_note(int kind, int sig, uint64_t a, uint64_t b, const void *uctx, uint64_t tok)
{
    uint64_t self = (uint64_t)(uintptr_t)pthread_self();
    fault_act *s = fault_act_slot(self);
    uint64_t n = __atomic_load_n(&s->key, __ATOMIC_RELAXED) == self ? s->n + 1 : 1;
    __atomic_store_n(&s->key, 0, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    s->kind = (uint64_t)kind;
    s->sig = (uint64_t)sig;
    s->a = a;
    s->b = b;
    s->uctx = (uint64_t)(uintptr_t)uctx;
    s->tok = tok;
    s->n = n;
    __atomic_store_n(&s->key, self, __ATOMIC_RELEASE);
}

static bool fault_act_get(fault_act *out)
{
    uint64_t self = (uint64_t)(uintptr_t)pthread_self();
    fault_act *s = fault_act_slot(self);
    if (__atomic_load_n(&s->key, __ATOMIC_ACQUIRE) != self) return false;
    *out = *s;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&s->key, __ATOMIC_RELAXED) == self && out->key == self;
}

/* fault_diag in progress on these threads: a fault inside it (on_fault is
 * SA_NODEFER) skips the diagnostics on that thread only */
static pthread_t diag_busy[16];

/* The fatal-fault report's own stack (hao_install_fault_handler maps it,
 * with a guard page below): one, taken by one thread at a time; another
 * thread's report meanwhile runs on its own stack, as before. */
#define DIAG_STACK_SIZE (256u << 10)
static void *diag_stack_top;
static int diag_stack_busy;
typedef struct { int sig; uint64_t addr; ucontext_t *uc; int crit; } fault_diag_args;
extern void hao_call_on_stack(void (*fn)(void *), void *arg, void *top);
static void fault_diag(int sig, uint64_t addr, ucontext_t *uc, int crit);
static void fault_diag_on(void *p)
{
    const fault_diag_args *a = p;
    fault_diag(a->sig, a->addr, a->uc, a->crit);
}

static int diag_claim(void)
{
    pthread_t self = pthread_self();
    for (int k = 0; k < 16; k++)
        if (pthread_equal(__atomic_load_n(&diag_busy[k], __ATOMIC_ACQUIRE), self)) return -1;   /* nested */
    for (int k = 0; k < 16; k++) {
        pthread_t none = NULL;
        if (__atomic_compare_exchange_n(&diag_busy[k], &none, self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return k;
    }
    return -1;   /* 16 reports at once: this one is skipped */
}

/* resume the faulting context with its own errno (fault_resume: the calls made here may set it) */
static inline void fault_resume(ucontext_t *uc, int e, int path) { errno = e; hao_sig_resume(uc, path); errno = e; }

__attribute__((visibility("hidden"))) void hao_on_fault(int sig, siginfo_t *si, void *ctx, uint64_t tok);
void hao_on_fault(int sig, siginfo_t *si, void *ctx, uint64_t tok)
{
    int fe = errno;   /* the interrupted code's, restored before it resumes */
    ucontext_t *uc = ctx;
    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    uint64_t addr = (uint64_t)(uintptr_t)si->si_addr;
    /* hao_probe_read8's load: it reports the failure (signals.c, fault-tolerant reads) */
    if (rip == (uint64_t)(uintptr_t)hao_probe_read8_load && (sig == SIGSEGV || sig == SIGBUS)) {
        __atomic_fetch_add(&hao_probe_faults, 1, __ATOMIC_RELAXED);
        hao_fault_act_note(HAO_ACT_PROBE, sig, addr, rip, uc, tok);
        uc->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)hao_probe_read8_fail;
        fault_resume(uc, fe, HAO_RESUME_PROBE);
        return;
    }
    /* the same fault, stale: another signal's frame was set up first (Rosetta delivers a pending SIGALRM
     * before it), so the context stands at _sigtramp's entry. Let that handler run; the load faults again,
     * cleanly, when it returns (as a stale NX fault, below) */
    if ((sig == SIGSEGV || sig == SIGBUS) && hao_sig_stale_at(uc, addr, (uint64_t)(uintptr_t)hao_probe_read8_load)) {
        hao_fault_act_note(HAO_ACT_STALE, sig, addr, rip, uc, tok);
        fault_resume(uc, fe, HAO_RESUME_STALE);
        return;
    }
    if (hao_opt.crash_trace && !(rip == addr && (hao_image_for_code(rip) || hao_is_trap(rip)))) {
        /* raw log first: the formatted report below may itself fault */
        char b[256];
        int n = snprintf(b, sizeof b, "hao: fault sig=%d addr=%#llx rip=%#llx rsp=%#llx rbp=%#llx (guest stack %p+%#zx)\n", sig,
                         (unsigned long long)addr, (unsigned long long)rip, (unsigned long long)uc->uc_mcontext->__ss.__rsp,
                         (unsigned long long)uc->uc_mcontext->__ss.__rbp, pthread_getspecific(thread_key) ? (void *)((hao_thread *)pthread_getspecific(thread_key))->stack : NULL,
                         pthread_getspecific(thread_key) ? ((hao_thread *)pthread_getspecific(thread_key))->stack_size : (size_t)0);
        write(2, b, (size_t)n);
    }
    if (rip == addr && hao_image_for_code(rip)) {
        __atomic_fetch_add(&hao_nx_faults, 1, __ATOMIC_RELAXED);
        /* a call or jump into guest code: [rsp] is the native caller's return address */
        uint64_t ret = *(uint64_t *)(uintptr_t)uc->uc_mcontext->__ss.__rsp;
        if (nx_stats) {
            uint64_t via = 0;
            hao_thread *t = hao_current_peek();
            if (t && ret - (uint64_t)(uintptr_t)hao_native_call < 0x400) via = t->cpu.pc;
            nx_record(ret, rip, via);
        }
        hao_fault_act_note(HAO_ACT_NX, sig, rip, ret, uc, tok);
        /* native -> guest call: run it through the translator */
        uc->uc_mcontext->__ss.__r11 = rip;
        uc->uc_mcontext->__ss.__r10 = HAO_CB_FAULT;
        uc->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)hao_callback_entry;
        fault_resume(uc, fe, HAO_RESUME_NX);   /* returns only to go back through __sigreturn */
        return;
    }
    if (rip == addr && hao_is_trap(rip)) {
        __atomic_fetch_add(&hao_nx_trap_faults, 1, __ATOMIC_RELAXED);
        uint64_t ret = *(uint64_t *)(uintptr_t)uc->uc_mcontext->__ss.__rsp;
        if (nx_stats) nx_record(ret, rip, 0);
        hao_fault_act_note(HAO_ACT_NX_TRAP, sig, rip, ret, uc, tok);
        uc->uc_mcontext->__ss.__r10 = 0;
        /* a native caller reached an interposed entry point: let the guest
         * dispatcher handle it as if called from guest code */
        uc->uc_mcontext->__ss.__r11 = rip;
        uc->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)hao_callback_entry;
        fault_resume(uc, fe, HAO_RESUME_TRAP);
        return;
    }
    /* A stale NX fault: an instruction fetch at guest code or a trap (the x86 error code's I/D bit, the
     * fault address = si_addr) whose context no longer stands at that address. Another signal's frame was
     * set up between the fault and this delivery (Rosetta delivers a pending SIGALRM first; xnu on
     * hardware one numbered below SIGSEGV/SIGBUS), so rip is _sigtramp's entry: let that handler run,
     * and the call faults again, cleanly, when it returns. (A sent SIGSEGV/SIGBUS carries the thread's
     * last fault code and address too, but stands after its kill syscall, not at _sigtramp.) */
    if (hao_sig_stale(uc, addr) && (hao_image_for_code(addr) || hao_is_trap(addr))) {
        hao_fault_act_note(HAO_ACT_STALE, sig, addr, rip, uc, tok);
        fault_resume(uc, fe, HAO_RESUME_STALE);   /* counted there ("stale") */
        return;
    }
    /* an uncaught exception at a libdispatch callout: its x86_64 personality cannot be called (unwind.c).
     * Before any forwarding: natively there is no fault at all */
    {
        uint64_t pers = uc->uc_mcontext->__ss.__rax;
        if (hao_eh_fault(uc)) {
            hao_fault_act_note(HAO_ACT_EHPERS, sig, pers, uc->uc_mcontext->__ss.__rip, uc, tok);
            fault_resume(uc, fe, HAO_RESUME_EHPERS);   /* the edited context, without __sigreturn when it can */
            return;
        }
    }
    /* HAO_CODE_ALIAS: the guest's own load of a poisoned code page (code.c); a bypass is reported and goes on */
    if ((sig == SIGSEGV || sig == SIGBUS) && rip != addr && __atomic_load_n(&hao_code_npoison, __ATOMIC_ACQUIRE)) {
        /* the guest's own access: from translated code, or from the interpreter running the instruction at
         * HELPER, unless it is that instruction's fetch (a reader that bypassed hao_code_read32) */
        hao_thread *t = hao_current_peek();
        uint64_t h = t ? t->cpu.host_scratch[HAO_SCRATCH_HELPER] : 0;
        bool guest = t && (hao_in_code_cache(rip) || (h && addr - h >= 4));
        const hao_image *pi = NULL;
        int r = hao_code_poison_fault(addr, (uc->uc_mcontext->__es.__err & 2) != 0, guest, &pi);
        if (r > 0) {
            fault_resume(uc, fe, HAO_RESUME_COMMPAGE);   /* the access again, now that the page is readable */
            return;
        }
        if (r < 0) {
            Dl_info d;
            bool named = dladdr((void *)(uintptr_t)rip, &d) && d.dli_sname && d.dli_saddr;
            char b[512];
            int n = snprintf(b, sizeof b, "hao: HAO_CODE_ALIAS: an access to guest code at %#llx (%s+%#llx) bypassed hao_code_*: "
                             "rip=%#llx [%s+%#llx]\n", (unsigned long long)addr,
                             strrchr(pi->path, '/') ? strrchr(pi->path, '/') + 1 : pi->path, (unsigned long long)(addr - pi->base),
                             (unsigned long long)rip, named ? d.dli_sname : "?",
                             (unsigned long long)(named ? rip - (uint64_t)(uintptr_t)d.dli_saddr : 0));
            if (n > 0) hao_diag_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
        }
    }
    /* a read of the arm64 commpage that is not mapped (commpage.c) */
    extern bool hao_commpage_fault(uint64_t addr);
    if (hao_commpage_fault(addr)) {
        hao_fault_act_note(HAO_ACT_COMMPAGE, sig, addr, rip, uc, tok);
        fault_resume(uc, fe, HAO_RESUME_COMMPAGE);   /* the read again, now that the page is there */
        return;
    }
    /* a guest handler on top of a hao critical section could need the lock
     * this thread holds: such a fault is fatal */
    int crit = hao_crit_depth();
    if (!crit) {
        hao_fault_act_note(HAO_ACT_FORWARD, sig, hao_signal_shadow_handler(sig), addr, uc, tok);
        errno = fe;   /* the handler sees the interrupted code's errno and leaves its own */
        if (hao_signal_forward(sig, si, ctx)) return;
    }
    hao_fault_act_note(HAO_ACT_GIVEUP, sig, addr, rip, uc, tok);
    /* A fault inside these diagnostics (on this thread) skips them. They
     * run on a stack of their own when it is free: a fault deep in nested
     * native->guest callbacks can leave this thread's stack (a 512 KB
     * dispatch worker's) nearly used up, and the report would then overflow
     * it and end the process with no report. */
    int slot = diag_claim();
    if (slot >= 0) {
        fault_diag_args a = {sig, addr, uc, crit};
        int free_ = 0;
        if (diag_stack_top && __atomic_compare_exchange_n(&diag_stack_busy, &free_, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            hao_call_on_stack(fault_diag_on, &a, diag_stack_top);
            __atomic_store_n(&diag_stack_busy, 0, __ATOMIC_RELEASE);
        } else
            fault_diag(sig, addr, uc, crit);
        __atomic_store_n(&diag_busy[slot], NULL, __ATOMIC_RELEASE);
    }
    /* The default action, certainly: the faulting instruction runs again and
     * takes it (a sent signal is raised again now). Handlers that were
     * installed before hao took the signal are in the shadow and had their
     * turn in hao_signal_forward. */
    hao_signal_giveup(sig, si, uc);
}

/* ------------------------------------------------ failed __sigreturn
 * _sigtramp (libsystem_platform, x86_64) calls the handler with the
 * ucontext in rbx and the token in r12, then __sigreturn(rbx, UC_FLAVOR,
 * r12), then ud2: a SIGILL whose pc is that ud2 means the sigreturn of the
 * frame at rbx failed (xnu bsd/dev/i386/unix_signal.c sigreturn: EFAULT
 * when the ucontext or its mcontext cannot be read, before the mask is
 * restored; EINVAL for a token other than uctx ^ the thread's token, or a
 * thread state thread_setstatus refuses, after it). unix_syscall64 can also
 * return a policy error (notably EPERM) before entering sigreturn at all.
 * The thread's token
 * changes only when no sigreturn is pending, and a failed one stays
 * pending, so this delivery has the same one: T = its token ^ its uctx. */

/* rip is the ud2 after _sigtramp's call of __sigreturn: the call's target */
static bool sigtramp_ud2(uint64_t rip, uint64_t *callee)
{
    Dl_info d;
    if (rip < 4096 || !dladdr((void *)(uintptr_t)rip, &d) || !d.dli_sname || !d.dli_fname || !d.dli_saddr) return false;
    if (strcmp(d.dli_sname, "_sigtramp") || !strstr(d.dli_fname, "libsystem_platform")) return false;
    if (rip - (uint64_t)(uintptr_t)d.dli_saddr < 5) return false;
    const uint8_t *p = (const uint8_t *)(uintptr_t)rip;   /* inside _sigtramp's text */
    if (p[0] != 0x0f || p[1] != 0x0b || p[-5] != 0xe8) return false;
    int32_t rel;
    memcpy(&rel, p - 4, 4);
    *callee = rip + (uint64_t)(int64_t)rel;
    return true;
}

/* N bytes at A into BUF, without touching A: false if any is unreadable */
static bool probe_read(uint64_t a, void *buf, size_t n)
{
    mach_vm_size_t got = 0;
    return a >= 4096 &&
           mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)buf, &got) == KERN_SUCCESS &&
           got == n;
}

static const char *errno_name(int e)
{
    switch (e) {
    case EFAULT: return "EFAULT";
    case EINVAL: return "EINVAL";
    case EPERM: return "EPERM";
    case ENOTSUP: return "ENOTSUP";
    case EINTR: return "EINTR";
    case 0: return "none";
    default: return "?";
    }
}

static void rep_add(char *rep, size_t cap, size_t *rn, dline *l)
{
    size_t k = l->n < cap - 1 - *rn ? l->n : cap - 1 - *rn;
    memcpy(rep + *rn, l->b, k);
    *rn += k;
    l->n = 0;
}

/* The report, one write: the first line is the bridge worklist tool's (not in this repository) */
static void sigreturn_report(int sig, int err, ucontext_t *uc, uint64_t tok, uint64_t callee)
{
    static char rep[4096];   /* the process dies after this; one report at a time */
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    size_t rn = 0;
    const _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t fuctx = ss->__rbx, ftok = ss->__r12, here = (uint64_t)(uintptr_t)uc;
    uint64_t T = tok ^ here, want_xnu = fuctx ^ T;
    ucontext_t fu;
    bool uok = probe_read(fuctx, &fu, sizeof fu);
    struct { _STRUCT_X86_EXCEPTION_STATE64 es; _STRUCT_X86_THREAD_STATE64 ss; } fm;
    _Static_assert(sizeof fm == __builtin_offsetof(struct __darwin_mcontext64, __fs), "mcontext64 es+ss");
    bool mok = uok && probe_read((uint64_t)(uintptr_t)fu.uc_mcontext, &fm, sizeof fm);
    const char *verdict = ftok == want_xnu ? "token matches (xnu rule)"
                          : ftok == tok    ? "token matches (Rosetta rule)"
                                           : "token mismatch";
    dline l = {.n = 0};
    dl_prefix(&l);
    dl_add(&l, "failed __sigreturn sig=%d at", sig);
    dl_native_addr(&l, ss->__rip);
    /* __sigreturn is not exported where _sigtramp calls it: its address */
    dl_add(&l, " (ud2 after the call of __sigreturn %#llx): errno=%d (%s) uctx=%#llx token=%#llx: uctx %s, %s; taking SIGTRAP's "
           "default action\n", (unsigned long long)callee, err, errno_name(err), (unsigned long long)fuctx, (unsigned long long)ftok, uok ? "readable" : "not readable", verdict);
    rep_add(rep, sizeof rep, &rn, &l);
    if (err == EPERM) {
        dl_add(&l, "  EPERM can be a syscall-unix policy denial of SYS_sigreturn before frame/token validation; "
               "a readable frame and matching token do not rule it out\n");
        rep_add(rep, sizeof rep, &rn, &l);
    }
    if (uok) {
        dl_add(&l, "  failed uctx: uc_onstack=%d uc_sigmask=%#x uc_stack=%p+%#zx flags=%#x uc_link=%p uc_mcsize=%#zx uc_mcontext=%p\n",
               fu.uc_onstack, (unsigned)fu.uc_sigmask, fu.uc_stack.ss_sp, (size_t)fu.uc_stack.ss_size, (unsigned)fu.uc_stack.ss_flags,
               (void *)fu.uc_link, (size_t)fu.uc_mcsize, (void *)fu.uc_mcontext);
        rep_add(rep, sizeof rep, &rn, &l);
        if (mok) {
#define F(x) (unsigned long long)fm.ss.__##x
            dl_add(&l, "  failed mcontext: rip=%#llx", F(rip));
            dl_native_addr(&l, fm.ss.__rip);
            dl_guest_addr(&l, fm.ss.__rip);
            dl_add(&l, " rsp=%#llx rbp=%#llx rbx=%#llx r12=%#llx r13=%#llx rdi=%#llx rflags=%#llx cs=%#llx fs=%#llx gs=%#llx",
                   F(rsp), F(rbp), F(rbx), F(r12), F(r13), F(rdi), F(rflags), F(cs), F(fs), F(gs));
#undef F
            dl_add(&l, "; trapno=%u err=%#x faultvaddr=%#llx\n", (unsigned)fm.es.__trapno, (unsigned)fm.es.__err,
                   (unsigned long long)fm.es.__faultvaddr);
        } else
            dl_add(&l, "  failed mcontext %p: not readable\n", (void *)fu.uc_mcontext);
        rep_add(rep, sizeof rep, &rn, &l);
    }
    dl_add(&l, "  token: this delivery uctx=%#llx token=%#llx, thread token %#llx; for the failed uctx expected %#llx (xnu: "
           "uctx ^ thread token) or %#llx (Rosetta: the token of this delivery)\n",
           (unsigned long long)here, (unsigned long long)tok, (unsigned long long)T, (unsigned long long)want_xnu,
           (unsigned long long)tok);
    dl_add(&l, "  signal mask at the ud2: %#x\n", (unsigned)uc->uc_sigmask);
    rep_add(rep, sizeof rep, &rn, &l);
    fault_act a;
    dl_add(&l, "  last hao handler action on this thread:");
    if (!fault_act_get(&a)) dl_add(&l, " none recorded");
    else {
        switch (a.kind) {
        case HAO_ACT_NX:
            dl_add(&l, " nx redirect of a native call to guest %#llx", (unsigned long long)a.a);
            dl_guest_addr(&l, a.a);
            dl_add(&l, " from");
            dl_native_addr(&l, a.b);
            break;
        case HAO_ACT_NX_TRAP:
            dl_add(&l, " native call of trap %#llx from", (unsigned long long)a.a);
            dl_native_addr(&l, a.b);
            break;
        case HAO_ACT_COMMPAGE:
            dl_add(&l, " arm64 commpage read at %#llx, rip %#llx", (unsigned long long)a.a, (unsigned long long)a.b);
            break;
        case HAO_ACT_FORWARD:
            dl_add(&l, " signal %d at %#llx forwarded to the handler %#llx", (int)a.sig, (unsigned long long)a.b,
                   (unsigned long long)a.a);
            dl_native_addr(&l, a.a);
            dl_guest_addr(&l, a.a);
            break;
        case HAO_ACT_STALE:
            dl_add(&l, " left a stale NX fault at %#llx alone, rip %#llx", (unsigned long long)a.a, (unsigned long long)a.b);
            break;
        case HAO_ACT_GIVEUP:
            dl_add(&l, " gave up on signal %d at %#llx, rip %#llx", (int)a.sig, (unsigned long long)a.a, (unsigned long long)a.b);
            break;
        case HAO_ACT_EHPERS:
            dl_add(&l, " personality %#llx is not an address: libunwind resumed at %#llx with _URC_FATAL_PHASE1_ERROR",
                   (unsigned long long)a.a, (unsigned long long)a.b);
            dl_native_addr(&l, a.b);
            break;
        case HAO_ACT_PROBE:
            dl_add(&l, " a fault-tolerant read of %#llx faulted (signal %d): resumed at its fail label", (unsigned long long)a.a, (int)a.sig);
            break;
        case HAO_ACT_TRAMP:
            dl_add(&l, " signal %d delivered to the guest handler %#llx", (int)a.sig, (unsigned long long)a.a);
            dl_guest_addr(&l, a.a);
            dl_add(&l, ", rip %#llx", (unsigned long long)a.b);
            break;
        default:
            dl_add(&l, " kind %llu", (unsigned long long)a.kind);
        }
        dl_add(&l, " (action %llu on this thread); its delivery uctx=%#llx token=%#llx", (unsigned long long)a.n,
               (unsigned long long)a.uctx, (unsigned long long)a.tok);
        /* signal frames at one stack depth share an address (Rosetta has one signal area): an NX redirect's frame
         * is told by the redirect it holds */
        bool redirect = mok && (a.kind == HAO_ACT_NX || a.kind == HAO_ACT_NX_TRAP) &&
                        fm.ss.__rip == (uint64_t)(uintptr_t)hao_callback_entry && fm.ss.__r11 == a.a;
        if (a.uctx != fuctx)
            dl_add(&l, ": another frame");
        else if (a.kind == HAO_ACT_NX || a.kind == HAO_ACT_NX_TRAP)
            dl_add(&l, redirect ? ": the failed frame (it holds this redirect), token %s"
                                : ": the failed frame's address, without this redirect in it (another delivery there)%.0s",
                   a.tok == ftok ? "unchanged" : "changed");
        else
            dl_add(&l, ": the failed frame's address, token %s", !a.tok ? "not recorded" : a.tok == ftok ? "unchanged" : "changed");
    }
    dl_add(&l, "\n");
    rep_add(rep, sizeof rep, &rn, &l);
    if (rn) {
        if (rep[rn - 1] != '\n') rep[rn - 1] = '\n';
        hao_diag_write(rep, rn);
    }
}

#include <execinfo.h>
/* SIGABRT/SIGTRAP/SIGILL under the diagnostics log: a report, then the
 * default action taken here (never after a __sigreturn, which may be what
 * failed). A SIGILL at the ud2 after _sigtramp's __sigreturn is a failed
 * sigreturn: its report, then SIGTRAP's default action, as an arm64
 * _sigtramp's brk after a failed sigreturn takes. */
__attribute__((visibility("hidden"))) void hao_on_abort(int sig, siginfo_t *si, void *ctx, uint64_t tok);
void hao_on_abort(int sig, siginfo_t *si, void *ctx, uint64_t tok)
{
    int err = errno;   /* a failed __sigreturn's */
    (void)si;
    ucontext_t *uc = ctx;
    uint64_t callee = 0;
    bool failed = uc && uc->uc_mcontext && sigtramp_ud2(uc->uc_mcontext->__ss.__rip, &callee);
    if (failed) sigreturn_report(sig, err, uc, tok, callee);
    hao_thread *t = hao_current_peek();
    dline l = {.n = 0};
    dl_prefix(&l);
    if (t) {
        dl_add(&l, "signal %d guest pc=%#llx", sig, (unsigned long long)t->cpu.pc);
        dl_guest_addr(&l, t->cpu.pc);
        dl_add(&l, " lr=%#llx", (unsigned long long)t->cpu.x[30]);
        dl_guest_addr(&l, t->cpu.x[30]);
    } else
        dl_add(&l, "signal %d (no guest state on this thread)", sig);
    dl_add(&l, "\n");
    dl_flush(&l);
    /* backtrace_symbols_fd's format, several whole lines per write */
    void *bt[48];
    int n = backtrace(bt, 48);
    for (int k = 0; k < n; k++) {
        if (l.n > sizeof l.b / 2) dl_flush(&l);
        Dl_info d = {0};
        uint64_t a = (uint64_t)(uintptr_t)bt[k];
        bool found = dladdr(bt[k], &d);
        const char *fn = found && d.dli_fname ? (strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname) : "???";
        if (found && d.dli_sname)
            dl_add(&l, "%-4d%-35s 0x%016llx %s + %llu\n", k, fn, (unsigned long long)a, d.dli_sname,
                   (unsigned long long)(a - (uint64_t)(uintptr_t)d.dli_saddr));
        else
            dl_add(&l, "%-4d%-35s 0x%016llx 0x%llx + 0\n", k, fn, (unsigned long long)a, (unsigned long long)a);
        if (l.n == sizeof l.b - 1) l.b[l.n - 1] = '\n';   /* a symbol too long for the buffer: cut, the line still ends */
    }
    dl_flush(&l);
    /* the counters the exit report would have printed (an abort runs no atexit) */
    if (hao_opt.stats || hao_opt.nx_stats) {
        char rb[512];
        size_t rn = hao_sig_resume_line(rb, sizeof rb);
        if (rn) hao_diag_write(rb, rn);
    }
    diag_sync();
    hao_signal_die(failed ? SIGTRAP : sig);
}

void hao_install_fault_handler(void)
{
    hao_signals_init();   /* the kernel's sigaction; the dispositions hao is about to replace */
    hao_diag_init();
    if (!diag_stack_top) {
        uint8_t *m = mmap(NULL, PAGE_SIZE + DIAG_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m != MAP_FAILED && mprotect(m, PAGE_SIZE, PROT_NONE) == 0) diag_stack_top = m + PAGE_SIZE + DIAG_STACK_SIZE;
    }
    if (hao_opt.nx_stats) {
        nx_stats = true;
        hao_nx_stats_atexit();
        pthread_t th;
        if (pthread_create(&th, NULL, nx_periodic, NULL) == 0) pthread_detach(th);
    }
    if (hao_opt.crash_trace || hao_diag_file) {
        struct sigaction ab;
        memset(&ab, 0, sizeof ab);
        ab.sa_sigaction = hao_abort_entry;
        ab.sa_flags = SA_SIGINFO | SA_RESETHAND;
        hao_sys_sigaction(SIGABRT, &ab, NULL);
        hao_sys_sigaction(SIGTRAP, &ab, NULL);
        hao_sys_sigaction(SIGILL, &ab, NULL);
    }
    hao_eh_fault_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = hao_fault_entry;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    hao_sys_sigaction(SIGSEGV, &sa, NULL);
    hao_sys_sigaction(SIGBUS, &sa, NULL);
    hao_signals_hook();   /* native installs of SIGSEGV/SIGBUS go to the shadow from now on */
}

/* ---------------------------------------------------- import binding */

/*
 * Guest images are linked with dyld's semantics:
 *  - a dependency's install name is expanded like dyld does: @executable_path
 *    is the guest program's directory, @loader_path the directory of the
 *    image that names it, @rpath every LC_RPATH of that image and of the
 *    images that loaded it, down to the main executable; in a set-ID
 *    process, with dyld's restrictions for one (for_each_candidate);
 *  - each candidate path is tried as a native x86_64 library (dlopen), then,
 *    if it has no x86_64 slice, as a guest image (absolute OS paths under
 *    HAO_GUEST_ROOT when set);
 *  - a guest image is recorded before its own imports are bound, so cyclic
 *    dependencies find it instead of recursing;
 *  - two-level namespace: a symbol imported from a guest library comes from
 *    that library or the libraries it re-exports (trie re-export entries
 *    and LC_REEXPORT_DYLIB), never from a flat search;
 *  - every symbol of a missing LC_LOAD_WEAK_DYLIB binds to 0;
 *  - initializers run after all images are bound, dependencies first
 *    (upward dependencies do not order), the main executable last;
 *  - LC_LAZY_LOAD_DYLIB_INFO libraries load on first call (__dyld_lazy_load).
 * A symbol not found in a native dependency is still looked up flat
 * (dlsym(RTLD_DEFAULT)): x86_64 and arm64 libraries do not always split their
 * exports the same way, and the native side keeps its own two-level binding.
 */
#define GUEST_DEP ((void *)(uintptr_t)-7)
#define MISSING_DEP ((void *)(uintptr_t)-9)

static pthread_mutex_t load_lock;
static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static void load_lock_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&load_lock, &a);
}
static void lock_loads(void) { pthread_once(&load_once, load_lock_init); pthread_mutex_lock(&load_lock); }
static void loads_fork_child(void) { load_lock_init(); }   /* recursive: re-made, not unlocked */
static void unlock_loads(void) { pthread_mutex_unlock(&load_lock); }

/* Guest libraries by real path. */
static struct guest_lib { char *path; hao_image *img; hao_image *loader; } *glibs;
static int nglibs, capglibs;

static void glib_mapped(hao_image *img, void *ctx)
{
    struct guest_lib *g = &glibs[(intptr_t)ctx];
    g->img = img;
    img->loader = g->loader;
}

static void set_main_mapped(hao_image *img, void *ctx) { (void)ctx; hao_main_image = img; }

static void libval_report(const char *line)
{
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG, "%s\n", line);
    fflush(HAO_DIAG);
}

hao_image *hao_load_main_image(const char *path, int fd, char *err, size_t errlen)
{
    lock_loads();
    hao_libval_set_verifier(hao_guest_sig_image, hao_opt.libval_user, libval_report);
    hao_libval_expect_main(!hao_opt.no_libval);   /* its signature sets library validation before any dependency loads */
    hao_image *img = hao_load_image_fd_cb(path, fd, hao_resolve_import, set_main_mapped, NULL, err, errlen);
    if (!img) hao_main_image = NULL;
    unlock_loads();
    return img;
}

static int by_load_seq(const void *a, const void *b)
{
    uint32_t x = (*(hao_image *const *)a)->load_seq, y = (*(hao_image *const *)b)->load_seq;
    return x < y ? -1 : x > y;
}

/* Every listed image in load order (main executable first), under
 * lock_loads, in an array the caller frees; *count is its length. */
static hao_image **images_in_order(size_t *count)
{
    size_t n = 0, cap = 0;
    hao_image **out = NULL;
    for (hao_image *im = hao_images_first(); im; im = hao_images_next(im)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            out = hao_alloc_check(realloc(out, cap * sizeof *out), "image list");
        }
        out[n++] = im;
    }
    if (n > 1) qsort(out, n, sizeof *out, by_load_seq);
    *count = n;
    return out;
}

static void dir_of(const char *path, char *out, size_t n)
{
    const char *sl = strrchr(path, '/');
    if (!sl) snprintf(out, n, ".");
    else if (sl == path) snprintf(out, n, "/");
    else snprintf(out, n, "%.*s", (int)(sl - path), path);
}

/* Expand @executable_path and @loader_path (relative to `im`), alone (an
 * LC_RPATH of just the directory, as dyld accepts) or followed by '/'. */
static bool expand_prefix(hao_image *im, const char *in, char *out, size_t n)
{
    char dir[1024];
    if (!strncmp(in, "@executable_path", 16) && (in[16] == '/' || !in[16])) {
        dir_of(hao_main_image ? hao_main_image->path : im->path, dir, sizeof dir);
        return (size_t)snprintf(out, n, "%s%s", dir, in + 16) < n;
    }
    if (!strncmp(in, "@loader_path", 12) && (in[12] == '/' || !in[12])) {
        dir_of(im->path, dir, sizeof dir);
        return (size_t)snprintf(out, n, "%s%s", dir, in + 12) < n;
    }
    if (in[0] == '@') return false;
    return (size_t)snprintf(out, n, "%s", in) < n;
}

/* os: the path came from an absolute install name or LC_RPATH entry (an OS
 * path, looked up under HAO_GUEST_ROOT for guest images), not from
 * @executable_path or @loader_path (a real path already). */
typedef bool (*cand_fn)(const char *cand, bool os, void *ctx);

/* Call fn for each path dyld would try for `name` as a dependency of `loader`.
 * In a set-ID process dyld expands no @executable_path, no @loader_path
 * from the main executable's LC_RPATHs and no relative path
 * (hao_dep_path_allowed, hao/aot.h): such a path is not tried, and *refused
 * is dyld's reason for the last one. */
static bool for_each_candidate(hao_image *loader, const char *name, cand_fn fn, void *ctx, const char **refused)
{
    char buf[2048];
    int tier = hao_env_tier();
    const char *why;
    *refused = NULL;
    if (!strncmp(name, "@rpath/", 7)) {
        hao_image *seen[64];
        int nseen = 0;
        for (hao_image *im = loader; im && nseen < 64; im = im->loader ? im->loader : im != hao_main_image ? hao_main_image : NULL) {
            bool dup = false;
            for (int k = 0; k < nseen; k++) dup |= seen[k] == im;
            if (dup) break;
            seen[nseen++] = im;
            for (int r = 0; r < im->nrpaths; r++) {
                char rp[1200];
                if (!hao_dep_path_allowed(im->rpaths[r], true, !hao_main_image || im == hao_main_image, tier, &why)) { *refused = why; continue; }
                if (!expand_prefix(im, im->rpaths[r], rp, sizeof rp)) continue;
                if ((size_t)snprintf(buf, sizeof buf, "%s/%s", rp, name + 7) >= sizeof buf) continue;
                if (fn(buf, im->rpaths[r][0] == '/', ctx)) return true;
            }
        }
        return false;
    }
    if (!hao_dep_path_allowed(name, false, !hao_main_image || loader == hao_main_image, tier, &why)) { *refused = why; return false; }
    if (!expand_prefix(loader, name, buf, sizeof buf)) return false;
    return fn(buf, name[0] == '/', ctx);
}

static void *dep_handle(hao_image *img, int ord);
static bool native_sym(void *h, const char *name, bool flat, uint64_t *addr);

/* An interposer answers the import with a trap (counted as a TYPED bind).
 * native: the import binds to a native definition. An ABI repair of a native
 * C++ function (hao_interpose_abi_repair) applies only then: guest code that
 * binds to guest libc++ calls it directly. */
static bool interposed(const char *name, uint64_t *addr, bool native)
{
    if (!native && hao_interpose_abi_repair(name)) return false;
    if (!hao_interpose_lookup(name, addr)) return false;
    hao_bridge_bind_trap(name, HAO_BRS_TRAP);
    return true;
}

static int glib_slot(const char *path, hao_image *loader)
{
    if (nglibs == capglibs) {
        capglibs = capglibs ? 2 * capglibs : 16;
        glibs = hao_alloc_check(realloc(glibs, capglibs * sizeof *glibs), "guest library list");
    }
    glibs[nglibs] = (struct guest_lib){hao_alloc_check(strdup(path), "guest library list"), NULL, loader};
    return nglibs++;
}

static hao_image *glib_find(const char *path)
{
    for (int k = 0; k < nglibs; k++)
        if (glibs[k].path && !strcmp(glibs[k].path, path)) return glibs[k].img;  /* or NULL while binding */
    return NULL;
}

static bool glib_known(const char *path)
{
    for (int k = 0; k < nglibs; k++) if (glibs[k].path && !strcmp(glibs[k].path, path)) return true;
    return false;
}

/* A symbol of a native-backed cache library, for dsc.c's binder. */
static uint64_t dsc_bind_native(const char *lib, const char *sym, bool *ok)
{
    uint64_t a;
    *ok = true;
    if (interposed(sym, &a, true)) return a;
    void *h = runtime_dlopen(lib, RTLD_LAZY | RTLD_GLOBAL);
    if (native_sym(h ? h : RTLD_DEFAULT, sym, true, &a)) return a;
    *ok = false;
    return 0;
}

/* Why the last guest load failed, for dyld's "tried:" list (under the load lock). */
static char last_load_err[700];
static void note_load_err(const char *path, const char *err)
{
    size_t pl = strlen(path);
    if (!strncmp(err, path, pl) && err[pl] == ':' && err[pl + 1] == ' ') err += pl + 2;   /* the loader's "PATH: " */
    snprintf(last_load_err, sizeof last_load_err, "%s", err);
}

static hao_image *load_cache_image(hao_image *loader, const char *path)
{
    if (glib_known(path)) return glib_find(path);
    intptr_t slot = glib_slot(path, loader);
    char err[512];
    hao_image *im = hao_dsc_load(path, glib_mapped, (void *)slot, err, sizeof err);
    if (!im) {   /* native dlopen already failed: a rejected cache image is worth saying */
        note_load_err(path, err);
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "guest dylib not loaded: %s\n", err);
        free(glibs[slot].path);
        glibs[slot].path = NULL;
        return NULL;
    }
    /* dependencies first, so that pointers into arm64e-only ones stay guest */
    for (int d = 0; d < im->ndeps; d++) (void)dep_handle(im, d + 1);
    hao_dsc_bind(im, dsc_bind_native);
    return im;
}

/* A guest framework is not a dyld image, so CoreFoundation never makes its
 * bundle: CFBundleGetBundleWithIdentifier and +[NSBundle bundleWithIdentifier:]
 * miss it, and a framework that finds its own resources by its identifier
 * (EngagementCollector: bundleWithIdentifier: then URLForResource:withExtension:
 * for its Core Data model) gets nil. Natively CF creates the bundle of every
 * loaded framework. Create it once when the framework's executable is loaded
 * (path .../NAME.framework/[Versions/V/]NAME) and keep it: CF's tables then
 * answer the identifier. CF returns the existing bundle for a URL it already
 * knows (a framework native code opened as a bundle). */
static void register_framework_bundle(const char *rp)
{
    static void *(*url_create)(void *, const uint8_t *, long, unsigned char);
    static void *(*bundle_create)(void *, void *);
    static void (*release)(const void *);
    const char *fw = strstr(rp, ".framework/");
    if (!fw) return;
    const char *slash = fw;
    while (slash > rp && slash[-1] != '/') slash--;
    size_t nlen = (size_t)(fw - slash), dlen = (size_t)(fw - rp) + strlen(".framework");
    const char *exe = strrchr(rp, '/') + 1;
    if (strlen(exe) != nlen || strncmp(exe, slash, nlen)) return;     /* not the framework's own executable */
    if (!url_create) {
        url_create = dlsym(RTLD_DEFAULT, "CFURLCreateFromFileSystemRepresentation");
        bundle_create = dlsym(RTLD_DEFAULT, "CFBundleCreate");
        release = dlsym(RTLD_DEFAULT, "CFRelease");
    }
    if (!url_create || !bundle_create || !release) return;
    void *url = url_create(NULL, (const uint8_t *)rp, (long)dlen, 1);
    if (!url) return;
    void *bundle = bundle_create(NULL, url);   /* kept: guest images stay resident */
    release(url);
    if (hao_verbose > 1) fprintf(stderr, "hao: framework bundle %.*s %s\n", (int)dlen, rp, bundle ? "registered" : "not created");
}

static hao_image *load_guest_file(hao_image *loader, const char *cand, bool os)
{
    char path[2048], rp[PATH_MAX];
    const char *root = hao_opt.guest_root;
    if (os && *root)
        snprintf(path, sizeof path, "%s%s", root, cand);
    else
        snprintf(path, sizeof path, "%s", cand);
    if (access(path, R_OK) != 0 || !realpath(path, rp)) {
        /* no file: an OS library that exists only in the arm64e shared cache */
        if (os && cand[0] == '/' && hao_dsc_has(cand)) return load_cache_image(loader, cand);
        snprintf(last_load_err, sizeof last_load_err, "%s", errno == ENOENT ? "no such file" : strerror(errno));
        return NULL;
    }
    /* already loaded, or still binding (a dependency cycle) */
    if (glib_known(rp)) return glib_find(rp);
    intptr_t slot = glib_slot(rp, loader);
    char err[512];
    hao_image *im = hao_load_image_cb(rp, hao_resolve_import, glib_mapped, (void *)slot, err, sizeof err);
    if (!im) {   /* native dlopen already failed: a rejected guest dylib is worth saying */
        note_load_err(rp, err);
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "guest dylib not loaded: %s\n", err);
        free(glibs[slot].path);
        glibs[slot].path = NULL;
        glibs[slot].img = NULL;
        return NULL;
    }
    register_framework_bundle(rp);
    if (hao_verbose)
        fprintf(stderr, "hao: loaded guest dylib %s at %#llx%s%s%s\n", rp, (unsigned long long)im->base,
                im->libval_route[0] ? " (code signature: " : "", im->libval_route, im->libval_route[0] ? ")" : "");
    return im;
}

/* A guest library hao loads for itself (a thunk library's arm64e slice,
 * src/thunks.c): mapped and bound like a dependency, by its real path. */
hao_image *hao_load_guest_library(const char *path, char *err, size_t n)
{
    char rp[PATH_MAX];
    if (!realpath(path, rp)) { snprintf(err, n, "%s: %s", path, strerror(errno)); return NULL; }
    lock_loads();
    hao_image *im = NULL;
    if (glib_known(rp)) im = glib_find(rp);
    else {
        intptr_t slot = glib_slot(rp, NULL);
        im = hao_load_image_cb(rp, hao_resolve_import, glib_mapped, (void *)slot, err, n);
        if (!im) { free(glibs[slot].path); glibs[slot].path = NULL; glibs[slot].img = NULL; }
    }
    unlock_loads();
    return im;
}


struct try_ctx { hao_image *loader; void *handle; hao_image *guest; bool cxx; char *tried; size_t tn; };

/*
 * Guest libc++. libc++'s interface is full of
 * types whose layout or calling convention differs between Apple arm64 and
 * x86_64 (std::string's alternate layout, class results through x8,
 * this-returning constructors, std::function), so guest code that links
 * /usr/lib/libc++.1.dylib gets the arm64e libc++ of the arm64e shared cache
 * as a guest image: arm64 code calls arm64 code. libc++abi stays native
 * (one __cxa_throw, __cxa_get_globals, terminate handler, personality and
 * the typeinfos it owns, which native libobjc and every native C++ library
 * share): guest libc++ binds to it like any native library. The image must
 * pass hao_dsc_eligible (no ObjC, Swift or TLV; every reference into another
 * image named and bindable). HAO_NATIVE_LIBCXX=1, a missing arm64e cache or
 * an ineligible image keep today's native libc++.
 */
static const char LIBCXX[] = "/usr/lib/libc++.1.dylib";
static int libcxx_state;   /* 0 undecided, 1 guest, -1 native */

static hao_image *guest_libcxx(hao_image *loader, const char *cand, bool os)
{
    if (!os || strcmp(cand, LIBCXX) || hao_opt.native_libcxx || libcxx_state < 0) return NULL;
    if (libcxx_state == 0) {
        char why[256] = "";
        if (!hao_dsc_has(LIBCXX)) snprintf(why, sizeof why, "no arm64e shared cache with %s", LIBCXX);
        else (void)hao_dsc_eligible(LIBCXX, why, sizeof why);
        libcxx_state = why[0] ? -1 : 1;
        if (why[0]) {
            if (hao_verbose) fprintf(stderr, "hao: libc++ stays native: %s\n", why);
            return NULL;
        }
    }
    bool fresh = !glib_known(LIBCXX);
    hao_image *g = load_cache_image(loader, LIBCXX);
    if (g && fresh) (void)hao_dsc_share_type_names(g, dsc_bind_native, LIBCXX);
    if (!g) libcxx_state = -1;
    else if (fresh && hao_verbose) fprintf(stderr, "hao: guest libc++ at %#llx\n", (unsigned long long)g->base);
    return g;
}

/*
 * A library guest code calls with C++ names (an import named __Z...) whose
 * file also has an arm64 slice runs as guest code too: its interface has the
 * same layout-divergent types as libc++'s, if it passes the
 * eligibility check. Not when its native slice is loaded already (one
 * copy of its state), nor when it carries ObjC or Swift metadata
 * (hao_macho_guest_cxx_eligible). Its own libc++ is then guest libc++.
 */
static hao_image *guest_cxx_library(hao_image *loader, const char *cand, bool os)
{
    /* the file native dlopen would open (under HAO_GUEST_ROOT an OS path names another file: native, as before) */
    if (hao_opt.native_libcxx || !strcmp(cand, LIBCXX) || (os && *hao_opt.guest_root)) return NULL;
    char rp[PATH_MAX], why[400];
    if (access(cand, R_OK) != 0 || !realpath(cand, rp)) return NULL;   /* a cache library: native, as before */
    if (glib_known(rp)) return glib_find(rp);
    void *h = runtime_dlopen(rp, RTLD_LAZY | RTLD_NOLOAD);
    if (h) {
        if (hao_verbose) fprintf(stderr, "hao: %s stays native: its native slice is loaded already\n", rp);
        return NULL;
    }
    if (!hao_macho_guest_cxx_eligible(rp, why, sizeof why)) {
        if (hao_verbose) fprintf(stderr, "hao: C++ library stays native: %s\n", why);
        return NULL;
    }
    hao_image *g = load_guest_file(loader, cand, os);
    if (g && hao_verbose) fprintf(stderr, "hao: C++ library %s runs as guest code\n", rp);
    return g;
}

static bool try_candidate(const char *cand, bool os, void *c)
{
    struct try_ctx *x = c;
    hao_image *gl = guest_libcxx(x->loader, cand, os);
    if (!gl && x->cxx) gl = guest_cxx_library(x->loader, cand, os);
    if (gl) { x->guest = gl; return true; }
    void *h = runtime_dlopen(cand, RTLD_LAZY | RTLD_GLOBAL);
    if (h) { x->handle = h; return true; }
    if (hao_verbose > 1) fprintf(stderr, "hao: native dlopen %s: %s\n", cand, dlerror());
    last_load_err[0] = 0;
    hao_image *g = load_guest_file(x->loader, cand, os);
    if (g) { x->guest = g; return true; }
    if (x->tried) {   /* dyld's "tried:" entry for this path */
        size_t l = strlen(x->tried);
        snprintf(x->tried + l, x->tn - l, "%s'%s' (%s)", l ? ", " : "", cand, last_load_err[0] ? last_load_err : "not loaded");
    }
    return false;
}

/* Load (once) and return the library a load command names: a native handle,
 * GUEST_DEP (img->dep_image) or MISSING_DEP. */
static void *open_library(hao_image *img, const char *name, bool cxx, hao_image **guest, char *tried, size_t tn)
{
    struct try_ctx x = {img, NULL, NULL, cxx, tried, tn};
    if (tried && tn) tried[0] = 0;
    const char *refused;
    for_each_candidate(img, name, try_candidate, &x, &refused);
    if (refused && !x.handle && !x.guest) {
        if (tried && tn) {   /* as dyld's "tried:" list says it */
            size_t l = strlen(tried);
            snprintf(tried + l, tn - l, "%s'%s' (%s)", l ? ", " : "", name, refused);
        }
        if (hao_verbose) fprintf(stderr, "hao: %s: %s (set-ID process)\n", name, refused);
    }
    *guest = x.guest;
    return x.handle ? x.handle : x.guest ? GUEST_DEP : MISSING_DEP;
}

/* Set once the launch's images are loaded and initialised (hao_run_initializers). */
static bool launched;

/* xnu sys/reason.h, dyld's dyld_priv.h (dyld_abort_payload) */
void abort_with_payload(uint32_t reason_namespace, uint64_t reason_code, void *payload, uint32_t payload_size,
                        const char *reason_string, uint64_t reason_flags) __attribute__((noreturn));
#define OS_REASON_DYLD 6
#define DYLD_EXIT_REASON_DYLIB_MISSING 1

/*
 * What dyld does when a non-weak dependency of the launch does not load:
 * "dyld[PID]: Library not loaded: NAME / Referenced from: <UUID> PATH /
 * Reason: tried: 'CANDIDATE' (WHY), ..." on stderr, then
 * abort_with_payload(OS_REASON_DYLD, DYLD_EXIT_REASON_DYLIB_MISSING) with
 * the dylib and client paths (crash reports: "Namespace DYLD, Code 1,
 * Library missing"; the process dies of SIGABRT).
 */
static void dyld_halt_library(hao_image *img, const char *name, const char *tried)
{
    char uuid[48] = "<no uuid>", msg[3072];
    const uint8_t *u = img->uuid;
    if (img->has_uuid)
        snprintf(uuid, sizeof uuid, "<%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X>", u[0], u[1], u[2],
                 u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    snprintf(msg, sizeof msg, "Library not loaded: %s\n  Referenced from: %s %s\n  Reason: %s%s", name, uuid, img->path,
             tried[0] ? "tried: " : "no candidate path", tried);
    fprintf(stderr, "dyld[%d]: %s\n", getpid(), msg);
    if (HAO_DIAG != stderr) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "dyld: %s\n", msg);
        fflush(HAO_DIAG);
    }
    struct { uint32_t version, flags, target, client, symbol; } hdr = {1, 0, 0, 0, 0};
    static uint8_t payload[2048];
    size_t nl = strlen(name) + 1, cl = strlen(img->path) + 1;
    if (sizeof hdr + nl + cl <= sizeof payload) {
        hdr.target = (uint32_t)sizeof hdr;
        hdr.client = (uint32_t)(sizeof hdr + nl);
        memcpy(payload + sizeof hdr, name, nl);
        memcpy(payload + hdr.client, img->path, cl);
    }
    memcpy(payload, &hdr, sizeof hdr);
    size_t len = hdr.client ? hdr.client + cl : sizeof hdr;
    abort_with_payload(OS_REASON_DYLD, DYLD_EXIT_REASON_DYLIB_MISSING, payload, (uint32_t)len, msg, 0);
}

static void *dep_handle(hao_image *img, int ord)
{
    if (ord < 1 || ord > img->ndeps) return RTLD_DEFAULT;
    int k = ord - 1;
    if (!img->dep_handle[k]) {
        hao_image *g = NULL;
        char tried[2048];
        void *h = open_library(img, img->deps[k], img->dep_cxx[k], &g, tried, sizeof tried);
        /* dyld: a non-weak dependency that does not load ends the launch */
        if (h == MISSING_DEP && !img->dep_weak[k] && !launched && (img == hao_main_image || img->loader))
            dyld_halt_library(img, img->deps[k], tried);
        if (h == MISSING_DEP && !img->dep_weak[k] && hao_verbose)
            fprintf(stderr, "hao: %s: dependency %s not found (native or guest)\n", img->path, img->deps[k]);
        img->dep_image[k] = g;
        img->dep_handle[k] = h;
    }
    return img->dep_handle[k];
}

/* The canonical copy of an unresolved symbol's name (interned: the trap
 * is keyed by this pointer). Load time only. */
static hao_gmap unres_names = HAO_GMAP_INIT(2, 1, 8, 17);   /* FNV-1a 64 of the name -> copy */
static const char *unresolved_name(const char *name)
{
    uint64_t h = 14695981039346656037ull;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    if (!h) h = 1;
    const char *r = NULL;
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&unres_lock);
    uint64_t *e = hao_gmap_claim1(&unres_names, h, &fresh);
    if (e && !fresh) r = (const char *)(uintptr_t)e[1];
    if (r && strcmp(r, name)) r = NULL;   /* another name with the same hash: an own copy (and trap) */
    if (!r && (r = strdup(name)) && e && fresh) { e[1] = (uint64_t)(uintptr_t)r; hao_gmap_publish1(&unres_names, e, h); }
    pthread_mutex_unlock(&unres_lock);
    hao_crit_leave();
    return r ? r : "(unresolved import)";   /* out of memory */
}

static void trap_unresolved(hao_thread *t, uint64_t name)
{
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG, "call to unresolved import %s (lr %#llx)\n", (const char *)(uintptr_t)name,
            (unsigned long long)t->cpu.x[30]);
    abort();
}

/* A call lands in a trap that names the symbol instead of jumping to 0:
 * one trap and one copy of the name per symbol, however many images
 * import it. */
static uint64_t unresolved_trap(const char *name)
{
    const char *copy = unresolved_name(name);
    return hao_trap_address(copy, trap_unresolved, (uint64_t)(uintptr_t)copy);
}

/* Imports whose x86_64 function has another name (src/variants_gen.inc):
 * suffixes that are empty on arm64 (sys/cdefs.h __DARWIN_ONLY_64_BIT_INO_T,
 * __DARWIN_ONLY_VERS_1050: arm64 stat is x86_64 stat$INODE64, select is
 * select$1050) and arm64-only ones (Security's $LEGACYMAC). Every other
 * import, $DARWIN_EXTSN and $NOCANCEL variants and their plain names among
 * them, is the same function under the same name in both slices. */
typedef struct { const char *guest, *native, *lib; char kind; } variant_ent;
static const variant_ent variants[] = {
#include "variants_gen.inc"
};

static int variant_cmp(const void *key, const void *e) { return strcmp(key, ((const variant_ent *)e)->guest); }

static void *native_dlsym(void *h, const char *sym, bool flat)
{
    void *p = dlsym(h, sym);
    if (!p && flat && h != RTLD_DEFAULT) p = dlsym(RTLD_DEFAULT, sym);
    return p;
}

static bool defined_in(void *p, const char *lib)
{
    Dl_info di;
    return dladdr(p, &di) && di.dli_fname && !strcmp(di.dli_fname, lib);
}

/* A symbol in a native library (h may be RTLD_DEFAULT); flat: also search
 * every native library. */
static bool native_sym(void *h, const char *name, bool flat, uint64_t *addr)
{
    const char *sym = name[0] == '_' ? name + 1 : name;
    void *p = NULL;
    const variant_ent *v = bsearch(name, variants, sizeof variants / sizeof *variants, sizeof *variants, variant_cmp);
    if (v) {
        /* only where the table's library defines it: another library's own
         * opendir or reallocarray keeps its name */
        void *same = native_dlsym(h, sym, flat);
        p = native_dlsym(h, v->native + 1, flat);
        if (p && (!defined_in(p, v->lib) || (same && !defined_in(same, v->lib)))) p = NULL;
        if (hao_verbose > 1)
            fprintf(stderr, "hao: %s -> %s%s\n", name, p ? v->native : name, p ? "" : " (variant not in this library)");
    }
    const char *nname = p ? v->native : name;
    if (!p) p = native_dlsym(h, sym, flat);
    if (!p) return false;
    /* a function an interposer answers under another of its names (a renamed
     * re-export, an alias at the same address): its trap, as for that name */
    uint64_t trap;
    if (hao_bridge_trap_at((uint64_t)(uintptr_t)p, &trap) > 0) {
        if (hao_verbose > 1) fprintf(stderr, "hao: %s -> trap %#llx (another name of its function)\n", name, (unsigned long long)trap);
        hao_bridge_bind_trap(name, HAO_BRS_TRAP);
        *addr = trap;
        return true;
    }
    /* a variadic native function no interposer table handles (varsniff.c) */
    if (hao_varsniff_bind(name, (uint64_t)(uintptr_t)p, addr)) { hao_bridge_bind_trap(name, HAO_BRS_VARSNIFF); return true; }
    /* its plan: interface unit, compiled-in tables, Swift async narrow result */
    hao_bridge_bind(name, nname, (uint64_t)(uintptr_t)p);
    *addr = (uint64_t)(uintptr_t)p;
    return true;
}

static bool guest_lookup(hao_image *im, const char *name, uint64_t *addr, int depth);

/* `name` as exported by dependency `ord` of `im` (two-level, no flat search). */
static bool dep_lookup(hao_image *im, int ord, const char *name, uint64_t *addr, int depth)
{
    void *h = dep_handle(im, ord);
    if (h == GUEST_DEP) return guest_lookup(im->dep_image[ord - 1], name, addr, depth);
    if (h == MISSING_DEP || h == RTLD_DEFAULT) return false;
    /* a guest image's re-export of a native symbol binds natively: its ABI repair applies */
    if (hao_interpose_abi_repair(name) && interposed(name, addr, true)) return true;
    return native_sym(h, name, false, addr);
}

/* `name` as exported by a guest image: its own definitions, trie re-exports,
 * then the libraries it re-exports whole (LC_REEXPORT_DYLIB). */
static bool guest_lookup(hao_image *im, const char *name, uint64_t *addr, int depth)
{
    if (!im || depth > 32) return false;
    int ord = 0;
    const char *imp = NULL;
    switch (hao_image_export_info(im, name, addr, &ord, &imp)) {
    case HAO_EXPORT_ADDR: return true;
    case HAO_EXPORT_REEXPORT: return dep_lookup(im, ord, imp ? imp : name, addr, depth + 1);
    default: break;
    }
    for (int k = 0; k < im->ndeps; k++)
        if (im->dep_reexport[k] && dep_lookup(im, k + 1, name, addr, depth + 1)) return true;
    return false;
}

uint64_t hao_resolve_import(hao_image *img, int ordinal, const char *name, bool weak, bool *found)
{
    uint64_t a = 0;
    *found = true;
    lock_loads();
    /* Load the library the import names even when a trap answers it (a trap
     * finds its native function with dlsym(RTLD_DEFAULT) at call time). */
    if (ordinal > 0) (void)dep_handle(img, ordinal);
    /* runtime interposers first; ABI repairs only where the import binds natively */
    if (interposed(name, &a, false)) goto out;
    if (ordinal == 0 && guest_lookup(img, name, &a, 0)) goto out;
    if (ordinal == -1 && guest_lookup(hao_main_image, name, &a, 0)) goto out;
    if (ordinal <= 0) {
        /* flat and weak-definition lookups: guest images in load order, then native */
        size_t n;
        hao_image **ims = images_in_order(&n);
        bool hit = false;
        for (size_t k = 0; k < n && !hit; k++) hit = guest_lookup(ims[k], name, &a, 0);
        free(ims);
        if (hit) goto out;
        if (interposed(name, &a, true) || native_sym(RTLD_DEFAULT, name, true, &a)) goto out;
    } else {
        void *h = dep_handle(img, ordinal);
        if (h == GUEST_DEP) {
            if (guest_lookup(img->dep_image[ordinal - 1], name, &a, 0)) goto out;
        } else if (h == MISSING_DEP && img->dep_weak[ordinal - 1]) {
            a = 0;  /* a missing weak library: every symbol from it is NULL */
            goto out;
        } else if (interposed(name, &a, true) || native_sym(h == MISSING_DEP ? RTLD_DEFAULT : h, name, true, &a)) goto out;
    }
    *found = false;
    unlock_loads();
    if (weak) return 0;
    fprintf(stderr, "hao: unresolved import %s (ordinal %d) in %s\n", name, ordinal, img->path);
    /* A missing class is nil, as for a missing weak import: guest messages to
     * it return zero, and the ObjC registration ignores the image's classes
     * that derive from it (and its categories), as the native runtime does.
     * A trap address here would be dereferenced as a class (objc_readClassPair
     * reads superclass->data) and fault. Swift class metadata ("$s...CN") is
     * what Swift subclasses and extensions (categories) point at. */
    size_t len = strlen(name);
    if (!strncmp(name, "_OBJC_CLASS_$_", 14) || !strncmp(name, "_OBJC_METACLASS_$_", 18) ||
        (!strncmp(name, "_$s", 3) && len > 5 && !strcmp(name + len - 2, "CN")))
        return 0;
    /* A call lands in a trap that names the symbol instead of jumping to 0. */
    return unresolved_trap(name);
out:
    unlock_loads();
    return a;
}

/* ---------------------------------------------------- initializers */

extern void hao_guest_image_init(hao_image *);

/* Dependencies first; upward dependencies do not order (they may be cycles). */
static void init_tree(hao_image *im, int depth)
{
    if (im->init_state || depth > 512) return;
    im->init_state = 1;
    for (int k = 0; k < im->ndeps; k++)
        if (!im->dep_upward[k] && im->dep_handle[k] == GUEST_DEP && im->dep_image[k]) init_tree(im->dep_image[k], depth + 1);
    hao_guest_image_init(im);
    im->init_state = 2;
}

/*
 * dyld's order: every dependency of every loaded image is loaded and bound
 * first (also libraries no import refers to), then the images initialise
 * bottom-up, the main executable last. Images loaded later (lazy loads)
 * initialise when the next call finds them.
 */
void hao_run_initializers(void)
{
    lock_loads();
    hao_image **ims;
    size_t n, done = 0;
    for (;;) {
        ims = images_in_order(&n);
        if (done >= n) break;
        for (size_t k = done; k < n; k++)
            for (int d = 0; d < ims[k]->ndeps; d++) (void)dep_handle(ims[k], d + 1);
        free(ims);
        done = n;
    }
    for (size_t k = 0; k < n; k++) init_tree(ims[k], 0);
    free(ims);
    launched = true;
    unlock_loads();
}

/* Native CFBundle uses dlopen internally. A failed native open of an ARM-only
 * MH_BUNDLE may use the same validated loader/initializers as a guest dependency.
 * Never retry a universal bundle's native validation failure as guest code. */
struct bundle_handle { struct bundle_handle *next; hao_image *image; unsigned refs; bool first, global; };
static struct bundle_handle *bundle_handles;
static struct bundle_handle *bundle_handle(void *handle)
{
    for (struct bundle_handle *h = bundle_handles; h; h = h->next)
        if (h == handle) return h;
    return NULL;
}
/* The guest image whose code called dlopen/dlsym, when the interposer's return address `ra` is in the bridge's
 * native-call trampolines (guest code called the function); NULL for a native caller */
void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack, uint64_t nstack, uint64_t out[6]);
static hao_image *guest_caller_image(const void *ra)
{
    uint64_t a = (uint64_t)(uintptr_t)ra;
    if (a - (uint64_t)(uintptr_t)hao_native_call >= 0x400 && a - (uint64_t)(uintptr_t)hao_sysv_call >= 0x400) return NULL;
    hao_thread *t = hao_current_peek();
    hao_image *im = t ? hao_image_for_code(t->cpu.x[30]) : NULL;
    return im ? im : hao_main_image;
}
struct first_file { char *out; size_t n; bool found; };
static bool first_file(const char *cand, bool os, void *ctx)
{
    (void)os;
    struct first_file *ff = ctx;
    if (access(cand, F_OK) != 0) return false;
    ff->found = (size_t)snprintf(ff->out, ff->n, "%s", cand) < ff->n;
    return ff->found;
}
/* An @rpath, @loader_path or @executable_path name guest code dlopens: the first existing candidate dyld would try
 * from the calling guest image (its LC_RPATHs, its loaders', the main executable's; set-ID rules included) */
static int native_bundle_expand(const char *path, const void *ra, char *out, size_t n)
{
    hao_image *im = path && path[0] == '@' ? guest_caller_image(ra) : NULL;
    if (!im) return 0;
    struct first_file ff = {out, n, false};
    const char *refused;
    lock_loads();
    for_each_candidate(im, path, first_file, &ff, &refused);
    unlock_loads();
    return ff.found;
}
static void *bundle_open(const char *path, int flags, bool dylib, char *why, size_t n);
static void *native_bundle_open(const char *path, int flags, char *why, size_t n) { return bundle_open(path, flags, false, why, n); }
/* a guest caller's dlopen may also load an ARM-only dylib as a guest image; a native caller's keeps the bundle rule */
static void *native_bundle_open2(const char *path, int flags, const void *ra, char *why, size_t n)
{
    return bundle_open(path, flags, guest_caller_image(ra) != NULL, why, n);
}
static void *bundle_open(const char *path, int flags, bool dylib, char *why, size_t n)
{
    if (!path || path[0] != '/' || !hao_is_arm_only_loadable(path, dylib)) return NULL;
    const int supported = RTLD_LAZY | RTLD_NOW | RTLD_LOCAL | RTLD_GLOBAL | RTLD_NOLOAD | RTLD_NODELETE | RTLD_FIRST;
    if (flags & ~supported) { snprintf(why, n, "unsupported guest bundle dlopen flags"); return NULL; }
    char real[PATH_MAX];
    if (!realpath(path, real)) return NULL;
    lock_loads();
    hao_image *im = glib_find(real);
    if (!im && !(flags & RTLD_NOLOAD)) {
        intptr_t slot = glib_slot(real, hao_main_image);
        im = dylib ? hao_load_dlopen_cb(real, hao_resolve_import, glib_mapped, (void *)slot, last_load_err, sizeof last_load_err)
                   : hao_load_bundle_cb(real, hao_resolve_import, glib_mapped, (void *)slot, last_load_err, sizeof last_load_err);
        if (!im) { free(glibs[slot].path); glibs[slot].path = NULL; glibs[slot].img = NULL; }
    }
    if (!im) {
        if (!(flags & RTLD_NOLOAD)) snprintf(why, n, "%s", last_load_err);
        unlock_loads(); return NULL;
    }
    struct bundle_handle *h;
    for (h = bundle_handles; h && (h->image != im || h->first != !!(flags & RTLD_FIRST)); h = h->next) {}
    if (!h) {
        h = calloc(1, sizeof *h);
        if (!h) { snprintf(why, n, "guest bundle handle allocation failed"); unlock_loads(); return NULL; }
        h->image = im; h->first = !!(flags & RTLD_FIRST); h->next = bundle_handles; bundle_handles = h;
    }
    h->global |= !(flags & RTLD_LOCAL);
    h->refs++;
    hao_run_initializers();
    unlock_loads();
    return h;
}
static int bundle_sym(void *handle, const char *name, bool guest_caller, void **result);
static int native_bundle_sym(void *handle, const char *name, void **result) { return bundle_sym(handle, name, false, result); }
static int native_bundle_sym2(void *handle, const char *name, const void *ra, void **result)
{
    return bundle_sym(handle, name, guest_caller_image(ra) != NULL, result);
}
static int bundle_sym(void *handle, const char *name, bool guest_caller, void **result)
{
    lock_loads();
    struct bundle_handle *h = bundle_handle(handle);
    if (!h && handle != RTLD_DEFAULT) { unlock_loads(); return -1; }
    uint64_t address = 0;
    char symbol[2048];
    bool found = false;
    if (name && snprintf(symbol, sizeof symbol, "_%s", name) < (int)sizeof symbol) {
        if (h) {
            if (h->first) found = hao_image_export_info(h->image, symbol, &address, NULL, NULL) == HAO_EXPORT_ADDR;
            else found = guest_lookup(h->image, symbol, &address, 0);
        } else {
            for (h = bundle_handles; h && !found; h = h->next)
                if (h->global) found = guest_lookup(h->image, symbol, &address, 0);
        }
    }
    /* ObjC principal classes use registered method encodings. Untyped C function exports have no
     * aggregate/result contract for a native caller and are refused there; guest code calls the guest
     * function itself, so a guest caller gets its address. */
    if (found && !guest_caller && hao_image_for_code(address)) found = false;
    *result = found ? (void *)(uintptr_t)address : NULL;
    unlock_loads();
    return found;
}
static int native_bundle_close(void *handle)
{
    lock_loads();
    struct bundle_handle *h = bundle_handle(handle);
    if (!h) { unlock_loads(); return -1; }
    if (h->refs) h->refs--;
    /* Registered ObjC classes and escaping callbacks retain code identities.
     * Keep guest images resident, as the existing dependency loader does. */
    unlock_loads();
    return 0;
}
static const char *native_bundle_class(const void *cls)
{
    hao_identity_info info;
    return hao_identity_guest_image(cls, &info) ? info.path : NULL;
}
void hao_native_bundles_init(void)
{
    hao_bundle_install2_fn install2 = (hao_bundle_install2_fn)dlsym(RTLD_DEFAULT, "hao_identity_bundle_hooks2");
    if (install2) {
        native_open = install2(native_bundle_open2, native_bundle_sym2, native_bundle_close, native_bundle_class, native_bundle_expand);
        return;
    }
    hao_bundle_install_fn install = (hao_bundle_install_fn)dlsym(RTLD_DEFAULT, "hao_identity_bundle_hooks");
    if (install) native_open = install(native_bundle_open, native_bundle_sym, native_bundle_close, native_bundle_class);
}

/* ---------------------------------------------------- lazy loading */

/*
 * LC_LAZY_LOAD_DYLIB_INFO (ld -lazy_library; xpcproxy uses it for
 * XPCSupport and libcryptex_trampoline). One blob per library:
 *   u32 path offset, u32 state offset (image-relative u32 flag),
 *   u16 version, u16 pointer format of the slots, u32 slots offset
 *   (image-relative, 8-byte slots), u32 count, u32 name-table offset,
 *   u32 name offsets[count], then the strings (offsets are blob-relative).
 * Each symbol's stub in __TEXT,__lazy_helpers tests the flag; while it is 0
 * the stub calls __dyld_lazy_load(&flag, mach_header), saving only x0/x1,
 * then branches through the slot.
 */
static uint32_t rd32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

static bool lazy_bind(hao_image *img, const uint8_t *b, uint32_t size)
{
    uint32_t path_off = rd32(b), state_off = rd32(b + 4), slots_off = rd32(b + 12), count = rd32(b + 16),
             names_off = rd32(b + 20);
    if (path_off >= size || names_off > size || count > (size - names_off) / 4 || slots_off >= img->vmsize ||
        count > (img->vmsize - slots_off) / 8 || state_off > img->vmsize - 4)
        return false;
    const char *lib = (const char *)b + path_off;
    hao_image *g = NULL;
    void *h = open_library(img, lib, false, &g, NULL, 0);
    if (h == MISSING_DEP) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "lazy-loaded library %s not found (native or guest)\n", lib);
    }
    if (g) hao_run_initializers();
    uint64_t *slot = (uint64_t *)(uintptr_t)(img->base + slots_off);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t no = rd32(b + names_off + 4 * k);
        if (no >= size) return false;
        const char *name = (const char *)b + no;
        uint64_t a = 0;
        bool ok = interposed(name, &a, false) ||
                  (h == GUEST_DEP ? guest_lookup(g, name, &a, 0)
                                  : h != MISSING_DEP && (interposed(name, &a, true) || native_sym(h, name, true, &a)));
        if (!ok) {
            if (h != MISSING_DEP) fprintf(stderr, "hao: unresolved lazy import %s from %s in %s\n", name, lib, img->path);
            a = unresolved_trap(name);
        }
        slot[k] = a;
    }
    if (hao_verbose) fprintf(stderr, "hao: lazy-loaded %s for %s (%u symbols)\n", lib, img->path, count);
    return true;
}

static void trap_dyld_lazy_load(hao_thread *t, uint64_t arg)
{
    (void)arg;
    uint64_t state = t->cpu.x[0], mh = t->cpu.x[1];
    lock_loads();
    hao_image *img = hao_image_for_addr(mh);
    bool done = false;
    if (img && img->base == mh && __atomic_load_n((uint32_t *)(uintptr_t)state, __ATOMIC_ACQUIRE)) done = true;
    for (int k = 0; img && img->base == mh && !done && k < img->nlazy; k++)
        if (img->base + rd32(img->lazy_info[k] + 4) == state && lazy_bind(img, img->lazy_info[k], img->lazy_info_size[k])) {
            __atomic_store_n((uint32_t *)(uintptr_t)state, 1, __ATOMIC_RELEASE);
            done = true;
        }
    unlock_loads();
    if (done) return;
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG, "__dyld_lazy_load(%#llx, %#llx): no lazy-load record for this image\n", (unsigned long long)state,
            (unsigned long long)mh);
    abort();
}

/* The runtime's own traps, first in hao_interpose_lookup's order. */
static const struct { const char *name; hao_trap_fn trap; } runtime_traps[] = {
    {"__dyld_lazy_load", trap_dyld_lazy_load},
};
static hao_name_index runtime_ix = HAO_NAME_INDEX(runtime_traps, NULL);

bool hao_runtime_interpose(const char *name, uint64_t *addr)
{
    long k = hao_name_find(&runtime_ix, name);
    if (k < 0) return false;
    *addr = hao_trap_address(runtime_traps[k].name, runtime_traps[k].trap, 0);
    return true;
}

void hao_runtime_interpose_names(hao_name_fn cb, void *ctx) { hao_name_each(&runtime_ix, cb, ctx); }
