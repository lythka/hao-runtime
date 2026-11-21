/* AOT v9. Each record describes a nonempty host byte interval belonging to
 * one guest instruction. Offsets use the v8 blob origin (16 bytes before
 * the first instruction). Padding has no record. */
#ifndef HAO_AOT_MAPPED_H
#define HAO_AOT_MAPPED_H
#include "hao/aot.h"
#define HAO_AOT_MAPPED_INFO_OFFSET 512u

typedef struct {
    uint32_t start, end, guest_off, flags_in_host;
} hao_aot_pc_range;

typedef struct {
    char magic[8];
    uint32_t version, header_size;
    uint32_t block_count, range_count;
    uint64_t forward_offset, ranges_offset, legacy_offset, legacy_size, text_offset, text_size;
} hao_aot_mapped_header;

typedef struct {
    const hao_aot_mapped_header *header;
    const uint32_t *forward;
    const hao_aot_pc_range *ranges;
    hao_aot_view legacy;
    uint32_t signature_offset, signature_size;
} hao_aot_mapped_view;

int hao_aot_mapped_check(const void *file, size_t size, const hao_aot_expect *expect,
                         hao_aot_mapped_view *view, char *why, size_t why_size);
bool hao_aot_mapped_write(int fd, const uint8_t *legacy, size_t legacy_size, const uint64_t *by_guest,
                          const hao_aot_pc_range *ranges, uint32_t range_count);
#endif
