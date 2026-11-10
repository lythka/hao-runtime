/*
 * hao — native APIs that take a structure of guest callbacks built at run
 * time.
 *
 * Many C APIs receive their callbacks inside a structure: rb_tree_init's
 * rb_tree_ops_t, the CF collections' key/value callbacks, the five-field
 * contexts of run-loop sources, timers, ports and streams, CG/CT callback
 * tables, zlib's allocator, ... hao_thunk_native_args only sees x0-x7, so a
 * native caller loaded such a field into a register and called a guest
 * function on a non-executable page: one fault, and one kernel "execution
 * not permitted" line, per call.
 *
 * Structures initialized statically in guest data are handled at image load
 * (fnslots.c rewrites their slots). This file covers the ones the guest
 * fills in at run time, on its stack or heap: each API below binds to a
 * trap that does what the dispatcher does for any native call
 * (hao_thunk_native_args), rewrites the function fields of the callback
 * structures in place to x86 thunks (hao_thunk_for), and calls the native
 * function. Only fields that hold guest function starts change, and only in
 * writable memory outside the shared cache: the calling thread's guest
 * stack, a guest image's data, or a writable heap or anonymous region.
 * A field is patched with a compare-and-swap, as in blocks.c. Guest code
 * that later calls a patched field lands on the thunk, which the
 * dispatcher maps back to the guest function. Versioned structures are
 * patched only for the versions whose layout is listed here. APIs that only
 * ever see static structures (CFRuntimeClass, sqlite3_module) are left to
 * fnslots.c.
 *
 * Swift generic class metadata gets the same treatment when
 * swift_allocateGenericClassMetadata builds it (see below).
 *
 * Off with HAO_NO_STRUCT_THUNKS=1, or boot argument hao_no_struct_thunks=1:
 * the imports then bind to the native functions directly, as before.
 *
 * Not covered: callbacks of structures that native code fills in itself.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One callback structure layout. Function fields are given as a bit mask of
 * 8-byte slots (bit k: the pointer at byte offset 8k). */
typedef struct cb_layout {
    const char *name;
    uint16_t size;          /* bytes the structure spans (fixed layouts) */
    int8_t ver;             /* offset of the version word, -1 for none */
    uint8_t ver4;           /* the version word is 32 bits wide */
    int64_t want;           /* the only version this layout describes */
    uint32_t slots;
    /* Versioned layouts: slots for this structure (0: leave it alone) and
     * the bytes they span. Called with the structure's first 8 bytes known
     * to be readable guest memory. */
    uint32_t (*select)(const uint8_t *s, uint32_t *size);
} cb_layout;

/* CFRunLoopSourceContext: version 0 {version, info, retain, release,
 * copyDescription, equal, hash, schedule, cancel, perform}; version 1
 * (CFRunLoopSourceContext1) {..., hash, getPort, perform}. */
static uint32_t sel_rlsource(const uint8_t *s, uint32_t *size)
{
    int64_t v = *(const int64_t *)s;
    if (v == 0) { *size = 80; return 0x3FCu; }   /* slots 2..9 */
    if (v == 1) { *size = 72; return 0x1FCu; }   /* slots 2..8 */
    return 0;
}

#define FIXED(nm, sz, vo, v4, w, sl) {nm, sz, vo, v4, w, sl, NULL}
static const cb_layout
    /* Security/cssmtype.h: four function pointers followed by AllocRef. */
    L_CSSMMEM    = FIXED("CSSM_API_MEMORY_FUNCS", 40, -1, 0, 0, 0xF),
    L_RBOPS      = FIXED("rb_tree_ops_t", 16, -1, 0, 0, 0x3),                 /* compare_nodes, compare_key */
    L_CFKEY      = FIXED("CFDictionaryKeyCallBacks", 48, 0, 0, 0, 0x3E),       /* retain .. hash; = CFSet/CFBagCallBacks */
    L_CFVAL      = FIXED("CFDictionaryValueCallBacks", 40, 0, 0, 0, 0x1E),     /* retain .. equal; = CFArrayCallBacks */
    L_CFHEAP     = FIXED("CFBinaryHeapCallBacks", 40, 0, 0, 0, 0x1E),          /* retain, release, copyDescription, compare */
    L_CTX5       = FIXED("context", 40, 0, 0, 0, 0x1C),                        /* {version, info, retain, release, copyDescription} */
    L_CFALLOC    = FIXED("CFAllocatorContext", 72, 0, 0, 0, 0x1FC),            /* retain .. preferredSize */
    L_CGDPDIRECT = FIXED("CGDataProviderDirectCallbacks", 40, 0, 1, 0, 0x1E),
    L_CGDC       = FIXED("CGDataConsumerCallbacks", 16, -1, 0, 0, 0x3),
    L_CGFUNC     = FIXED("CGFunctionCallbacks", 24, 0, 1, 0, 0x6),
    L_CGPATTERN  = FIXED("CGPatternCallbacks", 24, 0, 1, 0, 0x6),
    L_CTRUNDEL   = FIXED("CTRunDelegateCallbacks", 40, 0, 0, 1, 0x1E),         /* kCTRunDelegateVersion1 */
    L_AURENDER   = FIXED("AURenderCallbackStruct", 16, -1, 0, 0, 0x1),
    L_AUHOST     = FIXED("HostCallbackInfo", 40, -1, 0, 0, 0x1E),
    L_NSMAPKEY   = FIXED("NSMapTableKeyCallBacks", 48, -1, 0, 0, 0x1F),
    L_NSMAPVAL   = FIXED("NSMapTableValueCallBacks", 24, -1, 0, 0, 0x7),
    L_NSHASH     = FIXED("NSHashTableCallBacks", 40, -1, 0, 0, 0x1F),
    L_ZSTREAM    = FIXED("z_stream", 88, -1, 0, 0, 0x300),                     /* zalloc +64, zfree +72 */
    L_BZSTREAM   = FIXED("bz_stream", 80, -1, 0, 0, 0x180),                    /* bzalloc +56, bzfree +64 */
    L_LZMAALLOC  = FIXED("lzma_allocator", 24, -1, 0, 0, 0x3),
    L_RLSOURCE   = {"CFRunLoopSourceContext", 0, -1, 0, 0, 0, sel_rlsource};

/* One callback-structure argument of an API. */
typedef struct {
    const cb_layout *lay;
    int8_t arg;             /* x register that holds the structure pointer */
    int16_t deref;          /* >= 0: the structure pointer is the word at x[arg] + deref */
    int8_t key_arg;         /* >= 0: only when x[key_arg] equals key0 or key1 */
    uint32_t key0, key1;
    int8_t size_arg;        /* >= 0: x[size_arg] bytes are valid (patch the slots inside them) */
} cb_arg;

#define ARG(r, l)        {&l, r, -1, -1, 0, 0, -1}
#define ARG_DEREF(r, o, l) {&l, r, o, -1, 0, 0, -1}
#define ARG_KEY(r, l, k, a, b, s) {&l, r, -1, k, a, b, s}

typedef struct {
    const char *name;       /* imported symbol */
    cb_arg args[2];
    bool sig;               /* also in the aggregate signature DB (by-value structs): call through abi.c */
    const char *callback_sig; /* explicit SDK void callback contract, not the API's own signature */
    uint8_t callback_arg;     /* arm64 integer argument slot; floating arguments have separate slots */
} cb_api;

static const cb_api apis[] = {
    /* Security/cssmapi.h: MemoryFuncs is argument 2 of CSSM_ModuleAttach. */
    {"_CSSM_ModuleAttach", {ARG(2, L_CSSMMEM)}},
    /* libc */
    {"_rb_tree_init", {ARG(1, L_RBOPS)}},
    /* CoreFoundation collections (callbacks copied at create) */
    {"_CFDictionaryCreate", {ARG(4, L_CFKEY), ARG(5, L_CFVAL)}},
    {"_CFDictionaryCreateMutable", {ARG(2, L_CFKEY), ARG(3, L_CFVAL)}},
    {"_CFSetCreate", {ARG(3, L_CFKEY)}},
    {"_CFSetCreateMutable", {ARG(2, L_CFKEY)}},
    {"_CFBagCreate", {ARG(3, L_CFKEY)}},
    {"_CFBagCreateMutable", {ARG(2, L_CFKEY)}},
    {"_CFArrayCreate", {ARG(3, L_CFVAL)}},
    {"_CFArrayCreateMutable", {ARG(2, L_CFVAL)}},
    {"_CFBinaryHeapCreate", {ARG(2, L_CFHEAP), ARG(3, L_CTX5)}},
    /* CoreFoundation allocators, run-loop sources */
    {"_CFAllocatorCreate", {ARG(1, L_CFALLOC)}},
    {"_CFRunLoopSourceCreate", {ARG(2, L_RLSOURCE)}},
    /* five-field contexts {version, info, retain, release, copyDescription} */
    /* CoreFoundation/CFRunLoop.h: timer(ref, info), observer(ref, activity, info).
     * Timer fireDate/interval occupy v0/v1, leaving its callback in x3. */
    {"_CFRunLoopTimerCreate", {ARG(4, L_CTX5)}, .callback_sig = "v^v^v", .callback_arg = 3},
    {"_CFRunLoopObserverCreate", {ARG(5, L_CTX5)}, .callback_sig = "v^vQ^v", .callback_arg = 4},
    {"_CFMachPortCreate", {ARG(2, L_CTX5)}},
    {"_CFMachPortCreateWithPort", {ARG(3, L_CTX5)}},
    {"_CFMessagePortCreateLocal", {ARG(3, L_CTX5)}},
    {"_CFSocketCreate", {ARG(6, L_CTX5)}},
    {"_CFSocketCreateWithNative", {ARG(4, L_CTX5)}},
    {"_CFFileDescriptorCreate", {ARG(4, L_CTX5)}},
    {"_CFReadStreamSetClient", {ARG(3, L_CTX5)}},
    {"_CFWriteStreamSetClient", {ARG(3, L_CTX5)}},
    {"_CFHostSetClient", {ARG(2, L_CTX5)}},
    {"_CFNetServiceSetClient", {ARG(2, L_CTX5)}},
    {"_SCDynamicStoreCreate", {ARG(3, L_CTX5)}},
    {"_SCDynamicStoreCreateWithOptions", {ARG(4, L_CTX5)}},
    {"_SCNetworkReachabilitySetCallback", {ARG(2, L_CTX5)}},
    {"_SCPreferencesSetCallback", {ARG(2, L_CTX5)}},
    {"_FSEventStreamCreate", {ARG(2, L_CTX5)}},
    {"_FSEventStreamCreateRelativeToDevice", {ARG(2, L_CTX5)}},
    /* CoreGraphics, CoreText */
    {"_CGDataProviderCreateDirect", {ARG(2, L_CGDPDIRECT)}},
    {"_CGDataConsumerCreate", {ARG(1, L_CGDC)}},
    {"_CGFunctionCreate", {ARG(5, L_CGFUNC)}},
    /* (info x0, bounds d0-d3, matrix by reference x1, xStep d4, yStep d5, tiling x2, isColored x3, callbacks x4) */
    {"_CGPatternCreate", {ARG(4, L_CGPATTERN)}, true},
    {"_CTRunDelegateCreate", {ARG(0, L_CTRUNDEL)}},
    /* AudioUnitSetProperty(unit, inID, scope, element, inData, inDataSize) */
    {"_AudioUnitSetProperty", {ARG_KEY(4, L_AURENDER, 1, 23, 2005, 5),   /* SetRenderCallback, SetInputCallback */
                               ARG_KEY(4, L_AUHOST, 1, 27, 27, 5)}},     /* HostCallbacks */
    /* Foundation: structures passed by value (arm64: x0/x1 point to the caller's copies) */
    {"_NSCreateMapTable", {ARG(0, L_NSMAPKEY), ARG(1, L_NSMAPVAL)}, true},
    {"_NSCreateMapTableWithZone", {ARG(0, L_NSMAPKEY), ARG(1, L_NSMAPVAL)}, true},
    {"_NSCreateHashTable", {ARG(0, L_NSHASH)}, true},
    /* compression streams (the allocator fields are read on every allocation) */
    {"_inflateInit_", {ARG(0, L_ZSTREAM)}},
    {"_inflateInit2_", {ARG(0, L_ZSTREAM)}},
    {"_inflateBackInit_", {ARG(0, L_ZSTREAM)}},
    {"_deflateInit_", {ARG(0, L_ZSTREAM)}},
    {"_deflateInit2_", {ARG(0, L_ZSTREAM)}},
    {"_BZ2_bzCompressInit", {ARG(0, L_BZSTREAM)}},
    {"_BZ2_bzDecompressInit", {ARG(0, L_BZSTREAM)}},
    {"_lzma_auto_decoder", {ARG_DEREF(0, 48, L_LZMAALLOC)}},              /* lzma_stream.allocator */
    {"_lzma_stream_decoder", {ARG_DEREF(0, 48, L_LZMAALLOC)}},
    {"_lzma_alone_decoder", {ARG_DEREF(0, 48, L_LZMAALLOC)}},
    {"_lzma_easy_encoder", {ARG_DEREF(0, 48, L_LZMAALLOC)}},
};
#define NAPIS (sizeof apis / sizeof *apis)

static void *native_fn[NAPIS];
static const char *sig_enc[NAPIS];
static uint64_t st_calls, st_patched;

/* [a, a+n) lies on this thread's guest stack */
static bool on_guest_stack(const hao_thread *t, uint64_t a, uint64_t n)
{
    uint64_t lo = (uint64_t)(uintptr_t)t->stack, hi = lo + t->stack_size;
    return a >= lo && a < hi && hi - a >= n;
}

/* [a, a+n) lies inside one section of a guest image's data segments
 * (__DATA, __DATA_CONST, __DATA_DIRTY, __AUTH, __AUTH_CONST: writable under hao) */
static bool in_guest_data(uint64_t a, uint64_t n)
{
    hao_image *im = hao_image_for_addr(a);
    if (!im) return false;
    for (int s = 0; s < im->nsects; s++) {
        uint64_t lo = im->sects[s].addr, sz = im->sects[s].size;
        if (a < lo || a - lo >= sz || sz - (a - lo) < n) continue;
        return !strncmp(im->sects[s].seg, "__DATA", 6) || !strncmp(im->sects[s].seg, "__AUTH", 6);
    }
    return false;
}

/* [a, a+n) lies in one readable and writable VM region outside the shared
 * cache (heap, anonymous memory): structures the guest built at run time.
 * Native constants (kCFTypeDictionaryKeyCallBacks, ...) live in the shared
 * cache and are never touched. */
extern const void *_dyld_get_shared_cache_range(size_t *length);   /* libdyld (dyld_priv.h) */
static bool in_writable_region(uint64_t a, uint64_t n)
{
    static uint64_t sc_lo, sc_hi;
    static int sc_init;
    if (!sc_init) {
        size_t len = 0;
        const void *p = _dyld_get_shared_cache_range(&len);
        sc_lo = (uint64_t)(uintptr_t)p;
        sc_hi = sc_lo + len;
        __atomic_store_n(&sc_init, 1, __ATOMIC_RELEASE);
    }
    if (a + n < a || (a < sc_hi && a + n > sc_lo)) return false;
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return false;
    if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
    if (addr > a || a + n > addr + size) return false;
    return (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE);
}

static bool guest_owned(const hao_thread *t, uint64_t a, uint64_t n)
{
    if (on_guest_stack(t, a, n) || in_guest_data(a, n)) return true;
    return !hao_image_for_addr(a) && in_writable_region(a, n);
}

static void patch_struct(hao_thread *t, const cb_arg *ca)
{
    const hao_cpu *c = &t->cpu;
    /* the key is a 32-bit argument: the upper half of its register is undefined */
    if (ca->key_arg >= 0 && (uint32_t)c->x[ca->key_arg] != ca->key0 && (uint32_t)c->x[ca->key_arg] != ca->key1) return;
    uint64_t a = c->x[ca->arg];
    if (!a || (a & 7)) return;
    if (ca->deref >= 0) {
        /* The outer structure may live anywhere (a heap lzma_stream): the
         * native callee reads this word too. Only the structure it points
         * to is patched, and that one must be guest-owned. */
        a = *(const uint64_t *)(uintptr_t)(a + (uint64_t)ca->deref);
        if (!a || (a & 7)) return;
    }
    const cb_layout *l = ca->lay;
    uint32_t size = l->size, slots = l->slots;
    if (l->select) {
        if (!guest_owned(t, a, 8)) return;
        slots = l->select((const uint8_t *)(uintptr_t)a, &size);
        if (!slots) return;
    }
    if (ca->size_arg >= 0) {
        uint64_t have = c->x[ca->size_arg];
        if (have < size) {
            size = (uint32_t)(have & ~7ull);
            slots &= size >= 256 ? ~0u : (1u << (size / 8)) - 1;
            if (!slots) return;
        }
    }
    if (!guest_owned(t, a, size)) { if (hao_verbose > 1) fprintf(stderr, "hao: %s at %#llx (%u bytes) is not guest-owned\n", l->name, (unsigned long long)a, size); return; }
    uint64_t *s = (uint64_t *)(uintptr_t)a;
    if (l->ver >= 0) {
        int64_t v = l->ver4 ? (int64_t)*(const uint32_t *)((const uint8_t *)s + l->ver) : *(const int64_t *)((const uint8_t *)s + l->ver);
        if (v != l->want) return;
    }
    for (uint32_t m = slots; m; m &= m - 1) {
        int k = __builtin_ctz(m);
        uint64_t g = __atomic_load_n(&s[k], __ATOMIC_RELAXED);
        if (hao_verbose > 1) fprintf(stderr, "hao: %s+%d = %#llx%s\n", l->name, 8 * k, (unsigned long long)g, hao_is_guest_function(g) ? " (guest)" : "");
        if (!hao_is_guest_function(g)) continue;
        uint64_t e = hao_thunk_for(g);
        if (e != g && __atomic_compare_exchange_n(&s[k], &g, e, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            __atomic_fetch_add(&st_patched, 1, __ATOMIC_RELAXED);
    }
}

static void trap_cbstruct(hao_thread *t, uint64_t idx)
{
    const cb_api *api = &apis[idx];
    __atomic_fetch_add(&st_calls, 1, __ATOMIC_RELAXED);
    hao_thunk_native_args(t);
    if (api->callback_sig) {
        uint64_t a = t->cpu.x[api->callback_arg];
        /* Upgrade raw guest code or its ordinary thunk only. Preserve an
         * already typed entry or explicit Swift adapter. */
        uint64_t g = hao_thunk_target(a);
        if (!g && hao_is_guest_function(a)) g = a;
        if (g) t->cpu.x[api->callback_arg] = hao_guest_entry_for_void_sig(g, api->callback_sig);
    }
    for (int k = 0; k < 2 && api->args[k].lay; k++) patch_struct(t, &api->args[k]);
    t->native_calls++;
    uint64_t fn = (uint64_t)(uintptr_t)native_fn[idx];
    if (sig_enc[idx] && hao_call_native_sig(&t->cpu, fn, sig_enc[idx])) return;
    hao_native_call(fn, &t->cpu, 16);
}

/* --------------------------------------- Swift generic class metadata
 * A generic class's metadata pattern refers to its heap-object destroyer
 * and ivar destroyer with relative pointers; swift_allocateGenericClassMetadata
 * (called by the guest's instantiation function) copies them into the new
 * metadata as absolute guest addresses, and swift_release calls the
 * destroyer for every instance. Give them thunks in the new metadata. */
static void *swift_alloc_class_fn[2];
static uint64_t st_swift_meta;

static void thunk_word(uint64_t *w)
{
    uint64_t g = *w;
    if (hao_is_guest_function(g)) {
        uint64_t e = hao_thunk_for(g);
        if (e != g) *w = e;
    }
}

static void trap_swift_class_metadata(hao_thread *t, uint64_t arg)
{
    hao_cpu *c = &t->cpu;
    hao_thunk_native_args(t);
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)swift_alloc_class_fn[arg], c, 16);
    uint64_t m = c->x[0];
    if (!m || (m & 7)) return;
    /* full metadata: ..., destroy at -16, value witnesses at -8; class
     * metadata: ivar destroyer at +72 (after description) */
    uint64_t *md = (uint64_t *)(uintptr_t)m;
    thunk_word(&md[-2]);
    thunk_word(&md[9]);
    hao_objc_prepare_generic_class(m);
    __atomic_fetch_add(&st_swift_meta, 1, __ATOMIC_RELAXED);
}

/* HAO_NO_STRUCT_THUNKS=1 or boot argument hao_no_struct_thunks=1 (hao_opt) */
static bool enabled(void) { return !hao_opt.no_struct_thunks; }

const char *hao_sigdb_enc(const char *name);   /* interpose.c */

void hao_callbacks_interpose_names(hao_name_fn cb, void *ctx)
{
    cb("_swift_allocateGenericClassMetadata", ctx);
    cb("_swift_allocateGenericClassMetadataWithLayoutString", ctx);
    for (size_t k = 0; k < NAPIS; k++) cb(apis[k].name, ctx);
}

bool hao_callbacks_interpose(const char *name, uint64_t *addr)
{
    if (name[0] != '_') return false;
    if (!strcmp(name, "_swift_allocateGenericClassMetadata") ||
        !strcmp(name, "_swift_allocateGenericClassMetadataWithLayoutString")) {
        if (!enabled()) return false;
        unsigned variant = strcmp(name, "_swift_allocateGenericClassMetadata") != 0;
        if (!swift_alloc_class_fn[variant]) swift_alloc_class_fn[variant] = dlsym(RTLD_DEFAULT, name + 1);
        if (!swift_alloc_class_fn[variant]) return false;
        *addr = hao_trap_address(name, trap_swift_class_metadata, variant);
        return true;
    }
    for (size_t k = 0; k < NAPIS; k++) {
        if (strcmp(name, apis[k].name)) continue;
        if (!enabled()) return false;
        /* the importing library is loaded by now (hao_resolve_import); a
         * symbol this host lacks binds the ordinary way */
        if (!native_fn[k]) native_fn[k] = dlsym(RTLD_DEFAULT, name + 1);
        if (!native_fn[k]) return false;
        if (apis[k].sig && !(sig_enc[k] = hao_sigdb_enc(name))) return false;
        *addr = hao_trap_address(apis[k].name, trap_cbstruct, k);
        return true;
    }
    return false;
}

void hao_callbacks_stats(void)
{
    fprintf(stderr, "hao-structs: calls=%llu field_patches=%llu swift_class_metadata=%llu%s\n",
            (unsigned long long)st_calls, (unsigned long long)st_patched, (unsigned long long)st_swift_meta,
            enabled() ? "" : " (disabled)");
}
