/*
 * hao — the code identity Security.framework gives a listed hao stub or re-signed native
 * program (include/hao/codeid.h). Built into build/libhao_codeid.a with src/idlist.c; no
 * libhao dependency. The carrier is the image's x86_64 Security.framework.
 */
#include "../include/hao/codeid.h"
#include "../include/hao/idlist.h"
#include <string.h>

/* The cdhash Security computed for the code (kSecCodeInfoUnique), 20 bytes; false if none. */
static bool unique(SecStaticCodeRef code, uint8_t out[20])
{
    CFDictionaryRef info = NULL;
    bool ok = false;
    if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info) == errSecSuccess && info) {
        CFTypeRef u = CFDictionaryGetValue(info, kSecCodeInfoUnique);
        if (u && CFGetTypeID(u) == CFDataGetTypeID() && CFDataGetLength(u) >= 20) {
            memcpy(out, CFDataGetBytePtr(u), 20);
            ok = true;
        }
    }
    if (info) CFRelease(info);
    return ok;
}

/* The listed reference: the slice `arch` of `path`, if its cdhash is the listed one. */
static SecStaticCodeRef reference(const hao_idrow *r)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)r->path, (CFIndex)strlen(r->path), false);
    CFStringRef arch = CFStringCreateWithCString(NULL, r->arch, kCFStringEncodingUTF8);
    SecStaticCodeRef ref = NULL;
    if (url && arch) {
        const void *k[] = {kSecCodeAttributeArchitecture}, *v[] = {arch};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (attrs) {
            if (SecStaticCodeCreateWithPathAndAttributes(url, kSecCSDefaultFlags, attrs, &ref) != errSecSuccess) ref = NULL;
            CFRelease(attrs);
        }
    }
    if (url) CFRelease(url);
    if (arch) CFRelease(arch);
    uint8_t h[20];
    if (ref && (!unique(ref, h) || memcmp(h, r->reference, 20))) {
        CFRelease(ref);
        ref = NULL;
    }
    return ref;
}

OSStatus hao_codeid_decide(SecStaticCodeRef code, SecRequirementRef requirement, OSStatus status, CFErrorRef *errors)
{
    if (status != errSecCSReqFailed || !code || !requirement) return status;
    bool running = CFGetTypeID(code) == SecCodeGetTypeID();
    /* a running code must be its file (the kernel's cdhash is the file's) before its file is looked up */
    if (running && SecCodeCheckValidity((SecCodeRef)code, kSecCSDefaultFlags, NULL) != errSecSuccess) return status;
    uint8_t h[20];
    if (!unique(code, h)) return status;
    const hao_idrow *r = hao_idlist_find(h);
    if (!r) return status;
    SecStaticCodeRef ref = reference(r);
    if (!ref) return status;
    OSStatus v = SecStaticCodeCheckValidityWithErrors(ref, kSecCSDoNotValidateExecutable | kSecCSDoNotValidateResources,
                                                     requirement, NULL);
    CFRelease(ref);
    if (v != errSecSuccess) return status;
    if (errors && *errors) {
        CFRelease(*errors);
        *errors = NULL;
    }
    return errSecSuccess;
}
