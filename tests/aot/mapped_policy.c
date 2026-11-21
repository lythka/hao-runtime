/* Exercise the pure parser on mutations of a real signed v9 file, then
 * inspect the current and maximum VM protections of its admitted views. */
#include "hao/aot_mapped_vm.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool protections(const void *p, vm_prot_t expected)
{
    mach_vm_address_t address = (uintptr_t)p;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && address <= (uintptr_t)p && info.protection == expected && info.max_protection == expected;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int fd = open(argv[1], O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 4096 || st.st_size > (1 << 24)) return 2;
    size_t size = (size_t)st.st_size;
    uint8_t *file = malloc(size);
    if (!file || pread(fd, file, size, 0) != (ssize_t)size) return 2;
    hao_aot_hdr legacy;
    int legacy_fd = open(argv[2], O_RDONLY);
    if (legacy_fd < 0 || read(legacy_fd, &legacy, sizeof legacy) != sizeof legacy) return 2;
    close(legacy_fd);
    uint64_t lo = 0, hi = legacy.vmsize;
    hao_aot_expect x = {legacy.uuid, legacy.gen, hi, legacy.guest_id, &lo, &hi, 1,
                        legacy.id_kind, legacy.id_hash, legacy.code_id, legacy.cpu_profile};
    hao_aot_mapped_view view;
    char why[256];
    if (hao_aot_mapped_check(file, size, &x, &view, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 1; }
    hao_aot_mapped_header *h = (void *)(file + HAO_AOT_MAPPED_INFO_OFFSET);
    struct mach_header_64 *mh = (void *)file;
    struct segment_command_64 *sg = (void *)(mh + 1);
    struct linkedit_data_command *sig = (void *)(sg + 3);
    uint32_t *g = (void *)(file + h->forward_offset);
    hao_aot_pc_range *r = (void *)(file + h->ranges_offset);
    int cases = 0;
#define MUTATE(field, value) do { \
    __typeof__(field) saved = (field); (field) = (value); \
    if (!hao_aot_mapped_check(file, size, &x, NULL, why, sizeof why)) { fprintf(stderr, "accepted %s\n", #field); return 1; } \
    (field) = saved; cases++; \
} while (0)
    MUTATE(mh->ncmds, 0xffffffffu);
    MUTATE(sg[1].maxprot, VM_PROT_ALL);
    MUTATE(sig->dataoff, 0);
    MUTATE(h->version, 8);
    MUTATE(h->legacy_offset, UINT64_MAX);
    MUTATE(h->legacy_size, UINT64_MAX);
    MUTATE(h->text_offset, 16);
    MUTATE(h->text_size, UINT64_MAX);
    MUTATE(h->range_count, UINT32_MAX);
    MUTATE(g[0], h->block_count);
    MUTATE(g[1], g[0]);
    MUTATE(r[0].start, r[0].start + 1);
    MUTATE(r[1].start, r[0].start);
    MUTATE(r[0].flags_in_host, 2);
    MUTATE(r[0].guest_off, UINT32_MAX);
    MUTATE(r[h->range_count - 1].end, r[h->range_count - 1].end - 1);
    MUTATE(g[2 * h->block_count - 1], g[2 * h->block_count - 1] - 1);
    if (!hao_aot_mapped_check(file, size - 1, &x, NULL, why, sizeof why)) return 1;
    for (size_t k = 0; k < 4096; k++) if (!hao_aot_mapped_check(file, k, &x, NULL, why, sizeof why)) return 1;
    uint8_t digest[32];
    CC_SHA256(file, (CC_LONG)size, digest);
    hao_aot_mapping m;
    if (!hao_aot_mapped_open(fd, size, digest, &x, 0x12345678000, &m, why, sizeof why)) {
        fprintf(stderr, "%s\n", why); return 1;
    }
    uint64_t guest_base;
    memcpy(&guest_base, m.slot, sizeof guest_base);
    if (guest_base != 0x12345678000 || !protections(m.slot, VM_PROT_READ) ||
        !protections(m.slot + 16, VM_PROT_READ | VM_PROT_EXECUTE) || !protections(m.file, VM_PROT_READ))
        return 1;
    hao_aot_mapping_discard(&m);
    free(file);
    close(fd);
    printf("mapped parser rejected %d field mutations and 4097 truncations; slot R, text RX, metadata R including maxima\n", cases);
    return 0;
}
