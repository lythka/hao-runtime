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
