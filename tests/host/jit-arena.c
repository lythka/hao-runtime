/* Redirect hinted anonymous data 8 GB away from JIT memory. The old two-mapping
 * allocator loses its cache; the arena must retain working rel32 references and
 * disjoint RX/RW pages. No process entitlement or actual heap policy is assumed. */
#include "hao/runtime.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

extern uintptr_t arena;
extern size_t arena_size;
extern unsigned redirected;
extern unsigned fail_stage;

static int protection(uintptr_t a, int want, int max_want)
{
    mach_vm_address_t r = a;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &r, &size, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (object) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && r <= a && a - r < size && info.protection == want &&
           (max_want < 0 || info.max_protection == max_want);
}

static uintptr_t table_ref(uint64_t thunk, unsigned offset)
{
    int32_t d;
    memcpy(&d, (void *)(uintptr_t)(thunk + offset + 3), 4);
    return thunk + offset + 7 + d;
}

int wx_regions(void); /* tests/guest/wx.c: actual backing-object offset checks */
static int alias_control(void)
{
    size_t n = 2 * (size_t)getpagesize();
    uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return 1;
    memory_object_size_t bytes = n;
    mach_port_t entry = MACH_PORT_NULL;
    int bad = 0;
    mach_vm_address_t alias = 0;
    if (mach_make_memory_entry_64(mach_task_self(), &bytes, (uintptr_t)p,
                                 VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL) != KERN_SUCCESS || bytes < n) {
        bad = 1;
        goto out;
    }
    if (mprotect(p, n / 2, PROT_READ | PROT_EXEC)) { bad = 1; goto out; }
    /* Disjoint RX and RW pages of the same object are not writable code aliases. */
    bad += wx_regions() != 0;
    if (mach_vm_map(mach_task_self(), &alias, n / 2, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                    VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) != KERN_SUCCESS) {
        bad++;
        goto out;
    }
    /* But a writable alias of the actual RX page must still be detected. */
    bad += wx_regions() == 0;
    mach_vm_deallocate(mach_task_self(), alias, n / 2);
 out:
    if (entry) mach_port_deallocate(mach_task_self(), entry);
    munmap(p, n);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1) fail_stage = (unsigned)atoi(argv[1]);
    if (fail_stage) {
        bool initialized = hao_jit_init();
        void *(*records)(void) = dlsym(RTLD_DEFAULT, "hao_jit_async_records");
        int bad = initialized || !arena || !records || records() != NULL ||
                  hao_in_code_cache(arena) || hao_wx_covers(arena, arena_size) ||
                  hao_thunk_for(0x7100000000ull) != 0x7100000000ull;
        printf("jit-arena failure stage %u: %s\n", fail_stage, bad ? "FAIL" : "PASS");
        return bad;
    }
    if (!hao_jit_init()) {
        fprintf(stderr, "FAIL: cache lost after %u redirected data mappings\n", redirected);
        return 1;
    }
    /* Resolve dynamically so the regression can also run against the old library. */
    void *(*records)(void) = dlsym(RTLD_DEFAULT, "hao_jit_async_records");
    if (!records) { fprintf(stderr, "FAIL: no arena async pool\n"); return 1; }
    uint8_t *data = records();
    uint64_t lo, hi;
    if (!data || !hao_thunk_range(&lo, &hi)) return 1;
    int translated = 0;
    size_t translated_size = sizeof translated;
    sysctlbyname("sysctl.proc_translated", &translated, &translated_size, NULL, 0);
    unsigned bad = 0;
    for (unsigned i = 0; i < HAO_ASYNC_RECORD_SLOTS; i++) {
        uint64_t guest = 0x7100000000ull + i * 16ull;
        uint64_t t = hao_thunk_for(guest);
        uint64_t s = hao_sig_thunk(guest, (void *)(uintptr_t)(guest + 8));
        if (!s || t == guest) { bad++; break; }
        uintptr_t plain = table_ref(t, 0), sig0 = table_ref(s, 0), sig1 = table_ref(s, 7);
        bad += plain < arena || plain + 8 > arena + arena_size ||
               sig0 < arena || sig1 + 8 > arena + arena_size;
        if (bad) break;
        bad += *(uint64_t *)plain != guest || *(uint64_t *)sig0 != guest || *(uint64_t *)sig1 != guest + 8;
        bad += !protection(t, PROT_READ | PROT_EXEC, translated ? -1 : PROT_READ | PROT_EXEC) ||
               !protection(plain, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
        uint8_t *rec = data + HAO_ASYNC_RECORD_SIZE * i;
        int64_t delta = (int64_t)t - (int64_t)(uintptr_t)rec;
        bad += delta <= INT32_MIN || delta >= INT32_MAX;
        int32_t rel = (int32_t)delta;
        memcpy(rec, &rel, 4);
        bad += (uintptr_t)rec + (int64_t)*(int32_t *)rec != t;
    }
    bad += redirected != 0;
    bad += (uintptr_t)data < arena || (uintptr_t)data + HAO_ASYNC_RECORD_SLOTS * HAO_ASYNC_RECORD_SIZE > arena + arena_size;
    bad += !protection((uintptr_t)data, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
    bad += hao_in_code_cache((uintptr_t)data) || !hao_wx_covers((uintptr_t)data, HAO_ASYNC_RECORD_SIZE);
    /* Even the far ends of the two pools must remain representable. */
    int64_t far = (int64_t)(uintptr_t)(data + HAO_ASYNC_RECORD_SLOTS * HAO_ASYNC_RECORD_SIZE) - (int64_t)lo;
    bad += far >= INT32_MAX || far <= INT32_MIN;
    bad += alias_control();
    /* Tables and records share MAP_JIT backing now: verify inheritance and COW. */
    void (*fork_init)(void) = dlsym(RTLD_DEFAULT, "hao_fork_init");
    if (!fork_init) return 1;
    fork_init();
    uint64_t first = hao_thunk_for(0x7100000000ull);
    pid_t child = fork();
    if (child == 0) {
        alarm(10);
        int wrong = hao_thunk_target(first) != 0x7100000000ull ||
                    !protection(first, PROT_READ | PROT_EXEC, -1) ||
                    !protection((uintptr_t)data, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
        data[12] = 0x5a;
        _exit(wrong);
    }
    int status = 0;
    bad += child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    bad += data[12] != 0;
    printf("jit-arena: redirected=%u bad=%u -> %s\n", redirected, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
