#pragma once
#include "Prefix.h"
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
/* Returns the number of direct payload cases, or zero without a proof. */
int enum_payloads(const void *type, const void **payloads, uint64_t *extent);
/* Compiler-laid-out multi-payload enums the runtime tag witness does not
 * describe (Enum.c): an extra tag byte that is the case index (layout only),
 * or tags in bits 60-63 of the payload words (native witnesses only). For the
 * walk only: it proves the active case and where its payload is, not its
 * ownership; Aggregate.c's transfers stay with discover_enum_case. */
enum { HAO_SV_SPARE_WORDS = 16 };
typedef struct {
    const void *payload;   /* NULL only for the empty case of a tag-byte enum */
    uint32_t tag;
    int spare;             /* 1: walk COPY, the value with the tag bits MASK cleared;
                              0: walk the value itself, payload at offset 0 */
    uint64_t copy[HAO_SV_SPARE_WORDS], mask[HAO_SV_SPARE_WORDS];
} fixed_enum_case;
int discover_fixed_enum_case(const void *type, const void *value, fixed_enum_case *out);
