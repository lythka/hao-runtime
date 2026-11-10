/*
 * hao — the arm64 commpage for translated code.
 *
 * arm64 code reads kernel-exported values from fixed addresses inline (no
 * call): xnu osfmk/arm/cpu_capabilities.h, _COMM_PAGE64_BASE_ADDRESS
 * 0xFFFFFC000 and the read-only page _COMM_PAGE64_RO_ADDRESS 0xFFFFF4000.
 * On x86_64 nothing is mapped there (the x86 commpage is at 0x7fffffe00000),
 * so e.g. xpcproxy's _COMM_PAGE_DEV_FIRM check (RO+0x84) faulted in every
 * spawn. This maps both pages and fills the static fields
 * from this machine; values a translated process cannot keep current (user
 * timebase, approximate time, feature bits) are 0, which makes arm64 code
 * take its syscall or generic path.
 */
#include "hao/runtime.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef CP_BASE                     /* tests/host/commpage.c relocates both pages */
#define CP_BASE 0x0000000FFFFFC000ULL
#define CP_RO   0x0000000FFFFF4000ULL
#endif
#define CP_SIZE 0x4000              /* one arm64 16 KiB page each */

static void put8(uint8_t *p, unsigned off, uint8_t v) { p[off] = v; }
static void put16(uint8_t *p, unsigned off, uint16_t v) { memcpy(p + off, &v, 2); }
static void put32(uint8_t *p, unsigned off, uint32_t v) { memcpy(p + off, &v, 4); }
static void put64(uint8_t *p, unsigned off, uint64_t v) { memcpy(p + off, &v, 8); }

static uint64_t sysctl64(const char *name, uint64_t dflt)
{
    uint64_t v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0) != 0) return dflt;
    if (n == 4) return (uint32_t)v;
    return v;
}

static void fill(uint8_t *p)
{
    unsigned ncpu = (unsigned)sysctl64("hw.ncpu", 1);
    memcpy(p + 0x000, "commpage 64-bit", 16);                  /* _COMM_PAGE_SIGNATURE */
    put64(p, 0x010, 0);                                        /* _COMM_PAGE_CPU_CAPABILITIES64: no optional arm features */
    put16(p, 0x01E, 3);                                        /* _COMM_PAGE_VERSION (_COMM_PAGE_THIS_VERSION) */
    put32(p, 0x020, 0);                                        /* _COMM_PAGE_CPU_CAPABILITIES */
    put8(p, 0x022, (uint8_t)ncpu);                             /* _COMM_PAGE_NCPUS */
    put8(p, 0x024, 12);                                        /* _COMM_PAGE_USER_PAGE_SHIFT_32 (RO) / _LEGACY */
    put8(p, 0x025, 12);                                        /* _COMM_PAGE_USER_PAGE_SHIFT_64: x86_64 pages are 4 KiB */
    put16(p, 0x026, (uint16_t)sysctl64("hw.cachelinesize", 64)); /* _COMM_PAGE_CACHE_LINESIZE */
    put8(p, 0x02F, 1);                                         /* _COMM_PAGE_CPU_CLUSTERS */
    put32(p, 0x030, 0);                                        /* _COMM_PAGE_MEMORY_PRESSURE */
    put8(p, 0x034, (uint8_t)sysctl64("hw.activecpu", ncpu));   /* _COMM_PAGE_ACTIVE_CPUS */
    put8(p, 0x035, (uint8_t)sysctl64("hw.physicalcpu_max", ncpu)); /* _COMM_PAGE_PHYSICAL_CPUS */
    put8(p, 0x036, (uint8_t)sysctl64("hw.logicalcpu_max", ncpu));  /* _COMM_PAGE_LOGICAL_CPUS */
    put8(p, 0x037, 12);                                        /* _COMM_PAGE_KERNEL_PAGE_SHIFT (RO) / _LEGACY */
    put64(p, 0x038, sysctl64("hw.memsize", 0));                /* _COMM_PAGE_MEMORY_SIZE */
    put32(p, 0x080, 0);                                        /* _COMM_PAGE_CPUFAMILY: unknown -> generic code */
    put32(p, 0x084, 0);                                        /* _COMM_PAGE_DEV_FIRM (RO) / _LEGACY: production */
    put8(p, 0x090, 0);                                         /* _COMM_PAGE_USER_TIMEBASE: use the syscall */
    put8(p, 0x091, 0);                                         /* _COMM_PAGE_CONT_HWCLOCK */
    put8(p, 0x092, 0);                                         /* _COMM_PAGE_DTRACE_DOF_ENABLED */
    put8(p, 0x0C8, 0);                                         /* _COMM_PAGE_APPROX_TIME_SUPPORTED */
    put32(p, 0x100, 0);                                        /* _COMM_PAGE_KDEBUG_ENABLE */
    put32(p, 0x104, 0);                                        /* _COMM_PAGE_ATM_DIAGNOSTIC_CONFIG */
    put32(p, 0x108, (uint32_t)sysctl64("kern.multiuser_config", 0)); /* _COMM_PAGE_MULTIUSER_CONFIG */
    put64(p, 0x160, sysctl64("kern.dyld_system_flags", 0));    /* _COMM_PAGE_DYLD_FLAGS */
}

/* The contents, built once at startup (sysctl is not for signal context), and
 * what happened to each page: the fault path below reports it. */
static uint8_t tmpl[CP_SIZE];
static struct { int state; uint64_t got; } st[2];   /* 1 mapped, -1 address taken (got: where mmap put it), -2 mmap failed */
static const uint64_t at[2] = {CP_BASE, CP_RO};

static bool map_page(int k)
{
    mach_vm_address_t a = at[k];
    /* VM_FLAGS_FIXED without OVERWRITE: fails if anything is mapped there */
    if (mach_vm_allocate(mach_task_self(), &a, CP_SIZE, VM_FLAGS_FIXED) != KERN_SUCCESS || a != at[k]) return false;
    memcpy((void *)(uintptr_t)a, tmpl, CP_SIZE);
    mprotect((void *)(uintptr_t)a, CP_SIZE, PROT_READ);
    return true;
}

/* Map and fill both pages; quietly does nothing if the addresses are taken. */
void hao_commpage_init(void)
{
    fill(tmpl);
    for (int k = 0; k < 2; k++) {
        if (map_page(k)) { st[k].state = 1; continue; }
        /* never replace something already mapped there: report where a hint would have gone */
        void *p = mmap((void *)(uintptr_t)at[k], CP_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        st[k].state = p == MAP_FAILED ? -2 : -1;
        st[k].got = p == MAP_FAILED ? 0 : (uint64_t)(uintptr_t)p;
        if (p != MAP_FAILED) munmap(p, CP_SIZE);
        /* e.g. under Rosetta on Apple silicon, where the host's own arm64 commpage is there */
        if (hao_verbose) fprintf(stderr, "hao: arm64 commpage address %#llx is in use; not emulated\n", (unsigned long long)at[k]);
    }
}

/* SIGSEGV/SIGBUS handler (runtime.c on_fault), for any fault address: true if
 * addr is in a commpage page that is not mapped now, which is then mapped and
 * filled (the access is retried). Whatever left the page unmapped -- taken at
 * startup, or unmapped since -- is reported once per page on the diagnostic
 * descriptor. Only system calls and the prebuilt contents: signal context. */
bool hao_commpage_fault(uint64_t addr)
{
    for (int k = 0; k < 2; k++) {
        if (addr < at[k] || addr - at[k] >= CP_SIZE) continue;
        mach_vm_address_t r = at[k];
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        bool occupied = mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt,
                                       &obj) == KERN_SUCCESS && r <= at[k];
        static int told[2];
        char b[240];
        int n;
        if (!occupied && map_page(k)) {
            if (!__atomic_exchange_n(&told[k], 1, __ATOMIC_RELAXED)) {
                n = snprintf(b, sizeof b, "hao: arm64 commpage %#llx was not mapped (startup: %s %#llx); mapped at first use\n",
                             (unsigned long long)at[k], st[k].state == 1 ? "mapped, unmapped since" : st[k].state == -1 ? "taken, hint gave" : "mmap failed",
                             (unsigned long long)st[k].got);
                if (n > 0) hao_diag_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
            }
            return true;
        }
        if (!__atomic_exchange_n(&told[k], 1, __ATOMIC_RELAXED)) {
            n = snprintf(b, sizeof b, "hao: arm64 commpage %#llx: fault at %#llx; region %#llx+%#llx prot %d/%d (startup: %d %#llx)\n",
                         (unsigned long long)at[k], (unsigned long long)addr, (unsigned long long)r, (unsigned long long)sz,
                         occupied ? info.protection : -1, occupied ? info.max_protection : -1, st[k].state, (unsigned long long)st[k].got);
            if (n > 0) hao_diag_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
        }
        return false;
    }
    return false;
}
