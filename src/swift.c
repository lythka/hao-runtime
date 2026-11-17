/*
 * hao — register a guest image's Swift metadata with the native runtime.
 * The native Swift runtime learns about dyld-loaded images through image
 * callbacks; guest images are not loaded by dyld, so their protocol
 * conformances, type metadata records and protocol descriptors are handed
 * over explicitly (the same entry points static linking uses). Records are
 * relative pointers, valid wherever the guest image is mapped.
 */
#include "hao/runtime.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

typedef void (*reg_fn)(const void *begin, const void *end);

/* Swift.Float16's metadata once its 2-byte POD witnesses are installed and
 * read back (float16_layout); NULL before, or if that failed. */
static const void *float16_relaid;
bool hao_swift_relaid_type(const void *type)
{
    return type && __atomic_load_n(&float16_relaid, __ATOMIC_ACQUIRE) == type;
}

/* Swift.Float16 is a frozen 2-byte POD (Builtin.FPIEEE16) in the arm64 ABI
 * guest code is compiled for. x86_64 macOS libswiftCore declares it as a
 * stub with no stored field: its metadata's value witnesses are the empty
 * tuple's (size 0, stride 1), so generic code, native or guest, sized
 * and copied guest Float16 values as empty. Native x86_64 code
 * cannot use the type, so its metadata is the guest's: point it at the
 * value witnesses of a 2-byte POD (Builtin.Int16's: size and stride 2,
 * alignment 2, no extra inhabitants, as Builtin.FPIEEE16's on arm64).
 * Only a stub is changed: a struct with no fields whose witnesses have
 * size 0. Its members and conformances stay absent (docs/ISSUES.md). */
static void float16_layout(void *h)
{
    uint64_t *md = dlsym(h, "$ss7Float16VN");
    const uint64_t *pod = dlsym(h, "$sBi16_WV");
    if (!md || !pod || md[0] != 0x200 || !md[1]) return;
    const uint64_t *vwt = (const uint64_t *)(uintptr_t)md[-1];
    const uint32_t *desc = (const uint32_t *)(uintptr_t)md[1];
    /* StructDescriptor: flags, parent, name, accessor, fields, NumFields, FieldOffsetVectorOffset */
    if (!vwt || vwt[8] != 0 || desc[5] != 0) return;
    if (pod[8] != 2 || pod[9] != 2 || (uint32_t)pod[10] != 1 || pod[10] >> 32) return;
    long pg = getpagesize();
    uintptr_t lo = (uintptr_t)&md[-1] & ~(uintptr_t)(pg - 1);
    mach_vm_address_t region = lo;
    mach_vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &region, &bytes, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                      &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    int prot = PROT_READ | (kr == KERN_SUCCESS && region <= lo && (info.protection & VM_PROT_WRITE) ? PROT_WRITE : 0);
    if (kr != KERN_SUCCESS || region > lo || mprotect((void *)lo, (size_t)pg, PROT_READ | PROT_WRITE)) {
        if (hao_verbose) fprintf(stderr, "hao: Swift.Float16 metadata not writable; generic Float16 has size 0\n");
        return;
    }
    md[-1] = (uint64_t)(uintptr_t)pod;
    mprotect((void *)lo, (size_t)pg, prot);   /* as it was */
    if (*(volatile uint64_t *)&md[-1] == (uint64_t)(uintptr_t)pod) __atomic_store_n(&float16_relaid, (const void *)md, __ATOMIC_RELEASE);
    if (hao_verbose > 1) fprintf(stderr, "hao: Swift.Float16 metadata: 2-byte POD value witnesses\n");
}

/* The registration entry points are looked up only for an image that has
 * Swift metadata, and libswiftCore is opened RTLD_LOCAL: a program without
 * Swift (launchd, C tools) must not get the Swift runtime loaded, initialized
 * and visible to flat lookups. A Swift guest's own dependency on
 * libswiftCore already loads it natively. */
void hao_swift_register_image(hao_image *img)
{
    if (hao_opt.no_swiftreg) return;
    /* before the image's code runs: once libswiftCore is loaded (a Swift
     * guest's dependency on it loads it natively) */
    static int f16;
    if (!f16) {
        void *h = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
        if (h) {
            f16 = 1;
            float16_layout(h);
            dlclose(h);
        }
    }
    static reg_fn conf, types, protos;
    struct { const char *sect; reg_fn *fn; } tab[] = {
        {"__swift5_protos", &protos}, {"__swift5_types", &types}, {"__swift5_types2", &types}, {"__swift5_proto", &conf},
    };
    bool any = false;
    for (unsigned k = 0; k < sizeof tab / sizeof *tab && !any; k++) {
        uint64_t n;
        any = hao_image_section(img, NULL, tab[k].sect, &n) && n;
    }
    if (!any) return;
    static int init;
    if (!init) {
        init = 1;
        void *h = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            conf = (reg_fn)dlsym(h, "swift_registerProtocolConformances");
            types = (reg_fn)dlsym(h, "swift_registerTypeMetadataRecords");
            protos = (reg_fn)dlsym(h, "swift_registerProtocols");
        }
    }
    for (unsigned k = 0; k < sizeof tab / sizeof *tab; k++) {
        uint64_t n, a = hao_image_section(img, NULL, tab[k].sect, &n);
        if (!a || !n || !*tab[k].fn) continue;
        (*tab[k].fn)((const void *)(uintptr_t)a, (const void *)(uintptr_t)(a + n));
        if (hao_verbose > 1) fprintf(stderr, "hao: swift %s: %llu bytes registered\n", tab[k].sect, (unsigned long long)n);
    }
}
