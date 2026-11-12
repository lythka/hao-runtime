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

/* The direct cases of an enum with 2..64 payload cases and at most
 * MAX_EMPTY empty cases, whose field descriptor has kind FIELD_KIND: Enum
 * (2), as the runtime-laid-out enums below have, or MultiPayloadEnum (3), the
 * compiler's fixed multi-payload layout. Each payload type resolved, the
 * largest payload size in *maximum. Returns the payload case count, or zero.
 * Says nothing about the layout. */
enum { MAX_EMPTY = 1u << 16 };
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
    if ((u32(desc) & 0x1f) != 0x12 || count < 2 || count > HAO_SV_MAX_ENUM_PAYLOADS || empty > MAX_EMPTY ||
        v->size < 2 || v->size > 4096) return 0;
    const char *fields = relative(desc + 16);
    /* Direct cases only, ordinary 12-byte records. */
    if (!fields || u16(fields + 8) != field_kind || u16(fields + 10) != 12 || u32(fields + 12) != count + empty) return 0;
    for (unsigned e = 0; e < empty; ++e) {
        const char *record = fields + 16 + 12 * (count + e);
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

int enum_inline_payloads(const void *type, const void **payloads)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT, "swift_getMetadataKind");
    resolve_fn *resolve = dlsym(RTLD_DEFAULT, "swift_getTypeByMangledNameInContext2");
    if (!type || !kind || !resolve || kind(type) != 0x201) return -1;
    const void *const *m = type;
    const char *desc = m[1];
    unsigned count = u32(desc + 20) & 0xffffff;
    if ((u32(desc) & 0x1f) != 0x12 || count > HAO_SV_MAX_ENUM_PAYLOADS) return -1;
    if (!count) return 0;
    const char *fields = relative(desc + 16);
    if (!fields || u16(fields + 10) != 12 || u32(fields + 12) < count) return -1;
    int n = 0;
    for (unsigned i = 0; i < count; ++i) {
        const char *record = fields + 16 + 12 * i;
        if (u32(record) & 1) continue;       /* indirect: boxed, not inline */
        const char *name = relative(record + 4);
        if (!name) return -1;
        size_t length = 0;
        while (length < 4096 && name[length]) {
            unsigned c = (unsigned char)name[length++];
            if (c >= 1 && c <= 0x17) length += 4;
            else if (c >= 0x18 && c <= 0x1f) length += sizeof(void *);
        }
        if (length >= 4096) return -1;
        const void *payload = resolve(name, length, desc, (u32(desc) & 0x80) ? m + 2 : NULL);
        if (!payload) return -1;
        payloads[n++] = payload;
    }
    return n;
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
    if ((u32(desc) & 0x1f) != 0x12 || word_offset < 2 || word_offset > 64 || empty > 1 ||
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
 * (GenEnum.cpp MultiPayloadEnumImplStrategy::completeFixedLayout). It needs
 * extra tag bits when the payloads' common spare bits cannot hold the tag;
 * the extra tag then holds the case index shifted right by the spare bits
 * used, and its unused values are the extra inhabitants. So a one-byte extra
 * tag (size = largest payload + 1) with 256 - (payload cases + empty-case
 * tags) inhabitants uses no spare bit: the byte is the tag and the payload
 * sits unchanged at offset 0. Empty cases share tags: with no spare bit,
 * all USED = 8 * largest payload bits of the payload area hold an empty
 * case's index (getEmptyCasePayload), so empty case e has tag
 * payload cases + e / 2^USED and stores e % 2^USED from bit 0 of the
 * payload area; from 32 bits up, one tag holds every empty case, its index
 * in the payload's first 4 bytes. Measured identical in native arm64 and
 * x86_64 programs (tests/host/swiftlayout.swift) for String / (String,
 * String) with one, two and five empty cases, and Int / (Int, Int) with five
 * (size 33 or 17, xi 0xfd: tag byte = case index, the empty cases tag 2 with
 * their index in the payload's first bytes, the inhabitants tag bytes ff,
 * fe, ...). Layout-derived, so it also holds for a guest type whose
 * witnesses are arm64 code. */
static int tag_byte_case(const void *type, const unsigned char *value, fixed_enum_case *out)
{
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    unsigned empty;
    uint64_t maximum;
    int count = direct_cases(type, 3, payloads, &empty, &maximum);
    const witnesses *v = vwt(type);
    if (!count || !maximum || v->size != maximum + 1) return 0;
    uint64_t used = 8 * maximum;
    unsigned empty_tags = !empty ? 0 : used >= 32 ? 1 : (unsigned)((empty + (UINT64_C(1) << used) - 1) >> used);
    if ((unsigned)count + empty_tags > 256 || v->extra != 256 - (unsigned)count - empty_tags) return 0;
    unsigned tag = value[maximum];
    if (tag >= (unsigned)count + empty_tags) return 0;
    if (tag >= (unsigned)count) {
        uint32_t index = 0;
        memcpy(&index, value, maximum < 4 ? (size_t)maximum : 4);   /* little-endian */
        uint64_t e = used >= 32 ? index : (uint64_t)(tag - (unsigned)count) << used | index;
        if (e >= empty) return 0;
    }
    out->payload = tag < (unsigned)count ? payloads[tag] : NULL;   /* an empty case: nothing to walk */
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
 * common spare bits (GenEnum.cpp: the highest ones), with native value
 * witnesses (spare-bit tags of a guest enum: guest_spare_bit_case).
 *
 * Without a table row (Tables.h): heap-pointer spare bits are
 * 0xF000000000000007 on arm64 and 0xFF00000000000007 on x86_64, so while the
 * tag needs at most bits 60-63 of a word both choose the same bits: measured
 * in native programs of both for a 16-byte enum of one class reference /
 * two class-reference words (case 1 sets bit 63 of word 1, live values
 * [p, 0] and [p, 0x8000000000000000 | p]) and a five-case enum of one class
 * reference (bits 61-63). A 17-case enum diverges (arm64 bit 2, x86_64 bit
 * 59), and empty cases are not covered: each payload case injected into
 * zeroed storage must set nothing but bits 60-63 of whole words, and there
 * is no empty case.
 *
 * With a row of the generated table for this type (mpenum_gen.inc: the
 * arm64 build's layout record compared to the x86_64 one) whose XI count is
 * the live one, the row decides instead: payload-case and empty-case
 * encodings identical (flags 1|2) admit the type wherever its tag bits lie
 * and whatever its empty cases; a row without flag 1 (a payload layout that
 * differs by architecture) or without flag 2 refuses it even where the bit
 * test above would pass. A row whose XI count is not the live one describes
 * a layout this type does not have (payloads that are not loadable get an
 * extra tag byte instead) and is ignored.
 *
 * Either way only native value witnesses are called, on zeroed storage that
 * holds no payload: each payload case, and with a row the first empty case
 * (index 0, so only its tag bits), is injected and must read back as that
 * case; the bits they set are the tag bits. The live case comes from the
 * native getEnumTag; an empty case has nothing to walk, a payload case's
 * payload is walked in a copy with every tag bit cleared, and the live
 * value is never projected. */
static int spare_bit_case(const void *type, const void *value, fixed_enum_case *out, const mpenum_row *row)
{
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    unsigned empty;
    uint64_t maximum;
    int count = direct_cases(type, 3, payloads, &empty, &maximum);
    const witnesses *v = vwt(type);
    if (!count || v->size < 8 || v->size > 8 * HAO_SV_SPARE_WORDS || v->stride < v->size ||
        !(v->flags & 0x00200000) || maximum > v->size) return 0;
    int tabled = row && row->xi == v->extra;
    if (tabled) {
        if (row->payload != (unsigned)count || row->empty != empty ||
            (row->flags & (MPENUM_CASES | MPENUM_EMPTY)) != (MPENUM_CASES | MPENUM_EMPTY)) return 0;
    } else if (empty || v->size % 8 || v->stride != v->size || (v->flags & 255) != 7) return 0;
    void *const *slots = (void *const *)v;
    for (unsigned k = 0; k <= 13; ++k)
        if ((k < 8 || k > 10) && !native_code(slots[k])) return 0;
    case_fn *get_tag = (case_fn *)slots[11];
    inject_fn *inject = (inject_fn *)slots[13];
    unsigned words = (unsigned)((v->size + 7) / 8);
    uint64_t mask[HAO_SV_SPARE_WORDS] = {0};
    unsigned probes = (unsigned)count + (tabled && empty);
    for (unsigned c = 0; c < probes; ++c) {
        _Alignas(16) uint64_t scratch[HAO_SV_SPARE_WORDS];
        memset(scratch, 0, sizeof scratch);
        inject(scratch, c, type);   /* no payload: nothing is retained or released */
        if (get_tag(scratch, type) != c) return 0;
        for (unsigned w = 0; w < words; ++w) {
            if (!tabled && (scratch[w] & ~UINT64_C(0xf000000000000000))) return 0;
            mask[w] |= scratch[w];
        }
    }
    unsigned tag = get_tag(value, type);
    if (tag >= (unsigned)count + (tabled ? empty : 0)) return 0;
    out->tag = tag;
    out->spare = 1;
    if (tag >= (unsigned)count) { out->payload = NULL; return 1; }   /* an empty case: nothing to walk */
    memcpy(out->copy, value, (size_t)v->size);
    for (unsigned w = 0; w < words; ++w) out->copy[w] &= ~mask[w];
    memcpy(out->mask, mask, sizeof mask);
    out->payload = payloads[tag];
    return 1;
}

/* Bit I of a little-endian byte array: byte I / 8, bit I % 8. */
static unsigned bit_at(const unsigned char *bytes, uint64_t i) { return bytes[i >> 3] >> (i & 7) & 1; }

/* The common spare-bit mask over the payload area (AREA bytes, into SPARE)
 * of the guest enum whose descriptor is DESC, from its image's layout record
 * (__swift5_mpenum: ReflectionRecords.h MultiPayloadEnumDescriptor, emitted
 * by GenReflection.cpp for every fixed-layout multi-payload enum): a
 * relative pointer to the type's mangled name, a size/flags word (contents
 * size in words in bits 16-31, bit 0 UsesPayloadSpareBits), then the mask's
 * byte offset (bits 16-31) and byte count (0-15) and the mask bytes. The
 * name is a symbolic reference to the descriptor, direct (1) or through a
 * pointer slot (2). Returns 1 when the record exists and uses spare bits. */
static int guest_spare_mask(hao_image *image, const char *desc, unsigned char *spare, uint64_t area)
{
    uint64_t size = 0, base = hao_image_section(image, NULL, "__swift5_mpenum", &size);
    if (!base || size > (1u << 24)) return 0;
    const char *p = (const char *)(uintptr_t)base, *end = p + size;
    while (end - p >= 8) {
        uint32_t header = u32(p + 4), words = header >> 16;
        if (!words || (uint64_t)(end - p) < 4 + 4 * (uint64_t)words) return 0;
        const char *name = relative(p), *target = NULL;
        if (name && hao_image_for_addr((uint64_t)(uintptr_t)name) == image &&
            hao_image_for_addr((uint64_t)(uintptr_t)name + 5) == image && (name[0] == 1 || name[0] == 2)) {
            const char *at = name + 1 + (int32_t)u32(name + 1);
            if (name[0] == 1) target = at;
            else if (hao_image_for_addr((uint64_t)(uintptr_t)at) == image) {
                uint64_t pointer;
                memcpy(&pointer, at, 8);
                target = (const char *)(uintptr_t)(pointer & ((UINT64_C(1) << 47) - 1));
            }
        }
        if (target == desc) {
            if (!(header & 1) || words < 2) return 0;   /* no spare bit: an extra tag byte (tag_byte_case) */
            uint32_t placement = u32(p + 8);
            uint64_t offset = placement >> 16, bytes = placement & 0xffff;
            if (bytes > 4 * (uint64_t)(words - 2) || offset + bytes > area) return 0;
            memset(spare, 0, (size_t)area);
            memcpy(spare + offset, p + 12, (size_t)bytes);
            return 1;
        }
        p += 4 + 4 * (uint64_t)words;
    }
    return 0;
}

/* A guest-defined multi-payload enum whose tag lies in its payloads' common
 * spare bits. Its witnesses are arm64 code and guest code reads it only by
 * the arm64 layout, so the question is that layout alone, and it is decoded
 * from the image's own record without running a witness. GenEnum.cpp
 * MultiPayloadEnumImplStrategy (completeFixedLayout, getEmptyCasePayload,
 * getFixedExtraInhabitantCount), over a payload area of AREA = largest
 * payload bytes holding S common spare bits:
 *  - empty-case tags: none without empty cases, else one when the U = 8 *
 *    AREA - S other bits number 32 or more, else ceil(empty / 2^U); with the
 *    payload cases they make T tags, needing N bits for T - 1;
 *  - N <= S (else extra tag bits follow the payload: not decoded here): the
 *    tag is in all S spare bits when N = S, else in the N highest;
 *  - the enum has no extra tag byte (size = AREA) and 2^S - T extra
 *    inhabitants (at most 0x7fffffff); the live value witness table must
 *    say so, or the record does not describe this layout (payloads that are
 *    not loadable get an extra tag byte instead);
 *  - a payload case's value holds the case in the tag bits; an empty case
 *    holds tag T' and index i in the other bits, from the lowest: empty case
 *    (T' - payload cases) * 2^U + i (i from 32 bits at most when U >= 32);
 *    a tag of T or more is an extra inhabitant, not a value.
 * The payload of a payload case is walked in a copy with the tag bits
 * cleared; an empty case has nothing to walk. Checked against arm64 programs
 * (tests/guest/optwitness.swift) through native tuple witnesses: pointer
 * tags with an empty case, tags in a Bool's spare bits with empty cases, and
 * a 17-case enum whose tag needs bit 2. */
static int guest_spare_bit_case(const void *type, const unsigned char *value, fixed_enum_case *out)
{
    const char *desc = ((const void *const *)type)[1];
    hao_image *image = desc ? hao_image_for_addr((uint64_t)(uintptr_t)desc) : NULL;
    if (!image) return 0;
    const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS];
    unsigned empty;
    uint64_t maximum;
    int count = direct_cases(type, 3, payloads, &empty, &maximum);
    const witnesses *v = vwt(type);
    if (!count || !maximum || maximum > 8 * HAO_SV_SPARE_WORDS || v->size != maximum) return 0;
    unsigned char spare[8 * HAO_SV_SPARE_WORDS], tag[8 * HAO_SV_SPARE_WORDS] = {0};
    if (!guest_spare_mask(image, desc, spare, maximum)) return 0;
    uint64_t bits = 8 * maximum, spares = 0;
    for (uint64_t i = 0; i < bits; ++i) spares += bit_at(spare, i);
    uint64_t used = bits - spares;
    uint64_t empty_tags = !empty ? 0 : used >= 32 ? 1 : (empty + (UINT64_C(1) << used) - 1) >> used;
    uint64_t tags = (uint64_t)count + empty_tags, tag_bits = 0;
    while ((UINT64_C(1) << tag_bits) < tags) ++tag_bits;
    if (!tag_bits) tag_bits = 1;
    if (!spares || tag_bits > spares) return 0;
    uint64_t xi = spares >= 31 ? 0x7fffffff : (UINT64_C(1) << spares) - tags;
    if (xi > 0x7fffffff) xi = 0x7fffffff;
    if (v->extra != xi) return 0;
    for (uint64_t i = bits, need = tag_bits; i-- > 0 && need;)
        if (bit_at(spare, i)) { tag[i >> 3] |= (unsigned char)(1u << (i & 7)); --need; }
    uint64_t t = 0;
    for (uint64_t i = 0, k = 0; i < bits; ++i)
        if (bit_at(tag, i)) t |= (uint64_t)bit_at(value, i) << k++;
    if (t >= tags) return 0;
    out->spare = 1;
    if (t >= (uint64_t)count) {
        uint64_t index = 0;
        for (uint64_t i = 0, k = 0; i < bits && k < 32 && k < used; ++i)
            if (!bit_at(spare, i)) index |= (uint64_t)bit_at(value, i) << k++;
        uint64_t e = used >= 32 ? index : (t - (uint64_t)count) << used | index;
        if (e >= empty) return 0;
        out->payload = NULL;   /* an empty case: nothing to walk */
        out->tag = (uint32_t)(count + e);
        return 1;
    }
    memcpy(out->copy, value, (size_t)maximum);
    memcpy(out->mask, tag, sizeof tag);
    unsigned char *copy = (unsigned char *)out->copy;
    for (uint64_t b = 0; b < maximum; ++b) copy[b] &= (unsigned char)~tag[b];
    out->payload = payloads[t];
    out->tag = (uint32_t)t;
    return 1;
}
#endif

static int fixed_case(const void *type, const void *value, fixed_enum_case *out, const mpenum_row *row)
{
    if (!type || !value || !out) return 0;
    memset(out, 0, sizeof *out);
    if (tag_byte_case(type, value, out)) return 1;
#ifdef HAO_SWIFT_VALUE_RUNTIME
    if (spare_bit_case(type, value, out, row)) return 1;
    if (guest_spare_bit_case(type, value, out)) return 1;
#else
    (void)row;
#endif
    return 0;
}

int discover_fixed_enum_case(const void *type, const void *value, fixed_enum_case *out)
{
    mpenum_row row;
    int have = 0;
#ifdef HAO_SWIFT_VALUE_RUNTIME
    have = type && mpenum_type(type, &row);
#endif
    return fixed_case(type, value, out, have ? &row : NULL);
}

int discover_fixed_enum_case_row(const void *type, const void *value, fixed_enum_case *out, const mpenum_row *row)
{
    return fixed_case(type, value, out, row);
}
