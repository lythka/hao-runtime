#include "Common.h"
#include "Enum.h"
#ifdef HAO_SWIFT_VALUE_RUNTIME
#include "NativeLeaf.h"
#endif
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifdef HAO_SWIFT_VALUE_RUNTIME
#include "hao/macho.h"
#endif
#define SWIFT __attribute__((swiftcall))
typedef struct { void *fn[8]; uint64_t size, stride; uint32_t flags, extra; } witnesses;
typedef struct { const char *name; void (*free_name)(const char *); bool strong, var; } field_info;
typedef uintptr_t SWIFT kind_fn(const void *);
typedef intptr_t SWIFT count_fn(const void *);
typedef intptr_t SWIFT offset_fn(const void *, intptr_t);
typedef const void *SWIFT field_fn(const void *, intptr_t, field_info *);
typedef const void *SWIFT resolve_fn(const char *, size_t, const void *, const void *const *);
typedef unsigned SWIFT tag_fn(const void *, const void *);
typedef unsigned SWIFT single_tag_fn(const void *, unsigned, const void *);
static uint32_t u32(const void *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint64_t u64(const void *p) { uint64_t n; memcpy(&n,p,8); return n; }
static const char *relative(const void *p) { int32_t n; memcpy(&n,p,4); return n ? (const char *)p + n : NULL; }
static const witnesses *vwt(const void *m) { return ((const witnesses *const *)m)[-1]; }

/* Int, Float and Double use every payload bit pattern, including NaNs. Their
 * first Optional therefore has a separate trailing tag byte on both targets
 * (Enum.cpp, swift_initEnumMetadataSinglePayload). Ordinary one-field wrappers
 * with identical storage preserve that rule; no pointer spare-bit proof is
 * inherited. This includes CGFloat's native Double field on 64-bit targets. */
static bool scalar_shape(const void *m, unsigned depth)
{
    if (depth > 8) return false;
    const witnesses *v = vwt(m);
    if (v->extra || (v->flags & 0x01910000) ||
        (v->size != 4 && v->size != 8) || v->stride != v->size ||
        (v->flags & 255) != v->size - 1) return false;
    if (m == dlsym(RTLD_DEFAULT,"$sSiN") || m == dlsym(RTLD_DEFAULT,"$sSfN") ||
        m == dlsym(RTLD_DEFAULT,"$sSdN")) return true;
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    count_fn *count = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount");
    offset_fn *offset = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata");
    if (!kind || !count || !offset || !field || kind(m) != 0x200 || count(m) != 1 || offset(m,0)) return false;
    field_info info = {0};
    const void *child = field(m,0,&info);
    if (info.free_name) info.free_name(info.name);
    return child && vwt(child)->size == v->size && scalar_shape(child,depth + 1);
}

/* First Optional over exactly one strong Swift reference, possibly wrapped
 * by single-field structs. Weak/unknown/ObjC references and another Optional
 * are separate contracts. The first consumed XI is null in both ABIs. */
static bool reference_shape(const void *m, unsigned depth)
{
    if (depth > 8) return false;
    kind_fn *kind = dlsym(RTLD_DEFAULT, "swift_getMetadataKind");
    const witnesses *v = vwt(m);
    if (v->size != 8 || v->stride != 8 || v->extra != 0x7fffffff ||
        (v->flags & 255) != 7 || (v->flags & 0x01900000)) return false;
    uintptr_t k = kind(m);
    if (k == 0) {
        /* ObjC-compatible ClassMetadata: Data at32 identifies Swift type
         * metadata (old/new Swift bit); ClassFlags at40 selects Swift RC. */
        return (u64((const char *)m + 32) & 3) && (u32((const char *)m + 40) & 2);
    }
    if (k != 0x200) return false;
    count_fn *count = dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveCount");
    offset_fn *offset = dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = dlsym(RTLD_DEFAULT, "swift_reflectionMirror_recursiveChildMetadata");
    if (!count || !offset || !field || count(m) != 1 || offset(m,0) != 0) return false;
    field_info info = {0};
    const void *child = field(m,0,&info);
    if (info.free_name) info.free_name(info.name);
    return child && info.strong && reference_shape(child, depth + 1);
}

/* Imported Objective-C classes have runtime wrapper metadata and the exact
 * ObjCRetainableBox witnesses (Metadata.cpp, MetadataImpl.h). First Optional
 * consumes only null on both Darwin64 targets. Live references already belong
 * to the shared native ObjC domain, including its tagged pointers: preserve
 * their bits and let their existing witnesses retain/release them. This helper
 * does not itself adapt Optional tags or admit weak storage, existential or
 * ForeignClass metadata.
 * The witness checks XI bits without dereferencing/retaining the object. */
int common_objc_reference_type(const void *m)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    if (!m || !kind || kind(m) != 0x305) return 0;
    const witnesses *v = vwt(m);
    if (!((const void *const *)m)[1] ||
        v != dlsym(RTLD_DEFAULT,"$sBOWV") || v->size != 8 || v->stride != 8 ||
        v->flags != 0x10007 || v->extra != 0x7fffffff || !v->fn[6]) return false;
    return 1;
}

/* AnyObject: the class-bound existential with no protocol and no witness
 * table (ExistentialTypeMetadata flags 0, zero protocols), one object word
 * with the Builtin.UnknownObject witnesses. Measured identical on arm64 and
 * x86_64 (size 8, flags 0x10007, xi 0x7fffffff; its first Optional: size 8,
 * flags 0x210007, nil 0). A live value is any object of the shared native
 * domain (Swift or ObjC, tagged pointers included), as for an imported ObjC
 * class: the same value rule applies. */
static int anyobject_type(const void *m)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    if (!m || !kind || kind(m) != 0x303 || u32((const char *)m + 8) || u32((const char *)m + 12)) return 0;
    const witnesses *v = vwt(m);
    return v == dlsym(RTLD_DEFAULT,"$sBOWV") && v->size == 8 && v->stride == 8 &&
           v->flags == 0x10007 && v->extra == 0x7fffffff && v->fn[6];
}

static int unknown_object_value(const void *m, const void *value)
{
    const witnesses *v = vwt(m);
    uint64_t bits = u64(value);
    if (!bits) return true;
    if (bits <= 2) return false;
    return ((single_tag_fn *)v->fn[6])(value,1,m) == 0;
}

/* The case count of a one-byte enum with no payload case (its own leaf rule
 * in common_leaf_value), else 0. */
static unsigned payload_free_enum_cases(const void *m)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    if (!m || !kind || kind(m) != 0x201) return 0;
    const char *desc = ((const void *const *)m)[1];
    if (!desc || (u32(desc) & 31) != 18 || (u32(desc + 20) & 0xffffff)) return 0;
    unsigned cases = u32(desc + 24);
    const witnesses *v = vwt(m);
    if (!cases || cases > 256 || v->size != 1 || v->stride != 1 || (v->flags & 0x100ff) ||
        v->extra != 256 - cases) return 0;
    return cases;
}

int common_objc_reference_value(const void *m, const void *value)
{
    if (!common_objc_reference_type(m)) return 0;
    const witnesses *v = vwt(m);
    uint64_t bits = u64(value);
    if (!bits) return true;
    if (bits <= 2) return false;
    return ((single_tag_fn *)v->fn[6])(value,1,m) == 0;
}

/* One-witness class-constrained Swift existential: object/weak-reference
 * word followed by an inactive witness word for nil (ExistentialContainer.h,
 * ExistentialMetadataImpl.h). The first Optional consumes XI0, null on both
 * Darwin64 targets. Reflection reports weak fields as their referent type;
 * a zero WeakReference is also nil, but nonzero weak storage is not a strong
 * object pointer. Admit only nil and never inspect the inactive witness word.
 * Error, extended/composed/superclass existentials and nested Optionals need
 * independent contracts. */
static bool one_witness_class_existential(const void *m)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    if (!m || !kind || kind(m) != 0x303) return false;
    const witnesses *v = vwt(m);
    if (v->size != 16 || v->stride != 16 || v->flags != 0x10007 ||
        v->extra != 0x7fffffff || u32((const char *)m + 8) != 1 ||
        u32((const char *)m + 12) != 1 || !v->fn[6]) return false;
    const char *protocol = ((const char *const *)m)[2];
    if (!protocol || ((uintptr_t)protocol & 1)) return false; /* Swift, not ObjC */
    uint32_t flags = u32(protocol);
    return (flags & 31) == 3 && !(flags & (1u << 16));
}

/* A struct whose only field, at offset 0 and of the struct's size, is such an
 * existential (Foundation.URL: `_url: any _URLProtocol`, measured identical in
 * native arm64 and x86_64 programs: struct size 16, xi 0x7fffffff; its
 * Optional flags 0x2210007, nil = a zero object word). A struct is never weak
 * storage, so its nonzero object word is a strong reference. */
static const void *existential_wrapper(const void *m)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    count_fn *count = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount");
    offset_fn *offset = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata");
    if (!m || !kind || !count || !offset || !field || kind(m) != 0x200 || count(m) != 1 || offset(m,0) != 0) return NULL;
    field_info info = {0};
    const void *child = field(m,0,&info);
    if (info.free_name) info.free_name(info.name);
    if (!child || !info.strong || !one_witness_class_existential(child)) return NULL;
    const witnesses *v = vwt(m);
    return v->size == 16 && v->stride == 16 && (v->flags & ~0x2000000u) == 0x10007 && v->extra == 0x7fffffff ? child : NULL;
}

/* The first Optional of either consumes XI0, a null object word, on both
 * Darwin64 targets; nil never inspects the inactive witness word. A live
 * value is admitted only in the struct wrapper (a bare existential field may
 * be weak), only in the runtime, and only when its witness table is native
 * (in no guest image): a guest conformance's table is arm64 code. Error,
 * extended/composed/superclass existentials and nested Optionals need
 * independent contracts. */
static bool class_existential_value(const void *m, const void *value)
{
    const void *wrapped = existential_wrapper(m);
    if (!wrapped && !one_witness_class_existential(m)) return false;
    if (!u64(value)) return true;
#ifdef HAO_SWIFT_VALUE_RUNTIME
    uint64_t table = u64((const char *)value + 8);
    if (!wrapped || !table || hao_image_for_addr(table)) return false;
    return ((single_tag_fn *)vwt(wrapped)->fn[6])(value,1,wrapped) == 0;
#else
    return false;
#endif
}

static bool no_generic_context(const char *desc)
{
    for (unsigned depth = 0; desc && depth < 16; depth++) {
        if (u32(desc) & 0x80) return false;
        const char *parent = relative(desc + 4);
        if ((uintptr_t)parent & 1) return false;
        desc = parent;
        if (!desc) return true;
    }
    return false;
}

/* Registered reflection data supplies a compiler-emitted no-payload-spare-
 * bits statement. Match its exact symbolic typeref to the field descriptor;
 * never select a record merely because its numeric layout resembles this one. */
static bool no_payload_spare_bits(const char *desc, const char *type_name,
                                  uint32_t size, uint32_t stride, uint32_t extra)
{
    Dl_info info = {0};
    if (!dladdr(desc,&info) || !info.dli_fbase || !type_name) return false;
    unsigned long bytes = 0;
    const unsigned char *p = getsectiondata(info.dli_fbase,"__TEXT","__swift5_mpenum",&bytes);
    if (!p || bytes > (1u << 24)) return false;
    size_t at = 0;
    bool found = false;
    while (at < bytes) {
        if (bytes - at < 8) return false;
        uint32_t sf = u32(p + at + 4), words = sf >> 16;
        size_t size = 4 + (size_t)words * 4;
        if (!words || size > bytes - at) return false;
        if (relative(p + at) == type_name) {
            if (found || sf != 0x10000) return false;
            found = true;
        }
        at += size;
    }
    if (!found) return false;
    p = getsectiondata(info.dli_fbase,"__TEXT","__swift5_builtin",&bytes);
    if (!p || bytes > (1u << 24) || bytes % 20) return false;
    found = false;
    for (at = 0; at < bytes; at += 20) {
        if (relative(p + at) != type_name) continue;
        if (found || u32(p + at + 4) != size || u32(p + at + 8) != 0x10008 ||
            u32(p + at + 12) != stride || u32(p + at + 16) != extra) return false;
        found = true;
    }
    return found;
}

static const void *field_type(const char *record, const char *desc)
{
    if (u32(record)) return NULL; /* direct ordinary case, no unknown flags */
    const char *name = relative(record + 4);
    if (!name) return NULL;
    size_t n = 0;
    while (n < 4096 && name[n]) {
        unsigned c = (unsigned char)name[n++];
        if (c >= 1 && c <= 0x17) n += 4;
        else if (c >= 0x18 && c <= 0x1f) n += sizeof(void *);
    }
    resolve_fn *resolve = dlsym(RTLD_DEFAULT,"swift_getTypeByMangledNameInContext2");
    return n < 4096 && resolve ? resolve(name,n,desc,NULL) : NULL;
}

/* A separate XI class from the Array-pointer empty_plan: fixed multipayload
 * enums count their trailing-byte XIs down from ff on both targets. A native
 * reflection record alone cannot prove the guest layout; require a direct,
 * full-extent payload with no spare bits in either Darwin64 ABI. Array has
 * frozen BridgeObject storage (GenObjC.cpp gives it zero fixed spare bits).
 * This checks metadata only, never the inactive payload's object word. */
typedef struct {
    kind_fn *kind;
    count_fn *count;
    offset_fn *offset;
    field_fn *field;
    const void *array, *optional, *integer, *real, *float32, *uint32;
} byte_xi_reflection;
typedef struct { uint64_t offset; unsigned consumed; } byte_xi_plan;

/* Runtime-sized direct enums use the same trailing-byte XI sequence in both
 * Darwin ABIs. The existing enum classifier checks the canonical runtime tag
 * witness, exact payload extent and direct case descriptors before this path
 * may use that byte. Payload ownership is still checked when the value is live. */
static bool runtime_byte_enum(const void *type, uint64_t *offset)
{
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    uint64_t extent;
    if (!enum_payloads(type,payloads,&extent)) return false;
    *offset = extent;
    return true;
}

static bool zero_spare_word(byte_xi_reflection *r, const void *type, unsigned depth)
{
    if (!type || depth > 8) return false;
    const witnesses *v = vwt(type);
    if (v->size != 8 || v->stride != 8 || (v->flags & 255) != 7 ||
        (v->flags & 0x01d00000)) return false;
    if (type == r->integer || type == r->real)
        return !v->extra && !(v->flags & 0x10000);
    const void *const *m = type;
    if (r->kind(type) != 0x200) return false;
    if (m[1] == r->array) return v->extra == 0x7fffffff;
    if (!m[1] || (u32(m[1]) & 31) != 17 || r->count(type) != 1 ||
        r->offset(type,0) != 0) return false;
    field_info info = {0};
    const void *child = r->field(type,0,&info);
    if (info.free_name) info.free_name(info.name);
    return child && info.strong && vwt(child)->extra == v->extra &&
           zero_spare_word(r,child,depth + 1);
}

static bool fixed_byte_enum(byte_xi_reflection *r, const void *type)
{
    const witnesses *v = vwt(type);
    const char *desc = ((const char *const *)type)[1];
    if (!desc || (u32(desc) & 31) != 18 || !no_generic_context(desc) ||
        v->size != 9 || v->stride != 16 || (v->flags & 255) != 7 ||
        (v->flags & 0x01d00000)) return false;
    /* A nonzero high byte is a runtime payload-size metadata offset, outside
     * this fixed-layout rule. Twelve empty cases, for example, share one tag
     * and use the eight-byte payload area to distinguish their indices. */
    uint32_t payloads = u32(desc + 20), empties = u32(desc + 24);
    if (payloads < 2 || payloads > 64 || empties > 64 - payloads ||
        v->extra != 256 - payloads - !!empties) return false;
    const char *fields = relative(desc + 16);
    if (!fields || u32(fields + 8) != (12u << 16 | 3) ||
        u32(fields + 12) != payloads + empties ||
        !no_payload_spare_bits(desc,relative(fields),9,16,v->extra)) return false;
    bool anchor = false;
    for (uint32_t i = 0; i < payloads + empties; i++) {
        const char *record = fields + 16 + i * 12;
        uint32_t flags = u32(record);
        if (i >= payloads) {
            if (flags || relative(record + 4)) return false;
            continue;
        }
        if (!relative(record + 4)) return false;
        if (flags == 1) continue; /* indirect payload lowers to an eight-byte box */
        if (flags) return false;
        const void *child = field_type(record,desc);
        if (!child) return false;
        const witnesses *cv = vwt(child);
        if (cv->size != 8 || cv->stride != 8 || (cv->flags & 255) != 7 ||
            (cv->flags & 0x01d00000)) return false;
        bool zero = zero_spare_word(r,child,0);
        if (!zero && r->kind(child) != 0) return false;
        anchor |= zero;
    }
    return anchor;
}

/* The native winner is not enough: an unrelated sibling enum could have
 * more XIs on ARM and change the selected field. Certify every sibling's XI
 * count, retaining the caller's ordinary matching field-layout contract. */
static bool stable_byte_xi_count(byte_xi_reflection *r, const void *type, unsigned depth)
{
    if (!type || depth > 16) return false;
    const witnesses *v = vwt(type);
    if (v->size > 4096 || (v->flags & 0x01d00000)) return false;
    unsigned scalar_size = type == r->integer || type == r->real ? 8 :
                           type == r->float32 || type == r->uint32 ? 4 : 0;
    if (scalar_size)
        return v->size == scalar_size && v->stride == scalar_size &&
               (v->flags & 255) == scalar_size - 1 &&
               !(v->flags & 0x10000) && !v->extra;
    uintptr_t k = r->kind(type);
    const void *const *m = type;
    if (k == 0x201) {
        const char *desc = m[1];
        if (!desc || (u32(desc) & 31) != 18) return false;
        uint32_t cases = u32(desc + 24);
        if (!u32(desc + 20) && cases && cases <= 256 &&
            no_generic_context(desc) && v->size == 1 && v->stride == 1 &&
            !(v->flags & 0x100ff) && v->extra == 256 - cases) return true;
        uint64_t offset;
        return fixed_byte_enum(r,type) || runtime_byte_enum(type,&offset);
    }
    if (k == 0x202) {
        if (m[1] != r->optional || !m[2] ||
            !stable_byte_xi_count(r,m[2],depth + 1)) return false;
        const witnesses *pv = vwt(m[2]);
        uint64_t size = pv->size + !pv->extra, align_mask = pv->flags & 255;
        return v->size == size && (v->flags & 255) == align_mask &&
               v->stride == ((size + align_mask) & ~align_mask) &&
               v->extra == (pv->extra ? pv->extra - 1 : 0);
    }
    if (k != 0x200 && k != 0x301) return false;
    if (k == 0x200 && (!m[1] || (u32(m[1]) & 31) != 17)) return false;
    intptr_t count = r->count(type);
    if (count < 0 || count > 64) return false;
    uint32_t extra = 0;
    for (intptr_t i = 0; i < count; i++) {
        field_info info = {0};
        const void *child = r->field(type,i,&info);
        intptr_t at = r->offset(type,i);
        if (info.free_name) info.free_name(info.name);
        if (!child || !info.strong || at < 0 || (uint64_t)at > v->size ||
            vwt(child)->size > v->size - (uint64_t)at ||
            !stable_byte_xi_count(r,child,depth + 1)) return false;
        if (vwt(child)->extra > extra) extra = vwt(child)->extra;
    }
    return extra == v->extra;
}

/* Follow only the first maximum-XI field, including Optional reductions.
 * No sibling value bytes are read: an outer nil may leave them arbitrary. */
static bool selected_byte_xi(byte_xi_reflection *r, const void *type,
                             byte_xi_plan *out, unsigned depth)
{
    if (!type || depth > 16) return false;
    const witnesses *v = vwt(type);
    if (v->size < 9 || v->size > 4096 || !v->extra ||
        (v->flags & 0x01d00000)) return false;
    uintptr_t k = r->kind(type);
    const void *const *m = type;
    if (k == 0x201) {
        uint64_t offset = 8;
        if (!fixed_byte_enum(r,type) && !runtime_byte_enum(type,&offset)) return false;
        *out = (byte_xi_plan){offset,0};
        return true;
    }
    if (k == 0x202) {
        if (m[1] != r->optional || !m[2]) return false;
        const witnesses *pv = vwt(m[2]);
        byte_xi_plan child;
        if (!pv->extra || pv->extra - 1 != v->extra || pv->size != v->size ||
            pv->stride != v->stride || (pv->flags & 255) != (v->flags & 255) ||
            !selected_byte_xi(r,m[2],&child,depth + 1)) return false;
        *out = child;
        out->consumed++;
        return true;
    }
    if (k != 0x200 && k != 0x301) return false;
    if (k == 0x200 && (!m[1] || (u32(m[1]) & 31) != 17)) return false;
    intptr_t count = r->count(type);
    if (count < 1 || count > 64) return false;
    const void *best = NULL;
    uint32_t extra = 0;
    uint64_t offset = 0;
    for (intptr_t i = 0; i < count; i++) {
        field_info info = {0};
        const void *child = r->field(type,i,&info);
        intptr_t at = r->offset(type,i);
        if (info.free_name) info.free_name(info.name);
        if (!child || !info.strong || at < 0 || (uint64_t)at > v->size ||
            vwt(child)->size > v->size - (uint64_t)at) return false;
        if (!stable_byte_xi_count(r,child,depth + 1)) return false;
        if (vwt(child)->extra > extra) {
            best = child; extra = vwt(child)->extra; offset = (uint64_t)at;
        }
    }
    byte_xi_plan child;
    if (!best || extra != v->extra || !selected_byte_xi(r,best,&child,depth + 1) ||
        child.offset >= vwt(best)->size || child.offset >= v->size - offset) return false;
    *out = (byte_xi_plan){offset + child.offset,child.consumed};
    return true;
}

int common_byte_optional_case(const void *type, const void *value)
{
    byte_xi_reflection r = {
        dlsym(RTLD_DEFAULT,"swift_getMetadataKind"),
        dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount"),
        dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset"),
        dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata"),
        dlsym(RTLD_DEFAULT,"$sSaMn"), dlsym(RTLD_DEFAULT,"$sSqMn"),
        dlsym(RTLD_DEFAULT,"$sSiN"), dlsym(RTLD_DEFAULT,"$sSdN"),
        dlsym(RTLD_DEFAULT,"$sSfN"), dlsym(RTLD_DEFAULT,"$ss6UInt32VN"),
    };
    byte_xi_plan plan;
    if (!type || !value || !(r.kind && r.count && r.offset && r.field && r.array && r.optional &&
           r.integer && r.real && r.float32 && r.uint32 &&
           r.kind(type) == 0x202 && ((const void *const *)type)[1] == r.optional &&
           selected_byte_xi(&r,type,&plan,0) &&
           plan.consumed && plan.consumed <= 255)) return -1;
    return ((const unsigned char *)value)[plan.offset] == 256 - plan.consumed;
}

static bool string_reference_enum(const void *m, const void *value, const char *desc)
{
    const witnesses *v = vwt(m);
    if (!no_generic_context(desc) || u32(desc + 20) != 2 || u32(desc + 24) ||
        v->size != 17 || v->stride != 24 || (v->flags & 255) != 7 || v->extra != 254) return false;
    const char *fields = relative(desc + 16);
    if (!fields || u32(fields + 8) != (12u << 16 | 3) || u32(fields + 12) != 2 ||
        !no_payload_spare_bits(desc,relative(fields),17,24,254)) return false;
    const void *string = dlsym(RTLD_DEFAULT,"$sSSN");
    if (!string || vwt(string)->size != 16 || vwt(string)->stride != 16 || (vwt(string)->flags & 255) != 7) return false;
    const void *a = field_type(fields + 16, desc), *b = field_type(fields + 28, desc);
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    bool layout = (a == string && b && kind(b) == 0 && reference_shape(b,0)) ||
                  (b == string && a && kind(a) == 0 && reference_shape(a,0));
    return layout && ((const unsigned char *)value)[16] < 2;
}

static bool compact_strong_enum(const void *m, const void *value, const char *desc)
{
    const witnesses *v = vwt(m);
    void *const *slots = (void *const *)v;
    if (u32(desc + 20) != 2 || u32(desc + 24) != 4 || v->size != 8 || v->stride != 8 ||
        (v->flags & 255) != 7 || slots[1] != dlsym(RTLD_DEFAULT,"swift_cvw_destroy") ||
        slots[2] != dlsym(RTLD_DEFAULT,"swift_cvw_initWithCopy") ||
        slots[11] != dlsym(RTLD_DEFAULT,"swift_cvw_enumFn_getEnumTag")) return false;
    const char *p = ((const char *const *)m)[-2];
    if (!p || u64(p + 16) != (UINT64_C(0x14) << 56) || u64(p + 32) != 2 ||
        u64(p + 40) != 32 || u64(p + 48) != 8 || u64(p + 56) != 0 || u64(p + 64) != 16 ||
        u64(p + 72) != (UINT64_C(2) << 56) || u64(p + 80) ||
        u64(p + 88) != (UINT64_C(2) << 56) || u64(p + 96) || u64(p + 104)) return false;
    /* Callable ownership/tag callbacks and native-pointer-domain live fields
     * are explicit caller preconditions, independently tested by
     * swift-compact-boundary. No native static enum interpretation is claimed. */
    return ((tag_fn *)slots[11])(value,m) < 6;
}

/* A Clang-imported enum defined in the guest has the guest integer layout.
 * Foreign metadata initialization plus __C identifies the compiler's C enum
 * strategy; exact canonical integer witnesses prove POD width/alignment. No
 * foreign frozen nominal declaration is inferred from native reflection. */
static bool guest_c_integer_enum(const void *type)
{
#ifdef HAO_SWIFT_VALUE_RUNTIME
    const void *const *m = type;
    const char *desc = m[1];
    if ((uintptr_t)m[0] != 0x201 || !desc || !hao_image_for_addr((uintptr_t)desc) ||
        (u32(desc) & 0x3009f) != 0x20012 || u32(desc + 16) || u32(desc + 20)) return false;
    const char *parent = relative(desc + 4);
    if (!parent || ((uintptr_t)parent & 1) || (u32(parent) & 31)) return false;
    const char *module = relative(parent + 8);
    if (!module || strcmp(module,"__C")) return false;
    const witnesses *v = vwt(type);
    const char *symbols[] = {"$ss4Int8VN","$ss5Int16VN","$ss5Int32VN","$sSiN"};
    for (unsigned i = 0; i < 4; ++i) {
        const void *integer = dlsym(RTLD_DEFAULT,symbols[i]);
        if (integer && v == vwt(integer) && v->size == (1u << i) &&
            v->stride == v->size && v->flags == v->size - 1 && !v->extra) return true;
    }
#else
    (void)type;
#endif
    return false;
}

/* Canonical scalars, their Optional trailing tags and structural tuples.
 * Do not infer a foreign nominal wrapper's guest layout from native metadata.
 * CGFloat's Darwin64 Double identity is separately proved against both
 * original cache slices and the SDK; require its exact metadata and VWT. */
static bool scalar_record(const void *type, const unsigned char *value, unsigned depth)
{
    if (!type || depth > 8) return false;
    const witnesses *v = vwt(type);
    if (!v->size || v->size > 64 || v->extra || (v->flags & 0x01910000)) return false;
    const void *integer = dlsym(RTLD_DEFAULT,"$sSiN");
    const void *real = dlsym(RTLD_DEFAULT,"$sSdN");
    const void *floating = dlsym(RTLD_DEFAULT,"$sSfN");
    unsigned bytes = type == floating ? 4 : type == integer || type == real ? 8 : 0;
    const void *cgfloat = dlsym(RTLD_DEFAULT,"$s12CoreGraphics7CGFloatVN");
    if (cgfloat && type == cgfloat && real && v == vwt(real)) bytes = 8;
    if (bytes) return v->size == bytes && v->stride == bytes && (v->flags & 255) == bytes - 1;
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    if (!kind) return false;
    const void *const *m = type;
    uintptr_t k = kind(type);
    if (k == 0x202 && m[1] == dlsym(RTLD_DEFAULT,"$sSqMn")) {
        const witnesses *p = vwt(m[2]);
        if (!scalar_record(m[2],NULL,depth + 1) || v->size != p->size + 1 ||
            (v->flags & 255) != (p->flags & 255) ||
            v->stride != ((v->size + (v->flags & 255)) & ~(uint64_t)(v->flags & 255))) return false;
        if (!value) return true;
        unsigned tag = value[p->size];
        return tag == 1 || (tag == 0 && scalar_record(m[2],value,depth + 1));
    }
    if (k != 0x301) return false;
    count_fn *count = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount");
    offset_fn *offset = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata");
    if (!count || !offset || !field || count(type) != 1 || offset(type,0)) return false;
    field_info info = {0};
    const void *child = field(type,0,&info);
    if (info.free_name) info.free_name(info.name);
    return child && info.strong && vwt(child)->size == v->size &&
        vwt(child)->stride == v->stride && (vwt(child)->flags & 255) == (v->flags & 255) &&
        scalar_record(child,value,depth + 1);
}

/* GenEnum.cpp SinglePayloadEnumImplStrategy puts the payload first and
 * asks its getEnumTagSinglePayload witness for a semantic index. With this
 * separately proved zero-XI scalar payload both architectures use the same
 * trailing tag and empty-case encoding. Reference spare bits are excluded. */
static bool scalar_single_enum(const void *type, const void *value, const char *desc)
{
    const witnesses *v = vwt(type);
    unsigned empty = u32(desc + 24);
    if (u32(desc + 20) != 1 || !empty || empty > 64 || !no_generic_context(desc) ||
        v->extra || (v->flags & 0x01910000)) return false;
    const char *fields = relative(desc + 16);
    if (!fields || u32(fields + 8) != (12u << 16 | 2) || u32(fields + 12) != empty + 1) return false;
    for (unsigned i = 1; i <= empty; ++i)
        if (u32(fields + 16 + 12*i) || relative(fields + 20 + 12*i)) return false;
    const void *payload = field_type(fields + 16,desc);
    if (!payload || !scalar_record(payload,NULL,0)) return false;
    const witnesses *p = vwt(payload);
    if (v->size != p->size + 1 || (v->flags & 255) != (p->flags & 255) ||
        v->stride != ((v->size + (v->flags & 255)) & ~(uint64_t)(v->flags & 255)) || !p->fn[6]) return false;
    unsigned tag = ((single_tag_fn *)p->fn[6])(value,empty,payload);
    return tag <= empty && (tag || scalar_record(payload,value,0));
}

/* Frozen String and tuples made solely of String have no fixed payload
 * spare bits in either Darwin64 ABI. Unlike arbitrary aggregate shapes this
 * proof follows exact String identity and every tuple member. */
static bool string_payload(const void *m, const unsigned char *value, unsigned depth)
{
    if (!m || depth > 8) return false;
    const witnesses *v = vwt(m);
    if (!v->size || v->size > 1024 || v->stride != v->size ||
        (v->flags & 255) != 7 || (v->flags & 0x01900000) || v->extra != 0x7fffffff) return false;
    if (m == dlsym(RTLD_DEFAULT,"$sSSN"))
        return v->size == 16 && (!value || u64(value + 8) > 2);
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    count_fn *count = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount");
    offset_fn *offset = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata");
    if (!kind || !count || !offset || !field || kind(m) != 0x301) return false;
    intptr_t n = count(m);
    if (n < 1 || n > 64) return false;
    uint64_t end = 0;
    for (intptr_t i = 0; i < n; i++) {
        field_info info = {0};
        const void *child = field(m,i,&info);
        intptr_t at = offset(m,i);
        if (info.free_name) info.free_name(info.name);
        if (!child || !info.strong || at < 0 || (uint64_t)at != end || end > v->size ||
            vwt(child)->size > v->size - end ||
            !string_payload(child,value ? value + end : NULL,depth + 1)) return false;
        end += vwt(child)->size;
    }
    return end == v->size;
}

#ifdef HAO_SWIFT_VALUE_RUNTIME
/* The enum tag belongs to the native owner, while a client may initialize its
 * exposed payload. Restrict this leaf to exactly String (or String tuples),
 * whose live representation already shares the native String ownership domain.
 * Do not recursively admit arbitrary foreign payloads just because their
 * enclosing enum is resilient. No discriminator bytes are guessed here. */
static bool resilient_string_enum(const void *type,const char *desc)
{
    unsigned payloads = u32(desc + 20), empties = u32(desc + 24);
    const char *fields = relative(desc + 16);
    if (payloads != 1 || !empties || empties > 63 || !fields ||
        u32(fields + 8) != (12u << 16 | 2) || u32(fields + 12) != 1 + empties ||
        !string_payload(field_type(fields + 16,desc),NULL,0)) return false;
    for (unsigned i = 1; i <= empties; ++i)
        if (u32(fields + 16 + 12*i) || relative(fields + 20 + 12*i)) return false;
    return hao_sv_native_resilient_enum(type);
}
#endif

int common_leaf_value(const void *type, const void *value)
{
#ifdef DISABLE_COMMON_LEAVES
    (void)type; (void)value;
    return 0;
#endif
    if (!type || !value) return 0;
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    if (!kind) return 0;
    uintptr_t k = kind(type);
    const witnesses *v = vwt(type);
    if (v->flags & 0x01900000) return 0;
    const void *const *m = type;
    if (k == 0x201 && guest_c_integer_enum(type)) return 1;
    if (k == 0x202 && m[1] == dlsym(RTLD_DEFAULT,"$sSqMn")) {
        const witnesses *payload = vwt(m[2]);
        if (v->size == 16 && v->stride == 16 && (v->flags & ~0x2000000u) == 0x210007 &&
            v->extra == 0x7ffffffe && class_existential_value(m[2],value)) return 1;
        if (scalar_shape(m[2],0) && v->size == payload->size + 1 &&
            v->stride == payload->size * 2 && !v->extra && !(v->flags & 0x10000) &&
            (v->flags & 255) == (payload->flags & 255))
            return ((const unsigned char *)value)[payload->size] < 2;
        if (v->size == 8 && v->stride == 8 && v->extra == 0x7ffffffe &&
            (v->flags & 255) == 7 && reference_shape(m[2],0)) {
            uint64_t bits = u64(value);
            return !bits || (bits >= 4096 && !(bits & 7));
        }
        if (v->size == 8 && v->stride == 8 && v->flags == 0x210007 &&
            v->extra == 0x7ffffffe && common_objc_reference_value(m[2],value)) return 1;
        if (v->size == 8 && v->stride == 8 && v->flags == 0x210007 &&
            v->extra == 0x7ffffffe && anyobject_type(m[2]) && unknown_object_value(m[2],value)) return 1;
        /* First Optional of a no-payload enum of n <= 255 cases: one byte, a
         * case is 0..n-1 and nil is n, measured identical in native arm64 and
         * x86_64 programs (no pointer extra inhabitants are involved). */
        unsigned cases = payload_free_enum_cases(m[2]);
        if (cases && cases < 256 && v->size == 1 && v->stride == 1 && !(v->flags & 0x100ff) &&
            v->extra == payload->extra - 1)
            return ((const unsigned char *)value)[0] <= cases;
        return common_byte_optional_case(type,value) == 1;
    }
    if (k != 0x201) return 0;
    const char *desc = m[1];
    if ((u32(desc) & 31) != 18) return 0;
#ifdef HAO_SWIFT_VALUE_RUNTIME
    if (resilient_string_enum(type,desc)) return 1;
#endif
    unsigned cases = u32(desc + 24);
    if (!(u32(desc + 20) & 0xffffff) && cases > 0 && cases <= 256 && v->size == 1 &&
        v->stride == 1 && !(v->flags & 0x100ff) && v->extra == 256 - cases)
        return *(const unsigned char *)value < cases;
    return scalar_single_enum(type,value,desc) || string_reference_enum(type,value,desc) || compact_strong_enum(type,value,desc);
}
