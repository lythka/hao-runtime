/* The process's load-time identity as dyld has it (tests/tools/platform.sh):
 * the platform, sdk and minos dyld took from the main executable, whether the
 * Mac Catalyst-only library the program links was loaded, and whether another
 * one can be opened. Both libraries need only libSystem (no display). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t dyld_get_active_platform(void);
extern uint32_t dyld_get_program_sdk_version(void);
extern uint32_t dyld_get_program_min_os_version(void);

#define IOS_PF "/System/iOSSupport/System/Library/PrivateFrameworks/"
#define LINKED IOS_PF "AccessibilitySharedUISupport.framework/Versions/A/AccessibilitySharedUISupport"
#define OPENED IOS_PF "UserNotificationsExtensionPoint.framework/Versions/A/UserNotificationsExtensionPoint"

int main(void) {
    uint32_t s = dyld_get_program_sdk_version(), m = dyld_get_program_min_os_version();
    printf("platform %u sdk %u.%u minos %u.%u\n", dyld_get_active_platform(), s >> 16, (s >> 8) & 255, m >> 16,
           (m >> 8) & 255);
    printf("linked %s\n", dlopen(LINKED, RTLD_NOLOAD) ? "loaded" : "not loaded");
    void *h = dlopen(OPENED, RTLD_NOW);
    const char *e = h ? NULL : dlerror();
    printf("dlopen %s\n", h ? "ok" : e && strstr(e, "wrong platform") ? "failed: wrong platform" : "failed");
    return 0;
}
