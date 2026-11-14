/* Native code the guest reaches only through pointers it got from dlsym (first-call binds, bridge.c
 * bind_bare): hao checks each target is executable host memory before it calls it (not_host_code).
 * "call": strlen, sqrt and CFAbsoluteTimeGetCurrent through such pointers (prints what native prints).
 * "data": a call through a pointer to native data (libc's optarg, in __DATA); natively a crash,
 * under hao a refusal that names the reason (tests/guest/hostcode.sh). */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
typedef size_t (*len_fn)(const char *);
typedef double (*d_fn)(double);
int main(int argc, char **argv)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (argc > 1 && !strcmp(argv[1], "data")) {
        void (*bad)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "optarg");
        printf("calling data %d\n", bad != NULL);
        fflush(stdout);
        bad();
        return 0;
    }
    len_fn len = (len_fn)dlsym(RTLD_DEFAULT, "strlen");
    d_fn sq = (d_fn)dlsym(RTLD_DEFAULT, "sqrt");
    double (*now)(void) = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    printf("strlen=%zu sqrt=%.1f now>0=%d\n", len("hostcode"), sq(81.0), now && now() > 0);
    return 0;
}
