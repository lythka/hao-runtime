#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PROBE)
static int calls;
int probe(void) {
#if defined(__x86_64__)
    return 86 + ++calls;
#else
    return 64 + ++calls;
#endif
}
#elif defined(HELPER)
int helper_open(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    int (*fn)(void) = h ? dlsym(h, "probe") : NULL;
    return fn ? fn() : -1;
}
#else
int main(int argc, char **argv) {
    if (argc == 1) { puts("main reached"); return 0; }
    if (argc != 4) return 2;
    const char *mode = argv[1], *helper = argv[2], *path = argv[3];
    if (strcmp(mode, "alias")) {
        void *h = dlopen(helper, RTLD_NOW | RTLD_LOCAL);
        int (*fn)(const char *) = h ? dlsym(h, "helper_open") : NULL;
        int value = fn ? fn(path) : -1;
        printf("native=%d\n", value);
        if (value != 87) return 3;
        if (!strcmp(mode, "native")) return 0;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    int (*fn)(void) = h ? dlsym(h, "probe") : NULL;
    int value = fn ? fn() : -1;
    printf("guest=%d\n", value);
    if (value != (!strcmp(mode, "alias") ? 65 : 88)) return 4;
    /* A repeated open must keep the same instance as well. */
    h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    fn = h ? dlsym(h, "probe") : NULL;
    return !fn || fn() != (!strcmp(mode, "alias") ? 66 : 89);
}
#endif
