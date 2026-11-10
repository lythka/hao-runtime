/* Native helper of tests/guest/swiftls.sh. With SWIFTLS_FILL=1 in the environment its initializer reserves
 * (PROT_NONE) every free range of the address space from 4 GB to 64 GB, as a process whose libraries have
 * filled the 2 GB around its main image before the image's Swift metadata is walked, as in
 * WindowManager, Finder, sharingd, searchpartyd, searchpartyuseragent, identityservicesd and findmylocateagent
 * printed "no island within 2 GB of its Swift metadata". Under hao the helper loads natively, with the
 * program's other dependencies, before the program is initialized; natively it does the same, so the
 * program's output is the same either way. swiftls_filled() is the number of ranges reserved. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include <sys/mman.h>

static int filled;

__attribute__((constructor)) static void fill(void)
{
    const char *e = getenv("SWIFTLS_FILL");
    if (!e || *e != '1') return;
    const mach_vm_address_t limit = 64ull << 30;
    mach_vm_address_t at = 4ull << 30;
    while (at < limit) {
        mach_vm_address_t r = at;
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
        if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
        mach_vm_address_t end = kr == KERN_SUCCESS && r < limit ? r : limit;
        if (end > at) {
            mach_vm_address_t a = at;
            if (mach_vm_allocate(mach_task_self(), &a, end - at, VM_FLAGS_FIXED) == KERN_SUCCESS) {
                mprotect((void *)(uintptr_t)a, end - at, PROT_NONE);
                filled++;
            }
        }
        if (kr != KERN_SUCCESS || r >= limit) break;
        at = r + sz;
    }
}

int swiftls_filled(void) { return filled; }
