/*
 * Library validation (tests/guest/libval.sh, src/libval.c): which libraries
 * a program may load depends on its own code signature. Linked weakly
 * against libgplug (arm64 only: a guest image under hao); dlopens each
 * argument (libnplug is universal: a native library under hao). Prints
 * loaded/refused per library; the labels, not the paths, so native and
 * hao output compare.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

extern int gplug_value(void) __attribute__((weak_import));

int main(int argc, char **argv)
{
    printf("weak libgplug: %s", gplug_value ? "loaded" : "missing");
    if (gplug_value) printf(" (%d)", gplug_value());
    printf("\n");
    for (int i = 1; i < argc; i++) {
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        void *h = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        printf("dlopen %s: %s", name, h ? "loaded" : "refused");
        int (*f)(void) = h ? (int (*)(void))dlsym(h, "plug_value") : NULL;
        if (f) printf(" (%d)", f());
        printf("\n");
    }
    return 0;
}
