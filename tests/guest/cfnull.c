/* IOMFB_bics_daemon's start (SIGSEGV in __CF_IS_OBJC on Intel): it reads IODeviceTree:/chosen's
 * "osenvironment" property, which only Apple silicon firmware sets, and calls CFDataGetLength on the result before
 * its NULL check. Native arm64 dies the same way when the property is missing: tests/guest/cfnull.sh. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    mach_port_t mp = 0;
    IOMainPort(0, &mp);
    io_registry_entry_t e = IORegistryEntryFromPath(mp, "IODeviceTree:/chosen");
    CFStringRef key = CFSTR("hao-no-such-property");
    CFTypeRef v = e ? IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0) : NULL;
    printf("chosen=%d property=%s\n", e != 0, v ? "present" : "NULL");
    fflush(stdout);
    if (argc > 1) return 0;
    CFIndex n = CFDataGetLength((CFDataRef)v);   /* as IOMFB_bics_daemon+0x14a8 does */
    printf("length=%ld\n", (long)n);
    return 0;
}
