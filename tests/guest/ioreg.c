#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <stdio.h>

int main(void)
{
    io_registry_entry_t entry = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen");
    assert(entry);
    CFMutableDictionaryRef properties = NULL;
    assert(IORegistryEntryCreateCFProperties(entry, &properties, NULL, 0) == KERN_SUCCESS);
    const CFStringRef keys[] = {CFSTR("mac-address-wifi0"), CFSTR("boot-uuid"), CFSTR("board-id"), CFSTR("hao-test-no-such-property")};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        CFTypeRef single = IORegistryEntryCreateCFProperty(entry, keys[i], NULL, 0);
        CFTypeRef search = IORegistryEntrySearchCFProperty(entry, "IODeviceTree", keys[i], NULL, 0);
        CFTypeRef dictionary = CFDictionaryGetValue(properties, keys[i]);
        assert((single != NULL) == (search != NULL));
        assert((single != NULL) == (dictionary != NULL));
        if (single) {
            assert(CFEqual(single, search) && CFEqual(single, dictionary));
            if (i == 0) { assert(CFGetTypeID(single) == CFDataGetTypeID()); assert(CFDataGetLength(single) == 6); }
            CFRelease(single); CFRelease(search);
        }
    }
    assert(!CFDictionaryGetValue(properties, keys[3]));
    CFRelease(properties); IOObjectRelease(entry);
    puts("ioreg: single, dictionary and nonrecursive search agree");
    return 0;
}
