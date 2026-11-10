/*
 * Native side of tests/guest/swiftentry.swift: what ExtensionFoundation's
 * +[_EXUtil _getEntryPointFunction] does with the main executable. Built for
 * x86_64 (hao loads it natively) and arm64 (the native reference run).
 */
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <ptrauth.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const struct mach_header *_dyld_get_prog_image_header(void);   /* libdyld (dyld_priv.h) */
typedef int (*main_fn)(int, char **);

int swiftentry_call(void)
{
    unsigned long n = 0;
    uint8_t *p = getsectiondata((const void *)_dyld_get_prog_image_header(), "__TEXT", "__swift5_entry", &n);
    if (!p || n <= 3) { printf("no __swift5_entry\n"); return -1; }
    int32_t rel;
    memcpy(&rel, p, 4);
    main_fn fn = (main_fn)ptrauth_sign_unauthenticated((void *)(p + rel), ptrauth_key_function_pointer, 0);
    char arg0[] = "swiftentry", *argv[] = {arg0, NULL};
    fflush(stdout);
    return fn(1, argv);
}
