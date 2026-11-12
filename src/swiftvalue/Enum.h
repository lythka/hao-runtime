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
/* The same with ROW in place of the type's table row (NULL: none), for tests
 * of the row rules on types the tables do not list. */
int discover_fixed_enum_case_row(const void *type, const void *value, fixed_enum_case *out, const mpenum_row *row);
