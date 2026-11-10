/*
 * hao — native entry points for the guest functions that Swift metadata
 * names by relative pointer.
 *
 * The native Swift runtime calls guest code through 32-bit relative
 * pointers inside the guest's own metadata: a conformance descriptor's
 * resilient witnesses (copied into the witness tables the runtime
 * instantiates, then tail-called by native protocol dispatch thunks), a
 * type descriptor's metadata accessor, the functions of generic metadata
 * patterns and singleton/foreign metadata initialization, class vtable and
 * override implementations, symbolic references to accessor functions in
 * mangled names, and the enum-tag and resilient-accessor functions of
 * type layout strings. Native frameworks also call the main executable's
 * entry point through one: __TEXT,__swift5_entry, the relative pointer to
 * the @main function swiftc emits, which ExtensionFoundation's
 * +[_EXUtil _getEntryPointFunction] (an .appex's NSExtensionMain) reads
 * with getsectiondata and calls. A rebase slot holding an absolute pointer gets a
 * thunk from fnslots.c; a relative pointer cannot point at hao's code
 * cache, which lies anywhere in the address space. So each such call
 * jumped to a non-executable guest page and took the NX-fault path
 * (25 us and one kernel log line per call; a system boot takes hundreds of
 * thousands of them, almost all through Swift relative pointers).
 *
 * Before the image's Swift records are registered with the native runtime
 * (swift.c), every such field is found from the metadata ABI, its target
 * checked to be a guest function start (LC_FUNCTION_STARTS), and the field
 * rewritten to point at a thunk in an island mapped within +-2 GB of the
 * image. The layouts are those of swift-6.4.0 (tools/fetch_swift_src.sh):
 *   include/swift/ABI/Metadata.h, MetadataValues.h, GenericContext.h
 *     (descriptors, trailing objects and their flags),
 *   stdlib/public/runtime/Metadata.cpp (initializeResilientWitnessTable,
 *     initProtocolWitness, the associated-conformance access functions),
 *   stdlib/public/runtime/BytecodeLayouts.cpp (layout-string ops, as the
 *     destroy/copy walkers read them), MetadataLookup.cpp and
 *     docs/ABI/Mangling.rst (symbolic references).
 * Nothing is inferred from bit patterns: a field is rewritten only when the
 * ABI walk reaches it and its target is a function start of the same
 * image; anything else (a field that does not resolve, an op the walker
 * does not know, an island that cannot be placed) leaves the field alone
 * and is counted.
 *
 * Signatures. The kinds whose type the metadata ABI fixes (accessors,
 * instantiation/completion functions, witness-table instantiators,
 * destroyers, layout-string functions, associated-conformance and accessor
 * references) take at most four integer-class arguments and return at most
 * two integer registers: AAPCS64 and SysV (C or swiftcc) put them in the
 * same register positions, rdi..r9/r13 -> x0..x5/x20 and x0..x3 -> rax,
 * rdx, rcx, r8, which is what the register bridge maps. Their island
 * entries are exact. Witnesses, vtable/override implementations and
 * default implementations have the signature of the protocol requirement
 * or method. Exported protocol requirements can use compiler-lowered
 * reverse signatures from their dispatch thunk's UUID-bound interface unit.
 * The others get the register bridge the fault path used and are counted
 * as requirement_untyped: no fault does not establish ABI correctness.
 *
 * An island entry has the bytes of a plain thunk (jit.c): movabs r11,
 * guest; xor r10d, r10d; jmp [rip+0]; .quad hao_callback_entry, in a
 * 32-byte slot. Typed entries jump to a signature thunk instead of directly
 * to hao_callback_entry. hao_swiftmeta_island_target reads the guest address at
 * entry+2 and guest code that branches to an entry (a witness table slot
 * the runtime filled) is mapped back to the guest function by the
 * dispatcher (hao_guest_of_entry). Off with HAO_NO_SWIFT_THUNKS=1 (an
 * ordinary process; boot argument hao_no_swift_thunks=1 in a
 * HAO_TEST_SWITCHES build); no thunks without JIT memory.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include "hao/swift_reverse.h"
#include "near_map.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------- kinds */

enum {
    K_WITNESS,           /* resilient witness (Method/Init/Getter/Setter/Read/Modify) */
    K_DEFAULT_IMPL,      /* protocol requirement default implementation */
    K_METHOD,            /* class vtable method descriptor Impl */
    K_OVERRIDE,          /* method override / default override Impl */
    K_ASYNC,             /* async or callee-allocated-coroutine function record's function */
    K_ACCESSOR,          /* type descriptor metadata access function */
    K_WT_INSTANTIATOR,   /* generic witness table instantiator */
    K_INSTANTIATE,       /* generic metadata pattern instantiation function */
    K_COMPLETE,          /* metadata completion function (pattern, singleton, foreign) */
    K_DESTROY,           /* class pattern heap-object destroyer, ivar destroyer */
    K_RELOCATE,          /* resilient class pattern relocation function */
    K_PRESPEC,           /* canonical prespecialized metadata accessor */
    K_SYMREF,            /* mangled name symbolic reference to an accessor/associated-conformance function */
    K_ENUMTAG,           /* layout string enum tag function */
    K_RESILIENT,         /* layout string resilient type accessor */
    K_VWT,               /* value witness table slot (absolute) */
    K_WTPAT,             /* witness table pattern slot (absolute) */
    K_KEYPATH,           /* key path pattern accessor (getter, setter, argument layout and initializer) */
    K_KEYPATH_ID,        /* key path computed component's identifier resolver (ID resolution FunctionCall) */
    K_DISTRIBUTED,       /* distributed accessor of an accessible function record (its async function record's function) */
    K_REPLACEMENT,       /* dynamic replacement function (or its async/coroutine record's function) */
    K_ENTRY,             /* __TEXT,__swift5_entry: the program's entry point (@main) */
    NKINDS
};

static const struct { const char *name; bool exact; } kinds[NKINDS] = {
    [K_WITNESS] = {"witness", false},
    [K_DEFAULT_IMPL] = {"default_impl", false},
    [K_METHOD] = {"method", false},
    [K_OVERRIDE] = {"override", false},
    [K_ASYNC] = {"async", false},
    /* swiftcc MetadataResponse(MetadataRequest, up to 3 key arguments or an argument array) */
    [K_ACCESSOR] = {"accessor", true},
    /* void(WitnessTable *, const Metadata *, const void *const *) */
    [K_WT_INSTANTIATOR] = {"wt_instantiator", true},
    /* Metadata *(const TypeContextDescriptor *, const void *const *, const GenericMetadataPattern *) */
    [K_INSTANTIATE] = {"instantiate", true},
    /* swiftcc MetadataDependency(Metadata *, MetadataCompletionContext *, const GenericMetadataPattern *) */
    [K_COMPLETE] = {"complete", true},
    /* swiftcc void(SWIFT_CONTEXT HeapObject *) */
    [K_DESTROY] = {"destroy", true},
    /* Metadata *(const ClassDescriptor *, const ResilientClassMetadataPattern *) */
    [K_RELOCATE] = {"relocate", true},
    /* swiftcc MetadataResponse(MetadataRequest) */
    [K_PRESPEC] = {"prespec_accessor", true},
    /* \x09: const Metadata *(const void *const *); \x07/\x08: swiftcc WitnessTable *(const Metadata *, const
     * Metadata *, const WitnessTable *) */
    [K_SYMREF] = {"symref", true},
    /* unsigned(const uint8_t *) */
    [K_ENUMTAG] = {"enum_tag", true},
    /* Metadata *(const Metadata *const *) */
    [K_RESILIENT] = {"layout_accessor", true},
    /* the value witnesses of ValueWitness.def: pointer arguments, pointer or unsigned results */
    [K_VWT] = {"value_witness", true},
    [K_WTPAT] = {"wt_pattern", false},
    /* the accessors' types are the stdlib's (KeyPath.swift ComputedAccessorsPtr, ComputedArgumentLayoutFn, ...),
     * generic over the property's types */
    [K_KEYPATH] = {"keypath", false},
    /* @convention(c) (UnsafeRawPointer?) -> UnsafeRawPointer? (KeyPath.swift, the .functionCall ID resolution: an
     * @objc property's selector getter) */
    [K_KEYPATH_ID] = {"keypath_id", true},
    /* DistributedAccessorSignature (Distributed/DistributedActor.cpp): swiftasync, nine pointer-sized arguments, so
     * not the register coincidence of the exact kinds */
    [K_DISTRIBUTED] = {"distributed_accessor", false},
    /* the replaced function's signature */
    [K_REPLACEMENT] = {"replacement", false},
    /* the C main swiftc emits for @main: int32_t(int32_t argc, char **argv) */
    [K_ENTRY] = {"entry", true},
};

/* totals over all images (loads are serialised; the stats line reads them at exit) */
static uint64_t st_kind[NKINDS], st_notfn[NKINDS], st_images, st_entries, st_fields, st_unplaced, st_bad_ls,
    st_ls, st_bad_desc, st_conf, st_types, st_protos, st_islands_bytes, st_nojit, st_us, st_abs_kept, st_abs_taken,
    st_slot_bad, st_reverse;

/* hao_swiftmeta_slots: the kinds of reference */
static const char *const slot_kinds[HAO_SK_N] = {
    [HAO_SK_TYPEREF_CTX] = "typeref.ctx",        [HAO_SK_TYPEREF_CONF] = "typeref.conf",
    [HAO_SK_TYPEREF_ASSOC] = "typeref.assoc",    [HAO_SK_TYPEREF_OBJCPROTO] = "typeref.objcproto",
    [HAO_SK_TYPE_RECORD] = "types.record",       [HAO_SK_PROTO_RECORD] = "protos.record",
    [HAO_SK_PARENT] = "ctx.parent",              [HAO_SK_REQ_PROTO] = "req.protocol",
    [HAO_SK_REQ_OBJCPROTO] = "req.objcprotocol", [HAO_SK_REQ_CONF] = "req.conformance",
    [HAO_SK_CONF_PROTO] = "conf.protocol",       [HAO_SK_CONF_TYPEREF] = "conf.typeref",
    [HAO_SK_CONF_OBJCCLASS] = "conf.objcclass",  [HAO_SK_CONF_RETRO] = "conf.retroactive",
    [HAO_SK_CONF_WITNESS] = "conf.witness",      [HAO_SK_CONF_GLOBALACTOR] = "conf.globalactor",
    [HAO_SK_SUPERCLASS] = "class.superclass",    [HAO_SK_OVERRIDE_CLASS] = "class.override.class",
    [HAO_SK_OVERRIDE_METHOD] = "class.override.method", [HAO_SK_DEFOVER_REPL] = "class.defover.replacement",
    [HAO_SK_DEFOVER_ORIG] = "class.defover.original", [HAO_SK_PATTERN_VWT] = "pattern.vwt",
    [HAO_SK_SHAPE_VWT] = "shape.vwt",            [HAO_SK_REPLACE_KEY] = "replace.key",
    [HAO_SK_REPLACE2_ORIG] = "replace2.original",
};

const char *hao_swiftmeta_slot_kind(int k) { return k >= 0 && k < HAO_SK_N && slot_kinds[k] ? slot_kinds[k] : "?"; }


/* ----------------------------------------------------- image accessors */

typedef struct {
    hao_image *img;
    struct ent { uint64_t field, target; uint8_t kind, abs; uint64_t bridge; } *e;
    size_t n, cap;
    struct ent *a;          /* absolute slots (value witness tables, witness table patterns) */
    size_t na, capa;
    struct { uint64_t vwt; bool en; } *vwt;   /* value witness tables found */
    size_t nv, capv;
    bool oom;
    /* slots mode (hao_swiftmeta_slots): no fields are noted; every relative indirect(able) reference whose slot
     * lies outside the image goes to slot(sctx, ...), and the records it reaches are walked once each */
    hao_swift_slot_fn slot;
    void *sctx;
    uint64_t *seen;
    size_t nseen, capseen, nreach;
    int depth;
} walk;

/* a descriptor the walk could not follow (counted; -vv names the walker's
 * line, the record's file vmaddr and, for a reference through a bound
 * slot, what the slot holds) */
static void bad_desc_at(const walk *w, int line, uint64_t at, uint64_t to)
{
    if (w->slot) st_slot_bad++;
    else st_bad_desc++;
    if (hao_verbose < 2) return;
    Dl_info d = {0};
    bool named = to && !hao_is_trap(to) && dladdr((void *)(uintptr_t)to, &d) && d.dli_sname;
    fprintf(stderr, "hao: swiftmeta: descriptor not followed (swiftmeta.c:%d) at %#llx%s%s%s%s\n", line,
            (unsigned long long)(at - w->img->slide), to ? " -> " : "", !to ? "" : hao_is_trap(to) ? "unresolved import" : named ? d.dli_sname : "?",
            named && d.dli_fname ? " in " : "", named && d.dli_fname ? (strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname) : "");
}
#define bad_desc(at, to) bad_desc_at(w, __LINE__, (at), (to))

/* [a, a+n) inside the image's mapped range */
static bool in_img(const walk *w, uint64_t a, uint64_t n)
{
    uint64_t end = hao_image_extent(w->img, a);
    return end && n <= end - a;
}

static bool rd32(const walk *w, uint64_t a, uint32_t *v)
{
    if (!in_img(w, a, 4)) return false;
    memcpy(v, (const void *)(uintptr_t)a, 4);
    return true;
}

static bool rd16(const walk *w, uint64_t a, uint16_t *v)
{
    if (!in_img(w, a, 2)) return false;
    memcpy(v, (const void *)(uintptr_t)a, 2);
    return true;
}

static bool rd64(const walk *w, uint64_t a, uint64_t *v)
{
    if (!in_img(w, a, 8)) return false;
    memcpy(v, (const void *)(uintptr_t)a, 8);
    return true;
}

/* a RelativeDirectPointer at a: its target, 0 if null or unreadable */
static uint64_t rel_direct(const walk *w, uint64_t a)
{
    uint32_t v;
    if (!rd32(w, a, &v) || !v) return 0;
    return a + (uint64_t)(int64_t)(int32_t)v;
}

/* slots mode: a reference of this kind at field through the pointer-sized
 * slot at slot. The image's own slots are bound with its segments (dsc.c's
 * segment pass, chained fixups for an image on disk), so only slots outside
 * it are reported: a shared-cache image's references into the cache
 * builder's uniqued GOTs. */
static void reach(walk *w, int kind, uint64_t field, uint64_t slot)
{
    if (!w->slot || in_img(w, slot, 8)) return;
    w->nreach++;
    w->slot(w->sctx, kind, field, slot);
}

/* a RelativeIndirectablePointer at a (low bit: through a pointer-sized slot,
 * which the loader has bound); 0 if null or unreadable. The target may lie
 * in another image (a native protocol's requirement, a native superclass's
 * method). A shared-cache image's slot can lie outside it, in the cache's
 * uniqued GOTs: what dsc.c bound it to (hao_dsc_bind, before this walk),
 * never its raw slide-info word. In slots mode such a slot is reported as a
 * reference of this kind and not read. */
static uint64_t rel_indirectable(walk *w, uint64_t a, int kind)
{
    uint32_t v;
    if (!rd32(w, a, &v) || !v) return 0;
    uint64_t t = a + (uint64_t)(int64_t)(int32_t)(v & ~1u);
    if (!(v & 1)) return t;
    reach(w, kind, a, t);
    uint64_t p;
    if (rd64(w, t, &p)) return p;
    return !w->slot && w->img->dsc ? hao_dsc_bound_ptr(t) : 0;
}

/* a 32-bit read through a pointer the loader bound (an indirectable
 * pointer's slot: another image's protocol requirement or method
 * descriptor); an unresolved import is 0 or one of hao's traps */
static bool rd32_any(uint64_t a, uint32_t *v)
{
    if (a < 0x1000 || (a & 3) || hao_is_trap(a)) return false;
    memcpy(v, (const void *)(uintptr_t)a, 4);
    return true;
}

static void note(walk *w, uint64_t field, uint64_t target, int kind)
{
    if (!target || w->slot) return;
    /* the native runtime calls exactly this address: it must be a function
     * start of this image (an entry of LC_FUNCTION_STARTS) */
    if (!hao_is_guest_function(target) || hao_image_for_code(target) != w->img) { st_notfn[kind]++; return; }
    if (w->n == w->cap) {
        size_t c = w->cap ? 2 * w->cap : 1024;
        void *p = realloc(w->e, c * sizeof *w->e);
        if (!p) { w->oom = true; return; }
        w->e = p;
        w->cap = c;
    }
    w->e[w->n++] = (struct ent){field, target, (uint8_t)kind, 0};
}

/* an absolute function pointer slot of this image that the native runtime
 * copies (value witness tables, witness table patterns): fnslots.c would
 * thunk it, but only after ObjC registration, which runs Swift metadata
 * initializers that copy these tables first */
static void note_abs(walk *w, uint64_t slot, int kind)
{
    uint64_t v;
    if (w->slot || !rd64(w, slot, &v) || !v) return;
    if (!hao_is_guest_function(v) || hao_image_for_code(v) != w->img) return;   /* data, or not a function start */
    if (w->na == w->capa) {
        size_t c = w->capa ? 2 * w->capa : 256;
        void *p = realloc(w->a, c * sizeof *w->a);
        if (!p) { w->oom = true; return; }
        w->a = p;
        w->capa = c;
    }
    w->a[w->na++] = (struct ent){slot, v, (uint8_t)kind, 1};
}

/* a value witness table (ValueWitnessTable.h: 8 function pointers, size, stride, flags and extra inhabitants;
 * an enum's adds getEnumTag, destructiveProjectEnumData, destructiveInjectEnumTag) */
static void note_vwt(walk *w, uint64_t vwt, bool en)
{
    if (w->slot || !vwt || !in_img(w, vwt, en ? 112 : 88)) return;
    for (size_t k = 0; k < w->nv; k++) if (w->vwt[k].vwt == vwt) return;
    if (w->nv == w->capv) {
        size_t c = w->capv ? 2 * w->capv : 64;
        void *p = realloc(w->vwt, c * sizeof *w->vwt);
        if (!p) { w->oom = true; return; }
        w->vwt = p;
        w->capv = c;
    }
    w->vwt[w->nv].vwt = vwt;
    w->vwt[w->nv++].en = en;
    for (int k = 0; k < 8; k++) note_abs(w, vwt + 8ull * k, K_VWT);
    if (en) for (int k = 11; k < 14; k++) note_abs(w, vwt + 8ull * k, K_VWT);
}

/* a relative function pointer field (CompactFunctionPointer on Darwin) */
static void fnfield(walk *w, uint64_t field, int kind) { note(w, field, rel_direct(w, field), kind); }

/* an async function pointer or callee-allocated coroutine function pointer
 * record: {CompactFunctionPointer Function; uint32_t ExpectedContextSize}
 * (ABI/Executor.h) */
static void fnrecord_kind(walk *w, uint64_t rec, int kind)
{
    if (rec && in_img(w, rec, 8)) fnfield(w, rec, kind);
}

static void fnrecord(walk *w, uint64_t rec) { fnrecord_kind(w, rec, K_ASYNC); }

/* --------------------------------------------- slots mode: records walked */

static void context(walk *w, uint64_t d);

/* true if a was walked before in this slots walk (and record it) */
static bool seen_once(walk *w, uint64_t a)
{
    if (2 * (w->nseen + 1) > w->capseen) {
        size_t oc = w->capseen, c = oc ? 2 * oc : 1024;
        uint64_t *n = calloc(c, sizeof *n), *old = w->seen;
        if (!n) { w->oom = true; return true; }
        w->seen = n;
        w->capseen = c;
        w->nseen = 0;
        for (size_t k = 0; k < oc; k++) {
            if (!old[k]) continue;
            size_t j = (old[k] >> 2) & (c - 1);
            while (n[j]) j = (j + 1) & (c - 1);
            n[j] = old[k];
            w->nseen++;
        }
        free(old);
    }
    size_t j = (a >> 2) & (w->capseen - 1);
    for (; w->seen[j]; j = (j + 1) & (w->capseen - 1))
        if (w->seen[j] == a) return true;
    w->seen[j] = a;
    w->nseen++;
    return false;
}

/* A TargetGenericRequirementDescriptor {flags, rel32 Param, union} at q
 * (GenericContext.h): a Protocol requirement's protocol is a
 * RelativeTargetProtocolDescriptorPointer (bit 0 indirect, bit 1 an ObjC
 * protocol), a SameConformance requirement's conformance a
 * RelativeIndirectablePointer; the other kinds hold mangled names (walked
 * as __swift5_typeref), a layout kind or invertible protocols. */
static void requirement(walk *w, uint64_t q)
{
    uint32_t f, v;
    if (!rd32(w, q, &f) || !rd32(w, q + 8, &v)) { bad_desc(q, 0); return; }
    if ((f & 0x1f) == 0) {
        uint64_t t = q + 8 + (uint64_t)(int64_t)(int32_t)(v & ~3u);
        if (v & 1) reach(w, v & 2 ? HAO_SK_REQ_OBJCPROTO : HAO_SK_REQ_PROTO, q + 8, t);
        else if (v && !(v & 2)) context(w, t);
    } else if ((f & 0x1f) == 3)
        (void)rel_indirectable(w, q + 8, HAO_SK_REQ_CONF);
}

static void requirements(walk *w, uint64_t q, uint64_t n)
{
    if (!in_img(w, q, 12 * n)) { if (n) bad_desc(q, 0); return; }
    for (uint64_t k = 0; k < n; k++) requirement(w, q + 12 * k);
}

/* A TargetExtendedExistentialTypeShape (Metadata.h): {flags, rel32
 * ExistentialType, TargetGenericContextDescriptorHeader ReqSigHeader}, then
 * the generalization signature's header, the type expression and the
 * suggested value witnesses (an indirectable pointer) when the flags have
 * them, the generic parameters of both signatures that are not implicit,
 * and both signatures' requirements. */
static void shape(walk *w, uint64_t s)
{
    uint32_t f;
    uint16_t rnp, rnr, gnp = 0, gnr = 0;
    if (!rd32(w, s, &f) || !rd16(w, s + 8, &rnp) || !rd16(w, s + 10, &rnr)) { bad_desc(s, 0); return; }
    if (seen_once(w, s)) return;
    uint64_t p = s + 16;
    if (f & 0x100) {
        if (!rd16(w, p, &gnp) || !rd16(w, p + 2, &gnr)) { bad_desc(s, 0); return; }
        p += 8;
    }
    if (f & 0x200) p += 4;
    if (f & 0x400) { (void)rel_indirectable(w, p, HAO_SK_SHAPE_VWT); p += 4; }
    p += (f & 0x800 ? 0 : rnp) + (f & 0x1000 ? 0 : gnp);
    requirements(w, (p + 3) & ~3ull, (uint64_t)rnr + gnr);
}

/* ------------------------------------------------------ conformances */

/* ProtocolRequirementFlags (MetadataValues.h) */
enum { RQ_BASE, RQ_METHOD, RQ_INIT, RQ_GETTER, RQ_SETTER, RQ_READ, RQ_MODIFY, RQ_ASSOC_TYPE, RQ_ASSOC_CONF };

static bool rq_async_bit(uint32_t f) { return f & 0x20; }
static bool rq_function_kind(uint32_t f) { uint32_t k = f & 0xf; return k >= RQ_METHOD && k <= RQ_MODIFY; }

/* a witness or default implementation for a requirement with flags f */
static void requirement_bridge(struct ent *e, uint64_t proto, uint64_t req)
{
    const hao_swift_result_signature *result = NULL;
    const hao_swift_reverse *sig = hao_swift_reverse_requirement(req, &result);
    if (!sig) return;
    const hao_swift_result_context *context = hao_swift_result_bind(proto, req, sig, result);
    uintptr_t tag = context ? (uintptr_t)context | HAO_CB_SWIFT_RESULT_TAG : (uintptr_t)sig | HAO_CB_SWIFT_TAG;
    e->bridge = hao_sig_thunk(e->target, (void *)tag);
}

static void requirement_impl(walk *w, uint64_t field, uint32_t f, int kind, uint64_t proto, uint64_t req)
{
    if (!rq_function_kind(f)) {
        /* associated conformance: a mangled name, or a 5-byte "\x07"/"\x08" reference to an access function */
        if ((f & 0xf) == RQ_ASSOC_CONF) {
            uint64_t s = rel_direct(w, field);
            if (s && in_img(w, s, 1) && *(const uint8_t *)(uintptr_t)s == 0xff) s++;   /* relative to the protocol */
            if (s && in_img(w, s, 6) && (*(const uint8_t *)(uintptr_t)s == 7 || *(const uint8_t *)(uintptr_t)s == 8) &&
                *(const uint8_t *)(uintptr_t)(s + 5) == 0)
                fnfield(w, s + 1, K_SYMREF);
        }
        return;
    }
    /* async (isAsync: a non-coroutine with the async bit), or a callee-allocated coroutine (a coroutine with it):
     * the witness is a function record, not code (ProtocolRequirementFlags::isData) */
    if (rq_async_bit(f))
        fnrecord(w, rel_direct(w, field));
    else {
        size_t before = w->n;
        fnfield(w, field, kind);
        if (w->n > before && (f & 0xf) <= RQ_SETTER)
            requirement_bridge(&w->e[before], proto, req);
    }
}

/* A TargetProtocolConformanceDescriptor {rel32 indirectable Protocol,
 * TypeRef, rel32 WitnessTablePattern, flags}, then its trailing objects
 * (Metadata.h): the retroactive context, conditional requirements, pack
 * shapes, resilient witnesses, the generic witness table and the global
 * actor reference. */
static void conformance(walk *w, uint64_t cd)
{
    uint32_t flags;
    if (!rd32(w, cd + 12, &flags)) { bad_desc(cd, 0); return; }
    uint64_t p = cd + 16;
    if (w->slot) {
        if (seen_once(w, cd)) return;
        uint64_t pr = rel_indirectable(w, cd, HAO_SK_CONF_PROTO);
        if (pr && in_img(w, pr, 8)) context(w, pr);
        uint32_t v;
        unsigned tk = (flags >> 3) & 7;                  /* TypeReferenceKind */
        if (rd32(w, cd + 4, &v) && v) {
            uint64_t t = cd + 4 + (uint64_t)(int64_t)(int32_t)v;
            if (tk == 1 || tk == 3) reach(w, tk == 1 ? HAO_SK_CONF_TYPEREF : HAO_SK_CONF_OBJCCLASS, cd + 4, t);
            else if (tk == 0) context(w, t);
        }
        if (flags & (1u << 6)) {
            uint64_t r = rel_indirectable(w, p, HAO_SK_CONF_RETRO);
            if (r && in_img(w, r, 8)) context(w, r);
        }
    } else
        st_conf++;
    if (flags & (1u << 6)) p += 4;                       /* retroactive context */
    if (w->slot) requirements(w, p, (flags >> 8) & 0xff);
    p += 12 * ((flags >> 8) & 0xff);                     /* conditional requirements */
    p += 8 * ((flags >> 24) & 0xff);                     /* conditional pack shape descriptors */
    if (flags & (1u << 16)) {                            /* resilient witnesses */
        uint64_t proto = rel_indirectable(w, cd, HAO_SK_CONF_PROTO);
        uint32_t n;
        if (!rd32(w, p, &n) || !in_img(w, p + 4, 8ull * n)) { bad_desc(cd, 0); return; }
        p += 4;
        for (uint32_t k = 0; k < n; k++, p += 8) {
            uint64_t req = rel_indirectable(w, p, HAO_SK_CONF_WITNESS);
            uint32_t rf;
            if (w->slot || !req) continue;               /* (not slots mode) a requirement newer than the protocol */
            if (!rd32_any(req, &rf)) { bad_desc(p, req); continue; }
            requirement_impl(w, p + 4, rf, K_WITNESS, proto, req);
        }
    }
    if (w->slot) {
        if (flags & (1u << 17)) p += 12;                 /* generic witness table */
        if (flags & (1u << 19)) (void)rel_indirectable(w, p + 4, HAO_SK_CONF_GLOBALACTOR);   /* {type, conformance} */
        return;
    }
    /* the witness table pattern: [0] the conformance descriptor, then one word per requirement; the generic witness
     * table gives its size in words, else the protocol's NumRequirements + 1 */
    uint64_t wt = rel_direct(w, cd + 8), proto = rel_indirectable(w, cd, HAO_SK_CONF_PROTO);
    uint32_t words = 0;
    if (flags & (1u << 17)) {                            /* generic witness table: {u16 size, u16, Instantiator, PrivateData} */
        uint16_t sz;
        if (rd16(w, p, &sz)) words = sz;
        fnfield(w, p + 4, K_WT_INSTANTIATOR);
    } else if (proto && rd32_any(proto + 16, &words))
        words++;
    /* Ordinary protocols have no exported Tq/Tj pair for the current exact
     * requirement lookup. Keep these untyped; never infer result semantics
     * from a target shared with a different witness slot. */
    if (wt && words < 4096 && in_img(w, wt, 8ull * words))
        for (uint32_t k = 1; k < words; k++) note_abs(w, wt + 8ull * k, K_WTPAT);
}

static void protocol(walk *w, uint64_t pd)
{
    uint32_t f, nsig, nreq;
    if (!rd32(w, pd, &f) || (f & 0x1f) != 3 || !rd32(w, pd + 12, &nsig) || !rd32(w, pd + 16, &nreq)) { bad_desc(pd, 0); return; }
    if (w->slot) { requirements(w, pd + 24, nsig); return; }   /* the requirement signature */
    uint64_t r = pd + 24 + 12ull * nsig;
    if (!in_img(w, r, 8ull * nreq)) { bad_desc(pd, 0); return; }
    st_protos++;
    for (uint32_t k = 0; k < nreq; k++, r += 8) {
        uint32_t rf;
        rd32(w, r, &rf);
        requirement_impl(w, r + 4, rf, K_DEFAULT_IMPL, pd, r);
    }
}

/* ------------------------------------------------------------- types */

/* MethodDescriptorFlags: kinds Method, Init, Getter, Setter, ModifyCoroutine, ReadCoroutine; the async bit
 * (0x40) makes a method async or a coroutine callee-allocated, and either Impl a function record (isData) */
static bool md_data(uint32_t f) { return f & 0x40; }

static void method_impl(walk *w, uint64_t field, uint32_t mflags, int kind)
{
    if (md_data(mflags)) fnrecord(w, rel_direct(w, field));
    else fnfield(w, field, kind);
}

static uint64_t align_up(uint64_t a, uint64_t n) { return (a + n - 1) & ~(n - 1); }

/* Skip TrailingGenericContextObjects (GenericContext.h) after a
 * TargetGenericContextDescriptorHeader {u16 NumParams, NumRequirements,
 * NumKeyArguments, Flags} at h; 0 if unreadable. In slots mode its
 * requirements are walked. */
static uint64_t generic_context(walk *w, uint64_t h)
{
    uint16_t np, nr, fl;
    if (!rd16(w, h, &np) || !rd16(w, h + 2, &nr) || !rd16(w, h + 6, &fl)) return 0;
    uint64_t p = h + 8 + np;                             /* GenericParamDescriptor: 1 byte each */
    if (w->slot) requirements(w, align_up(p, 4), nr);
    p = align_up(p, 4) + 12ull * nr;                     /* GenericRequirementDescriptor */
    if (fl & 1) {                                        /* type packs: header {u16 NumPacks, u16}, 8-byte descriptors */
        uint16_t npk;
        if (!rd16(w, p, &npk)) return 0;
        p += 4 + 8ull * npk;
    }
    if (fl & 2) {                                        /* conditional invertible protocols */
        uint16_t set, cnt = 0;
        if (!rd16(w, p, &set)) return 0;
        p += 2;
        unsigned nc = (unsigned)__builtin_popcount(set);
        if (nc && !rd16(w, p + 2ull * (nc - 1), &cnt)) return 0;   /* counts are cumulative */
        p += 2ull * nc;
        if (w->slot) requirements(w, align_up(p, 4), cnt);
        p = align_up(p, 4) + 12ull * cnt;
    }
    if (fl & 4) {                                        /* value parameters: {u32 NumValues}, 4-byte descriptors */
        uint32_t nv;
        p = align_up(p, 4);
        if (!rd32(w, p, &nv)) return 0;
        p += 4 + 4ull * nv;
    }
    return in_img(w, h, p - h) ? align_up(p, 4) : 0;
}

/* ... after a TypeGenericContextDescriptorHeader {rel32 InstantiationCache,
 * rel32 DefaultInstantiationPattern, the header above} at h */
static uint64_t skip_generic_context(walk *w, uint64_t h) { return generic_context(w, h + 8); }

/* GenericMetadataPattern (+ class pattern's Destroy, IVarDestroyer) */
static void generic_pattern(walk *w, uint64_t pat, bool cls, bool en)
{
    if (!pat || !in_img(w, pat, cls ? 20 : 16)) return;
    fnfield(w, pat, K_INSTANTIATE);
    fnfield(w, pat + 4, K_COMPLETE);
    if (cls) {
        fnfield(w, pat + 12, K_DESTROY);
        fnfield(w, pat + 16, K_DESTROY);
    } else
        note_vwt(w, rel_indirectable(w, pat + 12, HAO_SK_PATTERN_VWT), en);   /* GenericValueMetadataPattern.ValueWitnesses */
}

static void type_descriptor(walk *w, uint64_t td)
{
    uint32_t f;
    if (!rd32(w, td, &f)) { bad_desc(td, 0); return; }
    unsigned kind = f & 0x1f;
    if (kind < 16 || kind > 18) return;                  /* class, struct, enum */
    bool generic = f & 0x80, cls = kind == 16;
    uint32_t ks = f >> 16;                               /* TypeContextDescriptorFlags */
    unsigned init = ks & 3;                              /* 1 singleton, 2 foreign */
    if (!w->slot) st_types++;
    fnfield(w, td + 12, K_ACCESSOR);
    uint64_t p = td + (cls ? 44 : 28);
    if (generic) {
        generic_pattern(w, rel_direct(w, p + 4), cls, kind == 18);   /* TypeGenericContextDescriptorHeader.DefaultInstantiationPattern */
        if (!(p = skip_generic_context(w, p))) { bad_desc(td, 0); return; }
    }
    if (!cls) {
        if (init == 2) fnfield(w, p, K_COMPLETE);                       /* ForeignMetadataInitialization */
        else if (init == 1) fnfield(w, p + 8, K_COMPLETE);              /* SingletonMetadataInitialization */
        return;
    }
    bool resilient_super = ks & (1u << 13), vtable = ks & (1u << 15), overrides = ks & (1u << 14);
    uint32_t extra = 0;
    rd32(w, td + 28, &extra);
    if (resilient_super) {
        /* TargetResilientSuperclass: a nullable rel32 whose TypeReferenceKind is in the class flags (bits 9-11) */
        uint32_t v;
        unsigned rk = (ks >> 9) & 7;
        if (w->slot && rd32(w, p, &v) && v) {
            uint64_t t = p + (uint64_t)(int64_t)(int32_t)v;
            if (rk == 1 || rk == 3) reach(w, HAO_SK_SUPERCLASS, p, t);
            else if (rk == 0) context(w, t);
        }
        p += 4;
    }
    if (init == 2) { fnfield(w, p, K_COMPLETE); p += 4; }
    if (init == 1) {
        fnfield(w, p + 8, K_COMPLETE);
        if (resilient_super) {                                          /* ResilientClassMetadataPattern */
            uint64_t rp = rel_direct(w, p + 4);
            if (rp && in_img(w, rp, 12)) {
                fnfield(w, rp, K_RELOCATE);
                fnfield(w, rp + 4, K_DESTROY);
                fnfield(w, rp + 8, K_DESTROY);
            }
        }
        p += 12;
    }
    if (vtable) {
        uint32_t n;
        if (!rd32(w, p + 4, &n) || !in_img(w, p + 8, 8ull * n)) { bad_desc(td, 0); return; }
        p += 8;
        for (uint32_t k = 0; k < n; k++, p += 8) {
            uint32_t mf;
            rd32(w, p, &mf);
            method_impl(w, p + 4, mf, K_METHOD);
        }
    }
    if (overrides) {
        uint32_t n;
        if (!rd32(w, p, &n) || !in_img(w, p + 4, 12ull * n)) { bad_desc(td, 0); return; }
        p += 4;
        for (uint32_t k = 0; k < n; k++, p += 12) {   /* {Class, Method, Impl} */
            if (w->slot) {
                uint64_t c = rel_indirectable(w, p, HAO_SK_OVERRIDE_CLASS);
                if (c && in_img(w, c, 8)) context(w, c);
            }
            uint64_t m = rel_indirectable(w, p + 4, HAO_SK_OVERRIDE_METHOD);
            uint32_t mf;
            if (w->slot) continue;
            if (m && rd32_any(m, &mf)) method_impl(w, p + 8, mf, K_OVERRIDE);
            else bad_desc(p, m);
        }
    }
    if (resilient_super && (extra & 1)) p += 4;                         /* ObjCResilientClassStubInfo */
    if (generic && (ks & (1u << 3))) {                                  /* canonical prespecializations */
        uint32_t n;
        if (!rd32(w, p, &n) || !in_img(w, p + 4, 8ull * n + 4)) { bad_desc(td, 0); return; }
        p += 4 + 4ull * n;
        for (uint32_t k = 0; k < n; k++, p += 4) fnfield(w, p, K_PRESPEC);
        p += 4;                                                         /* caching once token */
    }
    if (f & 0x20) p = align_up(p + 2, 4);                               /* InvertibleProtocolSet (u16) */
    if (!generic && (ks & (1u << 3))) p += 4;                           /* SingletonMetadataPointer */
    if (ks & (1u << 6)) {                                               /* default override table */
        uint32_t n;
        if (!rd32(w, p, &n) || !in_img(w, p + 4, 12ull * n)) { bad_desc(td, 0); return; }
        p += 4;
        for (uint32_t k = 0; k < n; k++, p += 12) {   /* {Replacement, Original, Impl} */
            uint64_t m = rel_indirectable(w, p, HAO_SK_DEFOVER_REPL);
            uint32_t mf;
            if (w->slot) { (void)rel_indirectable(w, p + 4, HAO_SK_DEFOVER_ORIG); continue; }
            if (m && rd32_any(m, &mf)) method_impl(w, p + 8, mf, K_OVERRIDE);
            else bad_desc(p, m);
        }
    }
}

/* Slots mode: a context descriptor of this image, once: its Parent, then by
 * kind (MetadataValues.h ContextDescriptorKind) the generic requirements
 * of an extension (after rel32 ExtendedContext), anonymous or opaque type
 * context, a protocol's requirement signature, or a type's records. */
static void context(walk *w, uint64_t d)
{
    uint32_t f;
    if (!w->slot || !d || (d & 3) || !rd32(w, d, &f) || !in_img(w, d, 8) || w->depth > 32 || seen_once(w, d)) return;
    w->depth++;
    uint64_t parent = rel_indirectable(w, d + 4, HAO_SK_PARENT);
    if (parent && in_img(w, parent, 8)) context(w, parent);
    unsigned kind = f & 0x1f;
    bool generic = f & 0x80;
    if (kind == 1 && generic) {
        if (!generic_context(w, d + 12)) bad_desc(d, 0);
    } else if ((kind == 2 || kind == 4) && generic) {
        if (!generic_context(w, d + 8)) bad_desc(d, 0);
    } else if (kind == 3)
        protocol(w, d);
    else if (kind >= 16 && kind <= 18)
        type_descriptor(w, d);
    w->depth--;
}

/* ------------------------------------------------------ layout strings */

/* One op sequence of a type layout string (BytecodeLayouts.cpp), from p
 * until its End; entries go to w. Returns the address after the End, 0 if
 * an op is unknown or the sequence leaves [p, lim). */
static uint64_t ls_ops(walk *w, uint64_t p, uint64_t lim, int depth)
{
    if (depth > 8) return 0;
    for (;;) {
        uint64_t tag;
        if (p + 8 > lim || !rd64(w, p, &tag)) return 0;
        p += 8;
        unsigned k = (unsigned)(tag >> 56);
        uint64_t n1, n2, n3;
        switch (k) {
        case 0x00: return p;                                            /* End */
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
        case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0b:          /* reference counting kinds */
        case 0x0e:                                                      /* Existential */
            break;
        case 0x0c: p += 8; break;                                       /* Metatype: Metadata * */
        case 0x0f:                                                      /* Resilient: relative accessor (8 bytes) */
            if (p + 8 > lim) return 0;
            note(w, p, p + (uint64_t)(int64_t)*(const int32_t *)(uintptr_t)p, K_RESILIENT);
            p += 8;
            break;
        case 0x10: p += 48; break;                                      /* SinglePayloadEnumSimple; payload ops follow */
        case 0x11:                                                      /* SinglePayloadEnumFN: fn, refCountBytes, skip */
            if (p + 24 > lim) return 0;
            note(w, p, p + (uint64_t)(int64_t)*(const int32_t *)(uintptr_t)p, K_ENUMTAG);
            p += 24;
            break;
        case 0x12: p += 24; break;                                      /* SinglePayloadEnumFNResolved */
        case 0x13: p += 44; break;                                      /* SinglePayloadEnumGeneric */
        case 0x14: case 0x15: case 0x16: {                              /* MultiPayloadEnum FN / FNResolved / Generic */
            uint64_t hdr = 32;
            if (p + hdr > lim || !rd64(w, p + 8, &n1) || !rd64(w, p + 16, &n2) || !rd64(w, p + 24, &n3)) return 0;
            /* FN, FNResolved: {fn, numPayloads, refCountBytes, enumSize}; Generic: {tagBytes, numPayloads,
             * refCountBytes, enumSize} */
            uint64_t np = n1, rc = n2;
            if (np > 4096 || rc > lim - p) return 0;
            if (k == 0x14) note(w, p, p + (uint64_t)(int64_t)*(const int32_t *)(uintptr_t)p, K_ENUMTAG);
            p += hdr;
            uint64_t offs = p, cases = p + 8 * np, end = cases + rc;
            if (end > lim) return 0;
            for (uint64_t c = 0; c < np; c++) {
                uint64_t o;
                if (!rd64(w, offs + 8 * c, &o) || o >= rc || !ls_ops(w, cases + o, end, depth + 1)) return 0;
            }
            p = end;
            break;
        }
        default: return 0;                                              /* Generic (0x0d) never appears in a final string */
        }
        if (p > lim) return 0;
    }
}

/* A layout string: {u64 flags, size_t size} then ops. Its entries are kept
 * only if the whole string parses. */
static void layout_string(walk *w, uint64_t ls)
{
    uint64_t lim = hao_image_extent(w->img, ls);
    if (!lim || (ls & 7)) { st_bad_ls++; return; }
    size_t mark = w->n;
    if (!ls_ops(w, ls + 16, lim, 0)) { w->n = mark; st_bad_ls++; return; }
    st_ls++;
}

/* Static value metadata of this image's non-generic struct and enum types:
 * the full metadata is {layoutString (only with HasLayoutString),
 * valueWitnesses, kind, description, ...} (TargetTypeMetadataLayoutPrefix,
 * Metadata.h) in a data section, description pointing at the descriptor.
 * desc[k] is a descriptor address | 1 if it has a layout string | 2 if an
 * enum (descriptors are 4-aligned). */
static int cmp_desc(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a & ~3ull, y = *(const uint64_t *)b & ~3ull;
    return x < y ? -1 : x > y;
}

static void value_metadata(walk *w, uint64_t *desc, size_t nd)
{
    if (!nd) return;
    qsort(desc, nd, sizeof *desc, cmp_desc);
    uint64_t *seen = NULL;
    size_t ns = 0, cs = 0;
    hao_image *img = w->img;
    for (int s = 0; s < img->nsects; s++) {
        uint32_t type = img->sects[s].flags & SECTION_TYPE;
        if (type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL) continue;
        if (strncmp(img->sects[s].seg, "__DATA", 6) && strncmp(img->sects[s].seg, "__AUTH", 6)) continue;
        uint64_t a = align_up(img->sects[s].addr, 8), e = img->sects[s].addr + img->sects[s].size;
        for (a += 24; a + 8 <= e; a += 8) {
            uint64_t d = *(const uint64_t *)(uintptr_t)a;
            if (d & 3) continue;
            size_t lo = 0, hi = nd;
            while (lo < hi) { size_t m = (lo + hi) / 2; if ((desc[m] & ~3ull) < d) lo = m + 1; else hi = m; }
            if (lo == nd || (desc[lo] & ~3ull) != d) continue;
            uint64_t kind = *(const uint64_t *)(uintptr_t)(a - 8);
            if (kind != 0x200 && kind != 0x201 && kind != 0x202) continue;   /* MetadataKind Struct, Enum, Optional */
            note_vwt(w, *(const uint64_t *)(uintptr_t)(a - 16), desc[lo] & 2);
            uint64_t ls = *(const uint64_t *)(uintptr_t)(a - 24);
            if (!(desc[lo] & 1) || !ls || !hao_image_extent(img, ls)) continue;
            bool dup = false;
            for (size_t k = 0; k < ns && !dup; k++) dup = seen[k] == ls;
            if (dup) continue;
            if (ns == cs) {
                size_t c = cs ? 2 * cs : 64;
                void *q = realloc(seen, c * sizeof *seen);
                if (!q) break;
                seen = q;
                cs = c;
            }
            seen[ns++] = ls;
            layout_string(w, ls);
        }
    }
    free(seen);
}

/* -------------------------------------------------------- mangled names */

/* __swift5_typeref holds NUL-terminated mangled names; a byte 0x01-0x17
 * starts a 4-byte relative symbolic reference (0x18-0x1f an 8-byte
 * absolute one), whose bytes may be zero (Mangling.rst). \x09 is a direct
 * reference to a metadata access function, \x07/\x08 to an associated
 * conformance access function. In slots mode: \x02, \x04 and \x06 point
 * at a slot holding a context descriptor, conformance descriptor or
 * associated conformance descriptor (indirect references), \x01 at a
 * context descriptor, \x0a/\x0b at an extended existential type shape
 * (unique, or {rel32 UniqueCache, the shape}), \x0c at a rel32 to an ObjC
 * protocol reference (MetadataLookup.cpp _asObjectiveCProtocol). */
static void typerefs(walk *w)
{
    uint64_t n, a = hao_image_section(w->img, "__TEXT", "__swift5_typeref", &n);
    if (!a) return;
    const uint8_t *s = (const uint8_t *)(uintptr_t)a;
    for (uint64_t i = 0; i < n;) {
        uint8_t b = s[i];
        if (b >= 1 && b <= 0x17) {
            if (i + 5 > n) break;
            uint64_t f = a + i + 1;
            if (b == 7 || b == 8 || b == 9) fnfield(w, f, K_SYMREF);
            if (w->slot) {
                int32_t r;
                memcpy(&r, (const void *)(uintptr_t)f, 4);
                uint64_t t = f + (uint64_t)(int64_t)r;
                if (b == 2 || b == 4 || b == 6)
                    reach(w, b == 2 ? HAO_SK_TYPEREF_CTX : b == 4 ? HAO_SK_TYPEREF_CONF : HAO_SK_TYPEREF_ASSOC, f, t);
                else if (b == 1) context(w, t);
                else if (b == 0x0a) shape(w, t);
                else if (b == 0x0b) shape(w, t + 4);
                else if (b == 0x0c) {
                    uint32_t v;
                    if (rd32(w, t, &v) && v) reach(w, HAO_SK_TYPEREF_OBJCPROTO, t, t + (uint64_t)(int64_t)(int32_t)v);
                }
            }
            i += 5;
        } else if (b >= 0x18 && b <= 0x1f) i += 9;
        else i++;
    }
}

/* ------------------------------------- accessible functions, replacements */

/* __swift5_acfuncs: AccessibleFunctionRecord {rel32 Name, rel32
 * GenericEnvironment, rel32 FunctionType, rel32 Function, u32 Flags}
 * (Metadata.h). swift_distributed_execute_target reads a distributed
 * record's Function as an async function record and calls its function
 * (Distributed/DistributedActor.cpp). Flags define nothing else: a record
 * that is not distributed is not followed. */
static void accessible_functions(walk *w)
{
    uint64_t n, a = hao_image_section(w->img, "__TEXT", "__swift5_acfuncs", &n);
    for (uint64_t r = a; a && r + 20 <= a + n; r += 20) {
        uint32_t f;
        if (!rd32(w, r + 16, &f) || !(f & 1)) { bad_desc(r, 0); continue; }
        uint64_t rec = rel_direct(w, r + 12);
        if (rec && in_img(w, rec, 8)) fnrecord_kind(w, rec, K_DISTRIBUTED);
        else bad_desc(r, 0);
    }
}

/* __swift5_replace: AutomaticDynamicReplacements {u32 flags, u32 numScopes,
 * {rel32 scope, u32 flags}[numScopes]}, one after another
 * (MetadataLookup.cpp addImageDynamicReplacementBlockCallback); a
 * DynamicReplacementScope is {u32 flags, u32 numReplacements} and
 * DynamicReplacementDescriptor {rel32 indirectable replacedFunctionKey,
 * rel32 replacement, rel32 chainEntry, u32 flags}[numReplacements]
 * (Metadata.h). Enabling one stores the replacement's address in the
 * replaced function's chain root, which may be another image's (a native
 * library's dynamic function): a CompactFunctionPointer, or for an async or
 * callee-allocated coroutine key (key flags bit 16, 17) the function's
 * record. __swift5_replac2 maps opaque type descriptors only. */
static void replacements(walk *w)
{
    uint64_t n, a = hao_image_section(w->img, "__TEXT", "__swift5_replace", &n);
    for (uint64_t r = a; a && r + 8 <= a + n;) {
        uint32_t ns;
        if (!rd32(w, r + 4, &ns) || !in_img(w, r + 8, 8ull * ns)) { bad_desc(r, 0); return; }
        for (uint32_t k = 0; k < ns; k++) {
            uint64_t sc = rel_direct(w, r + 8 + 8ull * k);
            uint32_t nr;
            if (!sc || !rd32(w, sc + 4, &nr) || !in_img(w, sc + 8, 16ull * nr)) { bad_desc(r + 8 + 8ull * k, 0); continue; }
            for (uint32_t j = 0; j < nr; j++) {
                uint64_t d = sc + 8 + 16ull * j, key = rel_indirectable(w, d, HAO_SK_REPLACE_KEY);
                uint32_t kf;
                if (w->slot) continue;
                if (!key || !rd32_any(key + 4, &kf)) { bad_desc(d, key); continue; }
                if (kf & 0x30000u) fnrecord_kind(w, rel_direct(w, d + 4), K_REPLACEMENT);
                else fnfield(w, d + 4, K_REPLACEMENT);
            }
        }
        r += 8 + 8ull * ns;
    }
}

/* --------------------------------------------------------------- islands */

#define ISLAND_SLOT 32
#define MAX_ISLANDS 4096
/* [lo, hi) holds the entries made so far (hi is published after an entry is
 * written), [hi, cap) is room for the entries made at run time (key path
 * patterns, below) */
static struct { uint64_t lo, hi, cap; hao_image *img; } islands[MAX_ISLANDS];
static uint32_t n_islands;      /* published count: entries below it are complete */
#define DYN_SLOTS 1024          /* room per image for run-time entries */

bool hao_swiftmeta_is_island(uint64_t a)
{
    uint32_t n = __atomic_load_n(&n_islands, __ATOMIC_ACQUIRE);
    for (uint32_t k = 0; k < n; k++)
        if (a >= islands[k].lo && a < __atomic_load_n(&islands[k].hi, __ATOMIC_ACQUIRE)) return true;
    return false;
}

uint64_t hao_swiftmeta_island_target(uint64_t a)
{
    uint32_t n = __atomic_load_n(&n_islands, __ATOMIC_ACQUIRE);
    for (uint32_t k = 0; k < n; k++)
        if (a >= islands[k].lo && a < __atomic_load_n(&islands[k].hi, __ATOMIC_ACQUIRE)) {
            if ((a - islands[k].lo) % ISLAND_SLOT) return 0;
            uint64_t g;
            memcpy(&g, (const void *)(uintptr_t)(a + 2), 8);
            return g;
        }
    return 0;
}

static void emit_entry(uint8_t *e, uint64_t guest, uint64_t bridge)
{
    static const uint8_t pre[2] = {0x49, 0xbb};                /* movabs r11, imm64 */
    static const uint8_t mid[9] = {0x45, 0x31, 0xd2,           /* xor r10d, r10d */
                                   0xff, 0x25, 0, 0, 0, 0};     /* jmp [rip+0] */
    uint64_t cb = bridge ? bridge : (uint64_t)(uintptr_t)hao_callback_entry;
    memcpy(e, pre, 2);
    memcpy(e + 2, &guest, 8);
    memcpy(e + 10, mid, 9);
    memcpy(e + 19, &cb, 8);
    memset(e + 27, 0xcc, ISLAND_SLOT - 27);                    /* int3 */
}

/* Write e[0..n) (sorted by field, each field once) into the image: a rel32
 * to the island entry, or for an absolute slot the 8-byte thunk address.
 * Each run of fields inside one VM region is made writable once (copy on
 * write), written, and given back the protection it had. Returns how many
 * were written. */
static size_t rewrite(hao_image *img, const struct ent *e, size_t n)
{
    size_t done = 0;
    uint64_t pg = (uint64_t)getpagesize();
    for (size_t k = 0; k < n;) {
        if (img->dsc) {
            int32_t rel = (int32_t)(int64_t)(e[k].target - e[k].field);
            hao_dsc_writable(e[k].field, e[k].abs ? 8 : 4);
            if (e[k].abs) memcpy((void *)(uintptr_t)e[k].field, &e[k].target, 8);
            else memcpy((void *)(uintptr_t)e[k].field, &rel, 4);
            st_kind[e[k].kind]++;
            if (e[k].bridge) st_reverse++;
            done++, k++;
            continue;
        }
        mach_vm_address_t ra = e[k].field & ~(pg - 1);
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS ||
            ra > e[k].field) {
            st_unplaced++, k++;
            continue;
        }
        size_t j = k;
        while (j < n && e[j].field + (e[j].abs ? 8 : 4) <= ra + rs) j++;
        if (j == k) { st_unplaced++, k++; continue; }
        uint64_t lo = e[k].field & ~(pg - 1), hi = align_up(e[j - 1].field + 8, pg);
        if (hi > ra + rs) hi = ra + rs;
        int prot = (info.protection & VM_PROT_READ ? PROT_READ : 0) | (info.protection & VM_PROT_WRITE ? PROT_WRITE : 0);
        bool ro = !(prot & PROT_WRITE);
        if (ro && mprotect((void *)(uintptr_t)lo, hi - lo, prot | PROT_WRITE)) { st_unplaced += j - k; k = j; continue; }
        for (; k < j; k++) {
            int32_t rel = (int32_t)(int64_t)(e[k].target - e[k].field);
            if (e[k].abs) memcpy((void *)(uintptr_t)e[k].field, &e[k].target, 8);
            else memcpy((void *)(uintptr_t)e[k].field, &rel, 4);
            st_kind[e[k].kind]++;
            if (e[k].bridge) st_reverse++;
            done++;
        }
        if (ro) mprotect((void *)(uintptr_t)lo, hi - lo, prot);
    }
    return done;
}

static int cmp_target(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->target != y->target) return (x->target > y->target) - (x->target < y->target);
    if (x->bridge != y->bridge) return (x->bridge > y->bridge) - (x->bridge < y->bridge);
    return (x->field > y->field) - (x->field < y->field);
}

static int cmp_field(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    return (x->field > y->field) - (x->field < y->field);
}

/* ----------------------------------------------------------------- image */

/* The absolute slots get the plain thunk fnslots.c would give them later.
 * fnslots.c keeps the guest address of a function whose address the
 * image's code materializes (pointer identity: C code compares a table
 * slot with &f). A value witness table or witness table pattern slot is
 * read only by the Swift runtime and by Swift witness calls, neither of
 * which compares a witness with anything (stdlib/public/runtime only
 * calls them; ReflectionMirror.cpp calls destructiveProjectEnumData), so
 * these get the thunk even then: identical code folding makes one
 * function both a value witness and, say, the "ret" continuation a
 * yield-once coroutine returns (xprotectd: the enum projection witness
 * Mirror calls was kept and took the fault path). Counted as
 * thunked_address_taken. */
static void abs_slots(walk *w)
{
    if (!w->na || w->oom) return;
    qsort(w->a, w->na, sizeof *w->a, cmp_target);
    uint64_t *fn = malloc(w->na * sizeof *fn);
    uint8_t *taken = calloc(w->na, 1);
    if (!fn || !taken) { free(fn); free(taken); return; }
    size_t nf = 0;
    for (size_t k = 0; k < w->na; k++) if (!nf || fn[nf - 1] != w->a[k].target) fn[nf++] = w->a[k].target;
    hao_fnslots_address_taken(w->img, fn, nf, taken);
    size_t m = 0, f = 0;
    for (size_t k = 0; k < w->na; k++) {
        while (fn[f] != w->a[k].target) f++;
        if (taken[f]) {
            if (w->a[k].kind != K_VWT && w->a[k].kind != K_WTPAT) { st_abs_kept++; continue; }
            st_abs_taken++;
            if (hao_verbose > 1)
                fprintf(stderr, "hao: swiftmeta: %s slot %#llx -> %#llx: address taken in code, thunked (Swift ABI slot)\n",
                        kinds[w->a[k].kind].name, (unsigned long long)(w->a[k].field - w->img->slide),
                        (unsigned long long)(w->a[k].target - w->img->slide));
        }
        uint64_t t = hao_thunk_for(w->a[k].target);
        if (t == w->a[k].target) { st_unplaced++; continue; }   /* thunk table full (logged) */
        w->a[m] = w->a[k];
        w->a[m++].target = t;
    }
    qsort(w->a, m, sizeof *w->a, cmp_field);
    size_t u = 0;
    for (size_t k = 0; k < m; k++) if (!u || w->a[k].field != w->a[u - 1].field) w->a[u++] = w->a[k];
    st_fields += rewrite(w->img, w->a, u);
    free(fn);
    free(taken);
}

static uint64_t *types_of(walk *w, size_t *nd_out)
{
    uint64_t *desc = NULL;
    size_t nd = 0, cd = 0;
    const char *secs[] = {"__swift5_types", "__swift5_types2"};
    for (int s = 0; s < 2; s++) {
        uint64_t n, a = hao_image_section(w->img, "__TEXT", secs[s], &n);
        for (uint64_t r = a; a && r + 4 <= a + n; r += 4) {
            uint32_t v;
            if (!rd32(w, r, &v) || (v & 3) != 0) continue;   /* TypeReferenceKind 0: direct; indirect ones are not ours */
            uint64_t td = r + (uint64_t)(int64_t)(int32_t)(v & ~3u);
            if (!in_img(w, td, 28)) { bad_desc(r, 0); continue; }
            type_descriptor(w, td);
            uint32_t f;
            rd32(w, td, &f);
            if (((f & 0x1f) == 17 || (f & 0x1f) == 18) && !(f & 0x80)) {   /* non-generic struct or enum */
                if (nd == cd) {
                    size_t c = cd ? 2 * cd : 256;
                    void *q = realloc(desc, c * sizeof *desc);
                    if (!q) continue;
                    desc = q;
                    cd = c;
                }
                desc[nd++] = td | ((f >> 16) & (1u << 4) ? 1 : 0) | ((f & 0x1f) == 18 ? 2 : 0);   /* HasLayoutString, enum */
            }
        }
    }
    *nd_out = nd;
    return desc;
}

/* Slots mode: a TargetGenericEnvironment (GenericContext.h) at g, once: {flags: NumGenericParameterLevels
 * (12 bits), NumGenericRequirements (16 bits)}, u16 parameter counts per level, the last level's
 * GenericParamDescriptors, the requirements (accessible functions, key path patterns) */
static void generic_environment(walk *w, uint64_t g)
{
    uint32_t gf;
    uint16_t last = 0;
    if (!g || !rd32(w, g, &gf) || seen_once(w, g)) return;
    unsigned lv = gf & 0xfff, nr = (gf >> 12) & 0xffff;
    if (lv && !rd16(w, g + 4 + 2ull * (lv - 1), &last)) { bad_desc(g, 0); return; }
    requirements(w, align_up(g + 4 + 2ull * lv + last, 4), nr);
}

/*
 * Every relative indirect(able) reference of img's Swift metadata that the
 * native Swift runtime follows and whose pointer-sized slot lies outside the
 * image: fn(ctx, kind, field, slot) for each (a slot reached from several
 * fields is reported for each). A shared-cache image's metadata references
 * other images' descriptors (a native protocol, its requirements, a native
 * type in a mangled name) through slots in the cache builder's uniqued
 * GOTs, which no segment of the image holds, and which dsc.c binds from
 * this walk (hao_dsc_bind) before the image is registered. The records are
 * the image's conformances (__swift5_proto), protocols, types and their
 * records (__swift5_protos, __swift5_types, __swift5_types2), mangled names
 * (__swift5_typeref), accessible functions' generic environments
 * (__swift5_acfuncs) and dynamic replacements (__swift5_replace,
 * __swift5_replac2), and every context descriptor, conformance and
 * existential shape of the image that those reach (parents, symbolic
 * references), each walked once. Data only: independent of
 * HAO_NO_SWIFT_THUNKS and of JIT memory. Returns how many were reported.
 */
size_t hao_swiftmeta_slots(hao_image *img, hao_swift_slot_fn fn, void *ctx)
{
    walk w = {.img = img, .slot = fn, .sctx = ctx};
    uint64_t n, a = hao_image_section(img, "__TEXT", "__swift5_proto", &n);
    for (uint64_t r = a; a && r + 4 <= a + n; r += 4) {
        uint64_t cd = rel_direct(&w, r);
        if (cd && in_img(&w, cd, 16)) conformance(&w, cd);
        else bad_desc_at(&w, __LINE__, r, 0);
    }
    /* TargetProtocolRecord: a RelativeContextPointerIntPair (bit 0 indirect, bit 1 reserved) */
    a = hao_image_section(img, "__TEXT", "__swift5_protos", &n);
    for (uint64_t r = a; a && r + 4 <= a + n; r += 4) {
        uint32_t v;
        if (!rd32(&w, r, &v) || !v) continue;
        uint64_t t = r + (uint64_t)(int64_t)(int32_t)(v & ~3u);
        if (v & 1) reach(&w, HAO_SK_PROTO_RECORD, r, t);
        else context(&w, t);
    }
    /* TargetTypeMetadataRecord: TypeReferenceKind in the low two bits (0 direct, 1 indirect descriptor) */
    const char *secs[] = {"__swift5_types", "__swift5_types2"};
    for (int s = 0; s < 2; s++) {
        a = hao_image_section(img, "__TEXT", secs[s], &n);
        for (uint64_t r = a; a && r + 4 <= a + n; r += 4) {
            uint32_t v;
            if (!rd32(&w, r, &v) || !v) continue;
            uint64_t t = r + (uint64_t)(int64_t)(int32_t)(v & ~3u);
            if ((v & 3) == 1) reach(&w, HAO_SK_TYPE_RECORD, r, t);
            else if ((v & 3) == 0) context(&w, t);
        }
    }
    typerefs(&w);
    /* AccessibleFunctionRecord {Name, GenericEnvironment, FunctionType, Function, Flags} */
    a = hao_image_section(img, "__TEXT", "__swift5_acfuncs", &n);
    for (uint64_t r = a; a && r + 20 <= a + n; r += 20) generic_environment(&w, rel_direct(&w, r + 4));
    replacements(&w);
    /* AutomaticDynamicReplacementsSome {flags, numEntries}, {rel32 indirectable original, rel32 replacement}
     * (MetadataLookup.cpp) */
    a = hao_image_section(img, "__TEXT", "__swift5_replac2", &n);
    uint32_t ne;
    if (a && n >= 8 && rd32(&w, a + 4, &ne) && in_img(&w, a + 8, 8ull * ne))
        for (uint32_t k = 0; k < ne; k++) (void)rel_indirectable(&w, a + 8 + 8ull * k, HAO_SK_REPLACE2_ORIG);
    free(w.seen);
    return w.nreach;
}

/* Executable room of size bytes for island entries within rel32 reach of [lo, hi): first the room the loader
 * mapped after the image (macho.c, hao_island_reserve), then for a shared-cache image a gap of the cache's
 * reservation (dsc.c), then a free range hao_map_near finds now. The first two exist because by the time
 * an image's metadata is walked (its initialization, after every dependency has loaded and initialized) the
 * process may have filled the 2 GB around it (WindowManager, searchpartyd, searchpartyuseragent,
 * identityservicesd, findmylocateagent, sharingd, Finder and AutoUnlockDaemon, a cache image, do; without
 * the room their entries keep the NX-fault path). */
static uint64_t st_isl_room, st_isl_gap, st_isl_search, st_isl_failed;
static uint8_t *island_place(hao_image *img, uint64_t lo, uint64_t hi, uint64_t size)
{
    uint64_t at = __atomic_load_n(&img->isl_next, __ATOMIC_ACQUIRE);
    while (img->isl_lo && at + size <= img->isl_hi && hao_near_range(at, lo, hi, size))
        if (__atomic_compare_exchange_n(&img->isl_next, &at, at + size, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            st_isl_room++;
            return (uint8_t *)(uintptr_t)at;
        }
    uint8_t *p = img->dsc ? hao_dsc_island(lo, hi, size) : NULL;
    if (p) { st_isl_gap++; return p; }
    if ((p = hao_map_near(lo, hi, size))) st_isl_search++;
    return p;
}

void hao_swiftmeta_image(hao_image *img)
{
    if (hao_opt.no_swift_thunks) return;
    uint64_t n;
    bool any = false;
    const char *secs[] = {"__swift5_proto", "__swift5_types", "__swift5_types2", "__swift5_protos", "__swift5_typeref",
                          "__swift5_acfuncs", "__swift5_replace", "__swift5_entry"};
    for (int s = 0; s < 8 && !any; s++) any = hao_image_section(img, "__TEXT", secs[s], &n) && n;
    if (!any) return;
    if (!hao_have_thunks()) { st_nojit++; return; }
    st_images++;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    walk w = {.img = img};

    uint64_t a = hao_image_section(img, "__TEXT", "__swift5_proto", &n);
    for (uint64_t r = a; a && r + 4 <= a + n; r += 4) {
        uint64_t cd = rel_direct(&w, r);
        if (cd && in_img(&w, cd, 16)) conformance(&w, cd);
        else bad_desc_at(&w, __LINE__, r, 0);
    }
    a = hao_image_section(img, "__TEXT", "__swift5_protos", &n);
    for (uint64_t r = a; a && r + 4 <= a + n; r += 4) {
        uint32_t v;
        if (!rd32(&w, r, &v) || (v & 1)) continue;       /* indirect: another image's protocol */
        uint64_t pd = r + (uint64_t)(int64_t)(int32_t)(v & ~3u);
        if (in_img(&w, pd, 24)) protocol(&w, pd);
        else bad_desc_at(&w, __LINE__, r, 0);
    }
    size_t nd = 0;
    uint64_t *desc = types_of(&w, &nd), bad_ls = st_bad_ls;
    value_metadata(&w, desc, nd);
    free(desc);
    if (st_bad_ls > bad_ls) {                            /* fail closed: said, not skipped silently */
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "%s: %llu layout strings not parsed: their functions keep the NX-fault path\n",
                strrchr(img->path, '/') ? strrchr(img->path, '/') + 1 : img->path, (unsigned long long)(st_bad_ls - bad_ls));
    }
    typerefs(&w);
    accessible_functions(&w);
    replacements(&w);
    /* the entry point record {CompactFunctionPointer main; u32 flags (bit 0: an @main type)}
     * (lib/IRGen/GenDecl.cpp IRGenerator::emitEntryPointInfo) */
    a = hao_image_section(img, "__TEXT", "__swift5_entry", &n);
    if (a && n >= 4) fnfield(&w, a, K_ENTRY);
    abs_slots(&w);
    free(w.a);
    free(w.vwt);
    if (w.oom || !w.n) { free(w.e); return; }

    /* one island entry per distinct target and reverse signature */
    qsort(w.e, w.n, sizeof *w.e, cmp_target);
    size_t nt = 0;
    for (size_t k = 0; k < w.n; k++)
        if (!k || w.e[k].target != w.e[k - 1].target || w.e[k].bridge != w.e[k - 1].bridge) nt++;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (size_t k = 0; k < w.n; k++) {
        if (w.e[k].field < lo) lo = w.e[k].field;
        if (w.e[k].field + 8 > hi) hi = w.e[k].field + 8;
    }
    if (!img->dsc) {                                     /* run-time entries may serve any field of the image */
        if (img->base < lo) lo = img->base;
        if (img->base + img->vmsize > hi) hi = img->base + img->vmsize;
    }
    uint64_t size = align_up((nt + DYN_SLOTS) * ISLAND_SLOT, (uint64_t)getpagesize());
    uint32_t slot = __atomic_load_n(&n_islands, __ATOMIC_RELAXED);
    uint8_t *isl = slot < MAX_ISLANDS ? island_place(img, lo, hi, size) : NULL;
    if (!isl) {
        st_unplaced += w.n;
        st_isl_failed++;
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "%s: no island within 2 GB of its Swift metadata: %zu entries keep the NX-fault path "
                "(fields [%#llx, %#llx), %llu KB; room %s)\n",
                strrchr(img->path, '/') ? strrchr(img->path, '/') + 1 : img->path, w.n, (unsigned long long)lo,
                (unsigned long long)hi, (unsigned long long)(size >> 10),
                img->isl_lo ? "too small" : img->dsc ? "none in the cache's gaps" : "not reserved at load");
        free(w.e);
        return;
    }
    size_t t = 0;
    uint64_t prev = 0, prev_bridge = 0;
    for (size_t k = 0; k < w.n; k++) {
        uint64_t g = w.e[k].target;
        bool different = !k || g != prev || w.e[k].bridge != prev_bridge;
        if (k && different) t++;
        if (different) emit_entry(isl + t * ISLAND_SLOT, g, w.e[k].bridge);
        prev = g;
        prev_bridge = w.e[k].bridge;
        w.e[k].target = (uint64_t)(uintptr_t)(isl + t * ISLAND_SLOT);   /* now: the entry */
    }
    memset(isl + nt * ISLAND_SLOT, 0xcc, size - nt * ISLAND_SLOT);
    /* publish the island before any field points at it */
    islands[slot].lo = (uint64_t)(uintptr_t)isl;
    islands[slot].hi = (uint64_t)(uintptr_t)isl + nt * ISLAND_SLOT;
    islands[slot].cap = (uint64_t)(uintptr_t)isl + size;
    islands[slot].img = img;
    __atomic_store_n(&n_islands, slot + 1, __ATOMIC_RELEASE);
    st_islands_bytes += size;

    /* rewrite the fields; one reached twice (a layout string two metadata records share) once */
    qsort(w.e, w.n, sizeof *w.e, cmp_field);
    size_t m = 0;
    for (size_t k = 0; k < w.n; k++) if (!m || w.e[k].field != w.e[m - 1].field) w.e[m++] = w.e[k];
    size_t done = rewrite(img, w.e, m);
    st_entries += nt;
    st_fields += done;
    uint64_t us = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000;
    st_us += us;
    if (hao_verbose)
        fprintf(stderr, "hao: %s: %zu Swift metadata fields -> %zu island entries (%llu KB, %llu us)\n",
                strrchr(img->path, '/') ? strrchr(img->path, '/') + 1 : img->path, done, nt, (unsigned long long)(size >> 10),
                (unsigned long long)us);
    free(w.e);
}

/* ------------------------------------------------- async entry points
 *
 * Guest async code hands native code more kinds of guest entry point,
 * none through metadata:
 *   - an AsyncContext's ResumeParent (include/swift/ABI/Task.h: {Parent,
 *     ResumeParent, ...}): the continuation a native async callee returns
 *     to, and the one swift_continuation_init copies into the task
 *     (Concurrency/Task.cpp). It is an absolute pointer the guest stored,
 *     so it gets a plain thunk (hao_thunk_for);
 *   - an async function pointer record {rel32 Function, u32
 *     ExpectedContextSize} passed as closureEntry to swift_task_create and
 *     swift_asyncLet_begin (Runtime/Concurrency.h), which read it once
 *     (getAsyncClosureEntryPointAndContextSize). The call gets a copy of the
 *     record whose Function is the guest function's plain thunk;
 *   - the same record as an async function value passed to a native Swift
 *     function that takes one (hao_swiftmeta_async_args, below).
 * Counted in the stats line (resume_parent, task_entry, async_arg). */

static uint64_t st_resume, st_afp, st_afp_full, st_afp_arg;

/* ctx: the AsyncContext a native swiftasync callee receives (x22), or a
 * ContinuationAsyncContext */
void hao_swiftmeta_resume_parent(uint64_t ctx)
{
    if (hao_opt.no_swift_thunks || !ctx || (ctx & 7)) return;
    uint64_t *rp = (uint64_t *)(uintptr_t)(ctx + 8);
    uint64_t r = *rp & 0x00007fffffffffffull;          /* the arm64e signature, if the guest signed it */
    if (!hao_is_guest_function(r)) return;
    uint64_t e = hao_thunk_for(r);
    if (e == r) return;
    *rp = e;
    __atomic_fetch_add(&st_resume, 1, __ATOMIC_RELAXED);
}

#define AFP_SLOTS 8192
static struct { uint64_t key, rec; } afp_tab[AFP_SLOTS];
static uint8_t *afp_area;
static uint32_t afp_used;
static pthread_once_t afp_once = PTHREAD_ONCE_INIT;

static void afp_map(void)
{
    uint64_t size = 16ull * AFP_SLOTS;
    uint64_t lo, hi;
    /* Future thunks can occupy the entire reserved area. Placement near
     * only the first thunk could put later records outside rel32 reach. */
    if (hao_thunk_range(&lo, &hi))
        afp_area = hao_map_near_prot(lo, hi, size, PROT_READ | PROT_WRITE, 0);
}

/* closureEntry of a task: a hao-owned copy of the record with Function at
 * the thunk, or the guest's own record when none can be made (arg: made for
 * an async function value handed to a native Swift function, below) */
static uint64_t task_entry(uint64_t afp, bool arg)
{
    hao_image *im = afp ? hao_image_for_addr(afp) : NULL;
    if (hao_opt.no_swift_thunks || !im || (afp & 3) || !hao_image_extent(im, afp + 7)) return afp;
    int32_t rel;
    uint32_t size;
    memcpy(&rel, (const void *)(uintptr_t)afp, 4);
    memcpy(&size, (const void *)(uintptr_t)(afp + 4), 4);
    uint64_t fn = afp + (uint64_t)(int64_t)rel;
    if (!hao_is_guest_function(fn)) return afp;
    uint64_t th = hao_thunk_for(fn);
    if (th == fn) return afp;
    uint32_t h = (uint32_t)((afp * 0x9E3779B97F4A7C15ull) >> 51) & (AFP_SLOTS - 1);
    for (uint32_t k = 0; k < AFP_SLOTS; k++, h = (h + 1) & (AFP_SLOTS - 1)) {
        uint64_t cur = __atomic_load_n(&afp_tab[h].key, __ATOMIC_ACQUIRE), zero = 0;
        if (cur == afp) {
            uint64_t r;
            while (!(r = __atomic_load_n(&afp_tab[h].rec, __ATOMIC_ACQUIRE))) { }   /* its maker is filling it */
            return r;
        }
        if (cur) continue;
        if (!__atomic_compare_exchange_n(&afp_tab[h].key, &zero, afp, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (zero != afp) continue;
            uint64_t r;
            while (!(r = __atomic_load_n(&afp_tab[h].rec, __ATOMIC_ACQUIRE))) { }
            return r;
        }
        hao_crit_enter();
        pthread_once(&afp_once, afp_map);
        hao_crit_leave();
        uint32_t i = __atomic_fetch_add(&afp_used, 1, __ATOMIC_RELAXED);
        int64_t d = afp_area ? (int64_t)(th - ((uint64_t)(uintptr_t)afp_area + 16ull * i)) : 0;
        uint64_t r = afp;
        if (afp_area && i < AFP_SLOTS && d > INT32_MIN && d < INT32_MAX) {
            uint8_t *rec = afp_area + 16ull * i;
            int32_t r32 = (int32_t)d;
            memcpy(rec, &r32, 4);
            memcpy(rec + 4, &size, 4);
            r = (uint64_t)(uintptr_t)rec;
            __atomic_fetch_add(arg ? &st_afp_arg : &st_afp, 1, __ATOMIC_RELAXED);
            if (hao_verbose > 1)
                fprintf(stderr, "hao: swiftmeta: async function record %#llx (%s %#llx) -> %#llx for %s\n",
                        (unsigned long long)(afp - im->slide), strrchr(im->path, '/') ? strrchr(im->path, '/') + 1 : im->path,
                        (unsigned long long)(fn - im->slide), (unsigned long long)r, arg ? "a native Swift function's async parameter" : "a task");
        } else
            __atomic_fetch_add(&st_afp_full, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&afp_tab[h].rec, r, __ATOMIC_RELEASE);
        return r;
    }
    __atomic_fetch_add(&st_afp_full, 1, __ATOMIC_RELAXED);
    return afp;
}

/* An async function value (a thick async function: {record, context}) that
 * guest code passes to a native Swift function whose mangled name has a
 * parameter of async function type (bridge.c, HAO_PLAN_ASYNCARG): the
 * native callee, or a task it creates with it (searchpartyd: FindMyBase
 * WorkItemQueue.enqueue), reads the record's Function and jumps to the
 * guest function. No metadata names such a record; IRGen emits it in
 * __TEXT,__const ({CompactFunctionPointer Function, u32
 * ExpectedContextSize}, ABI/Executor.h AsyncFunctionPointer). So each
 * argument register that points 4-aligned into a non-code section of a
 * guest image's __TEXT segment, at a relative pointer to a function start
 * of the same image, is replaced by the hao-owned copy task_entry makes
 * (one per record). Stack arguments are not looked at. */
static bool afp_record(uint64_t a)
{
    if (!a || (a & 3)) return false;
    hao_image *im = hao_image_for_addr(a);
    if (!im || !hao_image_extent(im, a + 7)) return false;
    for (int s = 0; s < im->nsects; s++) {
        if (a - im->sects[s].addr >= im->sects[s].size || a + 8 > im->sects[s].addr + im->sects[s].size) continue;
        if (strcmp(im->sects[s].seg, "__TEXT") || (im->sects[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)))
            return false;
        int32_t rel;
        memcpy(&rel, (const void *)(uintptr_t)a, 4);
        uint64_t fn = a + (uint64_t)(int64_t)rel;
        return rel && hao_is_guest_function(fn) && hao_image_for_code(fn) == im;
    }
    return false;
}

void hao_swiftmeta_async_args(hao_cpu *c)
{
    if (hao_opt.no_swift_thunks) return;
    for (int k = 0; k < 8; k++)
        if (afp_record(c->x[k])) c->x[k] = task_entry(c->x[k], true);
}

static void *nat_continuation_init, *nat_task_create, *nat_asynclet_begin;
static void *nat_get_keypath;
static void trap_get_keypath(hao_thread *t, uint64_t arg);   /* key paths, below */
static void *nat_instantiate_ls[2];
static void trap_instantiate_ls(hao_thread *t, uint64_t arg);   /* layout string templates, below */

static void trap_continuation_init(hao_thread *t, uint64_t arg)
{
    (void)arg;
    hao_swiftmeta_resume_parent(t->cpu.x[0]);
    hao_thunk_native_args(t);
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)nat_continuation_init, &t->cpu, 16);
}

/* swift_task_create(flags, options, futureResultType, closureEntry, closureContext);
 * swift_asyncLet_begin(alet, options, futureResultType, closureEntryPoint, closureContext, resultBuffer) */
static void trap_task_entry(hao_thread *t, uint64_t arg)
{
    t->cpu.x[3] = task_entry(t->cpu.x[3], false);
    hao_thunk_native_args(t);
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)(arg ? nat_asynclet_begin : nat_task_create), &t->cpu, 16);
}

static const struct { const char *name; void **fn; hao_trap_fn trap; uint64_t arg; } swiftrt_traps[] = {
    {"_swift_continuation_init", &nat_continuation_init, trap_continuation_init, 0},
    {"_swift_task_create", &nat_task_create, trap_task_entry, 0},
    {"_swift_asyncLet_begin", &nat_asynclet_begin, trap_task_entry, 1},
    {"_swift_getKeyPath", &nat_get_keypath, trap_get_keypath, 0},
    {"_swift_cvw_instantiateLayoutString", &nat_instantiate_ls[0], trap_instantiate_ls, 0},
    {"_swift_generic_instantiateLayoutString", &nat_instantiate_ls[1], trap_instantiate_ls, 1},
};

static hao_name_index swiftrt_ix = HAO_NAME_INDEX(swiftrt_traps, NULL);

void hao_swiftmeta_interpose_names(hao_name_fn cb, void *ctx) { hao_name_each(&swiftrt_ix, cb, ctx); }

bool hao_swiftmeta_interpose(const char *name, uint64_t *addr)
{
#define tab swiftrt_traps
    long k = hao_name_find(&swiftrt_ix, name);
    if (k < 0) return false;
    /* without thunks only swift_getKeyPath, which binds shared-cache patterns' slots (data) */
    if (hao_opt.no_swift_thunks && tab[k].trap != trap_get_keypath) return false;
    if (!*tab[k].fn) *tab[k].fn = dlsym(RTLD_DEFAULT, name + 1);
    if (!*tab[k].fn) return false;
    *addr = hao_trap_address(name, tab[k].trap, tab[k].arg);
    return true;
#undef tab
}

/* --------------------------------------------------------- key paths
 *
 * A key path literal is a pattern in the guest's __TEXT,__const that guest
 * code hands to swift_getKeyPath(pattern, arguments); the native stdlib
 * instantiates a KeyPath object from it, resolving the relative pointers
 * of computed components' accessors to absolute ones, and calls them
 * (KeyPath.swift _swift_getKeyPath, _walkKeyPathPattern). No metadata
 * names a pattern, so each one is walked on the call that hands it over,
 * once, and its accessors' relative pointers rewritten to island entries
 * made then. Layout (KeyPath.swift, SwiftShims KeyPath.h): {rel32 once
 * cache, rel32 generic environment, rel32 root, rel32 leaf, rel32 KVC
 * string, u32 buffer header (size in the low 24 bits)}, then components:
 * a u32 header (kind in bits 24-30: 0 external, 1 struct, 2 computed,
 * 3 class, 4 optional), its body, and between two components a rel32 to
 * the intermediate type. */

static pthread_mutex_t dyn_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t st_kp, st_kp_bad, st_dyn_full, st_kp_slots, st_kp_slots_left;
#define KP_DONE 4096
static uint64_t kp_done[KP_DONE];
static uint32_t n_kp_done;

void hao_swiftmeta_fork_child(void) { dyn_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

/* an island entry for guest function g of img, made now if need be (dyn_lock held); 0 if none can be */
static uint64_t dyn_entry(hao_image *img, uint64_t g, uint64_t field)
{
    uint32_t n = __atomic_load_n(&n_islands, __ATOMIC_ACQUIRE), k;
    for (k = 0; k < n && islands[k].img != img; k++) { }
    if (k == n) {                                        /* an image with no static entries: its island now */
        if (n >= MAX_ISLANDS) return 0;
        uint64_t size = align_up(DYN_SLOTS * ISLAND_SLOT, (uint64_t)getpagesize());
        /* a cache image's [base, base+vmsize) is its __TEXT, where patterns and layout strings lie */
        uint8_t *isl = island_place(img, img->base, img->base + img->vmsize, size);
        if (!isl) { st_isl_failed++; return 0; }
        memset(isl, 0xcc, size);
        islands[n].lo = islands[n].hi = (uint64_t)(uintptr_t)isl;
        islands[n].cap = (uint64_t)(uintptr_t)isl + size;
        islands[n].img = img;
        st_islands_bytes += size;
        __atomic_store_n(&n_islands, n + 1, __ATOMIC_RELEASE);
    }
    for (uint64_t e = islands[k].lo; e < islands[k].hi; e += ISLAND_SLOT) {
        uint64_t bridge;
        memcpy(&bridge, (const void *)(uintptr_t)(e + 19), 8);
        if (bridge == (uint64_t)(uintptr_t)hao_callback_entry && hao_swiftmeta_island_target(e) == g)
            return hao_near_range(e, field, field + 4, ISLAND_SLOT) ? e : 0;
    }
    uint64_t e = islands[k].hi;
    if (e + ISLAND_SLOT > islands[k].cap) { st_dyn_full++; return 0; }
    if (!hao_near_range(e, field, field + 4, ISLAND_SLOT)) return 0;
    emit_entry((uint8_t *)(uintptr_t)e, g, 0);
    __atomic_store_n(&islands[k].hi, e + ISLAND_SLOT, __ATOMIC_RELEASE);
    st_entries++;
    return e;
}

/* an accessor field of a pattern component in img */
static void kp_fn_kind(hao_image *img, uint64_t field, int kind)
{
    walk w = {.img = img};
    uint64_t g = rel_direct(&w, field);
    if (!g) return;
    if (!hao_is_guest_function(g) || hao_image_for_code(g) != img) { st_notfn[kind]++; return; }
    uint64_t e = dyn_entry(img, g, field);
    if (!e) { st_unplaced++; return; }
    struct ent one = {field, e, (uint8_t)kind, 0};
    st_fields += rewrite(img, &one, 1);
}

static void kp_fn(hao_image *img, uint64_t field) { kp_fn_kind(img, field, K_KEYPATH); }

/* a computed component's body at p (header h): id, getter, [setter], [layout, witnesses, initializer]. An id of
 * kind pointer (neither ComputedIDByStoredProperty 0x00200000 nor ComputedIDByVTableOffset 0x00100000) whose
 * resolution (the low 4 bits) is UnresolvedFunctionCall (1) is a CompactFunctionPointer to a function the stdlib
 * calls to make the identifier (KeyPath.swift, InstantiateKeyPathBuffer: an @objc property's selector getter). */
static uint64_t kp_computed(hao_image *img, uint64_t p, uint32_t h, bool accessors, bool args)
{
    if (accessors && !(h & 0x00300000u) && (h & 0xfu) == 1) kp_fn_kind(img, p, K_KEYPATH_ID);
    if (accessors) kp_fn(img, p + 4);
    p += 8;
    if (h & 0x00400000u) { if (accessors) kp_fn(img, p); p += 4; }   /* settable */
    if (h & 0x00080000u) {                                            /* has arguments */
        if (args) { kp_fn(img, p); kp_fn(img, p + 8); }
        p += 12;
    }
    return p;
}

static uint64_t kp_body(uint32_t h)
{
    unsigned kind = (h >> 24) & 0x7f;
    uint32_t pl = h & 0x007fffffu;
    switch (kind) {
    case 1: case 3: return pl >= 0x007ffffdu ? 4 : 0;
    case 2: return 8 + (h & 0x00400000u ? 4 : 0) + (h & 0x00080000u ? 12 : 0);
    case 4: return 0;
    case 0: return 4ull * (1 + (h & 0x00ffffffu));
    default: return ~0ull;
    }
}

/* A shared-cache image's pattern can reach a slot outside the image, in the
 * cache's uniqued GOTs, that the stdlib follows: an external component's
 * property descriptor (a RelativeIndirectablePointer: KeyPath.swift
 * _resolveRelativeIndirectableAddress), a computed component's ID of
 * resolution UnresolvedIndirectPointer (2: a rel32 to a pointer holding
 * the ID), the generic environment's requirements. No metadata names a
 * pattern, so dsc.c's load-time pass cannot bind these: each is bound here,
 * before the native call reads it, in every mode (data, not a thunk). */
static void kp_slot(hao_image *img, uint64_t slot)
{
    walk w = {.img = img};
    if (!img->dsc || in_img(&w, slot, 8)) return;
    if (hao_dsc_bind_slot(slot)) st_kp_slots++;
    else {
        if (!st_kp_slots_left++ || hao_verbose) {
            hao_diag_prefix(HAO_DIAG);
            fprintf(HAO_DIAG, "%s: key path pattern slot %#llx outside the image not bound\n", img->path,
                    (unsigned long long)(slot - img->slide));
        }
    }
}

static void kp_slot_cb(void *c, int kind, uint64_t field, uint64_t slot)
{
    (void)kind, (void)field;
    kp_slot(c, slot);
}

/* a computed component's ID at p (header h) through a slot */
static void kp_id_slot(hao_image *img, uint64_t p, uint32_t h)
{
    walk w = {.img = img};
    uint32_t v;
    if (img->dsc && !(h & 0x00300000u) && (h & 0xfu) == 2 && rd32(&w, p, &v))
        kp_slot(img, p + (uint64_t)(int64_t)(int32_t)v);
}

static void keypath_pattern(uint64_t pat)
{
    hao_image *img = pat ? hao_image_for_addr(pat) : NULL;
    if (!img || (pat & 3)) return;
    bool thunks = !hao_opt.no_swift_thunks;
    if (!thunks && !img->dsc) return;                    /* nothing to bind, nothing to thunk */
    walk w = {.img = img};
    uint32_t bh;
    if (!rd32(&w, pat + 20, &bh)) return;
    uint64_t p = pat + 24, end = p + (bh & 0x00ffffffu);
    if (!in_img(&w, p, end - p)) { st_kp_bad++; return; }
    hao_crit_enter();
    pthread_mutex_lock(&dyn_lock);
    bool seen = false;
    for (uint32_t k = 0; k < n_kp_done && !seen; k++) seen = kp_done[k] == pat;
    if (!seen && n_kp_done < KP_DONE) kp_done[n_kp_done++] = pat;
    if (!seen && img->dsc) {                             /* {once cache, generic environment, ...} */
        walk ws = {.img = img, .slot = kp_slot_cb, .sctx = img};
        generic_environment(&ws, rel_direct(&w, pat + 4));
        free(ws.seen);
    }
    while (!seen && p + 4 <= end) {
        uint32_t h;
        rd32(&w, p, &h);
        p += 4;
        unsigned kind = (h >> 24) & 0x7f;
        if (kind == 2) {
            kp_id_slot(img, p, h);
            p = kp_computed(img, p, h, thunks, thunks);
        } else if (kind == 0) {                          /* external: {rel32 descriptor, generic arguments}, local candidate */
            uint32_t v;
            if (img->dsc && rd32(&w, p, &v) && (v & 1)) kp_slot(img, p + (uint64_t)(int64_t)(int32_t)(v & ~1u));
            uint64_t d = rel_indirectable(&w, p, -1);
            p += 4ull * (1 + (h & 0x00ffffffu));
            uint32_t dh = 0;
            if (d && !rd32_any(d, &dh)) { st_kp_bad++; break; }
            if (!d || !dh) continue;                     /* trivial or absent: the local candidate is the next component */
            uint32_t lh;
            if (!rd32(&w, p, &lh)) { st_kp_bad++; break; }
            p += 4;
            hao_image *dimg = hao_image_for_addr(d);
            if (thunks && dimg && ((dh >> 24) & 0x7f) == 2) {   /* a guest module's computed property descriptor: its accessors */
                kp_computed(dimg, d + 4, dh & ~0x00080000u, true, false);
            }
            if (((lh >> 24) & 0x7f) == 2) kp_id_slot(img, p, lh);
            if (((dh >> 24) & 0x7f) == 2 && ((lh >> 24) & 0x7f) == 2 && (lh & 0x00080000u))
                p = kp_computed(img, p, lh, false, thunks);  /* the local candidate's arguments */
            else {
                uint64_t b = kp_body(lh);
                if (b == ~0ull) { st_kp_bad++; break; }
                p += b;
            }
        } else {
            uint64_t b = kp_body(h);
            if (b == ~0ull) { st_kp_bad++; break; }
            p += b;
        }
        if (p > end) { st_kp_bad++; break; }
        if (p < end) p += 4;                             /* the intermediate component type */
    }
    if (!seen && thunks) st_kp++;
    pthread_mutex_unlock(&dyn_lock);
    hao_crit_leave();
}

/* ------------------------------------------- layout string templates
 *
 * A generic struct or enum whose layout does not depend on its generic
 * arguments has one layout string for all its instantiations: IRGen emits
 * it in __TEXT,__const and the type's metadata instantiation function hands
 * it to swift_cvw_instantiateLayoutString(layoutStr, metadata) (or
 * swift_generic_instantiateLayoutString, its older name), which only
 * stores it in the new metadata (BytecodeLayouts.cpp). No static metadata
 * names it, so value_metadata never sees it, and the value-witness
 * interpreter reads its enum-tag and resilient-accessor relative pointers
 * in place (WindowManager 0x10048bde8, a MultiPayloadEnumFN, from the
 * instantiation function at 0x1002f5ddc). Each one is walked on the call
 * that hands it over, once, and its functions get island entries made then
 * (dyn_entry); a string ls_ops cannot parse is counted and reported. */
#define LS_DONE 4096
static uint64_t ls_done[LS_DONE];
static uint32_t n_ls_done;
static uint64_t st_ls_tmpl, st_ls_tmpl_bad;

void hao_swiftmeta_layout_template(uint64_t ls)
{
    hao_image *img = ls ? hao_image_for_addr(ls) : NULL;
    if (!img || hao_opt.no_swift_thunks) return;
    hao_crit_enter();
    pthread_mutex_lock(&dyn_lock);
    bool seen = false;
    for (uint32_t k = 0; k < n_ls_done && !seen; k++) seen = ls_done[k] == ls;
    if (!seen) {
        if (n_ls_done < LS_DONE) ls_done[n_ls_done++] = ls;
        walk w = {.img = img};
        uint64_t lim = hao_image_extent(img, ls);
        if (!lim || (ls & 7) || !ls_ops(&w, ls + 16, lim, 0) || w.oom) {
            if (!st_ls_tmpl_bad++ || hao_verbose) {
                hao_diag_prefix(HAO_DIAG);
                fprintf(HAO_DIAG, "%s: layout string %#llx (swift_cvw_instantiateLayoutString) not parsed: its functions "
                        "keep the NX-fault path\n", strrchr(img->path, '/') ? strrchr(img->path, '/') + 1 : img->path,
                        (unsigned long long)(ls - img->slide));
            }
        } else {
            st_ls_tmpl++;
            for (size_t k = 0; k < w.n; k++) {
                uint64_t e = dyn_entry(img, w.e[k].target, w.e[k].field);
                if (!e) { st_unplaced++; continue; }
                struct ent one = {w.e[k].field, e, w.e[k].kind, 0, 0};
                st_fields += rewrite(img, &one, 1);
            }
        }
        free(w.e);
    }
    pthread_mutex_unlock(&dyn_lock);
    hao_crit_leave();
}

static void trap_instantiate_ls(hao_thread *t, uint64_t arg)
{
    hao_swiftmeta_layout_template(t->cpu.x[0]);
    hao_thunk_native_args(t);
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)nat_instantiate_ls[arg & 1], &t->cpu, 16);
}

static void *nat_get_keypath;

static void trap_get_keypath(hao_thread *t, uint64_t arg)
{
    (void)arg;
    keypath_pattern(t->cpu.x[0]);
    hao_thunk_native_args(t);
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)nat_get_keypath, &t->cpu, 16);
}

void hao_swiftmeta_stats(void)
{
    hao_swift_result_stats();
    hao_swift_objects_stats();
    uint64_t od, oa, oc;
    hao_objc_late_stats(&od, &oa, &oc);
    if (od || oc)
        fprintf(HAO_DIAG, "hao-objc: swift_realized_classes=%llu deferred_categories=%llu attached=%llu\n",
                (unsigned long long)oc, (unsigned long long)od, (unsigned long long)oa);
    if (!st_images && !st_nojit && !st_resume && !st_afp && !st_afp_arg && !st_kp && !st_kp_slots && !st_kp_slots_left &&
        !st_ls_tmpl && !st_ls_tmpl_bad && !st_isl_failed)
        return;
    char b[2048];
    int o = snprintf(b, sizeof b, "hao-swiftmeta: images=%llu fields=%llu entries=%llu island_kb=%llu walk_us=%llu", (unsigned long long)st_images,
                     (unsigned long long)st_fields, (unsigned long long)st_entries, (unsigned long long)(st_islands_bytes >> 10), (unsigned long long)st_us);
    uint64_t exact = 0, req = 0, notfn = 0;
    for (int k = 0; k < NKINDS; k++) {
        if (kinds[k].exact) exact += st_kind[k]; else req += st_kind[k];
        notfn += st_notfn[k];
        if (st_kind[k] && o < (int)sizeof b) o += snprintf(b + o, sizeof b - (size_t)o, " %s=%llu", kinds[k].name, (unsigned long long)st_kind[k]);
    }
    if (o < (int)sizeof b)
        snprintf(b + o, sizeof b - (size_t)o,
                 " | abi_typed=%llu requirement_untyped=%llu not_function=%llu unplaced=%llu layout_strings=%llu bad_layout=%llu"
                 " bad_desc=%llu conformances=%llu types=%llu protocols=%llu nojit=%llu resume_parent=%llu task_entry=%llu"
                 " task_entry_unmade=%llu kept_address_taken=%llu thunked_address_taken=%llu async_arg=%llu"
                 " keypath_patterns=%llu keypath_bad=%llu island_full=%llu",
                 (unsigned long long)exact, (unsigned long long)(req >= st_reverse ? req - st_reverse : 0),
                 (unsigned long long)notfn, (unsigned long long)st_unplaced,
                 (unsigned long long)st_ls, (unsigned long long)st_bad_ls, (unsigned long long)st_bad_desc,
                 (unsigned long long)st_conf, (unsigned long long)st_types, (unsigned long long)st_protos,
                 (unsigned long long)st_nojit, (unsigned long long)st_resume, (unsigned long long)st_afp,
                 (unsigned long long)st_afp_full, (unsigned long long)st_abs_kept, (unsigned long long)st_abs_taken,
                 (unsigned long long)st_afp_arg, (unsigned long long)st_kp,
                 (unsigned long long)st_kp_bad, (unsigned long long)st_dyn_full);
    if (st_kp_slots || st_kp_slots_left) {
        size_t l = strlen(b);
        snprintf(b + l, sizeof b - l, " keypath_slots=%llu keypath_slots_left=%llu", (unsigned long long)st_kp_slots,
                 (unsigned long long)st_kp_slots_left);
    }
    size_t l = strlen(b);
    snprintf(b + l, sizeof b - l, " reverse_typed=%llu islands_room=%llu islands_gap=%llu islands_search=%llu"
             " islands_failed=%llu layout_templates=%llu layout_templates_bad=%llu", (unsigned long long)st_reverse,
             (unsigned long long)st_isl_room, (unsigned long long)st_isl_gap, (unsigned long long)st_isl_search,
             (unsigned long long)st_isl_failed, (unsigned long long)st_ls_tmpl, (unsigned long long)st_ls_tmpl_bad);
    fprintf(HAO_DIAG, "%s\n", b);
}
