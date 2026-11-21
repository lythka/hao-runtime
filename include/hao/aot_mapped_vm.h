#ifndef HAO_AOT_MAPPED_VM_H
#define HAO_AOT_MAPPED_VM_H
#include "hao/aot_mapped.h"
typedef struct {
    void *file;
    size_t size;
    void *reservation;
    size_t reserved;
    uint8_t *slot;
    hao_aot_mapped_view view;
} hao_aot_mapping;
/* Caller has verified directory, file and immutable backing, obtained an
 * independently authorized digest, and keeps fd open through this call. */
bool hao_aot_mapped_open(int fd, size_t size, const uint8_t digest[32], const hao_aot_expect *expect,
                         uint64_t base, hao_aot_mapping *mapping, char *why, size_t n);
void hao_aot_mapping_discard(hao_aot_mapping *mapping);
#endif
