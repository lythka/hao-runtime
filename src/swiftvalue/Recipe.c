/* Bounded representation policy, operating on trusted initialized native
 * metadata supplied by the admitted callback or fixture. No arbitrary memory probing.
 * ABI: Metadata.h, ValueWitnessTable.h, ReflectionMirror.cpp, Reflection.h.
 * The selected XI field is the first with maximum XI count. An Array's
 * buffer word supplies only empty encodings here: its elements stay opaque. */
#include "Recipe.h"
#include "Common.h"
#include "Enum.h"
#include "Facts.h"
#include "hao/macho.h"
#include "hao/runtime.h"
#include <dlfcn.h>
#include <pthread.h>
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
    unsigned surveyed;  /* survey mode: refusing types printed (or, quiet, counted) so far (collect) */
    int survey;         /* 1 survey mode, 2 the same without printing (collect_partial_patches), 3 the same
                           listing the refusing types (collect_audit_patches) */
    unsigned passes;    /* HAO_SV_PASS_* classes by which the walk left parts of the value as found */
    unsigned boxed;     /* collect: existential payloads the walk is inside */
    unsigned opaque;    /* collect: native opaque types (native_opaque) the walk is inside */
    const void *chain[HAO_SV_AUDIT_CHAIN];   /* collect: the types from the root down to the current one */
    audit_entry *audit; /* survey mode 3: the innermost refusing types, at most naudit_max (collect_audit_patches) */
    unsigned naudit, naudit_max;
    /* Exact identities of the standard types selected() plans (nominals()): */
    const void *set, *dictionary;               /* nominal descriptors $sShMn, $sSDMn */
    const void *native_box, *unknown_box;       /* class reference witness tables $sBoWV, $sBOWV */
    const void *metatype_box;                   /* thick metatype witness table $sBomWV */
    const void *single[5];                      /* one-XI pointer nominals */
    int source;         /* HAO_SV_SOURCE_GUEST / _NATIVE: who wrote the value's bytes; 0 unknown or mixed */
    unsigned looking;   /* collect_inner: the partial walk is inside a type refused for fields not known */
    unsigned unknown;   /* collect_inner: types it refused there for their own fields not known */
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
static int guest_witnessed(reflection *r, const void *type);
static int holds_archdiff(reflection *r, const void *type, unsigned depth);
/* A refusal the walk knows to be right, not a missing rule (Recipe.h
 * HAO_SV_KNOWN_WRONG): a value native code would misread or that is no value
 * of its type, a REFUSE row, a nil past the patch capacity. */
#define WRONG(r) ((r)->passes |= HAO_SV_KNOWN_WRONG, 0)
static int scalar_only(reflection *r, const void *type, unsigned depth);
static int guest_tag_witnesses(reflection *r, const void *g);

static const witnesses *vwt(const void *type)
{
    return ((const witnesses *const *)type)[-1];
}

/* The number of a struct's or tuple's fields reflection reports, or -1:
 * more than HAO_SV_FIELD_LIMIT, or none for a value that has bytes (a struct compiled
 * without reflection metadata reports none: its fields are not known, which
 * is not the same as having none). */
static intptr_t known_fields(reflection *r, const void *type)
{
    intptr_t count = r->count(type);
    if (count < 0 || count > HAO_SV_FIELD_LIMIT || (!count && vwt(type)->size)) return -1;
    return count;
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

/* An existential metatype with witness tables (`any P.Type`, `(any P &
 * Q).Type`): Metadata.h TargetExistentialMetatypeMetadata is the kind, the
 * instance type, then ExistentialTypeFlags (witness tables in bits 0-23,
 * special protocol in 24-29). Its value is the dynamic type's metadata
 * pointer word and then one witness table pointer per table
 * (ExistentialMetatypeValueWitnesses<N>), POD, 8 * (1 + N) bytes. Its extra
 * inhabitants are the metadata word's, those of a thick metatype ($sBomWV,
 * planned below): XI0 0, XI k k on arm64 and 2k on x86_64, the table words
 * untouched; measured for `(any P.Type)??`, `(any P.Type)???` and a struct
 * whose most-inhabited field is `any P.Type`, in native programs of both
 * (tests/host/swiftlayout.swift section M). The words are pointers to
 * metadata and witness tables, immutable data both runtimes read alike. The
 * table count, or 0 for any other layout (the tableless one is $sBomWV's). */
static unsigned existential_metatype_tables(const void *type)
{
    const witnesses *v = vwt(type);
    uint32_t flags; memcpy(&flags,(const char *)type + 16,4);
    unsigned tables = flags & 0xffffff;
    if (!tables || tables > 64 || (flags & 0x3f000000u) || v->size != 8 * (1 + (uint64_t)tables) ||
        v->stride != v->size || (v->flags & 255) != 7 || (v->flags & 0x00310000) || v->extra != 0x7fffffff) return 0;
    return tables;
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
        if (kind == 0x306 && existential_metatype_tables(type)) {
            *out = (empty_plan){v->size, 0, 0};
            return 1;
        }
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
    if (count < 1 || count > HAO_SV_FIELD_LIMIT) return 0;
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
        if (count < 1 || count > HAO_SV_FIELD_LIMIT) return 0;
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
        if (count < 1 || count > HAO_SV_FIELD_LIMIT) return NULL;
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

/* The extra-inhabitant count TYPE has in arm64 code compiled against it:
 * the count a native frozen type's layout gives on arm64 (its fields'
 * maximum, as Metadata.cpp performBasicLayout takes it), where it holds a
 * native spare-bit enum whose row gives arm64's count (Data: 12 against
 * x86_64's 252); the live count for everything else (a resilient native
 * type, whose count guest code reads from its metadata at run time; a guest
 * type, whose table guest code or this rule made). Through tuples and
 * Optionals (payload count - 1). *MOVED is set where a row changed a
 * count; a type without one keeps its live count, so a field reflection
 * does not list (a Builtin's in Bool) never changes it. 0 if not known. */
static int arm_xi(reflection *r, const void *type, unsigned depth, uint32_t *out, int *moved)
{
    const witnesses *v = vwt(type);
    const void *const *m = type;
    uintptr_t kind = r->kind(type);
    *out = v->extra;
    *moved = 0;
    if (depth > 16) return 0;
    if (kind == 0x202 && m[1] == r->optional) {
        uint32_t p;
        if (!arm_xi(r, m[2], depth + 1, &p, moved)) return 0;
        if (!*moved) { *out = v->extra; return 1; }
        if (!p) return 0;                       /* a tag byte on arm64, inhabitants on x86_64: another size */
        *out = p - 1;
        return 1;
    }
    bool native = (kind == 0x200 || kind == 0x201) && m[1] && !hao_image_for_addr((uint64_t)(uintptr_t)m[1]);
    if (kind == 0x201 && native && depth) {         /* inside a frozen struct: its layout is compiled in */
        mpenum_row row;
        if (mpenum_type(type, &row) && row.xi == v->extra && !(row.flags & MPENUM_XI) &&
            !row.x86_extra_bits && !row.arm_extra_bits && row.arm_xi != v->extra) {
            *out = row.arm_xi;
            *moved = 1;
        }
        return 1;
    }
    if (kind == 0x301 || (kind == 0x200 && native && native_frozen_type(type) && type != r->string &&
                          m[1] != r->array)) {
        intptr_t count = r->count(type);
        if (count < 0 || count > HAO_SV_FIELD_LIMIT) return 0;
        uint32_t best = 0;
        int any = 0;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r->field(type, i, &info);
            if (info.free_name) info.free_name(info.name);
            uint32_t c;
            int mv;
            if (!child || !arm_xi(r, child, depth + 1, &c, &mv)) return 0;
            any |= mv;
            if (c > best) best = c;
        }
        if (any) { *out = best; *moved = best != v->extra; }
        return 1;
    }
    return 1;
}

int arm_struct_xi(const void *const *fields, const uint8_t *tags, size_t n, uint32_t *out)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, 0,
    };
    if (!r.kind || !r.count || !r.field || !r.optional || n > 4096) return 0;
    uint32_t best = 0;
    int any = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c;
        int mv;
        if (tags[i] || !fields[i] || !arm_xi(&r, fields[i], 0, &c, &mv)) return 0;
        any |= mv;
        if (c > best) best = c;
    }
    if (!any) return 0;
    *out = best;
    return 1;
}

/* The byte that supplies TYPE's extra inhabitants, when they come from a
 * one-byte carrier whose inhabitants are byte values: Bool (exact $sSbN:
 * Builtin.Int1 in a byte, values 0 and 1, inhabitants 2...255) or an enum
 * with no payload case and N cases in one byte (values 0..N-1, inhabitants
 * N...255). IRGen gives both their inhabitants as the first value past the
 * cases plus the index (GenEnum.cpp NoPayloadEnumImplStrategy,
 * getFixedExtraInhabitantValue; GenType.cpp for integers with unused bits),
 * a rule of bit widths only, so XI k is the same byte on arm64 and x86_64.
 * Found through Optionals (payload count - 1) and, in a struct or tuple, its
 * first field with the most inhabitants (Metadata.cpp performBasicLayout),
 * each step checked against the value witness tables. That choice is the
 * same in arm64 code only where every field's arm64 count (arm_xi: a
 * native frozen type holding a spare-bit enum whose table row gives another
 * arm64 count) keeps the same field first: else 0. *OFFSET is the byte's
 * offset in TYPE, *FIRST the carrier's first inhabitant value, *XI its
 * count. Measured: `Optional<ToolbarItemGroup<V>>`, whose inhabitants are
 * its trailing Bool's (nil the byte 2 on both; tests/host/swiftlayout.swift
 * section M). */
static int byte_value_xi(reflection *r, const void *type, uint64_t *offset, unsigned *first, uint32_t *xi)
{
    const void *boolean = HAO_DLSYM("$sSbN");
    uint64_t at = 0;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const void *const *m = type;
        const witnesses *v = vwt(type);
        uintptr_t kind = r->kind(type);
        if (!v->extra) return 0;
        if ((boolean && type == boolean) || kind == 0x201) {
            unsigned cases;
            if (kind != 0x201) cases = 2;
            else if (m[1] && !(*(const uint32_t *)((const char *)m[1] + 20) & 0xffffff))
                cases = *(const uint32_t *)((const char *)m[1] + 24);   /* no payload case: N empty cases */
            else return 0;
            if (v->size != 1 || v->stride != 1 || (v->flags & 0x001100ff) || cases < 2 || cases > 255 ||
                v->extra != 256 - cases) return 0;
            *offset = at; *first = cases; *xi = v->extra;
            return 1;
        }
        if (kind == 0x202 && m[1] == r->optional) {
            if (vwt(m[2])->extra != v->extra + 1 || vwt(m[2])->size != v->size) return 0;
            type = m[2];
            continue;
        }
        if (kind != 0x200 && kind != 0x301) return 0;
        intptr_t count = r->count(type);
        if (count < 1 || count > HAO_SV_FIELD_LIMIT) return 0;
        const void *best = NULL;
        intptr_t best_at = -1, best_i = -1, arm_i = -1;
        uint32_t arm_best = 0;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r->field(type,i,&info);
            intptr_t o = r->offset(type,i);
            if (info.free_name) info.free_name(info.name);
            uint32_t c;
            int moved;
            if (!child || o < 0 || (uint64_t)o + vwt(child)->size > v->size || !arm_xi(r,child,1,&c,&moved)) return 0;
            if (!best || vwt(child)->extra > vwt(best)->extra) { best = child; best_at = o; best_i = i; }
            if (arm_i < 0 || c > arm_best) { arm_best = c; arm_i = i; }
        }
        if (vwt(best)->extra != v->extra || arm_i != best_i) return 0;
        at += (uint64_t)best_at;
        type = best;
    }
    return 0;
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

/* A native struct its module positively did not freeze (Tables.h
 * native_resilient_type): guest code holds it only through native
 * witnesses, so nothing inline in it is guest-laid-out. A native enum, or a
 * struct of an image the table does not cover, has no such evidence. */
static int native_opaque(reflection *r, const void *type)
{
    uintptr_t kind = r->kind(type);
    const void *const *m = type;
    if (kind != 0x200 || !m[1] || type == r->string || m[1] == r->array ||
        hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return 0;
    return native_resilient_type(type);
}

/* A nominal type with generic arguments (its context descriptor's generic flag). */
static int generic_nominal(const void *type)
{
    const char *desc = ((const char *const *)type)[1];
    uint32_t flags;
    if (!desc) return 1;
    memcpy(&flags,desc,4);
    return (flags & 0x80) != 0;
}

/* A native struct positively resilient (native_opaque) with no generic
 * argument, or an Optional (of any depth) of one. Guest code cannot lay out
 * such a value or name its extra inhabitants: it is made and read only by
 * native witnesses (the Optional's ask the payload's), so every byte of it,
 * nils included, is in the native encoding. Its fields are not reached by
 * guest-laid-out values either (no generic argument brings one in). */
static int native_owned_opaque(reflection *r, const void *type)
{
    for (unsigned depth = 0; depth < 16; ++depth) {
        const void *const *m = type;
        if (r->kind(type) == 0x202 && m[1] == r->optional && m[2]) { type = m[2]; continue; }
        return native_opaque(r,type) && !generic_nominal(type);
    }
    return 0;
}

/* A type built of integers and floats only: Builtin integers and floats
 * (no extra inhabitants, or under 8 bytes: Builtin.Int1), and structs,
 * tuples, Optionals and enums (inline payloads) of them, POD throughout and
 * holding no type declared differently on arm64 (a relaid one admitted).
 * No reference, metatype, function or raw pointer: their inhabitants and
 * spare bits are the ones that differ by architecture (a metatype's XI k is
 * k on arm64 and 2k on x86_64). A scalar's inhabitants and spare bits are
 * the same on both, so the compiler and the runtime lay out every enum of
 * them alike, and a value is the same bytes on both whatever its case. */
static int scalar_only(reflection *r, const void *type, unsigned depth)
{
    const witnesses *v = vwt(type);
    const void *const *m = type;
    if (depth > 16 || (v->flags & 0x10000) || v->size > 4096) return 0;
    if (guest_laid_listed(type)) return hao_swift_relaid_type(type);
    switch (r->kind(type)) {
    case 0x300: return !v->extra || v->size < 8;
    case 0x202: return m[2] && scalar_only(r,m[2],depth + 1);
    case 0x200: case 0x301: {
        intptr_t count = known_fields(r,type);
        if (count < 0) return 0;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r->field(type,i,&info);
            if (info.free_name) info.free_name(info.name);
            if (!child || !scalar_only(r,child,depth + 1)) return 0;
        }
        return 1;
    }
    case 0x201: {
        const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
        int n = enum_inline_payloads(type,payloads);
        if (n < 0) return 0;
        for (int i = 0; i < n; ++i) if (!scalar_only(r,payloads[i],depth + 1)) return 0;
        return 1;
    }
    default: return 0;
    }
}

int xi_native_owned(const void *type)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, 1,
    };
    if (!type || !r.kind || !r.count || !r.field || !r.optional) return 0;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const void *const *m = type;
        const witnesses *v = vwt(type);
        uintptr_t kind = r.kind(type);
        if (native_owned_opaque(&r,type)) return 1;
        if (kind == 0x202 && m[1] == r.optional && m[2]) { type = m[2]; continue; }
        if (kind != 0x200 && kind != 0x301) return 0;
        intptr_t count = known_fields(&r,type);
        const void *best = NULL;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r.field(type,i,&info);
            if (info.free_name) info.free_name(info.name);
            if (!child) return 0;
            if (!best || vwt(child)->extra > vwt(best)->extra) best = child;
        }
        if (!best || vwt(best)->extra != v->extra) return 0;
        type = best;
    }
    return 0;
}

int guest_owned_type(const void *type)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, 1,
    };
    if (!type || !r.kind || !r.optional) return 0;
    const void *const *m = type;
    if (guest_witnessed(&r,type)) return 1;
    return r.kind(type) == 0x202 && m[1] == r.optional && m[2] && vwt(type)->size == vwt(m[2])->size &&
           guest_tag_witnesses(&r,m[2]) && guest_witnessed(&r,m[2]);
}

int settled_node(const void *type, int *whole)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, 1,
    };
    nominals(&r);
    *whole = 0;
    if (!type || !r.kind || !r.count || !r.offset || !r.field || !r.array || !r.optional) return 0;
    const witnesses *v = vwt(type);
    const void *const *m = type;
    uintptr_t kind = r.kind(type);
#ifdef HAO_SWIFT_VALUE_RUNTIME
    /* A row of the walk's facts table (Facts.h) decides first, before any
     * rule below (a REFUSE row on a small POD too): SAME settles the whole
     * type; CONVERT, DYNAMIC and REFUSE need the value (fact_verdict). */
    walk_fact f;
    if (walk_fact_of(type,&f)) {
        if (f.verdict != WALK_FACT_SAME) return 0;
        *whole = 1;
        return 1;
    }
#endif
    /* A listed type is settled only where hao gave its native metadata the
     * arm64 layout and verified it (Swift.Float16: src/swift.c). */
    if (guest_laid_listed(type) && !hao_swift_relaid_type(type)) return 0;
    /* A POD of fewer than 8 bytes with no extra inhabitants holds no pointer
     * and no nil: its bytes mean the same to both. */
    if (v->size < 8 && !(v->flags & 0x10000) && !v->extra) { *whole = 1; return 1; }
    if (scalar_only(&r,type,0)) { *whole = 1; return 1; }
    /* The walk's own rules for these (collect): no pointer word in fewer than 8 bytes; a native type its
     * module did not freeze, with no generic argument, and Optionals of it, made and read by native code only. */
    if (v->size < 8 || native_owned_opaque(&r,type)) { *whole = 1; return 1; }
    switch (kind) {
    case 0x200: case 0x301: return known_fields(&r,type) >= 0;   /* by its fields, where they are known */
    case 0x202: {                       /* by its payload, once its own nil reads alike */
        const witnesses *payload = vwt(m[2]);
        empty_plan plan;
        uint64_t at;
        uint32_t xi;
        unsigned first;
        if (function_value(&r,type)) { *whole = 1; return 1; }
        if (m[1] != r.optional) return 0;
        return !payload->extra ||                                                   /* a trailing tag byte */
               (selected(&r,type,&plan,0) && plan.consumed == 1) ||               /* a zero word */
               (v->extra + 1 == payload->extra && tag_byte_xi(&r,m[2],&at,&xi) && xi >= payload->extra) ||
               (v->extra + 1 == payload->extra && function_xi_word(&r,m[2],&at)) ||
               (v->extra + 1 == payload->extra && v->size == payload->size &&                 /* a byte value */
                byte_value_xi(&r,m[2],&at,&first,&xi) && xi >= payload->extra);
    }
    case 0x302:
        if (function_value(&r,type)) { *whole = 1; return 1; }
        {
            uint64_t flags; memcpy(&flags,(const char *)type + 8,8);
            if (((flags >> 16) & 0xff) != 1 || v->size != 8) return 0;
        }
        *whole = 1;
        return 1;                       /* a block: an ObjC object */
    case 0: case 0x305: case 0x203: case 0x204: case 0x300: case 0x304: case 0x306:
        *whole = 1;                     /* objects of the shared native domain, metatypes, Builtins */
        return 1;
    default:                            /* enums, existentials, anything else: by the value */
        return 0;
    }
}

/* Whether TYPE is, or holds inline where guest code lays it out, a listed
 * frozen type: through struct fields, tuple elements, Optional payloads and
 * an enum's inline payload cases, stopping at a native opaque type. Such a
 * type's size and so its enclosing layouts can differ too, also where the
 * walk does not descend (an inactive Optional payload, an enum's inactive
 * cases). The search is bounded at 16 levels and 64 fields a level; what
 * lies beyond is not searched. 1 one is held, 2 fields not known (nothing
 * shows it holds none), else 0. */
static int holds_archdiff(reflection *r, const void *type, unsigned depth)
{
    if (guest_laid_listed(type)) return 1;
    uintptr_t kind = r->kind(type);
    const void *const *m = type;
    if (depth > 16 || (kind != 0x200 && kind != 0x201 && kind != 0x202 && kind != 0x301) ||
        type == r->string || (kind == 0x200 && m[1] == r->array) || native_opaque(r, type)) return 0;
    int memo = holds_archdiff_memo(type);
    if (memo >= 0) return memo;
    int listed = 0, c;
    if (kind == 0x202) listed = holds_archdiff(r, m[2], depth + 1);
    else if (kind == 0x201) {
        const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
        int n = enum_inline_payloads(type, payloads);
        for (int i = 0; i < n && listed != 1; ++i) if ((c = holds_archdiff(r, payloads[i], depth + 1))) listed = c;
    } else {
        intptr_t count = known_fields(r, type);
        if (count < 0) listed = 2;   /* fields not known: nothing shows it holds none */
        for (intptr_t i = 0; i < count && listed != 1; ++i) {
            field_info info = {0};
            const void *child = r->field(type, i, &info);
            if (info.free_name) info.free_name(info.name);
            if (child && (c = holds_archdiff(r, child, depth + 1))) listed = c;
        }
    }
    holds_archdiff_remember(type, listed);
    return listed;
}

/* A Common.c leaf rule (common_leaf_value): each one is a layout measured
 * identical on both architectures, live references in it the shared native
 * domain's. */
static int leaf(reflection *r, const void *type, const void *value)
{
    if (!common_leaf_value(type,value)) return 0;
    r->passes |= HAO_SV_PASS_IDENTICAL;
    return 1;
}

/* A live value of a kind other than a struct, tuple, enum, Optional, String,
 * ObjC class wrapper, or (published) existential and Swift object, which the
 * rules above take. Each kind passed as found names its class; a kind with
 * no rule here has no certified representation and refuses the value
 * (Metadata.h MetadataKind). */
static int other_kind(reflection *r, const void *type, uintptr_t kind)
{
    const witnesses *v = vwt(type);
    switch (kind) {
    case 0x300:   /* Builtin: an integer, float or vector is the same bits on both; an object reference
                     belongs to the native runtime that retains and releases it */
        r->passes |= (v->flags & 0x10000) ? HAO_SV_PASS_OWNED : HAO_SV_PASS_IDENTICAL;
        return 1;
    case 0x304: case 0x306:   /* a metatype: a pointer to the native runtime's metadata, which guest code uses too */
        r->passes |= HAO_SV_PASS_IDENTICAL;
        return 1;
    case 0x203: case 0x204:   /* a CF or foreign reference: the native runtime's object, retained by it */
        r->passes |= HAO_SV_PASS_OWNED;
        return 1;
    case 0x302: {             /* a block (function_value took every other function): an ObjC object */
        uint64_t flags; memcpy(&flags,(const char *)type + 8,8);
        if (((flags >> 16) & 0xff) != 1 || v->size != 8) return 0;
        r->passes |= HAO_SV_PASS_OWNED;
        return 1;
    }
    case 0:                   /* a Swift object (guest call of a native witness): the shared native domain's */
        if (!r->existentials) return 0;
        r->passes |= HAO_SV_PASS_OWNED;
        return 1;
    default:                  /* extended existentials, fixed-size arrays, anything newer: no rule */
        return 0;
    }
}

/* An Optional whose nil is an extra inhabitant of a guest-defined struct or
 * enum G whose single-payload tag witnesses (getEnumTagSinglePayload,
 * storeEnumTagSinglePayload) are guest code. The native Optional witnesses
 * learn the case only from those (Enum.cpp: a payload with extra
 * inhabitants decides a single-payload enum's case), so the guest's encoding
 * is read and written by the code that made it, and a present G is handed
 * whole to G's own witnesses; a guest call of a native witness rewrites
 * nothing inside a guest-defined type (swiftvalue.c guest_ranges). The whole
 * value then is opaque storage of its owner, whatever G's extra-inhabitant
 * count says on either architecture (hao_swift_struct_layout_done). Only for
 * that walk (r->existentials): a published value's native readers are not
 * shown to be G's witnesses. */
static int guest_witnessed_optional(reflection *r, const void *type)
{
    const void *const *m = type;
    if (!r->existentials || m[1] != r->optional || !m[2] || vwt(type)->size != vwt(m[2])->size) return 0;
    return guest_tag_witnesses(r,m[2]);
}
/* G is a guest-defined struct or enum with extra inhabitants whose single-
 * payload tag witnesses are guest code (guest_witnessed_optional). The same
 * holds for a guest single-payload enum whose empty cases are G's
 * inhabitants: its tag is asked of G's witnesses too. */
static int guest_tag_witnesses(reflection *r, const void *g)
{
#ifdef HAO_SWIFT_VALUE_RUNTIME
    const void *const *gm = g;
    uintptr_t k = r->kind(g);
    if ((k != 0x200 && k != 0x201) || !gm[1] || !hao_image_for_addr((uint64_t)(uintptr_t)gm[1]) ||
        !vwt(g)->extra) return 0;
    for (unsigned slot = 6; slot <= 7; ++slot) {
        uint64_t f = (uint64_t)(uintptr_t)vwt(g)->functions[slot];
        if (!f || (!hao_is_trap(f) && !hao_image_for_code(f) && !hao_guest_of_entry(f))) return 0;   /* guest code */
    }
    return 1;
#else
    (void)r; (void)g;
    return 0;
#endif
}

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth);

/* A guest-defined struct or enum whose copy, move and destroy witnesses
 * (slots 0-5) are all guest code: native code holding one, an existential's
 * runtime included, reaches its bytes only through them. */
static int guest_witnessed(reflection *r, const void *type)
{
#ifdef HAO_SWIFT_VALUE_RUNTIME
    const void *const *m = type;
    uintptr_t k = r->kind(type);
    if ((k != 0x200 && k != 0x201) || !m[1] || !hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return 0;
    for (unsigned slot = 0; slot <= 5; ++slot) {
        uint64_t f = (uint64_t)(uintptr_t)vwt(type)->functions[slot];
        if (!f || (!hao_is_trap(f) && !hao_image_for_code(f) && !hao_guest_of_entry(f))) return 0;
    }
    return 1;
#else
    (void)r; (void)type;
    return 0;
#endif
}

/* An out-of-line existential value: the buffer's first word is a box
 * (swift_allocBox: a HeapObject, then the value at the header size rounded
 * up to its alignment). Copying the container only retains the box; but
 * destroying or assigning it can release the last reference, and the
 * runtime then destroys the value with the dynamic type's own witnesses. A
 * box is shared storage no conversion may rewrite, so its value must need
 * none: walked as the dynamic type, every nil in it already in the encoding
 * of the code that will read it. */
static int boxed_payload(reflection *r, const void *payload, const unsigned char *value, unsigned depth)
{
#ifdef HAO_SWIFT_VALUE_RUNTIME
    const witnesses *pv = vwt(payload);
    uint64_t box, word, mask = pv->flags & 255, at = (16 + mask) & ~mask;
    memcpy(&box,value,8);
    if (box < 4096 || (box & 7) || pv->size > 4096 || !hao_probe_read8(box,&word) ||
        (pv->size && !hao_probe_read8(box + at + ((pv->size - 1) & ~(uint64_t)7),&word))) return WRONG(r);   /* no box */
    empty_patch local[64];
    unsigned n = 0;
    r->boxed++;
    int ok = collect(r,payload,(const unsigned char *)(uintptr_t)(box + at),0,local,64,&n,depth + 1);
    r->boxed--;
    /* Each nil must already be in the encoding its reader takes: the
     * receiving witnesses' for a value native code gets, arm64's for one
     * guest code reads inline (r->known_layout). One that is not cannot be
     * converted in a shared box, also where the walk then failed. */
    for (unsigned i = 0; i < n && i < 64; ++i)
        if (local[i].bits != (r->known_layout ? local[i].arm_bits : local[i].receiver_bits)) {
            r->passes |= HAO_SV_UNCONVERTIBLE;
            return 0;
        }
    if (!ok) return 0;
    r->passes |= HAO_SV_PASS_OWNED;
    return 1;
#else
    (void)r; (void)payload; (void)value; (void)depth;
    return 0;
#endif
}

/* The contents of a live existential (Metadata.h TargetExistentialTypeMetadata,
 * ExistentialMetadataImpl.h). Native code copying, assigning or destroying
 * the container calls the dynamic type's own witnesses on an inline value,
 * so a guest-encoded nil inside it reaches them as it is: the inline value is
 * walked as a value of the dynamic type, at the container's offset 0. Passed
 * as found: a class-bound container's object (the shared native domain's),
 * and an out-of-line value whose boxed contents need no conversion
 * (boxed_payload). */
static int existential_payload(reflection *r, const void *type, const unsigned char *value, uint64_t base,
                               empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    uint32_t flags; memcpy(&flags,(const char *)type + 8,4);
    if (!(flags & 0x80000000u)) { r->passes |= HAO_SV_PASS_OWNED; return 1; }   /* class-bound: an object word */
    uint64_t dynamic; memcpy(&dynamic,value + 24,8);
#ifdef HAO_SWIFT_VALUE_RUNTIME
    uint64_t table = 0;
    if (dynamic < 4096 || (dynamic & 7) || !hao_probe_read8(dynamic - 8,&table) || !table || (table & 7)) return WRONG(r);
#else
    return 0;
#endif
    const void *payload = (const void *)(uintptr_t)dynamic;
    const witnesses *pv = vwt(payload);
    int whole = 0;
    if (settled_node(payload,&whole) && whole) { r->passes |= HAO_SV_PASS_IDENTICAL; return 1; }   /* by its metadata */
    if (pv->flags & 0x00020000) return boxed_payload(r,payload,value,depth);   /* out of line: a box */
    if (pv->size > 24) return WRONG(r);   /* inline in a 3-word buffer only up to its size */
    r->boxed++;
    int ok = collect(r,payload,value,base,out,capacity,used,depth + 1);
    r->boxed--;
    return ok;
}

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth);

/* An Optional TYPE whose nil is the pointer-word extra inhabitant PLAN
 * describes (consumed 1 or 2: XI0 0 on both, XI1 1 on arm64 and 2 on
 * x86_64, so either encoding is recognized): its nil is a patch into the
 * receiving witness set's encoding, anything else a present payload. */
static int pointer_xi_optional(reflection *r, const void *type, const empty_plan *plan, const unsigned char *value,
                               uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    if (!plan->consumed || plan->consumed > 2 || plan->offset > vwt(type)->size - 8) return 0;
    uint64_t word; memcpy(&word, value + plan->offset, 8);
    if (plan->consumed == 1 ? word == 0 : (word == 1 || word == 2)) {
        if (*used == capacity) return WRONG(r);
        /* Native generic containers may delegate to ARM field witnesses.
         * Their receiving encoding comes from the actual witness set,
         * independently of both the process architecture and source bits. */
        uint64_t receiver;
        if (!receiver_empty_bits(type,plan,&receiver)) return WRONG(r);   /* a nil to convert, no encoding to convert to */
        out[(*used)++] = (empty_patch){base + plan->offset, word, receiver, plan->consumed, plan->consumed - 1u};
        return 1;                     /* inactive payload must remain opaque */
    }
    /* This is a present payload in either admitted representation. */
    return collect(r, ((const void *const *)type)[2], value, base, out, capacity, used, depth + 1);
}

#ifdef HAO_SWIFT_VALUE_RUNTIME
/* The bytes TYPE's own destructiveInjectEnumTag writes for case WHICH over
 * zeroed storage (no payload: nothing retained or released), N from offset
 * AT into OUT. */
static int injected_over(const void *type, unsigned which, int fill, uint64_t at, uint64_t n, unsigned char *out);
static int injected(const void *type, unsigned which, uint64_t at, uint64_t n, unsigned char *out)
{
    return injected_over(type,which,0,at,n,out);
}
/* The whole value's bytes case WHICH writes over storage filled with FILL. */
static int injected_fill(const void *type, unsigned which, int fill, unsigned char *out)
{
    return injected_over(type,which,fill,0,vwt(type)->size,out);
}
static int injected_over(const void *type, unsigned which, int fill, uint64_t at, uint64_t n, unsigned char *out)
{
    const witnesses *v = vwt(type);
    inject_tag_fn *inject = ((inject_tag_fn *const *)v)[13];
    size_t alignment = (v->flags & 255) + 1;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    void *scratch = NULL;
    if (!inject || !(v->flags & 0x00200000) || v->stride < v->size || v->stride > 8192 ||
        at > v->size || n > v->size - at || posix_memalign(&scratch,alignment,(size_t)v->stride)) return 0;
    memset(scratch,fill,(size_t)v->stride);
    inject(scratch,which,type);
    memcpy(out,(const char *)scratch + at,(size_t)n);
    free(scratch);
    return 1;
}

/* case_images' buffers, too big for a guest stack. */
static pthread_mutex_t images_lock = PTHREAD_MUTEX_INITIALIZER;
void recipe_fork_child(void) { images_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

/* A CONVERT row (Facts.h): an enum whose empty cases' encodings differ by
 * architecture, every case listed with each architecture's image and mask
 * (the bits its injection defines: the same over zeros and over ones). A
 * value is in an empty case when its bits under that case's mask are the
 * image's; zero bits of an image are bits like any other, and an empty case
 * whose mask is all zero is unproven: refused. The x86_64 image and mask are
 * checked against the type's own witness (injected over zeros and over
 * ones); a stale row is refused. Which architecture's encoding the bytes are
 * in is the value's source (pointer_word_xi); where a value of unknown source
 * reads as different cases it is refused. A recognized empty case is patched
 * word by word, only its defined bits rewritten (the payload bits kept), each
 * patched word wholly inside the value (sizes need not be a multiple of 8:
 * the last word ends at the value's end, and may not overlap an earlier
 * patched one). A value in no empty case is in the payload case, whose mask
 * is all zero (a single-payload enum): an Optional's payload is walked. A
 * payload case with a mask of its own (a multi-payload enum) is not walked
 * from the images, and a CONVERT row of another enum than an Optional is
 * refused. */
static int image_case(reflection *r, const void *type, const walk_fact *f, const unsigned char *value,
                      uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, int multi,
                      unsigned *found, uint64_t *defined_from);
static int case_images(reflection *r, const void *type, const walk_fact *f, const unsigned char *value,
                       uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    uintptr_t kind = r->kind(type);
    unsigned which;
    uint64_t from;
    if (kind != 0x202 || ((const void *const *)type)[1] != r->optional) return 0;
    int verdict = image_case(r,type,f,value,base,out,capacity,used,0,&which,&from);
    if (verdict >= 0) return verdict;
    return collect(r,((const void *const *)type)[2],value,base,out,capacity,used,depth + 1);
}
/* The case of VALUE by F's images: -1 in no case with an image of its own
 * (the payload case of a single-payload enum), 0 refused, 1 in case *FOUND
 * (its differing defined words patched; *DEFINED_FROM the first byte either
 * mask defines). Without MULTI as case_images says; with MULTI (a guest
 * enum's row) every case may have an image, payload cases too, and exactly
 * one must match; a case with no image is the one matched when none other is. */
static int image_case(reflection *r, const void *type, const walk_fact *f, const unsigned char *value,
                      uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, int multi,
                      unsigned *found, uint64_t *defined_from)
{
    const witnesses *v = vwt(type);
    uint64_t size = v->size;
    if (size < 8 || size > WALK_FACT_IMAGE_BYTES || !f->nimages) return 0;
    static unsigned char arm[WALK_FACT_IMAGE_BYTES], amask[WALK_FACT_IMAGE_BYTES], x86[WALK_FACT_IMAGE_BYTES],
                         xmask[WALK_FACT_IMAGE_BYTES], zeros[WALK_FACT_IMAGE_BYTES], ones[WALK_FACT_IMAGE_BYTES];
    int verdict = -1, matches = 0;   /* -1: in no case with an image (yet) */
    unsigned chosen = 0, unimaged = 0, nunimaged = 0;
    hao_crit_enter(); pthread_mutex_lock(&images_lock);
    for (unsigned i = 0; i < f->nimages && verdict != 0; ++i) {
        unsigned which;
        if (!walk_fact_image(f,i,&which,arm,amask,x86,xmask,size)) { verdict = 0; break; }
        int any_arm = 0, any_x86 = 0;
        for (uint64_t b = 0; b < size; ++b) { any_arm |= amask[b]; any_x86 |= xmask[b]; }
        if (!any_arm && !any_x86 && (multi || which == 0)) { unimaged = which; nunimaged++; continue; }
        if (which == 0 && !multi) { verdict = 0; break; }   /* an Optional's payload case with a mask: not a single payload */
        if (!any_arm || !any_x86 ||                  /* a case no bit proves */
            !injected_fill(type,which,0x00,zeros) || !injected_fill(type,which,0xff,ones)) { verdict = 0; break; }
        int stale = 0, is_arm = 1, is_x86 = 1;
        for (uint64_t b = 0; b < size; ++b) {
            stale |= zeros[b] != x86[b] || (unsigned char)~(zeros[b] ^ ones[b]) != xmask[b];
            is_arm &= ((value[b] ^ arm[b]) & amask[b]) == 0;
            is_x86 &= ((value[b] ^ x86[b]) & xmask[b]) == 0;
        }
        if (stale) { verdict = 0; break; }
        int reading = r->source == HAO_SV_SOURCE_GUEST ? is_arm : r->source == HAO_SV_SOURCE_NATIVE ? is_x86 :
                      is_arm == is_x86 ? is_arm : -1;
        if (reading < 0) { verdict = 0; break; }    /* unknown source, the readings differ */
        if (!reading) continue;
        if (matches++) { verdict = 0; break; }      /* two cases read alike: no case proven */
        chosen = i;
        if (!multi) break;
    }
    if (verdict && !matches) {
        verdict = multi && nunimaged == 1 ? 1 : nunimaged > 1 ? 0 : -1;
        if (verdict > 0) { *found = unimaged; *defined_from = size; }
        if (multi && !nunimaged) WRONG(r);   /* every case imaged, none matches: no case of this enum */
    } else if (verdict && matches == 1) {
        unsigned which;
        verdict = walk_fact_image(f,chosen,&which,arm,amask,x86,xmask,size);
        *found = which;
        *defined_from = size;
        for (uint64_t b = 0; b < size; ++b) if ((amask[b] | xmask[b]) && b < *defined_from) *defined_from = b;
        uint64_t last = 0;
        int patched = 0;
        for (uint64_t w = 0; w < size && verdict; w += 8) {
            uint64_t at = w + 8 <= size ? w : size - 8;
            int differs = 0;
            for (uint64_t b = at; b < at + 8; ++b) differs |= amask[b] != xmask[b] || ((arm[b] ^ x86[b]) & xmask[b]);
            if (!differs) continue;
            if ((patched && at < last + 8) || *used == capacity) { verdict = WRONG(r); break; }
            uint64_t word, guest = 0, receiver = 0;
            memcpy(&word,value + at,8);
            for (unsigned k = 0; k < 8; ++k) {
                unsigned char fb = value[at + k];
                receiver |= (uint64_t)(unsigned char)((fb & ~xmask[at + k]) | (x86[at + k] & xmask[at + k])) << (8 * k);
                guest |= (uint64_t)(unsigned char)((fb & ~amask[at + k]) | (arm[at + k] & amask[at + k])) << (8 * k);
            }
            out[(*used)++] = (empty_patch){base + at, word, receiver, which + 1, guest};
            last = at; patched = 1;
        }
        if (verdict && !multi) r->passes |= HAO_SV_PASS_INACTIVE;   /* an empty case: no payload */
    }
    pthread_mutex_unlock(&images_lock); hao_crit_leave();
    return verdict;
}

/* A guest-defined type's CONVERT row (Facts.h walk_fact_of): a struct by
 * its fields' rows, each field at its live offset (a SAME field as found, a
 * REFUSE one refused, any other walked by its own type's row or the rules);
 * an enum by its cases' images (image_case: the case, the tag words that
 * differ patched), then the active payload case by its row the same way,
 * walked only where no tag bit lies inside the payload's bytes. */
static int guest_convert(reflection *r, const void *type, const walk_fact *f, const unsigned char *value,
                         uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    uintptr_t kind = r->kind(type);
    uint64_t size = vwt(type)->size;
    char verdict;
    if (kind == 0x200) {
        intptr_t count = known_fields(r,type);
        if (count < 0) return 0;
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = r->field(type,i,&info);
            if (info.free_name) info.free_name(info.name);
            intptr_t at = r->offset(type,i);
            if (!child || at < 0 || (uint64_t)at > size || vwt(child)->size > size - (uint64_t)at ||
                !walk_fact_field(f,(unsigned)i,&verdict)) return 0;
            if (verdict == WALK_FACT_REFUSE) return WRONG(r);
            if (verdict == WALK_FACT_SAME) { r->passes |= HAO_SV_PASS_IDENTICAL; continue; }
            if (!collect(r,child,value + at,base + (uint64_t)at,out,capacity,used,depth + 1)) return 0;
        }
        return 1;
    }
#ifdef HAO_SWIFT_VALUE_RUNTIME
    unsigned which;
    uint64_t from;
    const void *payload = NULL;
    int indirect = 0;
    if (kind != 0x201 || image_case(r,type,f,value,base,out,capacity,used,1,&which,&from) != 1 ||
        !walk_fact_field(f,which,&verdict)) return 0;
    if (verdict == WALK_FACT_REFUSE) return WRONG(r);
    if (verdict == WALK_FACT_SAME || !walk_fact_payload(type,which,&payload,&indirect)) {
        r->passes |= HAO_SV_PASS_IDENTICAL;   /* an empty case (its row SAME) or a payload the same on both */
        return verdict == WALK_FACT_SAME;
    }
    if (indirect || !payload || vwt(payload)->size > from) return 0;
    return collect(r,payload,value,base,out,capacity,used,depth + 1);
#else
    return 0;
#endif
}

/* An Optional the table does not list (a guest's instantiation over a
 * listed type), under any number of Optionals, whose innermost payload T has
 * a row of XI class POINTER (Facts.h): T's extra inhabitants differ only in
 * the pointer word at the row's offset, pointer XI q being q on arm64 and 2q
 * on x86_64 (Metadata.h swift_getHeapObjectExtraInhabitantIndex: arm64's
 * inhabitants are the low values, x86_64's the low even ones, the odd ones
 * being ObjC tagged pointers). Each Optional consumes the next: T uses
 * 0x7fffffff - xi(T) of them itself, and the Optional L levels above T has
 * nil p = that + L - 1. Measured for ModifiedContent<...ZStack<Optional<
 * Leaf>>...>?? on both (tests/guest/overlaynil.swift: nil at word 16, 2 on
 * arm64, 4 on x86_64). The row's word is checked against this Optional's own
 * witness first: its nil must be 2p there.
 *
 * The word is read in the encoding of the side that wrote the value
 * (r->source: guest storage the guest's, a value native code made the
 * native one). Below 4096 it is an inhabitant: arm64's q, or x86_64's q/2
 * (even values only); q == p this Optional's nil (a patch to 2p, arm64's p
 * beside it), q < p a present payload holding an inner nil (walked), q > p an
 * outer Optional's nil, no value of this type. The word 2p (p > 0) reads as
 * this nil in both encodings, since arm64's 2p is past p. From p = 2 on the
 * encodings overlap: arm64's p is x86_64's inner nil p/2 (native
 * T???.some(.none) is 2, arm64's T???.none). Such a word is decided by the
 * source, and refused when the source is not known; an odd word in native
 * bytes is no inhabitant and refused. -1: not such an Optional. */
static int pointer_word_xi(reflection *r, const void *type, const unsigned char *value,
                           uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    const void *t = type;
    unsigned levels = 0;
    while (r->kind(t) == 0x202 && ((const void *const *)t)[1] == r->optional && levels < 16) {
        const void *payload = ((const void *const *)t)[2];
        if (!payload || vwt(payload)->extra != vwt(t)->extra + 1 || vwt(payload)->size != vwt(t)->size) return -1;
        t = payload;
        ++levels;
    }
    walk_fact f;
    if (!levels || !walk_fact_of(t,&f) || f.xi_class != WALK_XI_POINTER || f.xi_offset < 0 || f.xi_offset % 8 ||
        (uint64_t)f.xi_offset + 8 > vwt(type)->size || vwt(t)->extra > 0x7fffffff) return -1;
    uint64_t inner = 0x7fffffffu - vwt(t)->extra, p = inner + levels - 1, at = (uint64_t)f.xi_offset, word;
    if (p >= 2048) return 0;
    uint64_t receiver;
    if (!injected(type,1,at,8,(unsigned char *)&receiver) || receiver != 2 * p) return 0;
    memcpy(&word,value + at,8);
    int nil = word == 2 * p;                        /* both encodings: this nil */
    if (!nil && word < 4096) {
        /* Each reading: no value of this type, or this nil, or a present
         * payload (an inner nil, walked at its own level). */
        int arm = word > p ? 0 : word == p ? 1 : 2;
        int x86 = (word & 1) || word / 2 > p ? 0 : 2;   /* word / 2 == p is 2p, taken above */
        int reading = r->source == HAO_SV_SOURCE_GUEST ? arm : r->source == HAO_SV_SOURCE_NATIVE ? x86 :
                      !arm ? x86 : !x86 || arm == x86 ? arm : 0;   /* unknown: two valid readings must agree */
        if (!reading) return r->source || (!arm && !x86) ? WRONG(r) : 0;   /* no value of this type (in the writer's encoding) */
        nil = reading == 1;
    }
    if (nil) {
        if (*used == capacity) return WRONG(r);
        out[(*used)++] = (empty_patch){base + at, word, receiver, (uint32_t)p + 1, p};
        r->passes |= HAO_SV_PASS_INACTIVE;
        return 1;
    }
    return collect(r,((const void *const *)type)[2],value,base,out,capacity,used,depth + 1);
}
#endif

/* TYPE's row in the walk's facts table (Facts.h), when it has one: -1 none
 * (or DYNAMIC: the rules below walk it), else the verdict on the value (0
 * refused, 1 certified, a conversion's patches appended). A row's verdict
 * replaces the rules for that type: the table's generator compared both
 * architectures' metadata and case images for it, which the rules infer. */
static int fact_verdict(reflection *r, const void *type, const unsigned char *value, uint64_t base,
                        empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
#ifdef HAO_SWIFT_VALUE_RUNTIME
    walk_fact f;
    if (!walk_fact_of(type,&f)) {
        /* An Optional of a guest row's type whose extra inhabitants are the
         * same on both (XI class SAME): its nil is the same bytes, the ones
         * its own witness injects (over zeros and ones, in scratch storage:
         * the defined bits); a value that is not nil is a payload, walked. */
        const void *const *m = type;
        const witnesses *v = vwt(type);
        uint64_t size = v->size;
        if (r->kind(type) != 0x202 || m[1] != r->optional || !m[2] || !walk_fact_of(m[2],&f) || !f.guest ||
            f.verdict == WALK_FACT_REFUSE || f.xi_class != WALK_XI_SAME || vwt(m[2])->size != size || vwt(m[2])->extra != v->extra + 1 || size > 4096)
            return -1;
        typedef struct { const void *metadata; uintptr_t state; } response;
        typedef response SWIFT generic_fn(uintptr_t, const void *const *, const void *);
        generic_fn *get = HAO_DLSYM("swift_getGenericMetadata");
        if (!get || get(0,m + 2,r->optional).metadata != type) return 0;   /* the runtime's Optional of it only */
        hao_crit_enter();
        unsigned char *zeros = malloc((size_t)size * 2 + 1), *ones = zeros ? zeros + size : NULL;
        int nil = 1, defined = 0;
        if (!zeros || !injected_fill(type,1,0x00,zeros) || !injected_fill(type,1,0xff,ones)) {
            free(zeros); hao_crit_leave(); return 0;
        }
        for (uint64_t b = 0; b < size; ++b) {
            unsigned char mask = (unsigned char)~(zeros[b] ^ ones[b]);
            nil &= ((value[b] ^ zeros[b]) & mask) == 0;
            defined |= mask;
        }
        free(zeros);
        hao_crit_leave();
        if (!defined) return 0;   /* a nil no bit tells */
        if (nil) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }
        return collect(r,m[2],value,base,out,capacity,used,depth + 1);
    }
    switch (f.verdict) {
    case WALK_FACT_SAME:
        r->passes |= HAO_SV_PASS_IDENTICAL;
        return 1;
    case WALK_FACT_DYNAMIC:     /* the existential and field rules certify the dynamic parts */
        return -1;
    case WALK_FACT_CONVERT:
        if (f.guest) return guest_convert(r,type,&f,value,base,out,capacity,used,depth);
        return case_images(r,type,&f,value,base,out,capacity,used,depth);
    default:                    /* WALK_FACT_REFUSE, or a verdict this runtime does not know */
        return WRONG(r);
    }
#else
    (void)r; (void)type; (void)value; (void)base; (void)out; (void)capacity; (void)used; (void)depth;
    return -1;
#endif
}

static int collect_inner(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    if (depth > 32) return 0;
    uintptr_t kind = r->kind(type);
    const witnesses *v = vwt(type);
    if (v->size > 4096) return 0;
    int fact = fact_verdict(r,type,value,base,out,capacity,used,depth);
    if (fact >= 0) return fact;
    /* A type declared differently on arm64 that guest code lays out, or one
     * that holds such a type inline, has no representation both sides read
     * alike: refused under every rule below, the one for small values
     * included. Inside a native opaque type everything is native-laid-out. */
    int archdiff = r->opaque ? 0 : holds_archdiff(r, type, 0);
    if (archdiff == 1) return WRONG(r);
    if (archdiff == 2 && !r->looking) {
        /* Fields not known somewhere inside: refused as unproven. The partial
         * walk still walks the type by the rules below, for a known-wrong part
         * beside the unknown one or a nil to convert that the refusal would
         * leave unconverted (flags only; nothing is patched): only the
         * unknown types themselves may refuse for it to stay report-only. */
        if (!r->survey) return 0;
        unsigned mark = *used, refused = r->surveyed, unknown = r->unknown;
        r->looking++;
        int ok = collect_inner(r, type, value, base, out, capacity, used, depth);
        r->looking--;
        /* A part refused there for any other reason may hide such a nil
         * (an Optional around the unknown type whose case it cannot tell). */
        unsigned self = !ok && (kind == 0x200 || kind == 0x301) && known_fields(r,type) < 0;   /* the unknown type itself */
        if ((!ok && !self) || r->surveyed - refused > r->unknown - unknown - self) r->passes |= HAO_SV_KNOWN_WRONG;
        for (unsigned i = mark; i < *used; ++i)
            if (out[i].bits != (r->known_layout ? out[i].arm_bits : out[i].receiver_bits)) r->passes |= HAO_SV_KNOWN_WRONG;
        *used = mark;
        return 0;
    }
    /* A patch rewrites a whole pointer word. A value of fewer than 8 bytes
     * holds no pointer, so no nil in it has a pointer extra inhabitant and
     * the case it is in cannot hide one: there is nothing to patch. */
    if (v->size < 8) { r->passes |= HAO_SV_PASS_IDENTICAL; return 1; }
    /* A guest type built of integers and floats only (scalar_only): the same
     * bytes on both architectures, whatever its case. */
    const void *const *gm = type;
    if ((kind == 0x200 || kind == 0x201) && gm[1] && hao_image_for_addr((uint64_t)(uintptr_t)gm[1]) &&
        scalar_only(r,type,0)) { r->passes |= HAO_SV_PASS_IDENTICAL; return 1; }
    if ((kind == 0x302 || kind == 0x202) && function_value(r,type)) { r->passes |= HAO_SV_PASS_IDENTICAL; return 1; }
    if (type == r->string) {
        empty_plan shape;
        if (!selected(r,type,&shape,0)) return 0;
        uint64_t bits; memcpy(&bits,value + shape.offset,8);
        /* An active String cannot be any admitted nil discriminator. Live
         * storage uses the existing native object/ARC domain; this is not a
         * validator for arbitrary string bytes or foreign-runtime objects. */
        r->passes |= HAO_SV_PASS_OWNED;
        return bits > 2 ? 1 : WRONG(r);
    }
    if (kind == 0x202) {
        const void *const *m = type;
        const witnesses *payload = vwt(m[2]);
        int byte_case = common_byte_optional_case(type,value);
        if (byte_case >= 0) {
            if (byte_case) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; } /* inactive payload remains opaque */
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
            if (word == none) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }   /* inactive payload remains opaque */
            if (word > none && word < 0x7fffffffu) return WRONG(r); /* an outer Optional's nil: not this type's value */
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
            if (tag == none) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }    /* inactive payload remains opaque */
            if (tag >= 256 - enum_xi && tag < none) return WRONG(r); /* an outer Optional's nil */
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
                        (!memcmp(found,ox,(size_t)size) || (oh && !memcmp(found,oa,(size_t)size)))) return WRONG(r);
                }
                return collect(r,m[2],value,base,out,capacity,used,depth + 1);
            }
            if (!have_arm) return WRONG(r);   /* a nil arm64 cannot encode */
            if (xe.offset % 8) return 0;
            /* The receiving witness set's own nil. */
            const witnesses *ov = vwt(type);
            inject_tag_fn *inject = ((inject_tag_fn *const *)ov)[13];
            unsigned char *scratch = NULL;
            size_t alignment = (ov->flags & 255) + 1;
            if (alignment < sizeof(void *)) alignment = sizeof(void *);
            if (!inject || ov->stride < ov->size || ov->stride > 8192 ||
                posix_memalign((void **)&scratch,alignment,(size_t)ov->stride)) return WRONG(r);
            memset(scratch,0xa5,(size_t)ov->stride);
            inject(scratch,1,type);
            const unsigned char *receiver = scratch + xe.offset;
            int ok = !memcmp(receiver,x86,(size_t)size) || !memcmp(receiver,arm,(size_t)size);
            for (uint64_t w = 0; ok && w < size; w += 8) {
                if (!memcmp(x86 + w,arm + w,8)) continue;
                if (w + 8 > size || *used == capacity) { ok = WRONG(r); break; }
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
                if (!memcmp(found,nil,(size_t)size)) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }   /* inactive payload */
                for (uint32_t j = k + 1; j < vwt(guest_enum)->extra && j < k + 9; ++j)
                    if (guest_spare_bit_xi(guest_enum,j,nil) && !memcmp(found,nil,(size_t)size)) return WRONG(r);
                return collect(r,m[2],value,base,out,capacity,used,depth + 1);
            }
        }
        /* A payload whose inhabitants are a byte carrier's values
         * (byte_value_xi): this Optional's nil is the carrier's first
         * inhabitant plus the inhabitants inner Optionals consumed, the same
         * byte on both architectures; the next ones are outer Optionals'
         * nils, no value of this type; anything else is a present payload. */
        uint64_t byte_at;
        unsigned byte_first;
        uint32_t byte_xi;
        if (m[1] == r->optional && payload->extra && v->extra + 1 == payload->extra && v->size == payload->size &&
            byte_value_xi(r,m[2],&byte_at,&byte_first,&byte_xi) && byte_xi >= payload->extra) {
            unsigned found = value[byte_at], none = byte_first + (byte_xi - payload->extra);
            if (found == none) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }   /* inactive payload remains opaque */
            if (found > none) return WRONG(r);                                   /* an outer Optional's nil */
            return collect(r,m[2],value,base,out,capacity,used,depth + 1);
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
            if (tag == 1) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }
            if (tag != 0) return WRONG(r);   /* no case of this Optional */
            return collect(r,m[2],value,base,out,capacity,used,depth + 1);
        }
#ifdef HAO_SWIFT_VALUE_RUNTIME
        int pointer = pointer_word_xi(r,type,value,base,out,capacity,used,depth);
        if (pointer >= 0) return pointer;
#endif
        empty_plan plan;
        /* Unknown Optional means unknown active payload. Reject the entire
         * plan, even if another sibling already supplied a valid patch. */
        if (!selected(r, type, &plan, 0) || !plan.consumed) {
            if (guest_witnessed_optional(r,type)) { r->passes |= HAO_SV_PASS_OWNED; return 1; }
            /* A nil a native witness made for guest code to read inline
             * (r->known_layout): over a type guest code cannot lay out, it
             * reads none inline; the native encoding stays. */
            if (r->known_layout && native_owned_opaque(r,type)) { r->passes |= HAO_SV_PASS_OWNED; return 1; }
            return leaf(r,type,value);
        }
        return pointer_xi_optional(r,type,&plan,value,base,out,capacity,used,depth);
    }
    if (kind == 0x201) {
        if (leaf(r,type,value)) return 1;
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
        if (r->existentials && guest_single_payload_enum(type,&single,&empty_cases) && guest_tag_witnesses(r,single) &&
            !selected(r,single,&inner,0)) {
            r->passes |= HAO_SV_PASS_OWNED;   /* its case asked of the payload's guest witnesses */
            return 1;
        }
        if (guest_single_payload_enum(type,&single,&empty_cases) && selected(r,single,&inner,0) &&
            inner.offset <= v->size - 8) {
            uint64_t word, first = inner.consumed;
            memcpy(&word,value + inner.offset,8);
            if (word >= first && word < first + empty_cases) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }
            if (word >= first + empty_cases && word < first + empty_cases + 8) return WRONG(r);
            return collect(r,single,value,base,out,capacity,used,depth + 1);
        }
        enum_case_plan active;
        if (discover_enum_case(type,value,&active)) {
            if (!active.payload) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }   /* an empty case */
            return collect(r,active.payload,value,base,out,capacity,used,depth + 1);
        }
        const void *runtime_payloads[HAO_SV_MAX_ENUM_PAYLOADS];
        uint64_t extent;
        if (enum_payloads(type,runtime_payloads,&extent)) return WRONG(r);   /* its layout holds, the value is no case */
        fixed_enum_case fixed;
        fixed.malformed = 0;
        if (!discover_fixed_enum_case(type,value,&fixed)) return fixed.malformed ? WRONG(r) : 0;
        if (!fixed.payload) { r->passes |= HAO_SV_PASS_INACTIVE; return 1; }   /* the empty case: no payload */
        if (!fixed.spare) return collect(r,fixed.payload,value,base,out,capacity,used,depth + 1);
        unsigned first = *used;
        if (!collect(r,fixed.payload,(const unsigned char *)fixed.copy,base,out,capacity,used,depth + 1)) return 0;
        /* A patch is written into the live value: one over a tag bit would
         * change the case. */
        for (unsigned i = first; i < *used; ++i) {
            uint64_t at = out[i].offset - base;
            for (uint64_t b = at; b < at + 8; ++b)
                if ((fixed.mask[b / 8] >> (8 * (b % 8))) & 0xff) return WRONG(r);
        }
        return 1;
    }
    if (kind == 0x305) {
        r->passes |= HAO_SV_PASS_OWNED;
        if (common_objc_reference_value(type,value)) return 1;
        return common_objc_reference_type(type) ? WRONG(r) : 0;   /* a reference word that is no object */
    }
    /* A live existential or Swift object in a published value (the plain
     * walk; selected() plans their nils there too): native code takes it, so
     * each witness table must be native, its conformance descriptor in no
     * guest image (a guest conformance's table is arm64 code; a generic one's
     * table is instantiated on the heap), and a Swift object word a heap
     * pointer. A guest call of a native witness only copies them. Not
     * certified, but nothing known wrong either: a special representation
     * (the Error box) and a guest conformance to a native protocol, whose
     * calls native code makes through hao's callbacks; its payload is still
     * walked for a part that is known wrong. */
    if ((kind == 0x303 || kind == 0) && !r->existentials) {
        r->passes |= HAO_SV_PASS_OWNED;
        if (kind == 0) {
            uint64_t bits; memcpy(&bits,value,8);
            return bits >= 4096 && !(bits & 7) ? 1 : WRONG(r);
        }
        uint32_t flags, protocols; memcpy(&flags,(const char *)type + 8,4); memcpy(&protocols,(const char *)type + 12,4);
        uint64_t tables = flags & 0xffffff, first = (flags & 0x80000000u) ? 32 : 8;
        if (flags & 0x3f000000u) return 0;   /* a special representation: no rule */
        if (first + 8 * tables > v->size) return WRONG(r);
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
        int guest_conformance = 0;
        for (uint64_t i = 0; i < tables; ++i) {
            uint64_t table, conformance = 0;
            memcpy(&table,value + first + 8 * i,8);
            if (!table || !hao_probe_read8(table,&conformance) || !conformance) return WRONG(r);   /* no conformance */
            guest_conformance |= !guest_protocols && (hao_image_for_addr(table) || hao_image_for_addr(conformance));
        }
        int ok = existential_payload(r,type,value,base,out,capacity,used,depth);
        return guest_conformance ? 0 : ok;   /* not certified, not known wrong */
    }
    if (kind == 0x303) {   /* a guest call of a native witness (r->existentials) */
        uint32_t flags; memcpy(&flags,(const char *)type + 8,4);
        if ((flags & 0x3f000000u) == 0x01000000u) { r->passes |= HAO_SV_PASS_OWNED; return 1; }   /* Error: a runtime box */
        if (flags & 0x3f000000u) return 0;
        return existential_payload(r,type,value,base,out,capacity,used,depth);
    }
    if (kind != 0x200 && kind != 0x301) return other_kind(r,type,kind);
    intptr_t count = known_fields(r,type);
    if (count < 0) { r->unknown += r->looking != 0; return 0; }   /* fields not known */
    for (intptr_t i = 0; i < count; ++i) {
        field_info info = {0};
        const void *child = r->field(type, i, &info);
        intptr_t at = r->offset(type, i);
        if (info.free_name) info.free_name(info.name);
        if (!child || at < 0 || (uint64_t)at > v->size || vwt(child)->size > v->size - (uint64_t)at) return WRONG(r);
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
    r->surveyed++;
    if (r->survey == 3 && r->naudit < r->naudit_max) {
        audit_entry *e = &r->audit[r->naudit++];
        e->type = type; e->offset = base; e->depth = depth;
        for (unsigned k = 0; k < depth && k < HAO_SV_AUDIT_CHAIN; ++k) e->chain[k] = r->chain[k];
    }
    if (r->survey != 1) return;
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
}

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    /* Inside an existential, a guest type its own (guest) witnesses handle
     * whole: the guest's encoding is read by the code that wrote it. */
    if (r->boxed && guest_witnessed(r,type)) { r->passes |= HAO_SV_PASS_OWNED; return 1; }
    /* Also at a guest call of a native witness (r->existentials): the native
     * witness hands such a value whole to its own guest witnesses, as
     * swiftvalue.c guest_ranges takes it. */
    if (r->existentials && guest_witnessed(r,type)) { r->passes |= HAO_SV_PASS_OWNED; return 1; }
    int opaque = !r->opaque && native_opaque(r,type);
    r->opaque += (unsigned)opaque;
    unsigned before = r->surveyed, first = *used;
    if (depth < HAO_SV_AUDIT_CHAIN) r->chain[depth] = type;
    int ok = collect_inner(r,type,value,base,out,capacity,used,depth);
    r->opaque -= (unsigned)opaque;
    if (r->survey && (!ok || r->surveyed != before)) {
        if (!ok && r->surveyed == before) {
            if (!r->unresolved) r->unresolved = type;
            *used = first;  /* nothing inside a refusing type is patched: its layout is not known */
            survey(r,type,base,depth);
        }
        return 1;           /* keep walking; collect_patches refuses the value */
    }
    if (!ok && !r->unresolved) r->unresolved = type;
    return ok;
}

static int collect_patches(const void *type, const void *value, empty_patch *out,
                           unsigned capacity, const void **unresolved, int existentials,
                           int (*known_layout)(const void *), int survey_mode, unsigned *passes)
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
    r.source = existentials & (HAO_SV_SOURCE_GUEST | HAO_SV_SOURCE_NATIVE);
    r.existentials = existentials & 1;
    unsigned used = 0;
    if (passes) *passes = 0;
    if (unresolved) *unresolved = type;
    if (!type || !value || !out || !r.kind || !r.count || !r.offset || !r.field || !r.array || !r.optional) return -1;
    if (!collect(&r, type, value, 0, out, capacity, &used, 0) || (r.surveyed && survey_mode != 2)) {
        if (unresolved && r.unresolved) *unresolved = r.unresolved;
        if (passes) *passes = r.passes & (HAO_SV_UNCONVERTIBLE | HAO_SV_KNOWN_WRONG);
        return -1;
    }
    if (unresolved) *unresolved = NULL;
    if (passes) *passes = r.passes;
    return (int)used;
}

int collect_empty_patches_detail(const void *type, const void *value, empty_patch *out,
                                 unsigned capacity, const void **unresolved)
{
    return collect_patches(type,value,out,capacity,unresolved,0,NULL,0,NULL);
}

int collect_value_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                          const void **unresolved, int existentials, int (*known_layout)(const void *),
                          unsigned *passes)
{
    return collect_patches(type,value,out,capacity,unresolved,existentials,known_layout,0,passes);
}

int collect_survey_patches(const void *type, const void *value, empty_patch *out,
                           unsigned capacity, const void **unresolved)
{
    return collect_patches(type,value,out,capacity,unresolved,0,NULL,1,NULL);
}

int collect_partial_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                            int existentials, int (*known_layout)(const void *), unsigned *passes)
{
    return collect_patches(type,value,out,capacity,NULL,existentials,known_layout,2,passes);
}

int collect_audit_patches(const void *type, const void *value, empty_patch *out, unsigned capacity, int existentials,
                          audit_entry *entries, unsigned max, unsigned *n)
{
    reflection r = {
        HAO_DLSYM("swift_getMetadataKind"),
        HAO_DLSYM("swift_reflectionMirror_recursiveCount"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset"),
        HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata"),
        HAO_DLSYM("$sSaMn"), HAO_DLSYM("$sSqMn"), HAO_DLSYM("$sSSN"), NULL, existentials,
        NULL, 0, 3,
    };
    nominals(&r);
    r.source = existentials & (HAO_SV_SOURCE_GUEST | HAO_SV_SOURCE_NATIVE);
    r.existentials = existentials & 1;
    r.audit = entries; r.naudit_max = max;
    unsigned used = 0;
    *n = 0;
    if (!type || !value || !out || !r.kind || !r.count || !r.offset || !r.field || !r.array || !r.optional) return -1;
    (void)collect(&r, type, value, 0, out, capacity, &used, 0);
    *n = r.naudit;
    return (int)used;
}

int collect_witness_patches(const void *type, const void *value, empty_patch *out, unsigned capacity)
{
    return collect_patches(type,value,out,capacity,NULL,1,NULL,0,NULL);
}

int collect_inline_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                           int (*known_layout)(const void *))
{
    return collect_patches(type,value,out,capacity,NULL,1,known_layout,0,NULL);
}

int collect_empty_patches(const void *type, const void *value, empty_patch *out, unsigned capacity)
{
    return collect_empty_patches_detail(type,value,out,capacity,NULL);
}
