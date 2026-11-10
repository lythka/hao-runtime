/* sigcheck PATH ARCH: what the Security framework reads from one architecture
 * of a signed program or bundle, statically (nothing is launched):
 *   valid=<SecStaticCodeCheckValidity OSStatus>, strict=<the same with kSecCSStrictValidate>
 *   identifier=<kSecCodeInfoIdentifier>
 *   plist.CFBundleIdentifier=<kSecCodeInfoPList's CFBundleIdentifier, or (none)>
 *   plist.entries=<count, or 0>
 *   resources=<1 if the internal information has a ResourceDirectory (the seal)>
 * ARCH is x86_64 or arm64e. libsecinit reads the bundle identifier of an app
 * sandbox's container the same way (kSecCodeInfoPList of the running code). */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <mach/machine.h>
#include <stdio.h>
#include <string.h>

static void pr(const char *k, CFTypeRef v) {
    char buf[1024] = "(none)";
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
    printf("%s=%s\n", k, buf);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: sigcheck PATH x86_64|arm64e\n");
        return 2;
    }
    int cpu = strcmp(argv[2], "x86_64") == 0 ? CPU_TYPE_X86_64 : CPU_TYPE_ARM64;
    int sub = strcmp(argv[2], "arm64e") == 0 ? CPU_SUBTYPE_ARM64E : 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], strlen(argv[1]), false);
    CFNumberRef c = CFNumberCreate(NULL, kCFNumberIntType, &cpu), s = CFNumberCreate(NULL, kCFNumberIntType, &sub);
    const void *keys[] = {kSecCodeAttributeArchitecture, kSecCodeAttributeSubarchitecture};
    const void *vals[] = {c, s};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, sub ? 2 : 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    /* A fresh code object per check: one caches its validation result. */
    SecStaticCodeRef code = NULL, strict = NULL, info_code = NULL;
    OSStatus st = SecStaticCodeCreateWithPathAndAttributes(url, kSecCSDefaultFlags, attrs, &code);
    if (!st)
        st = SecStaticCodeCreateWithPathAndAttributes(url, kSecCSDefaultFlags, attrs, &strict);
    if (!st)
        st = SecStaticCodeCreateWithPathAndAttributes(url, kSecCSDefaultFlags, attrs, &info_code);
    if (st) {
        printf("create=%d\n", (int)st);
        return 1;
    }
    printf("valid=%d\n", (int)SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, NULL));
    printf("strict=%d\n", (int)SecStaticCodeCheckValidity(strict, kSecCSStrictValidate, NULL));
    CFDictionaryRef info = NULL;
    st = SecCodeCopySigningInformation(info_code, kSecCSSigningInformation | kSecCSInternalInformation, &info);
    if (st || !info) {
        printf("info=%d\n", (int)st);
        return 1;
    }
    pr("identifier", CFDictionaryGetValue(info, kSecCodeInfoIdentifier));
    CFDictionaryRef pl = CFDictionaryGetValue(info, kSecCodeInfoPList);
    pr("plist.CFBundleIdentifier",
       pl && CFGetTypeID(pl) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(pl, kCFBundleIdentifierKey) : NULL);
    printf("plist.entries=%ld\n", pl && CFGetTypeID(pl) == CFDictionaryGetTypeID() ? (long)CFDictionaryGetCount(pl) : 0L);
    printf("resources=%d\n", CFDictionaryGetValue(info, CFSTR("ResourceDirectory")) != NULL);
    return 0;
}
