/* Direct payload enums. Source: Swift ABI Metadata.h, ReflectionRecords.h,
 * MetadataLookup.cpp, Enum.cpp and GenEnum.cpp. Keep the outer enum's
 * representation proof separate from its payload's Optional discriminator. */
#include "Enum.h"
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>
#ifdef HAO_SWIFT_VALUE_RUNTIME
#include "hao/runtime.h"
#include "hao/macho.h"
#endif
#define SWIFT __attribute__((swiftcall))
typedef const void *SWIFT resolve_fn(const char *, size_t, const void *, const void *const *);
typedef uintptr_t SWIFT kind_fn(const void *);
typedef unsigned SWIFT case_fn(const void *, const void *);
typedef void SWIFT inject_fn(void *, unsigned, const void *);
typedef struct {
    void *functions[8]; uint64_t size, stride; uint32_t flags, extra;
} witnesses;
static uint32_t u32(const void *p) { uint32_t n; memcpy(&n, p, 4); return n; }
static uint16_t u16(const void *p) { uint16_t n; memcpy(&n, p, 2); return n; }
static const char *relative(const char *p) {
    int32_t offset; memcpy(&offset, p, 4);
    return offset ? p + offset : NULL;
}
static const witnesses *vwt(const void *type) { return ((const witnesses *const *)type)[-1]; }

/* The direct cases of an enum with 2..64 payload cases and at most one empty
 * case, whose field descriptor has kind FIELD_KIND: Enum (2), as the runtime-
 * laid-out enums below have, or MultiPayloadEnum (3), the compiler's fixed
 * multi-payload layout. Each payload type resolved, the largest payload size
 * in *maximum. Returns the payload case count, or zero. Says nothing about
 * the layout. */
static int direct_cases(const void *type, unsigned field_kind, const void **payloads, unsigned *empty_out, uint64_t *maximum)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT, "swift_getMetadataKind");
    resolve_fn *resolve = dlsym(RTLD_DEFAULT, "swift_getTypeByMangledNameInContext2");
    if (!type || !kind || !resolve || kind(type) != 0x201) return 0;
    const void *const *m = type;
    const char *desc = m[1];
    const witnesses *v = vwt(type);
    /* Context kind Enum=18, generic flag bit7. */
    unsigned count = u32(desc + 20) & 0xffffff;
    unsigned empty = u32(desc + 24);
    if ((u32(desc) & 0x1f) != 0x12 || count < 2 || count > HAO_SV_MAX_ENUM_PAYLOADS || empty > 1 ||
        v->size < 2 || v->size > 4096) return 0;
    const char *fields = relative(desc + 16);
    /* Direct cases only, ordinary 12-byte records. */
    if (!fields || u16(fields + 8) != field_kind || u16(fields + 10) != 12 || u32(fields + 12) != count + empty) return 0;
    if (empty) {
        const char *record = fields + 16 + 12 * count;
        if (u32(record) || relative(record + 4)) return 0;
    }
    uint64_t largest = 0;
    for (unsigned i = 0; i < count; ++i) {
        const char *record = fields + 16 + 12 * i;
        if (u32(record) & 1) return 0;       /* indirect case */
        const char *name = relative(record + 4);
        if (!name) return 0;
        size_t length = 0;
        while (length < 4096 && name[length]) {
            unsigned c = (unsigned char)name[length++];
            if (c >= 1 && c <= 0x17) length += 4;
            else if (c >= 0x18 && c <= 0x1f) length += sizeof(void *);
        }
        if (length >= 4096) return 0;
        /* Non-generic runtime-sized enums have no generic argument vector.
         * Their tuple payloads can still depend on resilient external types;
         * the caller's layout checks apply. */
        payloads[i] = resolve(name, length, desc, (u32(desc) & 0x80) ? m + 2 : NULL);
        if (!payloads[i]) return 0;
        uint64_t size = vwt(payloads[i])->size;
        if (size > largest) largest = size;
    }
    *empty_out = empty;
    *maximum = largest;
    return (int)count;
}

int enum_payloads(const void *type, const void **payloads, uint64_t *extent)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT, "swift_getMetadataKind");
    void *runtime_tag = dlsym(RTLD_DEFAULT, "swift_getMultiPayloadEnumTagSinglePayload");
    if (!type || !kind || !runtime_tag || kind(type) != 0x201) return 0;
    const void *const *m = type;
    const char *desc = m[1];
    const witnesses *v = vwt(type);
    /* The payload-size word is the runtime layout's authority, not a guessed
     * offset at end of allocation. */
    unsigned count = u32(desc + 20) & 0xffffff;
    unsigned empty = u32(desc + 24);
    unsigned word_offset = u32(desc + 20) >> 24;
    if ((u32(desc) & 0x1f) != 0x12 || word_offset < 2 || word_offset > 64 ||
        v->functions[6] != runtime_tag || v->extra != 256 - count - empty) return 0;
    uint64_t payload_extent = ((const uint64_t *)m)[word_offset];
    if (payload_extent + 1 != v->size) return 0;
    uint64_t maximum;
    int n = direct_cases(type, 2, payloads, &empty, &maximum);
    if (!n || maximum != payload_extent) return 0;
    *extent = payload_extent;
    return n;
}

int discover_enum_case(const void *type, const void *value, enum_case_plan *out)
{
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    uint64_t extent;
    if (!value) return 0;
    int count = enum_payloads(type,payloads,&extent);
    if (!count) return 0;
    unsigned tag = ((const unsigned char *)value)[extent];
    if (tag >= (unsigned)count) {
        const char *desc = ((const void *const *)type)[1];
        case_fn *get_case = dlsym(RTLD_DEFAULT, "swift_getEnumCaseMultiPayload");
        /* Empty cases also use payload storage as a discriminator. Even with
         * one empty case, a trailing tag alone does not establish validity. */
        if (tag != (unsigned)count || u32(desc + 24) != 1 || !get_case ||
            get_case(value,type) != (unsigned)count) return 0;
        *out = (enum_case_plan){NULL, vwt(type)->size, 0, extent, tag, 0};
        return 1;
    }
    const witnesses *payload = vwt(payloads[tag]);
    *out = (enum_case_plan){payloads[tag], vwt(type)->size, payload->size,
                           extent, tag, payload->flags};
    return 1;
}

/* A compiler-laid-out multi-payload enum with an extra tag byte
 * (GenEnum.cpp MultiPayloadEnumImplStrategy). It needs extra tag bits when
 * the payloads' common spare bits cannot hold the tag; the extra tag then
 * holds the case index shifted right by the spare bits used, and its unused
 * values are the extra inhabitants. So a one-byte extra tag (size = largest
 * payload + 1) with 256 - (payload cases + empty cases) inhabitants uses no
 * spare bit: the byte is the case index and the payload sits unchanged at
 * offset 0. Measured identical in native arm64 and x86_64 programs for
 * String / (String, String) / one empty case (size 33, stride 40, xi 0xfd:
 * tag byte 32 = case index, the empty case a zero payload and tag 2, the
 * inhabitants tag bytes ff, fe, ...). Layout-derived, so it also holds for a
 * guest type whose witnesses are arm64 code. Several empty cases share one
 * tag and keep their index in the payload area; only one was measured. */
static int tag_byte_case(const void *type, const unsigned char *value, fixed_enum_case *out)
{
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    unsigned empty;
    uint64_t maximum;
    int count = direct_cases(type, 3, payloads, &empty, &maximum);
    const witnesses *v = vwt(type);
    if (!count || !maximum || v->size != maximum + 1 || v->extra != 256 - (unsigned)count - empty) return 0;
    unsigned tag = value[maximum];
    if (tag > (unsigned)count || (tag == (unsigned)count && !empty)) return 0;
    out->payload = tag < (unsigned)count ? payloads[tag] : NULL;   /* the empty case: nothing to walk */
    out->tag = tag;
    out->spare = 0;
    return 1;
}

#ifdef HAO_SWIFT_VALUE_RUNTIME
static int native_code(const void *f)
{
    uint64_t pc = (uint64_t)(uintptr_t)f;
    return pc && !hao_is_trap(pc) && !hao_image_for_code(pc) && !hao_guest_of_entry(pc);
}

/* A compiler-laid-out multi-payload enum whose tag lies in its payloads'
 * common spare bits (GenEnum.cpp: the highest ones). Heap-pointer spare bits
 * are 0xF000000000000007 on arm64 and 0xFF00000000000007 on x86_64, so while
 * the tag needs at most bits 60-63 of a word both choose the same bits:
 * measured in native programs of both for a 16-byte enum of one class
 * reference / two class-reference words (case 1 sets bit 63 of word 1, live
 * values [p, 0] and [p, 0x8000000000000000 | p]) and a five-case enum of one
 * class reference (bits 61-63). A 17-case enum diverges (arm64 bit 2, x86_64
 * bit 59), and the empty encodings always differ (that 16-byte enum: xi
 * 0x3ffe and 0x3ffffe, XI0 f0..07 and ff..07 words), so an empty case or an
 * Optional of such an enum is not covered. Only native value witnesses are
 * called: each payload case injected into zeroed storage must set nothing
 * but bits 60-63 of whole words and read back as that case. The live case
 * comes from the native getEnumTag; its payload is walked in a copy with
 * every tag bit cleared, and the live value is never projected. */
static int spare_bit_case(const void *type, const void *value, fixed_enum_case *out)
{
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    unsigned empty;
    uint64_t maximum;
    int count = direct_cases(type, 3, payloads, &empty, &maximum);
    const witnesses *v = vwt(type);
    if (!count || empty || v->size < 8 || v->size % 8 || v->size > 8 * HAO_SV_SPARE_WORDS ||
        v->stride != v->size || (v->flags & 255) != 7 || !(v->flags & 0x00200000) || maximum > v->size) return 0;
    void *const *slots = (void *const *)v;
    for (unsigned k = 0; k <= 13; ++k)
        if ((k < 8 || k > 10) && !native_code(slots[k])) return 0;
    case_fn *get_tag = (case_fn *)slots[11];
    inject_fn *inject = (inject_fn *)slots[13];
    unsigned words = (unsigned)(v->size / 8);
    uint64_t mask[HAO_SV_SPARE_WORDS] = {0};
    for (int c = 0; c < count; ++c) {
        _Alignas(16) uint64_t scratch[HAO_SV_SPARE_WORDS];
        memset(scratch, 0, sizeof scratch);
        inject(scratch, (unsigned)c, type);   /* no payload: nothing is retained or released */
        if (get_tag(scratch, type) != (unsigned)c) return 0;
        for (unsigned w = 0; w < words; ++w) {
            if (scratch[w] & ~UINT64_C(0xf000000000000000)) return 0;
            mask[w] |= scratch[w];
        }
    }
    unsigned tag = get_tag(value, type);
    if (tag >= (unsigned)count) return 0;
    memcpy(out->copy, value, (size_t)v->size);
    for (unsigned w = 0; w < words; ++w) out->copy[w] &= ~mask[w];
    memcpy(out->mask, mask, sizeof mask);
    out->payload = payloads[tag];
    out->tag = tag;
    out->spare = 1;
    return 1;
}
#endif

int discover_fixed_enum_case(const void *type, const void *value, fixed_enum_case *out)
{
    if (!type || !value || !out) return 0;
    memset(out, 0, sizeof *out);
    if (tag_byte_case(type, value, out)) return 1;
#ifdef HAO_SWIFT_VALUE_RUNTIME
    if (spare_bit_case(type, value, out)) return 1;
#endif
    return 0;
}
