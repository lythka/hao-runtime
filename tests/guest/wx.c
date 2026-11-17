/* W^X (tests/guest/wx.sh): after translated code, chained branches and native->guest callbacks (qsort's
 * comparator, a block run by dispatch) have run, no memory of the process is writable and executable at
 * once. Prints the count; each such region goes to stderr. The program's constant data (__DATA_CONST,
 * a table of pointers that needed fixups) is read-only by the time main runs, as dyld leaves it. With
 * -DWX_LIB only wx_regions, for wx.swift. */
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach/mach_vm.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WX_LIB
extern const struct mach_header_64 __dso_handle;
static int (*const table[])(const void *, const void *);

/* the protection of this image's __DATA_CONST (-1: none found) */
static int const_prot(void)
{
    const struct mach_header_64 *mh = &__dso_handle;
    const struct load_command *lc = (const void *)(mh + 1);
    uint64_t text = 0, lo = 0;
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (!strcmp(sg->segname, "__TEXT")) text = sg->vmaddr;
        if (!strcmp(sg->segname, "__DATA_CONST")) lo = sg->vmaddr;
    }
    if (!lo) return -1;
    mach_vm_address_t a = lo - text + (uint64_t)(uintptr_t)mh;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return -1;
    return info.protection;
}

static int cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int (*const table[])(const void *, const void *) = {cmp};
#endif

int wx_regions(void)
{
    int wx = 0;
    mach_vm_address_t a = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        /* Rosetta's own memory (tags 230-239), present in every x86_64 process on an Apple-silicon Mac */
        bool rosetta = info.user_tag >= 230 && info.user_tag <= 239;
        if ((info.protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == (VM_PROT_WRITE | VM_PROT_EXECUTE) && !rosetta) {
            wx++;
            fprintf(stderr, "wx: [%#llx, %#llx) prot %d max %d tag %u\n", (unsigned long long)a, (unsigned long long)(a + size),
                    info.protection, info.max_protection, info.user_tag);
        }
        a += size;
    }
    return wx;
}

#ifndef WX_LIB
int main(void)
{
    enum { N = 4096 };
    static int v[N];
    for (int i = 0; i < N; i++) v[i] = (i * 7919) % N;
    qsort(v, N, sizeof *v, table[0]);
    __block long sum = 0;
    dispatch_sync(dispatch_get_global_queue(0, 0), ^{ for (int i = 0; i < N; i++) sum += v[i] * (i & 7); });
    printf("sorted %d..%d sum %ld\n", v[0], v[N - 1], sum);
    int cp = const_prot();
    printf("constant data: %s\n", cp < 0 ? "none" : cp & VM_PROT_WRITE ? "writable" : "read-only");
    printf("writable and executable regions: %d\n", wx_regions());
    return 0;
}
#endif
