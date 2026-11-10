/*
 * hao — native entry points for guest blocks.
 *
 * A block literal is { isa, flags, reserved, invoke, descriptor, ... };
 * its descriptor is { reserved, size, [copy, dispose,] [signature, layout] }
 * (libclosure Block_private.h). Native code (libdispatch, libxpc,
 * libclosure's _Block_copy/_Block_release) calls invoke, copy and dispose
 * directly. For a guest block those are arm64 functions on non-executable
 * pages, so every such call faulted into the translator. Before a guest
 * block reaches native code, hao points invoke (with a signature thunk when
 * the block's signature needs marshalling) and the copy/dispose helpers at
 * x86 thunks. Heap copies made by native _Block_copy inherit them. Guest
 * code that calls a patched invoke itself lands on a thunk, which the
 * dispatcher maps back to the guest function (hao_guest_of_entry).
 *
 * __block variables work the same way: a byref structure { isa (0, or 1
 * for __weak), forwarding, flags, size, keep, destroy, ... } reaches native
 * _Block_object_assign on the guest stack (forwarding == itself); its
 * keep/destroy helpers get thunks there and the heap copy inherits them.
 *
 * Covered: blocks and byrefs passed in x0-x7 or the first stack argument
 * slots to native functions (blocks also to native methods), on the guest
 * stack or in guest data; blocks also inside writable heap storage (Swift
 * async frames included), and every global block of a guest image at load
 * time. A block or byref that reaches native code another way (inside a
 * structure, or as a heap copy native code made from such a block) faults
 * once per helper: the fault path recognises it by its first argument and
 * prepares it (hao_blocks_heal). Not covered: copy/dispose of blocks with
 * small (relative) descriptors.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

extern void *_NSConcreteStackBlock[32];
extern void *_NSConcreteGlobalBlock[32];
extern void *_NSConcreteMallocBlock[32];

enum {
    BLOCK_SMALL_DESCRIPTOR = 1 << 22,
    BLOCK_HAS_COPY_DISPOSE = 1 << 25,
    BLOCK_IS_GLOBAL        = 1 << 28,
    BLOCK_HAS_SIGNATURE    = 1 << 30,
    BLOCK_BYREF_HAS_COPY_DISPOSE = 1 << 25,
};

static uint64_t st_invokes, st_helpers, st_global, st_byref, st_healed;

/* Off with HAO_NO_BLOCK_THUNKS=1 in an ordinary process's environment, or,
 * in a HAO_TEST_SWITCHES build, with the boot argument
 * hao_no_block_thunks=1 (every translated process reads it, so a whole
 * boot, PID 1 included, can be bisected). */
static bool enabled(void)
{
    return !hao_opt.no_block_thunks;
}

/* [a, a+n) lies inside one guest image's mapped range */
static bool in_guest_image(uint64_t a, uint64_t n)
{
    hao_image *im = hao_image_for_addr(a);
    return im && hao_image_extent(im, a) - a >= n;
}

/* [a, a+n) lies inside this thread's guest stack (no wrap-around for a near 2^64) */
static bool on_guest_stack(const hao_thread *t, uint64_t a, uint64_t n)
{
    uint64_t lo = (uint64_t)(uintptr_t)t->stack, hi = lo + t->stack_size;
    return a >= lo && a < hi && hi - a >= n;
}

/* Replace the guest function at *slot with its entry point; true if this call did it. */
static bool patch(uint64_t *slot, uint64_t guest, uint64_t entry)
{
    if (entry == guest) return false;
    return __atomic_compare_exchange_n(slot, &guest, entry, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

/* (guest invoke, canonical parsed signature) -> native entry. A plain first
 * encounter must not hide a later known signature. abi.c interns by copied
 * encoding content, so descriptor/string address reuse cannot alias types.
 * Typed thunks use the same stable identity in jit.c. Lock-free map reads. */
static hao_gmap invokes = HAO_GMAP_INIT(3, 2, 8, 15);
static pthread_mutex_t invoke_lock = PTHREAD_MUTEX_INITIALIZER;

/* fork child: see the lock comment in jit.c */
void hao_blocks_fork_child(void) { invoke_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

static uint64_t entry_for_invoke(uint64_t guest, const char *sig)
{
    if (!guest) return 0;
    uint64_t key[2] = {guest, (uint64_t)(uintptr_t)hao_sig_identity(sig)};
    const uint64_t *c = hao_gmap_get(&invokes, key);
    if (c) return c[2];
    uint64_t e = 0;
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&invoke_lock);
    uint64_t *s = hao_gmap_claim(&invokes, key, &fresh);
    if (s && !fresh) e = s[2];
    else if (s) {
        e = sig ? hao_guest_entry_for_sig(guest, sig) : hao_thunk_for(guest);
        /* thunk area full (logged there): keep the fault path, retry
         * later. Without executable memory the guest address is the
         * entry for good (its signature, if any, was noted for
         * hao_callback). */
        if (!(e == guest && hao_have_thunks())) { s[2] = e; hao_gmap_publish(&invokes, s, key); }
    }
    pthread_mutex_unlock(&invoke_lock);
    hao_crit_leave();
    if (!s) {
        /* table full: the same entry, uncached (hao_guest_entry_for_sig
         * shares signature thunks itself) */
        static int full;
        if (!__atomic_exchange_n(&full, 1, __ATOMIC_RELAXED)) {
            hao_diag_prefix(HAO_DIAG);
            fprintf(HAO_DIAG, "block invoke table full: invoke entries are no longer cached\n");
        }
        e = sig ? hao_guest_entry_for_sig(guest, sig) : hao_thunk_for(guest);
    }
    return e;
}

/* Exact compiler descriptor signature, with the same guest-image bounds used
 * for preparation. No speculative native descriptor or arbitrary heap string. */
static const char *block_signature(const uint64_t *b)
{
    uint32_t flags = (uint32_t)b[1];
    uint64_t desc = b[3];
    if (!(flags & BLOCK_HAS_SIGNATURE) || (flags & BLOCK_SMALL_DESCRIPTOR) || !desc || (desc & 7) ||
        !in_guest_image(desc, 16 + ((flags & BLOCK_HAS_COPY_DISPOSE) ? 16 : 0) + 8)) return NULL;
    const uint64_t *d = (const void *)(uintptr_t)desc;
    uint64_t a = d[(flags & BLOCK_HAS_COPY_DISPOSE) ? 4 : 2];
    return a && in_guest_image(a, 1) ? (const char *)(uintptr_t)a : NULL;
}

/* b points at a readable block literal of at least 32 bytes */
static void prepare_block(uint64_t *b)
{
    uint32_t flags = (uint32_t)b[1];
    uint64_t desc = b[3];
    bool desc_ok = !(flags & BLOCK_SMALL_DESCRIPTOR) && desc && !(desc & 7) &&
                   in_guest_image(desc, 16 + ((flags & BLOCK_HAS_COPY_DISPOSE) ? 16 : 0) + ((flags & BLOCK_HAS_SIGNATURE) ? 8 : 0));
    uint64_t *d = desc_ok ? (uint64_t *)(uintptr_t)desc : NULL;

    uint64_t inv = b[2];
    uint64_t guest = hao_thunk_target(inv);   /* ordinary plain thunk only */
    if (!guest) guest = inv;
    if (hao_is_guest_function(guest)) {
        const char *sig = block_signature(b);
        /* Keep previously plain nonvoid/unknown entries on their old path.
         * Compiler void encodings start with v; other forms remain generic. */
        if (guest == inv || (sig && sig[0] == 'v'))
            if (patch(&b[2], inv, entry_for_invoke(guest, sig))) __atomic_fetch_add(&st_invokes, 1, __ATOMIC_RELAXED);
    }
    if (d && (flags & BLOCK_HAS_COPY_DISPOSE)) {
        /* descriptors are shared, constant data in the guest image (its
         * __DATA_CONST stays writable under hao); patching them is
         * idempotent and each helper is patched once */
        for (int k = 2; k <= 3; k++) {
            uint64_t h = d[k];
            if (!hao_is_guest_function(h)) continue;
            if (patch(&d[k], h, hao_thunk_for(h))) __atomic_fetch_add(&st_helpers, 1, __ATOMIC_RELAXED);
        }
    }
}

static bool is_block_isa(uint64_t isa)
{
    return isa == (uint64_t)(uintptr_t)_NSConcreteStackBlock || isa == (uint64_t)(uintptr_t)_NSConcreteGlobalBlock ||
           isa == (uint64_t)(uintptr_t)_NSConcreteMallocBlock;
}

/* a __block byref on the guest stack: thunk its keep/destroy helpers */
static void prepare_byref(uint64_t *r)
{
    for (int k = 2; k <= 3; k++) {
        uint64_t h = r[k + 1];      /* keep at +24, destroy at +32 */
        if (hao_is_guest_function(h) && patch(&r[k + 1], h, hao_thunk_for(h))) __atomic_fetch_add(&st_byref, 1, __ATOMIC_RELAXED);
    }
}

/* N words at A into W, false if any is unreadable. A fault-tolerant load
 * per word (hao_probe_read8: no system call; a fault, which only an
 * unmapped or unreadable value takes, ends in on_fault and resumes at its
 * fail label) where the thread may fault, else one mach_vm_read_overwrite.
 * Never libmalloc: malloc_zone_from_ptr asks every zone for the size of a
 * value, and the tiny allocator logs "NO ZONE for ptr" for a region base
 * (stale caller-saved registers hold them after native malloc code).
 *
 * A fault costs far more than the Mach call (a signal delivery: 18 us under
 * Rosetta against 0.8 us), and a stale value tends to come back on every
 * call (tests/bridge/perf cxxg: 17 pages, 20 M calls), so a page whose read
 * faulted is remembered (a direct-mapped table, process-wide, no lock) with
 * the address-space generation it was unreadable in: libhao_identity counts
 * every call that can map memory or make it readable (mmap, mprotect, the
 * mach_vm and vm map/allocate/remap/protect/read calls, shmat; hao_vm_gen).
 * While the generation is the same the page reads as unreadable with no
 * system call; after a change, Mach decides once (and a page that reads
 * now is forgotten). The heal (RW_EXACT) asks Mach for every remembered
 * page. */
#define UNREADABLE_SLOTS 256
#define UR_PAGE ((1ull << 36) - 1)   /* low bits: page number + 1 (pages below 2^47); high 28: generation */
static uint64_t unreadable[UNREADABLE_SLOTS];

static uint64_t *unreadable_slot(uint64_t a)
{
    return &unreadable[((a >> 12) * 0x9E3779B97F4A7C15ull) >> 56];
}

static void unreadable_note(uint64_t *s, uint64_t a, uint64_t gen)
{
    __atomic_store_n(s, ((a >> 12) + 1) | (gen << 36), __ATOMIC_RELAXED);
}

static bool mach_read(uint64_t a, uint64_t *w, unsigned n)
{
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), a, 8 * n, (mach_vm_address_t)(uintptr_t)w, &copied) == KERN_SUCCESS &&
           copied == 8 * n;
}

/* RW_SCAN: the argument scan; RW_EXACT: a remembered page is read through Mach whatever the generation */
enum { RW_SCAN, RW_EXACT };

static bool read_words(const hao_thread *t, uint64_t a, uint64_t *w, unsigned n, int mode)
{
    if (!hao_probe_safe(t)) return mach_read(a, w, n);
    /* before any read: a mapping made after it changes the generation the page is noted with */
    const uint64_t *g = hao_vm_gen;
    uint64_t gen = g ? __atomic_load_n(g, __ATOMIC_ACQUIRE) & ((1ull << 28) - 1) : 0;
    for (unsigned k = 0; k < n; k++) {
        uint64_t f = a + 8 * k, *s = unreadable_slot(f), e = __atomic_load_n(s, __ATOMIC_RELAXED);
        if ((e & UR_PAGE) == (f >> 12) + 1) {
            if (mode == RW_SCAN && g && (e >> 36) == gen) return false;
            if (!mach_read(f, &w[k], 1)) { unreadable_note(s, f, gen); return false; }
            __atomic_compare_exchange_n(s, &e, 0, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);   /* readable now */
            continue;
        }
        if (!hao_probe_addr_ok(f) || ((f & 7) && !hao_probe_addr_ok(f + 7))) {
            /* Check both ends of the newly admitted unaligned word too. A
             * fault in the main-stack guard would kill the process. */
            if (!mach_read(f, &w[k], 1)) return false;
            continue;
        }
        if (!hao_probe_read8(f, &w[k])) { unreadable_note(s, f, gen); return false; }
    }
    return true;
}

/* Shared word scans use the same signal guards and unreadable-page cache.
 * An unaligned single word is allowed only wholly within one page. */
bool hao_probe_read_words(const hao_thread *t, uint64_t a, uint64_t *w, unsigned n)
{
    bool single_page_word = n == 1 && (a & 4095) <= 4096 - 8;
    if (!a || ((a & 7) && !single_page_word) || !n || n > UINT32_MAX / 8 || a > UINT64_MAX - 8ull * n) return false;
    return read_words(t, a, w, n, RW_SCAN);
}

/* [a, a+n) lies in one writable VM region (one Mach call: only for a value
 * that already reads as a guest block or byref) */
static bool writable(uint64_t a, uint64_t n)
{
    mach_vm_address_t region = a;
    mach_vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &region, &bytes,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && region <= a && bytes >= n && a - region <= bytes - n && (info.protection & VM_PROT_WRITE);
}

/* Swift async frames contain stack-block literals in heap storage, and
 * guest code moves block literals into the interior of allocations, where
 * malloc_size cannot validate them. A value outside the guest stack, the
 * guest images and the shared cache is read word by word (read_words):
 * only a known block ISA, guest invoke and guest descriptor opt into
 * rewriting, and its VM region must be writable. Scalars and invalid
 * pointers remain untouched. */
extern const void *_dyld_get_shared_cache_range(size_t *length);
static uint64_t block_cache_lo, block_cache_hi;
static pthread_once_t block_cache_once = PTHREAD_ONCE_INIT;
static void block_cache_bounds(void)
{
    size_t size = 0;
    block_cache_lo = (uint64_t)(uintptr_t)_dyld_get_shared_cache_range(&size);
    block_cache_hi = block_cache_lo + size;
}
static bool heap_candidate(uint64_t a)
{
    if (a < 0x100000000ull || a >= 0x0000800000000000ull || (a & 7)) return false;
    pthread_once(&block_cache_once, block_cache_bounds);
    return a < block_cache_lo || a >= block_cache_hi;
}
static bool heap_guest_block(hao_thread *t, uint64_t a)
{
    if (!heap_candidate(a)) return false;
    uint64_t b[4];
    if (!read_words(t, a, b, 1, RW_SCAN) || !is_block_isa(b[0]) || !read_words(t, a + 8, b + 1, 3, RW_SCAN)) return false;
    uint64_t guest = hao_thunk_target(b[2]);
    if (!guest) guest = b[2];
    if (!hao_is_guest_function(guest) || !in_guest_image(b[3], 16)) return false;
    if (guest != b[2]) {
        /* A healed plain block is not new work unless its authoritative void
         * descriptor resolves to a different typed entry. In particular do
         * not add a VM-region query on every ordinary nonvoid block visit. */
        const char *sig = block_signature(b);
        if (!sig || sig[0] != 'v') return false;
        uint64_t entry = entry_for_invoke(guest, sig);
        if (entry == b[2] || entry == guest) return false;
    }
    return writable(a, sizeof b);
}

/* The first word of a value that lies on the guest stack, in a guest image or (read_words checked) on
 * the heap: a guest function pointer lies in a guest image too, and its first word is code (hao/code.h) */
static uint64_t first_word(const uint64_t *b, bool stack)
{
    uint64_t w;
    memcpy(&w, stack ? (const void *)b : hao_code_view((uint64_t)(uintptr_t)b, 8), 8);
    return w;
}

static void prepare_arg(hao_thread *t, uint64_t a)
{
    if (!a || (a & 7)) return;
    bool stack = on_guest_stack(t, a, 48);
    if (!stack && !in_guest_image(a, 32) && !heap_guest_block(t, a)) return;
    uint64_t *b = (uint64_t *)(uintptr_t)a;
    if (is_block_isa(first_word(b, stack))) { prepare_block(b); return; }
    if (stack && b[0] <= 1 && b[1] == a && ((uint32_t)b[2] & BLOCK_BYREF_HAS_COPY_DISPOSE) && (uint32_t)(b[2] >> 32) >= 48)
        prepare_byref(b);
}

/* Stack argument slots scanned for blocks at each native call. Only the
 * blocks and byrefs they point to are prepared; the slots themselves are
 * never rewritten (a slot holding a guest function address may be a saved
 * register or a local, not an argument). */
#define STACK_ARG_SLOTS 8

/* A native function is about to receive the guest's x0-x7: give guest
 * function pointers, blocks and byrefs among them native entry points.
 * Blocks among the first stack argument slots are prepared too. mask
 * (hao/bridge.h argmask) names the slots a typed plan says may carry guest
 * code: bits 0-7 x0-x7, bits 8-15 the stack slots; HAO_BR_SCAN_ALL is the
 * untyped scan. */
void hao_thunk_native_args_mask(hao_thread *t, unsigned mask)
{
    hao_swift_objects_args(t);
    hao_cpu *c = &t->cpu;
    bool on = enabled();
    for (unsigned m = mask & 0xffu; m; m &= m - 1) {
        int k = __builtin_ctz(m);
        uint64_t a = c->x[k];
        if (!a) continue;
        if (hao_is_guest_function(a)) { c->x[k] = hao_thunk_for(a); continue; }
        if (on) prepare_arg(t, a);
    }
    if (!on || !(mask >> 8) || !on_guest_stack(t, c->x[31], 8 * STACK_ARG_SLOTS)) return;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (unsigned m = (mask >> 8) & 0xffu; m; m &= m - 1) {
        int k = __builtin_ctz(m);
        if (sp[k]) prepare_arg(t, sp[k]);
    }
}

void hao_thunk_native_args(hao_thread *t) { hao_thunk_native_args_mask(t, HAO_BR_SCAN_ALL); }

/* One argument of a generated thunk pair's native half (hao/bridge.h) that
 * may carry guest code: what the argument scan does for one slot. A guest
 * function with a known, non-trivial signature gets a signature thunk. */
uint64_t hao_bridge_code_arg(uint64_t a, const char *enc)
{
    hao_swift_object_prepare(a);
    if (!a) return a;
    if (hao_is_guest_function(a)) return enc && !hao_sig_simple(enc) ? hao_guest_entry_for_sig(a, enc) : hao_thunk_for(a);
    if (enabled()) prepare_arg(hao_current(), a);
    return a;
}

/* a points at a guest block literal or a __block byref prepare_arg would act on */
static bool code_object(hao_thread *t, uint64_t a)
{
    if (!a || (a & 7)) return false;
    bool stack = on_guest_stack(t, a, 48);
    if (!stack && !in_guest_image(a, 32) && !heap_guest_block(t, a)) return false;
    const uint64_t *b = (const uint64_t *)(uintptr_t)a;
    if (is_block_isa(first_word(b, stack))) return true;
    return stack && b[0] <= 1 && b[1] == a && ((uint32_t)b[2] & BLOCK_BYREF_HAS_COPY_DISPOSE) && (uint32_t)(b[2] >> 32) >= 48;
}

/* HAO_BRIDGE_CHECK: the slots (argmask bits) the full scan would act on --
 * a guest function, block or byref -- without changing anything. */
unsigned hao_args_code_slots(hao_thread *t)
{
    hao_cpu *c = &t->cpu;
    unsigned r = 0;
    for (int k = 0; k < 8; k++) {
        uint64_t a = c->x[k];
        if (a && (hao_is_guest_function(a) || code_object(t, a))) r |= 1u << k;
    }
    if (!on_guest_stack(t, c->x[31], 8 * STACK_ARG_SLOTS)) return r;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < STACK_ARG_SLOTS; k++) if (code_object(t, sp[k])) r |= 1u << (8 + k);
    return r;
}

/* The NX-fault path: native code called guest function `target` with a0 in
 * rdi. Block invokes, copy/dispose helpers and byref keep/destroy helpers
 * all take their block or byref first. If a0 is one (a heap copy native
 * code made from a block hao never saw, a block inside a structure, ...)
 * whose helper is `target`, prepare it now so later calls use thunks. The
 * current call proceeds on the plain bridge as before. */
bool hao_blocks_heal(uint64_t a0, uint64_t target)
{
    if (!enabled() || !a0 || (a0 & 7)) return false;
    hao_thread *t = hao_current();
    bool stack = on_guest_stack(t, a0, 48), heap = false;
    uint64_t w[6];
    unsigned nw = 0;   /* words of a heap a0 known readable */
    if (!stack && !in_guest_image(a0, 32)) {
        /* a heap block or byref (read_words: not through libmalloc) */
        if (!heap_candidate(a0) || !read_words(t, a0, w, 4, RW_EXACT)) return false;
        heap = true;
        nw = 4;
        if (!is_block_isa(w[0]) && read_words(t, a0 + 32, w + 4, 2, RW_EXACT)) nw = 6;
    }
    uint64_t *b = (uint64_t *)(uintptr_t)a0;
    if (is_block_isa(heap ? b[0] : first_word(b, stack))) {
        uint32_t flags = (uint32_t)b[1];
        bool hit = b[2] == target;
        uint64_t desc = b[3];
        if (!hit && (flags & BLOCK_HAS_COPY_DISPOSE) && !(flags & BLOCK_SMALL_DESCRIPTOR) && desc && !(desc & 7) &&
            in_guest_image(desc, 32)) {
            const uint64_t *d = (const uint64_t *)(uintptr_t)desc;
            hit = d[2] == target || d[3] == target;
        }
        if (!hit || (heap && !writable(a0, 32))) return false;
        prepare_block(b);
        __atomic_fetch_add(&st_healed, 1, __ATOMIC_RELAXED);
        return true;
    }
    /* byref: {isa 0/1, forwarding (itself), flags, size, keep, destroy} */
    if ((stack || (heap && nw == 6)) && b[0] <= 1 && b[1] == a0 &&
        ((uint32_t)b[2] & BLOCK_BYREF_HAS_COPY_DISPOSE) && (uint32_t)(b[2] >> 32) >= 48 && (b[3] == target || b[4] == target) &&
        (!heap || writable(a0, 48))) {
        prepare_byref(b);
        __atomic_fetch_add(&st_healed, 1, __ATOMIC_RELAXED);
        return true;
    }
    return false;
}

/* A native method is about to receive x0-x7: blocks only. A slot holding
 * a guest function address keeps it here: only the method's encoding says
 * whether the slot is a function pointer the method will call or a value it
 * hands back (a void * context, an NSValue's pointer) that guest code
 * compares with its own address (hao_thunk_native_code_slots). */
void hao_thunk_native_blocks(hao_thread *t)
{
    hao_swift_objects_args(t);
    if (!enabled()) return;
    for (int k = 0; k < 8; k++) prepare_arg(t, t->cpu.x[k]);
}

/* A native method is about to receive the argument slots its encoding
 * types as code (hao_sig_msgsend: '^?' function pointers, '@?' blocks,
 * those fields of an aggregate passed by value): a guest function there gets
 * its thunk, a block or byref in a stack slot its helpers (hao_thunk_native_blocks
 * did x0-x7). The typed stack slots are the callee's incoming arguments, so
 * they are rewritten in place. */
void hao_thunk_native_code_slots(hao_thread *t, unsigned slots)
{
    hao_cpu *c = &t->cpu;
    for (unsigned m = slots & 0xffu; m; m &= m - 1) {
        int k = __builtin_ctz(m);
        if (c->x[k] && hao_is_guest_function(c->x[k])) c->x[k] = hao_thunk_for(c->x[k]);
    }
    if (!(slots >> 8) || !on_guest_stack(t, c->x[31], 8 * STACK_ARG_SLOTS)) return;
    uint64_t *sp = (uint64_t *)(uintptr_t)c->x[31];
    bool on = enabled();
    for (unsigned m = (slots >> 8) & 0xffu; m; m &= m - 1) {
        int k = __builtin_ctz(m);
        uint64_t a = sp[k];
        if (!a) continue;
        if (hao_is_guest_function(a)) sp[k] = hao_thunk_for(a);
        else if (on) prepare_arg(t, a);
    }
}

/* Load time: every global block literal in the image's data. */
void hao_blocks_register_image(hao_image *img)
{
    if (!enabled()) return;
    uint64_t isa = (uint64_t)(uintptr_t)_NSConcreteGlobalBlock;
    for (int s = 0; s < img->nsects; s++) {
        if (strncmp(img->sects[s].seg, "__DATA", 6) && strncmp(img->sects[s].seg, "__AUTH", 6)) continue;
        uint32_t type = img->sects[s].flags & SECTION_TYPE;   /* a block literal is never zero-fill */
        if (type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL) continue;
        uint64_t a = (img->sects[s].addr + 7) & ~7ull, end = img->sects[s].addr + img->sects[s].size;
        for (; a + 32 <= end; a += 8) {
            uint64_t *b = (uint64_t *)(uintptr_t)a;
            if (b[0] != isa || (b[1] >> 32) || !((uint32_t)b[1] & BLOCK_IS_GLOBAL) || !hao_is_guest_function(b[2])) continue;
            prepare_block(b);
            st_global++;
        }
    }
}

/* The native block at a, when its invoke function is inv and it carries an
 * ObjC signature: the signature (hao_bridge_kind_for_enc types the call).
 * Every read guarded: a is whatever x0 held at a call of inv. */
const char *hao_native_block_sig(uint64_t a, uint64_t inv)
{
    uint64_t b[4], d[5];
    mach_vm_size_t got = 0;
    if (!a || (a & 7) || a < 0x1000 || a >= 0x0000800000000000ull || hao_image_for_addr(a)) return NULL;
    if (mach_vm_read_overwrite(mach_task_self(), a, sizeof b, (mach_vm_address_t)(uintptr_t)b, &got) != KERN_SUCCESS ||
        got != sizeof b || !is_block_isa(b[0]) || b[2] != inv)
        return NULL;
    uint32_t flags = (uint32_t)b[1];
    if (!(flags & BLOCK_HAS_SIGNATURE) || (flags & BLOCK_SMALL_DESCRIPTOR) || !b[3] || (b[3] & 7)) return NULL;
    got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), b[3], sizeof d, (mach_vm_address_t)(uintptr_t)d, &got) != KERN_SUCCESS ||
        got != sizeof d)
        return NULL;
    uint64_t sig = d[(flags & BLOCK_HAS_COPY_DISPOSE) ? 4 : 2];
    if (!sig) return NULL;
    /* The string up to its NUL, read in guarded pieces that never cross a page: a
     * signature may start anywhere in __cstring, so where it crosses a page (or is
     * longer than one piece) depends only on the image's layout. Up to 4096 bytes,
     * the most type_by_objc keeps (arena_str); longer is left untyped. */
    char probe[256];
    for (uint64_t at = sig, left = 4096; left;) {
        size_t n = 4096 - (size_t)(at & 4095);
        if (n > sizeof probe) n = sizeof probe;
        if (n > left) n = (size_t)left;
        got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), at, n, (mach_vm_address_t)(uintptr_t)probe, &got) != KERN_SUCCESS ||
            got != n)
            return NULL;
        if (memchr(probe, 0, n)) return (const char *)(uintptr_t)sig;
        at += n;
        left -= n;
    }
    return NULL;
}

/* A native block reached guest code where its type is known to be a
 * block (a '@?' argument of a signature callback or a method's block
 * result). Record its invoke function's type from the block's signature:
 * the result extended (bool/char/short), or the whole call through abi.c
 * when the signature is not register-exact (a CGRect argument, say). */
void hao_note_native_block(uint64_t a)
{
    if (!a || (a & 7) || hao_image_for_addr(a)) return;        /* guest blocks are guest code's own */
    const uint64_t *b = (const uint64_t *)(uintptr_t)a;
    if (!is_block_isa(b[0])) return;
    uint32_t flags = (uint32_t)b[1];
    uint64_t inv = b[2], desc = b[3];
    if (!(flags & BLOCK_HAS_SIGNATURE) || (flags & BLOCK_SMALL_DESCRIPTOR) || !desc || (desc & 7) || !inv) return;
    if (hao_image_for_code(inv) || hao_guest_of_entry(inv)) return;
    const hao_plan *known = hao_bridge_lookup(inv);
    if (known && known->cls != HAO_BR_UNKNOWN) return;
    const char *sig = (const char *)(uintptr_t)((const uint64_t *)(uintptr_t)desc)[(flags & BLOCK_HAS_COPY_DISPOSE) ? 4 : 2];
    char nk = hao_bridge_kind_for_enc(sig);
    if (nk) hao_bridge_note(inv, nk, nk == 'A' ? sig : NULL, HAO_BRS_BLOCKSIG);   /* a typed plan from the block's ObjC encoding */
}

void hao_blocks_stats(void)
{
    fprintf(stderr, "hao-blocks: global=%llu invoke_patches=%llu helper_patches=%llu byref_patches=%llu healed=%llu%s\n",
            (unsigned long long)st_global, (unsigned long long)st_invokes, (unsigned long long)st_helpers,
            (unsigned long long)st_byref, (unsigned long long)st_healed, enabled() ? "" : " (disabled)");
}
