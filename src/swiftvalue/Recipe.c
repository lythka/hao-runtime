/* Bounded representation policy, operating on trusted initialized native
 * metadata supplied by the admitted callback or fixture. No arbitrary memory probing.
 * ABI: Metadata.h, ValueWitnessTable.h, ReflectionMirror.cpp, Reflection.h.
 * The selected XI field is the first with maximum XI count. An Array of an
 * enum has native-only storage (ArrayBuffer.swift); BridgeObject alone does
 * not establish that invariant. */
#include "Recipe.h"
#include "Common.h"
#include "Enum.h"
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
} reflection;

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
    if (depth > 16) return 0;
    uintptr_t kind = r->kind(type);
    if (kind != 0x200 && kind != 0x202 && kind != 0x301 && kind != 0x305 &&
        ((kind != 0x303 && kind != 0) || !r->existentials)) return 0;
    const void *const *m = type;
    const witnesses *v = vwt(type);
    if (v->size < 8 || v->size > 4096) return 0;
    /* Existentials and Swift class references, only for guest calls of
     * native value witnesses (the value stays where it is; see
     * collect_witness_patches). Metadata.h
     * TargetExistentialTypeMetadata: after the kind a flags word (witness
     * tables in bits 0-23, special protocol in 24-29, superclass bit 30,
     * bit 31 set when not class-bound). The opaque container is three buffer
     * words, the type metadata word, the tables; the class-bound one the
     * object word, then the tables. That metadata or object word carries the
     * extra inhabitants: 0 first, then 1 on arm64 and 2 on x86_64, measured
     * for `(any P)??`, `(any Q: AnyObject)??` and `AnyObject??` in native
     * programs of both. The Error box (special protocol 1) and any other
     * special representation stay out. */
    /* A Swift class reference (MetadataKind::Class, its table Builtin.NativeObject's,
     * native): the same inhabitants, measured for `C??` (1 arm64, 2 x86_64). */
    if (kind == 0) {
        if (v->size != 8 || v->stride != 8 || (v->flags & 255) != 7 || v->extra < 2) return 0;
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
    if (kind == 0x200 && m[1] == r->array) {
        /* Only non-Optional enum Elements are admitted by this policy.
         * A class/ObjC existential Array must not inherit this proof. */
        /* The guest-call path rewrites only the buffer word's outer nil,
         * whose inhabitants are the BridgeObject's for any Element. */
        if ((r->kind(m[2]) != 0x201 && !r->existentials) || v->size != 8 || v->stride != 8 ||
            v->extra != 0x7fffffff || (v->flags & 255) != 7) return 0;
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
        dlsym(RTLD_DEFAULT, "swift_getMetadataKind"),
        dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveCount"),
        dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveChildOffset"),
        dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveChildMetadata"),
        dlsym(RTLD_DEFAULT, "$sSaMn"), dlsym(RTLD_DEFAULT, "$sSqMn"), dlsym(RTLD_DEFAULT, "$sSSN"), NULL, existentials,
    };
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

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth);
static int collect_inner(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    if (depth > 32) return 0;
    uintptr_t kind = r->kind(type);
    const witnesses *v = vwt(type);
    if (v->size > 4096) return 0;
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
            out[(*used)++] = (empty_patch){base + plan.offset, word, receiver, plan.consumed};
            return 1;                     /* inactive payload must remain opaque */
        }
        /* This is a present payload in either admitted representation. */
        return collect(r, ((const void *const *)type)[2], value, base,
                       out, capacity, used, depth + 1);
    }
    if (kind == 0x201) {
        if (common_leaf_value(type,value)) return 1;
        enum_case_plan active;
        if (!discover_enum_case(type,value,&active)) return 0;
        return !active.payload || collect(r,active.payload,value,base,out,capacity,used,depth + 1);
    }
    if (kind == 0x305) return common_objc_reference_value(type,value);
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

static int collect(reflection *r, const void *type, const unsigned char *value,
                   uint64_t base, empty_patch *out, unsigned capacity, unsigned *used, unsigned depth)
{
    int ok = collect_inner(r,type,value,base,out,capacity,used,depth);
    if (!ok && !r->unresolved) r->unresolved = type;
    return ok;
}

static int collect_patches(const void *type, const void *value, empty_patch *out,
                           unsigned capacity, const void **unresolved, int existentials)
{
    reflection r = {
        dlsym(RTLD_DEFAULT, "swift_getMetadataKind"),
        dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveCount"),
        dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveChildOffset"),
        dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveChildMetadata"),
        dlsym(RTLD_DEFAULT, "$sSaMn"), dlsym(RTLD_DEFAULT, "$sSqMn"), dlsym(RTLD_DEFAULT, "$sSSN"), NULL, existentials,
    };
    unsigned used = 0;
    if (unresolved) *unresolved = type;
    if (!type || !value || !out || !r.kind || !r.count || !r.offset || !r.field || !r.array || !r.optional) return -1;
    if (!collect(&r, type, value, 0, out, capacity, &used, 0)) {
        if (unresolved && r.unresolved) *unresolved = r.unresolved;
        return -1;
    }
    if (unresolved) *unresolved = NULL;
    return (int)used;
}

int collect_empty_patches_detail(const void *type, const void *value, empty_patch *out,
                                 unsigned capacity, const void **unresolved)
{
    return collect_patches(type,value,out,capacity,unresolved,0);
}

int collect_witness_patches(const void *type, const void *value, empty_patch *out, unsigned capacity)
{
    return collect_patches(type,value,out,capacity,NULL,1);
}

int collect_empty_patches(const void *type, const void *value, empty_patch *out, unsigned capacity)
{
    return collect_empty_patches_detail(type,value,out,capacity,NULL);
}
