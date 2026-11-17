#pragma once
#include "Prefix.h"
#include <stddef.h>
#include <stdint.h>
#include "Common.h"
typedef struct { uint64_t size, offset; uint32_t consumed; } empty_plan;
int discover_empty_plan(const void *type, empty_plan *out);
/* Whether TYPE is an Optional whose nil is an extra inhabitant of a native
 * spare-bit enum that differs between arm64 and x86_64 (translated by the
 * walk from the mpenum table's masks). */
int translated_nil_type(const void *type);
/* The extra-inhabitant count arm64 gives a struct whose N fields
 * swift_cvw_initStructMetadataWithLayoutString lays out (FIELDS metadata,
 * TAGS all 0): the fields' maximum, each field's count as arm64 code
 * compiled against it has it. 0 if any field's is not known. */
int arm_struct_xi(const void *const *fields, const uint8_t *tags, size_t n, uint32_t *out);
/* Destination is uninitialized, exclusive storage. Returns
 * zero without writing if type/value is outside the proven empty case. */
int initialize_empty(void *destination, const void *source, const void *type,
                     int destination_arm);
/* A nil's word at OFFSET: BITS as found, RECEIVER_BITS the receiving witness
 * set's encoding (native), ARM_BITS the arm64 encoding (guest code's). */
typedef struct { uint64_t offset, bits, receiver_bits; uint32_t consumed; uint64_t arm_bits; } empty_patch;
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
/* collect_empty_patches_detail in survey mode: every innermost refusing type
 * is printed (swiftvalue.c, after a refusal; tests/host/swiftlayout). */
int collect_survey_patches(const void *type, const void *value, empty_patch *out,
                           unsigned capacity, const void **unresolved);
/* The classes by which a certified walk leaves parts of a value as it found
 * them (collect_value_patches *PASSES). Every part of a value the walk
 * certifies is either a patched nil or in one of these:
 *   IDENTICAL  the same representation and ownership on both architectures
 *              (no pointer word, a function's code word, a measured leaf
 *              layout, a metatype, a Builtin scalar);
 *   INACTIVE   a payload proven inactive by a discriminator both read alike
 *              (trailing tag byte, tag byte, code word, an enum's empty case);
 *   OWNED      opaque storage only its owner manipulates: a live object of
 *              the shared native ObjC/Swift/CF domain (String's storage, an
 *              ObjC reference, a block, a Swift object), an existential a
 *              native witness only copies. */
enum { HAO_SV_PASS_IDENTICAL = 1, HAO_SV_PASS_INACTIVE = 2, HAO_SV_PASS_OWNED = 4 };
/* collect_empty_patches_detail, or with EXISTENTIALS collect_witness_patches,
 * also reporting the pass classes of a certified walk. */
int collect_value_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                          const void **unresolved, int existentials, int (*known_layout)(const void *),
                          unsigned *passes);
/* Survey mode without printing: the patches of every part the walk can
 * certify, nothing inside a refusing type. For the experimental unrestricted
 * pass-through only (swiftvalue.c decline), which otherwise drops them. */
int collect_partial_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                            int existentials, int (*known_layout)(const void *));
/* Whether the representation of TYPE's own storage is, from its metadata
 * alone, the same on both architectures or its native owner's alone, for
 * every value (the rules collect applies without reading the value). Its
 * fields, payloads and Optional payload are the caller's to visit, unless
 * *WHOLE says nothing below needs it. Anything not settled needs the walk
 * at a boundary (swiftvalue.c graph). */
int settled_node(const void *type, int *whole);
int collect_witness_patches(const void *type, const void *value, empty_patch *out, unsigned capacity);
/* The same, where each struct on an extra-inhabitant path (other than Array and String) must be one
 * KNOWN_LAYOUT accepts: the outer nils guest code can read inline, for a nil a native witness made. */
int collect_inline_patches(const void *type, const void *value, empty_patch *out, unsigned capacity,
                           int (*known_layout)(const void *));
