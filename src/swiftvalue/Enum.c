/* Runtime-sized direct payload enums. Source: Swift ABI Metadata.h,
 * ReflectionRecords.h, MetadataLookup.cpp and Enum.cpp. Keep the outer enum's
 * representation proof separate from its payload's Optional discriminator. */
#include "Enum.h"
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>
#define SWIFT __attribute__((swiftcall))
typedef const void *SWIFT resolve_fn(const char *, size_t, const void *, const void *const *);
typedef uintptr_t SWIFT kind_fn(const void *);
typedef unsigned SWIFT case_fn(const void *, const void *);
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

int enum_payloads(const void *type, const void **payloads, uint64_t *extent)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT, "swift_getMetadataKind");
    resolve_fn *resolve = dlsym(RTLD_DEFAULT, "swift_getTypeByMangledNameInContext2");
    void *runtime_tag = dlsym(RTLD_DEFAULT, "swift_getMultiPayloadEnumTagSinglePayload");
    if (!type || !kind || !resolve || !runtime_tag || kind(type) != 0x201) return 0;
    const void *const *m = type;
    const char *desc = m[1];
    const witnesses *v = vwt(type);
    /* Context kind Enum=18, generic flag bit7. The payload-size word is the
     * runtime layout's authority, not a guessed offset at end of allocation. */
    unsigned count = u32(desc + 20) & 0xffffff;
    unsigned empty = u32(desc + 24);
    if ((u32(desc) & 0x1f) != 0x12 || count < 2 || count > HAO_SV_MAX_ENUM_PAYLOADS || empty > 1) return 0;
    unsigned word_offset = u32(desc + 20) >> 24;
    if (word_offset < 2 || word_offset > 64 || v->size < 2 || v->size > 4096 ||
        v->functions[6] != runtime_tag || v->extra != 256 - count - empty) return 0;
    uint64_t payload_extent = ((const uint64_t *)m)[word_offset];
    if (payload_extent + 1 != v->size) return 0;
    const char *fields = relative(desc + 16);
    /* FieldDescriptorKind::Enum=2; direct cases only, ordinary 12-byte records. */
    if (!fields || u16(fields + 8) != 2 || u16(fields + 10) != 12 || u32(fields + 12) != count + empty) return 0;
    if (empty) {
        const char *record = fields + 16 + 12 * count;
        if (u32(record) || relative(record + 4)) return 0;
    }
    uint64_t maximum = 0;
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
         * the same runtime witness/extent/tag checks above apply. */
        payloads[i] = resolve(name, length, desc, (u32(desc) & 0x80) ? m + 2 : NULL);
        if (!payloads[i]) return 0;
        uint64_t size = vwt(payloads[i])->size;
        if (size > maximum) maximum = size;
    }
    if (maximum != payload_extent) return 0;
    *extent = payload_extent;
    return (int)count;
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
