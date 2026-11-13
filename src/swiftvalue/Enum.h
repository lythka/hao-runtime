#pragma once
#include "Prefix.h"
#include "Tables.h"
#include <stdint.h>
typedef struct {
    const void *payload; /* NULL only for the proved empty case. */
    uint64_t size, payload_size, tag_offset;
    uint32_t tag, payload_flags;
} enum_case_plan;
/* Trusted initialized native metadata and native-layout discriminator only.
 * This is an experiment, not a validator for arbitrary guest pointers. */
int discover_enum_case(const void *type, const void *value, enum_case_plan *out);
enum { HAO_SV_MAX_ENUM_PAYLOADS = 64 };
/* The types of an enum's inline (not indirect) payload cases, at most
 * HAO_SV_MAX_ENUM_PAYLOADS: their number, or -1 when one does not resolve. */
int enum_inline_payloads(const void *type, const void **payloads);
/* Returns the number of direct payload cases, or zero without a proof. */
int enum_payloads(const void *type, const void **payloads, uint64_t *extent);
/* Compiler-laid-out multi-payload enums the runtime tag witness does not
 * describe (Enum.c): an extra tag byte that holds the tag (layout only), or
 * tags in the payloads' spare bits (native witnesses only: in bits 60-63 of
 * the payload words, or wherever a generated table row for the type says the
 * encodings are the same on arm64), or decoded from a guest enum's own
 * layout record. For the walk only: it proves the active
 * case and where its payload is, not its ownership; Aggregate.c's transfers
 * stay with discover_enum_case. */
enum { HAO_SV_SPARE_WORDS = 16 };
typedef struct {
    const void *payload;   /* NULL only for an empty case */
    uint32_t tag;
    int spare;             /* 1: walk COPY, the value with the tag bits MASK cleared;
                              0: walk the value itself, payload at offset 0 */
    uint64_t copy[HAO_SV_SPARE_WORDS], mask[HAO_SV_SPARE_WORDS];
} fixed_enum_case;
int discover_fixed_enum_case(const void *type, const void *value, fixed_enum_case *out);
/* A compiler-laid-out multi-payload enum with one extra tag byte and no
 * spare bit (tag_byte_case's layout): the byte's offset and the number of
 * tags in use; its extra inhabitants are the byte's values 255, 254, ...,
 * the same on arm64 and x86_64. */
int tag_byte_layout(const void *type, uint64_t *tag_offset, unsigned *tags);
/* Extra inhabitant K of a fixed multi-payload layout whose tags lie in the
 * payloads' common spare bits SPARE (AREA bytes, the payload area, no extra
 * tag bits), as IRGen stores it (GenEnum.cpp getFixedExtraInhabitantValue):
 * the AREA bytes into OUT. Returns 0 for a layout that needs extra tag bits. */
int spare_bit_xi(const unsigned char *spare, uint64_t area, unsigned payload_cases, unsigned empty,
                 uint32_t k, unsigned char *out);
/* Extra inhabitant K of a guest-defined multi-payload enum whose tag lies in
 * its payloads' spare bits (Enum.c guest_spare_bit_case's layout, from the
 * image's __swift5_mpenum record, checked against the live witness table),
 * in its arm64 encoding: the enum's size in bytes into OUT. Returns 0 when
 * TYPE is not such an enum or K is not below its inhabitant count. */
int guest_spare_bit_xi(const void *type, uint32_t k, unsigned char *out);
/* The same with ROW in place of the type's table row (NULL: none), for tests
 * of the row rules on types the tables do not list. */
int discover_fixed_enum_case_row(const void *type, const void *value, fixed_enum_case *out, const mpenum_row *row);
