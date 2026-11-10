/* Non-destructive placement of executable callback islands within rel32 range.
 * mmap's address is only a hint. Enumerate VM holes instead of assuming eight
 * sparse hints cover the available address space. A racing allocation can make
 * mmap choose elsewhere; validate every result and never use MAP_FIXED. */
#ifndef HAO_NEAR_MAP_H
#define HAO_NEAR_MAP_H
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static int hao_near_range(uint64_t a, uint64_t lo, uint64_t hi, uint64_t size)
{
    const uint64_t reach = 0x7fff0000ull;
    if (!size || hi < lo || a > UINT64_MAX - size) return 0;
    /* The furthest pairs are island start / highest field and island end /
     * lowest field, not corresponding ends of the two ranges. */
    return (a >= hi || hi - a < reach) &&
           (a + size <= lo || a + size - lo < reach);
}

static void *hao_near_try_prot(uint64_t at, uint64_t lo, uint64_t hi, uint64_t size, int prot, int flags)
{
    void *p = mmap((void *)(uintptr_t)at, size, prot,
                   MAP_PRIVATE | MAP_ANON | flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (hao_near_range((uint64_t)(uintptr_t)p, lo, hi, size)) return p;
    munmap(p, size);
    return NULL;
}

static void *hao_map_near_prot(uint64_t lo, uint64_t hi, uint64_t size, int prot, int flags)
{
    const uint64_t reach = 0x7fff0000ull, pg = (uint64_t)getpagesize();
    if (!size || size % pg || hi < lo || lo > UINT64_MAX - reach || size >= reach * 2) return NULL;
    uint64_t first = hi >= reach ? hi - reach + 1 : pg;
    first = (first + pg - 1) & ~(pg - 1);
    if (lo + reach <= size) return NULL;
    uint64_t last = (lo + reach - size - 1) & ~(pg - 1);
    if (first > last) return NULL;
    /* Common case: immediately after the image. */
    uint64_t hint = (hi + pg - 1) & ~(pg - 1);
    void *p = NULL;
    if (hint >= first && hint <= last && (p = hao_near_try_prot(hint, lo, hi, size, prot, flags))) return p;
    for (uint64_t at = first; at <= last;) {
        mach_vm_address_t region = at;
        mach_vm_size_t bytes = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &region, &bytes,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS && kr != KERN_INVALID_ADDRESS) break;
        if (kr == KERN_INVALID_ADDRESS || (region > at && region - at >= size)) {
            p = hao_near_try_prot(at, lo, hi, size, prot, flags);
            if (p) return p;
        }
        if (kr == KERN_INVALID_ADDRESS || !bytes || region > UINT64_MAX - bytes || region + bytes <= at) break;
        at = region + bytes;
    }
    return NULL;
}
static inline void *hao_map_near(uint64_t lo, uint64_t hi, uint64_t size)
{
    return hao_map_near_prot(lo, hi, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_JIT);
}
#endif
