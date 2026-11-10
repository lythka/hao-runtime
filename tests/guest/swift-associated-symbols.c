#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    const char *names[] = {
        "$s16AssociatedNative0A0P5first5FirstQzvgTq",
        "$s16AssociatedNative0A0P5first5FirstQzvgTj",
        "$s16AssociatedNative0A0P6second6SecondQzvgTq",
        "$s16AssociatedNative0A0P6second6SecondQzvgTj",
    };
    for (unsigned k = 0; k < 4; k++) {
        void *p = dlsym(h, names[k]);
        Dl_info info = {0};
        if (!p || !dladdr(p, &info) || info.dli_saddr == p) return 3;
    }
    puts("four exact exports, no exact dladdr names");
    return 0;
}
