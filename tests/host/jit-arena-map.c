/* Test-only mmap redirection; loaded before libhao. */
#include <stdint.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
uintptr_t arena;
size_t arena_size;
unsigned redirected;
unsigned fail_stage;
unsigned denied_maps;
static void *redirect_mmap(void *hint, size_t size, int prot, int flags, int fd, off_t off)
{
    if (fail_stage == 3 && !hint && (flags & MAP_ANON) && !(flags & MAP_JIT)) {
        denied_maps++;
        return MAP_FAILED;
    }
    if (arena && hint && (flags & MAP_ANON) && !(flags & (MAP_JIT | MAP_FIXED))) {
        /* Reserve without overwriting anything, then replace only our reservation. */
        for (unsigned k = 8; k < 24; k++) {
            mach_vm_address_t a = (arena + ((uint64_t)k << 30) + 4095) & ~4095ull;
            if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS) continue;
            void *p = mmap((void *)(uintptr_t)a, size, prot, flags | MAP_FIXED, fd, off);
            if (p == MAP_FAILED) mach_vm_deallocate(mach_task_self(), a, size);
            else redirected++;
            return p;
        }
        abort();
    }
    void *p = mmap(hint, size, prot, flags, fd, off);
    if ((flags & MAP_JIT) && p != MAP_FAILED && !arena) { arena = (uintptr_t)p; arena_size = size; }
    return p;
}
static kern_return_t deny_protect(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size,
                                  boolean_t maximum, vm_prot_t prot)
{
    if (fail_stage == 1 && arena && address >= arena && address - arena < arena_size && maximum &&
        prot == (VM_PROT_READ | VM_PROT_WRITE)) return KERN_PROTECTION_FAILURE;
    return mach_vm_protect(task, address, size, maximum, prot);
}
static kern_return_t deny_entry(vm_map_t task, memory_object_size_t *size, memory_object_offset_t offset,
                                vm_prot_t permission, mach_port_t *entry, mach_port_t parent)
{
    if (fail_stage == 2 && arena && offset == arena) return KERN_PROTECTION_FAILURE;
    return mach_make_memory_entry_64(task, size, offset, permission, entry, parent);
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } interposers[] = {
    {redirect_mmap, mmap}, {deny_protect, mach_vm_protect}, {deny_entry, mach_make_memory_entry_64}
};
