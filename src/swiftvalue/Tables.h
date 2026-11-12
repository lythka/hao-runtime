#pragma once
#include "Prefix.h"
#include <stdint.h>
/* Generated per-build facts about native (x86_64) Swift types, keyed by the
 * defining image's LC_UUID and the type descriptor's offset from its mach
 * header (Tables.c, src/mpenum_gen.inc, src/archdiff_gen.inc). A row applies
 * only to the image build it was generated from: another build's image has
 * another UUID and finds no row. */

/* A compiler-laid-out multi-payload enum whose payloads have common spare
 * bits, with its arm64 layout record compared to the x86_64 one. */
enum { MPENUM_CASES = 1, MPENUM_EMPTY = 2, MPENUM_XI = 4 };
typedef struct {
    uint32_t payload, empty;   /* payload and empty case counts */
    uint32_t xi;               /* x86_64 XI count of the spare-bit layout: the row applies only
                                  when the live value witness table has this count */
    uint32_t flags;            /* MPENUM_CASES: payload-case encodings identical on arm64 and
                                  x86_64; MPENUM_EMPTY: empty-case ones too (or no empty case);
                                  MPENUM_XI: extra inhabitants identical */
} mpenum_row;
int mpenum_lookup(const uint8_t uuid[16], uint64_t offset, mpenum_row *out);

/* A native struct or enum whose value representation differs from the arm64
 * build's type of the same image and name: 's' fields or case indices
 * differ, 't' arm64 adds trailing cases, 'p' the multi-payload layouts
 * differ. 0: not listed. */
int archdiff_lookup(const uint8_t uuid[16], uint64_t offset);

/* By metadata: a struct, enum or Optional (kinds 0x200-0x202) through its
 * nominal descriptor, when that lies in a native image. 0 for anything else
 * (a guest type, another kind, an image without a row). */
int mpenum_type(const void *type, mpenum_row *out);
int archdiff_type(const void *type);
/* Whether a native struct's module froze it (frozenstruct_gen.inc), so that
 * guest code compiled against it lays it out inline (swiftvalue.c). */
int native_frozen_type(const void *type);
/* A memo for Recipe.c's holds_archdiff, by metadata: -1 unknown, 0, 1. */
int holds_archdiff_memo(const void *type);
void holds_archdiff_remember(const void *type, int listed);
void tables_fork_child(void);
