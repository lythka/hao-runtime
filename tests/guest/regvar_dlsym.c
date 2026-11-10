/* tests/guest/regvar.sh, built for x86_64: for each "NAME BASE" line on stdin (C names), whether
 * the native x86_64 runtimes define NAME and BASE (dlsym through RTLD_DEFAULT once libobjc and
 * libswiftCore are loaded): "NAME 0|1 BASE 0|1". */
#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
    if (!dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_NOW) || !dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW)) return 2;
    char name[256], base[256];
    while (scanf("%255s %255s", name, base) == 2)
        printf("%s %d %s %d\n", name, dlsym(RTLD_DEFAULT, name) != NULL, base, dlsym(RTLD_DEFAULT, base) != NULL);
    return 0;
}
