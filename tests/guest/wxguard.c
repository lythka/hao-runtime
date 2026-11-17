/* A guest cannot change the protection or mapping of hao's executable memory or of its own sealed
 * constant data (tests/guest/wxguard.sh). Anonymous executable memory that is no file's (under hao: the
 * code cache, thunks, islands; natively there is none) is found by walking the address space; each call
 * that would make it writable, unmap it or map over it is tried on one page: libc mprotect, munmap and
 * mmap MAP_FIXED, mach_vm_protect, vm_protect, mach_vm_deallocate, mach_vm_allocate with
 * VM_FLAGS_OVERWRITE, and the raw mprotect system call. Then mprotect of __DATA_CONST read/write
 * (natively allowed). Prints what was refused; the page's protection afterwards goes to stderr. */
#include <errno.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>

extern const struct mach_header_64 __dso_handle;

static int raw_mprotect(void *a, size_t n, int prot)
{
    register long x0 __asm__("x0") = (long)a, x1 __asm__("x1") = (long)n, x2 __asm__("x2") = prot;
    register long x16 __asm__("x16") = SYS_mprotect;
    long carry;
    __asm__ volatile("svc #0x80\n cset %[c], cs" : "+r"(x0), [c] "=r"(carry) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return carry ? (int)x0 : 0;
}

/* the first anonymous executable region, or 0 */
static uint64_t anon_exec(void)
{
    mach_vm_address_t a = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            return 0;
        bool rosetta = info.user_tag >= 230 && info.user_tag <= 239;
        if ((info.protection & VM_PROT_EXECUTE) && !info.external_pager && !info.is_submap && !rosetta && depth == 0) return a;
        a += size;
    }
}

static int prot_of(uint64_t p)
{
    mach_vm_address_t a = p;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS || a > p)
        return -1;
    return info.protection;
}

static uint64_t data_const(void)
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
    return lo ? lo - text + (uint64_t)(uintptr_t)mh : 0;
}

static int (*const keep[])(void) = {0};   /* something in __DATA_CONST */

int main(void)
{
    (void)keep;
    uint64_t p = anon_exec();
    if (!p) printf("anonymous executable memory: none\n");
    else {
        int before = prot_of(p), refused = 0, tried = 0;
        mach_vm_address_t at = p;
        tried++; refused += mprotect((void *)p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) && errno == EACCES;
        tried++; refused += mprotect((void *)p, 4096, PROT_READ | PROT_WRITE) && errno == EACCES;
        tried++; refused += raw_mprotect((void *)p, 4096, PROT_READ | PROT_WRITE) == EACCES;
        tried++; refused += mach_vm_protect(mach_task_self(), p, 4096, FALSE, VM_PROT_ALL) == KERN_PROTECTION_FAILURE;
        tried++; refused += vm_protect(mach_task_self(), (vm_address_t)p, 4096, FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_PROTECTION_FAILURE;
        tried++; refused += mach_vm_deallocate(mach_task_self(), p, 4096) == KERN_PROTECTION_FAILURE;
        tried++; refused += mach_vm_allocate(mach_task_self(), &at, 4096, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_PROTECTION_FAILURE;
        tried++; refused += munmap((void *)p, 4096) && errno == EACCES;
        tried++; refused += mmap((void *)p, 4096, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED && errno == EACCES;
        fprintf(stderr, "wxguard: %#llx prot %d before, %d after\n", (unsigned long long)p, before, prot_of(p));
        printf("anonymous executable memory: changes refused %d of %d, protection kept %s\n", refused, tried,
               prot_of(p) == before ? "yes" : "no");
    }
    uint64_t d = data_const();
    int r = d ? mprotect((void *)(d & ~4095ull), 4096, PROT_READ | PROT_WRITE) : -2;
    printf("__DATA_CONST made writable: %s\n", r == -2 ? "none" : r == 0 ? "yes" : errno == EACCES ? "refused" : "failed");
    return 0;
}
