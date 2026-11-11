#pragma once
#include "Prefix.h"
#include <stdint.h>
#include "Common.h"
typedef struct { uint64_t size, offset; uint32_t consumed; } empty_plan;
int discover_empty_plan(const void *type, empty_plan *out);
/* Destination is uninitialized, exclusive storage. Returns
 * zero without writing if type/value is outside the proven empty case. */
int initialize_empty(void *destination, const void *source, const void *type,
                     int destination_arm);
typedef struct { uint64_t offset, bits, receiver_bits; uint32_t consumed; } empty_patch;
/* Structural walk only; an unknown enum context rejects the whole plan. */
int collect_empty_patches(const void *type, const void *value, empty_patch *out, unsigned capacity);
/* On refusal, reports the deepest unresolved field's metadata. */
int collect_empty_patches_detail(const void *type, const void *value, empty_patch *out,
                                 unsigned capacity, const void **unresolved);
/* The same, also through existential payloads (their metadata or object word),
 * for a value a guest hands to a native value witness and keeps: it is never
 * published to native code by this, so the publication contracts for
 * existential values (weak storage, guest witness tables) do not apply. */
int discover_witness_plan(const void *type, empty_plan *out);
int collect_witness_patches(const void *type, const void *value, empty_patch *out, unsigned capacity);
