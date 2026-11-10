/* Shared by the guest-dylib tests: every initializer reports its arguments. */
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct ProgramVars { const void *mh; int *NXArgcPtr; char ***NXArgvPtr; char ***environPtr; const char **__prognamePtr; };
#define INIT(tag)                                                                                                   \
    __attribute__((constructor)) static void init_##tag(int argc, char **argv, char **envp, char **apple,          \
                                                         const struct ProgramVars *pv)                             \
    {                                                                                                               \
        (void)envp; (void)apple;                                                                                    \
        printf("init %s: argc=%d argv0=%s progname=%s mh=%s\n", #tag, argc, argv && argv[0] ? strrchr(argv[0], '/') + 1 : "?", \
               getprogname(), pv && pv->mh == (const void *)_dyld_get_image_header(0) ? "main" : "other");       \
        init_body_##tag();                                                                                          \
    }
