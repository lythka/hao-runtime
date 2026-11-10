#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <libproc.h>
int main(int argc, char **argv) { @autoreleasepool {
    char p[1024]; uint32_t n = sizeof p; _NSGetExecutablePath(p, &n);
    char pp[PROC_PIDPATHINFO_MAXSIZE]; proc_pidpath(getpid(), pp, sizeof pp);
    char nm[64]; proc_name(getpid(), nm, sizeof nm);
    printf("argv0=%s\nprogname=%s\nNSGetExecutablePath=%s\nproc_pidpath=%s\nproc_name=%s\n", argv[0], getprogname(), p, pp, nm);
    printf("processName=%s\nmainBundle.executablePath=%s\narguments0=%s\n", NSProcessInfo.processInfo.processName.UTF8String,
           NSBundle.mainBundle.executablePath.UTF8String, NSProcessInfo.processInfo.arguments[0].UTF8String);
} return 0; }
