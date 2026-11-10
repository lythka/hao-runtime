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
#include <stdio.h>
#include <stdlib.h>

typedef void (*reg_fn)(const void *begin, const void *end);

/* The registration entry points are looked up only for an image that has
 * Swift metadata, and libswiftCore is opened RTLD_LOCAL: a program without
 * Swift (launchd, C tools) must not get the Swift runtime loaded, initialized
 * and visible to flat lookups. A Swift guest's own dependency on
 * libswiftCore already loads it natively. */
void hao_swift_register_image(hao_image *img)
{
    if (hao_opt.no_swiftreg) return;
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
