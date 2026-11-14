/* Bounded representation policy, operating on trusted initialized native
 * metadata supplied by the admitted callback or fixture. No arbitrary memory probing.
 * ABI: Metadata.h, ValueWitnessTable.h, ReflectionMirror.cpp, Reflection.h.
 * The selected XI field is the first with maximum XI count. An Array's
 * buffer word supplies only empty encodings here: its elements stay opaque. */
#include "Recipe.h"
#include "Common.h"
#include "Enum.h"
#include "hao/macho.h"
#include "hao/runtime.h"
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef HAO_SWIFT_VALUE_RUNTIME
#include "hao/runtime.h"
#include "hao/dlsym_once.h"
#endif

#define SWIFT __attribute__((swiftcall))
typedef struct {
    void *functions[8];
    uint64_t size, stride;
    uint32_t flags, extra;
} witnesses;
typedef struct {
    const char *name;
    void (*free_name)(const char *);
    bool strong, var;
} field_info;
typedef uintptr_t SWIFT kind_fn(const void *);
typedef intptr_t SWIFT count_fn(const void *);
typedef intptr_t SWIFT offset_fn(const void *, intptr_t);
typedef const void *SWIFT field_fn(const void *, intptr_t, field_info *);
typedef unsigned SWIFT enum_tag_fn(const void *, const void *);
typedef void SWIFT inject_tag_fn(void *, unsigned, const void *);
typedef struct {
    kind_fn *kind;
    count_fn *count;
    offset_fn *offset;
    field_fn *field;
    const void *array, *optional, *string;
    const void *unresolved;
    int existentials;   /* admit existential and Swift class payloads (guest calls of native witnesses only) */
    int (*known_layout)(const void *type);   /* if set: a struct (other than Array and String) whose layout
                                                it rejects ends the XI path (collect_inline_patches) */
    unsigned surveyed;  /* survey mode: refusing types printed so far (collect) */
    int survey;
    unsigned opaque;    /* collect: native opaque types (native_opaque) the walk is inside */
    /* Exact identities of the standard types selected() plans (nominals()): */
    const void *set, *dictionary;               /* nominal descriptors $sShMn, $sSDMn */
    const void *native_box, *unknown_box;       /* class reference witness tables $sBoWV, $sBOWV */
    const void *metatype_box;                   /* thick metatype witness table $sBomWV */
    const void *single[5];                      /* one-XI pointer nominals */
} reflection;
static void nominals(reflection *r)
{
    r->set = HAO_DLSYM("$sShMn");
    r->dictionary = HAO_DLSYM("$sSDMn");
    r->native_box = HAO_DLSYM("$sBoWV");
    r->unknown_box = HAO_DLSYM("$sBOWV");
    r->metatype_box = HAO_DLSYM("$sBomWV");
    r->single[0] = HAO_DLSYM("$sSVMn");                       /* UnsafeRawPointer */
    r->single[1] = HAO_DLSYM("$sSvMn");                       /* UnsafeMutableRawPointer */
    r->single[2] = HAO_DLSYM("$ss13OpaquePointerVMn");
    r->single[3] = HAO_DLSYM("$sSOMn");                       /* ObjectIdentifier */
    r->single[4] = HAO_DLSYM("$s10ObjectiveC8SelectorVMn");   /* NULL until ObjectiveC is loaded */
}
static int guest_laid_listed(const void *type);

static const witnesses *vwt(const void *type)
{
    return ((const witnesses *const *)type)[-1];
}

/* Materialize a fresh empty case using the receiving type's own witness.
 * GenEnum.cpp emitStoreTag and the runtime single-payload injection use tag1
 * for Optional.none. No payload exists for this case: storage is uninitialized
 * and no reference ownership is created. Unlike querying foreign source bits,
 * this never passes an invalid receiving-domain value to getEnumTag. */
static int receiver_empty_bits(const void *type, const empty_plan *plan, uint64_t *bits)
{
    if (plan->consumed == 1) { *bits = 0; return 1; }
    const witnesses *v = vwt(type);
    enum_tag_fn *tag = ((enum_tag_fn *const *)v)[11];
    inject_tag_fn *inject = ((inject_tag_fn *const *)v)[13];
    if (!tag || !inject || v->stride < v->size || v->stride > 8192) return 0;
    size_t alignment = (v->flags & 255) + 1;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    void *scratch = NULL;
    if (posix_memalign(&scratch,alignment,(size_t)v->stride)) return 0;
    memset(scratch,0xa5,(size_t)v->stride);
    inject(scratch,1,type);
    uint64_t word; memcpy(&word,(const char *)scratch + plan->offset,8);
    int ok = (word == 1 || word == 2) && tag(scratch,type) == 1;
    free(scratch); /* A nil value owns no payload to destroy. */
    if (ok) *bits = word;
    return ok;
}

/* This follows only the selected XI path. It does not certify compatibility
 * of any live payload and must not be used to copy one. */
static int selected(reflection *r, const void *type, empty_plan *out, unsigned depth)
{
    if (depth > 16 || guest_laid_listed(type)) return 0;   /* declared differently on arm64 */
    uintptr_t kind = r->kind(type);
    if (kind != 0x200 && kind != 0x202 && kind != 0x301 && kind != 0x305 && kind != 0x303 && kind != 0 &&
        kind != 0x304 && kind != 0x306) return 0;
    const void *const *m = type;
    const witnesses *v = vwt(type);
    if (v->size < 8 || v->size > 4096) return 0;
    /* Existentials, Swift class references and metatypes, on every path
     * (published values too, since the nil probes of 26B131 applications;
     * before, only guest calls of native value witnesses). Only their own
     * word is planned: a present existential's contents and an object's
     * fields are not walked. Metadata.h
     * TargetExistentialTypeMetadata: after the kind a flags word (witness
     * tables in bits 0-23, special protocol in 24-29, superclass bit 30,
     * bit 31 set when not class-bound). The opaque container is three buffer
     * words, the type metadata word, the tables; the class-bound one the
     * object word, then the tables. That metadata or object word carries the
     * extra inhabitants: 0 first, then 1 on arm64 and 2 on x86_64, measured
     * for `(any P)??`, `(any Q: AnyObject)??` and `AnyObject??` in native
     * programs of both. The Error box (special protocol 1) and any other
     * special representation stay out. */
    /* A Swift class reference (MetadataKind::Class, its value witness table
     * Builtin.NativeObject's, or Builtin.UnknownObject's for a class with ObjC
     * ancestry such as NSKeyValueObservation): the same inhabitants, measured
     * for `C??` (1 arm64, 2 x86_64) and the XIs of both kinds (k, 2k). */
    if (kind == 0) {
        if (!v || (v != r->native_box && v != r->unknown_box) ||
            v->size != 8 || v->stride != 8 || (v->flags & 255) != 7 || v->extra < 2) return 0;
        *out = (empty_plan){8, 0, 0};
        return 1;
    }
    /* A thick metatype, concrete (`C.Type`) or existential (`Any.Type`,
     * `AnyObject.Type`): one metadata pointer word, value witness table the
     * Builtin metatype's ($sBomWV); XI0 0, XI k k on arm64 and 2k on x86_64,
     * measured for all three. */
    if (kind == 0x304 || kind == 0x306) {
        if (!v || v != r->metatype_box || v->size != 8 || v->stride != 8 || v->extra != 0x7fffffff) return 0;
        *out = (empty_plan){8, 0, 0};
        return 1;
    }
    if (kind == 0x303) {
        uint32_t flags; memcpy(&flags, (const char *)type + 8, 4);
        uint64_t tables = flags & 0xffffff, offset = (flags & 0x80000000u) ? 24 : 0;
        if ((flags & 0x3f000000u) || (offset && (flags & 0x40000000u)) || tables > 64 ||
            v->size != 8 * ((offset ? 4 : 1) + tables) || v->stride != v->size ||
            (v->flags & 255) != 7 || v->extra < 2) return 0;
        *out = (empty_plan){v->size, offset, 0};
        return 1;
    }
    /* ObjCRetainableBox consumes null first, then ARM1/Intel2. Its exact
     * runtime witness identity distinguishes it from raw pointers, weak
     * references and class-bound existentials. Two consumed XIs remain the
     * bound: later empty encodings overlap and need explicit source domains. */
    if (kind == 0x305) {
        if (!common_objc_reference_type(type)) return 0;
        *out = (empty_plan){8, 0, 0};
        return 1;
    }
    /* Darwin64 String is frozen (_StringObject: count/flags then
     * Builtin.BridgeObject). Its XI0 is a zero second word; XI1 is 1 on
     * ARM and 2 on x86. Exact standard-library identity supplies this
     * contract, never a matching arbitrary pair of integers. */
    if (type == r->string && kind == 0x200) {
        if (v->size != 16 || v->stride != 16 || v->extra != 0x7fffffff ||
            (v->flags & 255) != 7 || (v->flags & 0x01900000)) return 0;
        *out = (empty_plan){16, 8, 0};
        return 1;
    }
    /* Set and Dictionary: one storage word, like Array's buffer word (size
     * 8, flags 0x10007, xi 0x7fffffff; XI0 0, XI k k on arm64 and 2k on
     * x86_64, measured for Set<String> and Dictionary<String, Int>). Only
     * the empty encodings are planned; a live one is not walked. */
    if (kind == 0x200 && m[1] && (m[1] == r->set || m[1] == r->dictionary)) {
        if (v->size != 8 || v->stride != 8 || v->extra != 0x7fffffff || (v->flags & 255) != 7) return 0;
        *out = (empty_plan){8, 0, 0};
        return 1;
    }
    /* Pointers with a single extra inhabitant, the zero word, identical on
     * both: UnsafeRawPointer, UnsafeMutableRawPointer, OpaquePointer,
     * ObjectIdentifier and Selector (size 8, xi 1, XI0 0, measured). Only
     * the first Optional: the next one has no inhabitant left and adds a tag
     * byte (the Optional branch below finds none). */
    if (kind == 0x200 && m[1]) {
        for (unsigned k = 0; k < sizeof r->single / sizeof *r->single; ++k)
            if (r->single[k] && m[1] == r->single[k]) {
                if (v->size != 8 || v->stride != 8 || v->extra != 1 || (v->flags & 255) != 7) return 0;
                *out = (empty_plan){8, 0, 0};
                return 1;
            }
    }
    if (kind == 0x200 && m[1] == r->array) {
        /* The buffer word's inhabitants do not depend on the Element:
         * measured in native arm64 and x86_64 programs for struct, enum,
         * Swift class, AnyObject, NSObject and Any Elements (size 8, flags
         * 0x10007, xi 0x7fffffff; XI0 0, XI k k on arm64 and 2k on x86_64).
         * This plans the empty encodings only. A live Array is not walked
         * (collect stops at its buffer word): nothing here certifies the
         * representation of its elements, which may be ObjC-bridged. */
        if (v->size != 8 || v->stride != 8 || v->extra != 0x7fffffff || (v->flags & 255) != 7) return 0;
        *out = (empty_plan){8, 0, 0};
        return 1;
    }
    if (kind == 0x202) {
        if (m[1] != r->optional) return 0;
        const witnesses *payload = vwt(m[2]);
        empty_plan child;
        if (!payload->extra || payload->extra - 1 != v->extra ||
            payload->size != v->size || !selected(r, m[2], &child, depth + 1) ||
            child.consumed >= 2) return 0;
        *out = child;
        out->consumed++;
        return 1;
    }
    if (r->known_layout && !r->known_layout(type)) return 0;
    intptr_t count = r->count(type);
    if (count < 1 || count > 64) return 0;
    const void *best = NULL;
    uint32_t extra = 0;
    uint64_t offset = 0;
    for (intptr_t i = 0; i < count; ++i) {
        field_info info = {0};
        const void *child = r->field(type, i, &info);
        intptr_t at = r->offset(type, i);
        if (info.free_name) info.free_name(info.name);
        if (!child || at < 0) return 0;
        const witnesses *cv = vwt(child);
        if ((uint64_t)at > v->size || cv->size > v->size - (uint64_t)at) return 0;
        if (cv->extra > extra) { best = child; extra = cv->extra; offset = (uint64_t)at; }
    }
    empty_plan child;
    if (!best || extra != v->extra || !selected(r, best, &child, depth + 1) ||
        child.offset > v->size - 8 || offset > v->size - 8 - child.offset) return 0;
    *out = (empty_plan){v->size, offset + child.offset, child.consumed};
    return 1;
}

static int discover(const void *type, empty_plan *out, int existentials)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, existentials,
    };
    nominals(&r);
    if (!type || !r.kind || !r.count || !r.offset || !r.field || !r.array || !r.optional ||
        r.kind(type) != 0x202) return 0;
    empty_plan result;
    if (!selected(&r, type, &result, 0) || !result.consumed) return 0;
    *out = result;
    return 1;
}

int discover_empty_plan(const void *type, empty_plan *out) { return discover(type, out, 0); }
int discover_witness_plan(const void *type, empty_plan *out) { return discover(type, out, 1); }

int initialize_empty(void *destination, const void *source, const void *type, int arm)
{
    empty_plan plan;
    if (!discover_empty_plan(type, &plan)) return 0;
    uintptr_t d = (uintptr_t)destination, s = (uintptr_t)source;
    if (!d || !s || (d >= s ? d - s : s - d) < plan.size) return 0;
    uint64_t word;
    memcpy(&word, (const char *)source + plan.offset, 8);
    /* Complete outer Optional only. With two consumed XIs, 0 is an inner
     * nil with a live outer payload; this adapter must decline it. */
    if (plan.consumed == 1 ? word != 0 : (word != 1 && word != 2)) return 0;
    memset(destination, 0, plan.size);
    word = (plan.consumed - 1) * (arm ? 1u : 2u);
    memcpy((char *)destination + plan.offset, &word, 8);
    return 1;
}

/* A function value other than a block, under any number of Optionals
 * (Metadata.h TargetFunctionTypeMetadata: convention in flags bits 16-23,
 * Swift 0, block 1, thin 2, C 3). Measured in native arm64 and x86_64
 * programs: a Swift closure is a code word and a context word (16 bytes), a
 * thin or C function one code word, xi 0x7fffffff, and XI k is the code word
 * k on both, context untouched; each Optional consumes the next. So no nil in
 * such a value differs and nothing in it is patched; the context and its
 * captures are a separate heap object. A block is an ObjC reference: its XI k
 * is k on arm64 and 2k on x86_64, and it stays with the reference rules. */
static int function_value(reflection *r, const void *type)
{
    uint64_t size = vwt(type)->size;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const void *const *m = type;
        const witnesses *v = vwt(type);
        uintptr_t kind = r->kind(type);
        if (v->size != size || (v->flags & 255) != 7) return 0;
        if (kind == 0x302) {
            uint64_t flags; memcpy(&flags,(const char *)type + 8,8);
            unsigned convention = (unsigned)(flags >> 16) & 0xff;
            if (convention == 1 || convention > 3) return 0;
            return v->extra == 0x7fffffff && v->stride == size &&
                   size == (convention ? 8u : 16u);
        }
        if (kind != 0x202 || m[1] != r->optional) return 0;
        type = m[2];
    }
    return 0;
}

/* The offset of the code word that supplies TYPE's extra inhabitants, when
 * they come from a function value: Optionals take their payload's, a struct
 * or tuple its first field with the most (each step checked against the
 * value witness tables), down to a function value that is not a block
 * (function_value). Those inhabitants are code words 0..0x7ffffffe on both
 * architectures, so an Optional over such a type has the same nil on both. */
static int function_xi_word(reflection *r, const void *type, uint64_t *offset)
{
    uint64_t at = 0;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const void *const *m = type;
        const witnesses *v = vwt(type);
        uintptr_t kind = r->kind(type);
        if (!v->extra) return 0;
        if (kind == 0x302) {
            if (!function_value(r,type)) return 0;
            *offset = at;
            return 1;
        }
        if (kind == 0x202 && m[1] == r->optional) {
            if (vwt(m[2])->extra != v->extra + 1) return 0;
            type = m[2];
            continue;
        }
        if (kind != 0x200 && kind != 0x301) return 0;
        intptr_t count = r->count(type);
        if (count < 1 || count > 64) return 0;
        const void *best = NULL;
        intptr_t best_at = -1;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r->field(type,i,&info);
            intptr_t o = r->offset(type,i);
            if (info.free_name) info.free_name(info.name);
            if (!child || o < 0 || (uint64_t)o + vwt(child)->size > v->size) return 0;
            if (!best || vwt(child)->extra > vwt(best)->extra) { best = child; best_at = o; }
        }
        if (vwt(best)->extra != v->extra) return 0;
        at += (uint64_t)best_at;
        type = best;
    }
    return 0;
}

/* The enum (kind 0x201) whose extra inhabitants TYPE's are: TYPE itself, or
 * through Optionals and the first field with the most inhabitants of a
 * struct or tuple (with KNOWN, a struct only where r->known_layout admits
 * it), each step checked against the value witness tables. *OFFSET is the
 * enum's offset in TYPE. */
static const void *xi_carrier(reflection *r, const void *type, int known, uint64_t *offset)
{
    uint64_t at = 0;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const void *const *m = type;
        const witnesses *v = vwt(type);
        uintptr_t kind = r->kind(type);
        if (!v->extra) return NULL;
        if (kind == 0x201) { *offset = at; return type; }
        if (kind == 0x202 && m[1] == r->optional) {
            if (vwt(m[2])->extra != v->extra + 1) return NULL;
            type = m[2];
            continue;
        }
        if (kind != 0x200 && kind != 0x301) return NULL;
        if (known && kind == 0x200 && r->known_layout && !r->known_layout(type)) return NULL;
        intptr_t count = r->count(type);
        if (count < 1 || count > 64) return NULL;
        const void *best = NULL;
        intptr_t best_at = -1;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r->field(type,i,&info);
            intptr_t o = r->offset(type,i);
            if (info.free_name) info.free_name(info.name);
            if (!child || o < 0 || (uint64_t)o + vwt(child)->size > v->size) return NULL;
            if (!best || vwt(child)->extra > vwt(best)->extra) { best = child; best_at = o; }
        }
        if (vwt(best)->extra != v->extra) return NULL;
        at += (uint64_t)best_at;
        type = best;
    }
    return NULL;
}

/* The offset of the tag byte that supplies TYPE's extra inhabitants, when
 * they come from a multi-payload enum with one extra tag byte and no spare
 * bit (Enum.c tag_byte_layout), found by xi_carrier. The enum's inhabitants
 * are the byte's values 255, 254, ... on both architectures (measured for
 * IndexPath, whose storage is such an enum, and String / (String, String)
 * enums), so an Optional over such a type has the same nil on both. *XI is
 * the enum's inhabitant count. */
static int tag_byte_xi(reflection *r, const void *type, uint64_t *offset, uint32_t *xi)
{
    uint64_t at, tag_at;
    unsigned tags;
    const void *e = xi_carrier(r, type, 0, &at);
    if (!e || vwt(e)->extra > 255 || !tag_byte_layout(e, &tag_at, &tags)) return 0;
    *offset = at + tag_at;
    *xi = vwt(e)->extra;
    return 1;
}

/* The native spare-bit enum whose extra inhabitants TYPE's are, when the
 * mpenum table (row format 2) says they differ between arm64 and x86_64 and
 * gives both layouts' masks, found by xi_carrier (a struct only where
 * known_layout admits it). The row must apply (live XI count = its x86_64
 * one; a payload that is not loadable gives a tag byte instead) and use no
 * extra tag bits on either side. *OFFSET is the enum's offset in TYPE. */
typedef struct { const void *type; uint64_t offset; mpenum_row row; } xi_enum;
static int translated_xi_enum(reflection *r, const void *type, xi_enum *out)
{
    uint64_t at;
    mpenum_row row;
    const void *e = xi_carrier(r, type, 1, &at);
    if (!e) return 0;
    const witnesses *v = vwt(e);
    if (!mpenum_type(e, &row) || (row.flags & MPENUM_XI) || row.xi != v->extra ||
        row.x86_extra_bits || row.arm_extra_bits || !row.mask_bytes || !row.x86_mask || !row.arm_mask ||
        v->size < 8 || v->size > 8 * HAO_SV_SPARE_WORDS ||
        (uint64_t)row.mask_offset + row.mask_bytes > v->size) return 0;
    *out = (xi_enum){e, at, row};
    return 1;
}

/* XI K of E on both architectures (Enum.c spare_bit_xi over each row mask). */
static int xi_patterns(const xi_enum *e, uint32_t k, unsigned char *x86, unsigned char *arm, int *have_arm)
{
    uint64_t area = vwt(e->type)->size;
    unsigned char xm[8 * HAO_SV_SPARE_WORDS] = {0}, am[8 * HAO_SV_SPARE_WORDS] = {0};
    memcpy(xm + e->row.mask_offset, e->row.x86_mask, e->row.mask_bytes);
    memcpy(am + e->row.mask_offset, e->row.arm_mask, e->row.mask_bytes);
    if (!spare_bit_xi(xm, area, e->row.payload, e->row.empty, k, x86)) return 0;
    *have_arm = k < e->row.arm_xi && spare_bit_xi(am, area, e->row.payload, e->row.empty, k, arm);
    return 1;
}

int translated_nil_type(const void *type)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, 1,
    };
    xi_enum e;
    if (!type || !r.kind || !r.count || !r.offset || !r.field || !r.optional || r.kind(type) != 0x202 ||
        ((const void *const *)type)[1] != r.optional) return 0;
    return translated_xi_enum(&r, ((const void *const *)type)[2], &e);
}

/* A type declared differently on arm64 (Tables.h archdiff_type,
 * src/archdiff_gen.inc) whose bytes guest code lays out: one its module
 * froze (frozenstruct_gen.inc), whose layout is compiled into guest code.
 * A resilient listed type is held by guest code only through its native
 * witnesses, so its bytes are x86_64 throughout and its native layout is the
 * right one to walk; its row is ignored. (No listed type is defined in a
 * guest image: the rows are x86_64 images'.) */
static int guest_laid_listed(const void *type)
{
    return archdiff_type(type) && native_frozen_type(type);
}

/* A native struct or enum its module did not freeze: guest code holds it
 * only through native witnesses, so nothing inline in it is guest-laid-out. */
static int native_opaque(reflection *r, const void *type)
{
    uintptr_t kind = r->kind(type);
    const void *const *m = type;
    if ((kind != 0x200 && kind != 0x201) || !m[1] || type == r->string || m[1] == r->array ||
        hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return 0;
    return !native_frozen_type(type);
}

/* Whether TYPE is, or holds inline where guest code lays it out, a listed
 * frozen type: through struct fields, tuple elements, Optional payloads and
 * an enum's inline payload cases, stopping at a native opaque type. Such a
 * type's size and so its enclosing layouts can differ too, also where the
 * walk does not descend (an inactive Optional payload, an enum's inactive
 * cases). The search is bounded at 16 levels and 64 fields a level; what
 * lies beyond is not searched. */
static int holds_archdiff(reflection *r, const void *type, unsigned depth)
{
    if (guest_laid_listed(type)) return 1;
    uintptr_t kind = r->kind(type);
    const void *const *m = type;
    if (depth > 16 || (kind != 0x200 && kind != 0x201 && kind != 0x202 && kind != 0x301) ||
        type == r->string || (kind == 0x200 && m[1] == r->array) || native_opaque(r, type)) return 0;
    int memo = holds_archdiff_memo(type);
    if (memo >= 0) return memo;
    int listed = 0;
    if (kind == 0x202) listed = holds_archdiff(r, m[2], depth + 1);
    else if (kind == 0x201) {
        const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
        int n = enum_inline_payloads(type, payloads);
        for (int i = 0; i < n && !listed; ++i) listed = holds_archdiff(r, payloads[i], depth + 1);
    } else {
        intptr_t count = r->count(type);
        for (intptr_t i = 0; count <= 64 && i < count && !listed; ++i) {
            field_info info = {0};
            const void *child = r->field(type, i, &info);
            if (info.free_name) info.free_name(info.name);
            if (child) listed = holds_archdiff(r, child, depth + 1);
        }
    }
    holds_archdiff_remember(type, listed);
    return listed;
}

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth);
static int collect_inner(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    if (depth > 32) return 0;
    uintptr_t kind = r->kind(type);
    const witnesses *v = vwt(type);
    if (v->size > 4096) return 0;
    /* A type declared differently on arm64 that guest code lays out, or one
     * that holds such a type inline, has no representation both sides read
     * alike: refused under every rule below, the one for small values
     * included. Inside a native opaque type everything is native-laid-out. */
    if (!r->opaque && holds_archdiff(r, type, 0)) return 0;
    /* A patch rewrites a whole pointer word. A value of fewer than 8 bytes
     * holds no pointer, so no nil in it has a pointer extra inhabitant and
     * the case it is in cannot hide one: there is nothing to patch. */
    if (v->size < 8) return 1;
    if ((kind == 0x302 || kind == 0x202) && function_value(r,type)) return 1;
    if (type == r->string) {
        empty_plan shape;
        if (!selected(r,type,&shape,0)) return 0;
        uint64_t bits; memcpy(&bits,value + shape.offset,8);
        /* An active String cannot be any admitted nil discriminator. Live
         * storage uses the existing native object/ARC domain; this is not a
         * validator for arbitrary string bytes or foreign-runtime objects. */
        return bits > 2;
    }
    if (kind == 0x202) {
        const void *const *m = type;
        const witnesses *payload = vwt(m[2]);
        int byte_case = common_byte_optional_case(type,value);
        if (byte_case >= 0) {
            if (byte_case) return 1; /* inactive payload remains opaque */
            return collect(r,m[2],value,base,out,capacity,used,depth + 1);
        }
        /* A payload whose inhabitants are a function's code word: this
         * Optional's nil is that word equal to the inhabitants the payload
         * already consumed, the same on both architectures. */
        uint64_t code_at;
        if (m[1] == r->optional && payload->extra && v->extra + 1 == payload->extra &&
            function_xi_word(r,m[2],&code_at)) {
            uint64_t word, none = 0x7fffffffu - payload->extra;
            memcpy(&word,value + code_at,8);
            if (word == none) return 1;                      /* inactive payload remains opaque */
            if (word > none && word < 0x7fffffffu) return 0; /* an outer Optional's nil: not this type's value */
            return collect(r,m[2],value,base,out,capacity,used,depth + 1);
        }
        /* A payload whose inhabitants are an enum's tag byte: this
         * Optional's nil is the byte 255 - the inhabitants inner Optionals
         * consumed, the same on both architectures. */
        uint64_t tag_at;
        uint32_t enum_xi;
        if (m[1] == r->optional && payload->extra && v->extra + 1 == payload->extra &&
            tag_byte_xi(r,m[2],&tag_at,&enum_xi) && enum_xi >= payload->extra) {
            unsigned tag = value[tag_at], none = 255 - (enum_xi - payload->extra);
            if (tag == none) return 1;                       /* inactive payload remains opaque */
            if (tag >= 256 - enum_xi && tag < none) return 0; /* an outer Optional's nil */
            return collect(r,m[2],value,base,out,capacity,used,depth + 1);
        }
        /* A payload whose inhabitants are a native spare-bit enum's that
         * differ by architecture (Foundation's Data: its representation's
         * spare bits are one byte's top nibble on arm64, the whole byte on
         * x86_64): this Optional's nil is the enum's XI k, k the inhabitants
         * inner Optionals consumed, in either architecture's encoding. Each
         * word in which the two differ is a patch: the encoding found, the
         * receiving witness set's (materialized as receiver_empty_bits does)
         * and arm64's. An index past arm64's XI count has no arm64 encoding,
         * and an outer Optional's nil is no value of this one: both refuse. */
        xi_enum xe;
        if (m[1] == r->optional && payload->extra && v->extra + 1 == payload->extra &&
            translated_xi_enum(r,m[2],&xe) && vwt(xe.type)->extra >= payload->extra &&
            xe.offset + vwt(xe.type)->size <= payload->size) {
            uint64_t size = vwt(xe.type)->size;
            uint32_t k = vwt(xe.type)->extra - payload->extra;
            unsigned char x86[8 * HAO_SV_SPARE_WORDS], arm[8 * HAO_SV_SPARE_WORDS];
            int have_arm;
            const unsigned char *found = value + xe.offset;
            if (!xi_patterns(&xe,k,x86,arm,&have_arm)) return 0;
            int is_x86 = !memcmp(found,x86,(size_t)size), is_arm = have_arm && !memcmp(found,arm,(size_t)size);
            if (!is_x86 && !is_arm) {
                for (uint32_t j = k + 1; j < vwt(xe.type)->extra && j < k + 9; ++j) {   /* an outer nil */
                    unsigned char ox[8 * HAO_SV_SPARE_WORDS], oa[8 * HAO_SV_SPARE_WORDS];
                    int oh;
                    if (xi_patterns(&xe,j,ox,oa,&oh) &&
                        (!memcmp(found,ox,(size_t)size) || (oh && !memcmp(found,oa,(size_t)size)))) return 0;
                }
                return collect(r,m[2],value,base,out,capacity,used,depth + 1);
            }
            if (!have_arm || xe.offset % 8) return 0;
            /* The receiving witness set's own nil. */
            const witnesses *ov = vwt(type);
            inject_tag_fn *inject = ((inject_tag_fn *const *)ov)[13];
            unsigned char *scratch = NULL;
            size_t alignment = (ov->flags & 255) + 1;
            if (alignment < sizeof(void *)) alignment = sizeof(void *);
            if (!inject || ov->stride < ov->size || ov->stride > 8192 ||
                posix_memalign((void **)&scratch,alignment,(size_t)ov->stride)) return 0;
            memset(scratch,0xa5,(size_t)ov->stride);
            inject(scratch,1,type);
            const unsigned char *receiver = scratch + xe.offset;
            int ok = !memcmp(receiver,x86,(size_t)size) || !memcmp(receiver,arm,(size_t)size);
            for (uint64_t w = 0; ok && w < size; w += 8) {
                if (!memcmp(x86 + w,arm + w,8)) continue;
                if (w + 8 > size || *used == capacity) { ok = 0; break; }
                uint64_t f, rb, ab;
                memcpy(&f,found + w,8); memcpy(&rb,receiver + w,8); memcpy(&ab,arm + w,8);
                out[(*used)++] = (empty_patch){base + xe.offset + w, f, rb, k + 1, ab};
            }
            free(scratch);   /* a nil owns no payload */
            return ok;
        }
        /* A payload whose inhabitants are a guest enum's whose tag lies in
         * its payloads' spare bits (Enum.c guest_spare_bit_case's layout,
         * decoded from the image's record). Its witnesses are arm64 code and
         * read the arm64 encoding guest code wrote, so this Optional's nil,
         * the enum's XI k (k the inhabitants inner Optionals consumed), needs
         * no patch: it is inactive. A deeper XI is an outer Optional's nil,
         * no value of this one. Anything else is a present payload, walked as
         * the enum's case. Without this rule the Optional is refused, and the
         * whole value with it: a sibling's nil then reaches native witnesses
         * in the arm64 encoding (swift_retain(0x1)). */
        uint64_t guest_at;
        const void *guest_enum;
        if (m[1] == r->optional && payload->extra && v->extra + 1 == payload->extra &&
            (guest_enum = xi_carrier(r,m[2],0,&guest_at)) && vwt(guest_enum)->extra >= payload->extra &&
            guest_at + vwt(guest_enum)->size <= payload->size) {
            uint64_t size = vwt(guest_enum)->size;
            uint32_t k = vwt(guest_enum)->extra - payload->extra;
            unsigned char nil[8 * HAO_SV_SPARE_WORDS];
            if (guest_spare_bit_xi(guest_enum,k,nil)) {
                const unsigned char *found = value + guest_at;
                if (!memcmp(found,nil,(size_t)size)) return 1;      /* inactive payload remains opaque */
                for (uint32_t j = k + 1; j < vwt(guest_enum)->extra && j < k + 9; ++j)
                    if (guest_spare_bit_xi(guest_enum,j,nil) && !memcmp(found,nil,(size_t)size)) return 0;
                return collect(r,m[2],value,base,out,capacity,used,depth + 1);
            }
        }
        /* No payload extra inhabitants: Swift's single-payload enum layout
         * adds a trailing 0/1 tag. Admit POD payloads only, and still walk a
         * present payload so unknown active enum fields cannot hide behind
         * the outer tag. Inactive payload bytes are never inspected. */
        if (m[1] == r->optional && !payload->extra && !v->extra &&
            !(payload->flags & 0x01910000) && !(v->flags & 0x01910000) &&
            payload->size && payload->size < 4096 && v->size == payload->size + 1 &&
            (v->flags & 255) == (payload->flags & 255) &&
            v->stride == ((v->size + (v->flags & 255)) & ~(uint64_t)(v->flags & 255))) {
            unsigned tag = value[payload->size];
            if (tag == 1) return 1;
            if (tag != 0) return 0;
            return collect(r,m[2],value,base,out,capacity,used,depth + 1);
        }
        empty_plan plan;
        /* Unknown Optional means unknown active payload. Reject the entire
         * plan, even if another sibling already supplied a valid patch. */
        if (!selected(r, type, &plan, 0) || !plan.consumed) return common_leaf_value(type,value);
        uint64_t word; memcpy(&word, value + plan.offset, 8);
        if (plan.consumed == 1 ? word == 0 : (word == 1 || word == 2)) {
            if (*used == capacity) return 0;
            /* Native generic containers may delegate to ARM field witnesses.
             * Their receiving encoding comes from the actual witness set,
             * independently of both the process architecture and source bits. */
            uint64_t receiver;
            if (!receiver_empty_bits(type,&plan,&receiver)) return 0;
            out[(*used)++] = (empty_patch){base + plan.offset, word, receiver, plan.consumed, plan.consumed - 1u};
            return 1;                     /* inactive payload must remain opaque */
        }
        /* This is a present payload in either admitted representation. */
        return collect(r, ((const void *const *)type)[2], value, base,
                       out, capacity, used, depth + 1);
    }
    if (kind == 0x201) {
        if (common_leaf_value(type,value)) return 1;
        /* A guest enum with one payload case whose empty cases are the
         * payload's first extra inhabitants (Enum.c guest_single_payload_enum),
         * when selected() finds the word that carries them: there XI k is the
         * word k on arm64 (k from the inhabitants the payload's own Optionals
         * consumed). The enum's witnesses are arm64 code and read the
         * encoding guest code wrote, so an empty case needs no patch and has
         * no payload; the next inhabitants are an outer Optional's nils, no
         * value of this type; anything else is the payload, walked. Without
         * this rule the enum is refused, and the whole value with it. */
        const void *single;
        unsigned empty_cases;
        empty_plan inner;
        if (guest_single_payload_enum(type,&single,&empty_cases) && selected(r,single,&inner,0) &&
            inner.offset <= v->size - 8) {
            uint64_t word, first = inner.consumed;
            memcpy(&word,value + inner.offset,8);
            if (word >= first && word < first + empty_cases) return 1;
            if (word >= first + empty_cases && word < first + empty_cases + 8) return 0;
            return collect(r,single,value,base,out,capacity,used,depth + 1);
        }
        enum_case_plan active;
        if (discover_enum_case(type,value,&active))
            return !active.payload || collect(r,active.payload,value,base,out,capacity,used,depth + 1);
        fixed_enum_case fixed;
        if (!discover_fixed_enum_case(type,value,&fixed)) return 0;
        if (!fixed.payload) return 1;     /* the empty case: no payload */
        if (!fixed.spare) return collect(r,fixed.payload,value,base,out,capacity,used,depth + 1);
        unsigned first = *used;
        if (!collect(r,fixed.payload,(const unsigned char *)fixed.copy,base,out,capacity,used,depth + 1)) return 0;
        /* A patch is written into the live value: one over a tag bit would
         * change the case. */
        for (unsigned i = first; i < *used; ++i) {
            uint64_t at = out[i].offset - base;
            for (uint64_t b = at; b < at + 8; ++b)
                if ((fixed.mask[b / 8] >> (8 * (b % 8))) & 0xff) return 0;
        }
        return 1;
    }
    if (kind == 0x305) return common_objc_reference_value(type,value);
    /* A live existential or Swift object in a published value (the plain
     * walk; selected() plans their nils there too): native code takes it, so
     * each witness table must be native, its conformance descriptor in no
     * guest image (a guest conformance's table is arm64 code; a generic one's
     * table is instantiated on the heap), and a Swift object word a heap
     * pointer. A guest call of a native witness only copies them. */
    if ((kind == 0x303 || kind == 0) && !r->existentials) {
        if (kind == 0) {
            uint64_t bits; memcpy(&bits,value,8);
            return bits >= 4096 && !(bits & 7);
        }
        uint32_t flags, protocols; memcpy(&flags,(const char *)type + 8,4); memcpy(&protocols,(const char *)type + 12,4);
        uint64_t tables = flags & 0xffffff, first = (flags & 0x80000000u) ? 32 : 8;
        if ((flags & 0x3f000000u) || first + 8 * tables > v->size) return 0;
        /* Every protocol a Swift protocol defined in a guest image
         * (TargetExistentialTypeMetadata: kind, flags, protocol count, then
         * the superclass constraint if flag bit 30, then the protocol
         * references, bit 0 set for an ObjC protocol): native code cannot
         * name such a protocol, so it never calls through these tables; it
         * only copies the container. Then a guest conformance's table is
         * admitted. A native protocol's table must stay native. */
        int guest_protocols = protocols && protocols <= 64 && !(flags & 0x40000000u);
        for (uint32_t i = 0; guest_protocols && i < protocols; ++i) {
            uint64_t ref; memcpy(&ref,(const char *)type + 16 + 8 * i,8);
            if ((ref & 1) || !hao_image_for_addr(ref)) guest_protocols = 0;
        }
        for (uint64_t i = 0; i < tables; ++i) {
            uint64_t table, conformance = 0;
            memcpy(&table,value + first + 8 * i,8);
            if (!table || !hao_probe_read8(table,&conformance) || !conformance) return 0;
            if (!guest_protocols && (hao_image_for_addr(table) || hao_image_for_addr(conformance))) return 0;
        }
        return 1;
    }
    if (kind != 0x200 && kind != 0x301) return 1;
    intptr_t count = r->count(type);
    if (count < 0 || count > 64) return 0;
    for (intptr_t i = 0; i < count; ++i) {
        field_info info = {0};
        const void *child = r->field(type, i, &info);
        intptr_t at = r->offset(type, i);
        if (info.free_name) info.free_name(info.name);
        if (!child || at < 0 || (uint64_t)at > v->size || vwt(child)->size > v->size - (uint64_t)at) return 0;
        /* Reflection exposes a weak field's referent type. Nonzero weak ObjC
         * storage is not the strong object pointer certified by the first-
         * Optional rule; only its common zero representation is admitted. */
        if (!info.strong && r->kind(child) == 0x202 &&
            ((const void *const *)child)[1] == r->optional &&
            r->kind(((const void *const *)child)[2]) == 0x305) {
            uint64_t bits;
            if (vwt(child)->size != 8) return 0;
            memcpy(&bits,value + at,8);
            if (bits) return 0;
        }
        if (!collect(r, child, value + at, base + (uint64_t)at,
                     out, capacity, used, depth + 1)) return 0;
    }
    return 1;
}

/* Survey mode (collect_survey_patches): print a type the walk refuses at
 * value offset BASE and go on with its siblings, so that one refused value
 * names every refusing type in it. The value is still refused. Only the
 * innermost refusing types are printed: a type that refuses for its own
 * reason after a descendant was printed is not. */
static void survey(reflection *r, const void *type, uint64_t base, unsigned depth)
{
#ifdef HAO_SWIFT_VALUE_RUNTIME
    typedef struct { const char *text; uintptr_t length; } type_name;
    typedef type_name SWIFT name_fn(const void *, bool);
    name_fn *name = HAO_DLSYM("swift_getTypeName");
    type_name n = name ? name(type,true) : (type_name){"?",1};
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG,"Swift value survey: refused at offset %llu depth %u: metadata=%p kind=%#lx size=%llu flags=%#x xi=%#x type=%.*s\n",
        (unsigned long long)base,depth,type,(unsigned long)r->kind(type),(unsigned long long)vwt(type)->size,
        vwt(type)->flags,vwt(type)->extra,(int)(n.length > 4096 ? 4096 : n.length),n.text);
#else
    (void)type; (void)base; (void)depth;
#endif
    r->surveyed++;
}

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    int opaque = !r->opaque && native_opaque(r,type);
    r->opaque += (unsigned)opaque;
    unsigned before = r->surveyed;
    int ok = collect_inner(r,type,value,base,out,capacity,used,depth);
    r->opaque -= (unsigned)opaque;
    if (r->survey && (!ok || r->surveyed != before)) {
        if (!ok && r->surveyed == before) {
            if (!r->unresolved) r->unresolved = type;
            survey(r,type,base,depth);
        }
        return 1;           /* keep walking; collect_patches refuses the value */
    }
    if (!ok && !r->unresolved) r->unresolved = type;
    return ok;
}

static int collect_patches(const void *type, const void *value, empty_patch *out,
                           unsigned capacity, const void **unresolved, int existentials,
                           int (*known_layout)(const void *), int survey_mode)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, existentials,
        known_layout, 0, survey_mode,
    };
    nominals(&r);
    unsigned used = 0;
    if (unresolved) *unresolved = type;
    if (!type || !value || !out || !r.kind || !r.count || !r.offset || !r.field || !r.array || !r.optional) return -1;
    if (!collect(&r, type, value, 0, out, capacity, &used, 0) || r.surveyed) {
        if (unresolved && r.unresolved) *unresolved = r.unresolved;
        return -1;
    }
    if (unresolved) *unresolved = NULL;
    return (int)used;
}

int collect_empty_patches_detail(const void *type, const void *value, empty_patch *out,
                                 unsigned capacity, const void **unresolved)
{
    return collect_patches(type,value,out,capacity,unresolved,0,NULL,0);
}

int collect_survey_patches(const void *type, const void *value, empty_patch *out,
                           unsigned capacity, const void **unresolved)
{
    return collect_patches(type,value,out,capacity,unresolved,0,NULL,1);
}

int collect_witness_patches(const void *type, const void *value, empty_patch *out, unsigned capacity)
{
    return collect_patches(type,value,out,capacity,NULL,1,NULL,0);
}

int collect_inline_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                           int (*known_layout)(const void *))
{
    return collect_patches(type,value,out,capacity,NULL,1,known_layout,0);
}

int collect_empty_patches(const void *type, const void *value, empty_patch *out, unsigned capacity)
{
    return collect_empty_patches_detail(type,value,out,capacity,NULL);
}
