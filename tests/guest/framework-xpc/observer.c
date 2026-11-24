#include <dlfcn.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* dyld_priv.h declares an immediate replay of all currently loaded images. */
extern void _dyld_register_for_bulk_image_loads(void (*)(
    unsigned, const struct mach_header *[], const char *[]));

static struct stat framework, observer;
static atomic_uint batches, self_matches, framework_matches;

static void record(unsigned count, const struct mach_header *headers[], const char *paths[]) {
    atomic_fetch_add(&batches, 1);
    for (unsigned i = 0; i < count; ++i) {
        struct stat image;
        if (!paths[i] || stat(paths[i], &image)) continue;
        if (image.st_dev == observer.st_dev && image.st_ino == observer.st_ino)
            atomic_fetch_add(&self_matches, 1);
        if (image.st_dev == framework.st_dev && image.st_ino == framework.st_ino) {
            atomic_fetch_add(&framework_matches, 1);
            fprintf(stderr, "bulk framework cpu=%d path=%s\n", headers[i]->cputype, paths[i]);
        }
    }
}

__attribute__((constructor))
static void observe(void) {
    Dl_info info;
    char path[PATH_MAX];
    if (!dladdr((const void *)&observe, &info) || !realpath(info.dli_fname, path) ||
        stat(path, &observer)) goto failed;
    char *name = strrchr(path, '/');
    if (!name) goto failed;
#if defined(OPEN_FIXTURE_ADAPTER) || defined(DECLARED_FIXTURE_ADAPTER)
    const char leaf[] = "/HaoOwnedDiscovery.framework/Versions/A/HaoOwnedDiscovery";
#else
    const char leaf[] = "/ProbeKit.framework/Versions/A/ProbeKit";
#endif
    if ((size_t)(name - path) + sizeof(leaf) > sizeof(path)) goto failed;
    memcpy(name, leaf, sizeof(leaf));
    if (stat(path, &framework)) goto failed;
#ifdef OPEN_FIXTURE_ADAPTER
    void *adapter = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!adapter || dlsym(adapter, "fixture_service_name")) goto failed;
    fputs("native adapter open; framework implementation absent\n", stderr);
#endif
    _dyld_register_for_bulk_image_loads(record);
    fprintf(stderr, "native bulk replay batches=%u self=%u framework=%u expected=%s\n",
            atomic_load(&batches), atomic_load(&self_matches), atomic_load(&framework_matches), path);
    return;
failed:
    fputs("native bulk observer could not identify fixture files\n", stderr);
}
