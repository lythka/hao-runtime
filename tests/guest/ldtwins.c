/* Native arm64e check of src/libm.c's long double table: each argument is
 * "name:twin" (C names), and the host's arm64e libsystem_m must export name
 * as the same code as twin. Prints one line per pair that is not; exit 1 if
 * any. Run by tests/guest/bridgefix.sh. */
#include <dlfcn.h>
#include <ptrauth.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        char a[128], *b;
        snprintf(a, sizeof a, "%s", argv[i]);
        if (!(b = strchr(a, ':'))) continue;
        *b++ = 0;
        void *pa = dlsym(RTLD_DEFAULT, a), *pb = dlsym(RTLD_DEFAULT, b);
        if (pa) pa = ptrauth_strip(pa, ptrauth_key_function_pointer);
        if (pb) pb = ptrauth_strip(pb, ptrauth_key_function_pointer);
        if (!pa || pa != pb) {
            printf("ldtwins: %s is not %s (%p, %p)\n", a, b, pa, pb);
            bad++;
        }
    }
    printf("ldtwins: %d pairs, %d not aliases\n", argc - 1, bad);
    return bad != 0;
}
