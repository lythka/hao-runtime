/* App-sandboxed probe (tests/tools/stubdeps.sh): is the process sandboxed, and
 * may it look up launchservicesd? The app sandbox allows that lookup only when
 * CoreFoundation was loaded at launch (when libsecinit set the sandbox up).
 * Built with Foundation (ObjC, -DFOUNDATION) or as plain C. */
#include <servers/bootstrap.h>
#include <stdio.h>
#include <unistd.h>
#ifdef FOUNDATION
#import <Foundation/Foundation.h>
#endif
extern int sandbox_check(pid_t, const char *, int, ...);
int main(void)
{
    mach_port_t p = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port, "com.apple.coreservices.launchservicesd", &p);
    printf("sandboxed=%d launchservicesd kr=%d\n", sandbox_check(getpid(), NULL, 0) == 1, kr);
#ifdef FOUNDATION
    @autoreleasepool {
        printf("home=%s\n", [NSHomeDirectory() length] ? "set" : "empty");
    }
#endif
    return 0;
}
