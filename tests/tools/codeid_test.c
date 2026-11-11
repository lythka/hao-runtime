// tests/tools/codeid.sh: hao_codeid_decide (src/code_identity.c) on a static file or a running process.
//   codeid_test LISTDIR static FILE ARCH|- REQUIREMENT|-
//   codeid_test LISTDIR pid PID REQUIREMENT|-
//   codeid_test LISTDIR status FILE ARCH REQUIREMENT STATUS    (decide with a given original status)
// Prints "original=<status> decided=<status>".
#include "hao/codeid.h"
#include "hao/idlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SecRequirementRef req(const char *s)
{
    if (!strcmp(s, "-")) return NULL;
    SecRequirementRef r = NULL;
    CFStringRef t = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    if (SecRequirementCreateWithString(t, kSecCSDefaultFlags, &r)) { fprintf(stderr, "bad requirement %s\n", s); exit(2); }
    CFRelease(t);
    return r;
}

static SecStaticCodeRef file(const char *path, const char *arch)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CFDictionaryRef attrs = NULL;
    if (strcmp(arch, "-")) {
        CFStringRef a = CFStringCreateWithCString(NULL, arch, kCFStringEncodingUTF8);
        const void *k[] = {kSecCodeAttributeArchitecture}, *v[] = {a};
        attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    SecStaticCodeRef c = NULL;
    if (SecStaticCodeCreateWithPathAndAttributes(url, kSecCSDefaultFlags, attrs, &c)) { fprintf(stderr, "no static code %s\n", path); exit(2); }
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    hao_idlist_use_dir_for_test(argv[1]);
    OSStatus st;
    SecStaticCodeRef code;
    SecRequirementRef r;
    if (!strcmp(argv[2], "pid")) {
        int pid = atoi(argv[3]);
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &pid);
        const void *k[] = {kSecGuestAttributePid}, *v[] = {n};
        CFDictionaryRef a = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        SecCodeRef c = NULL;
        if (SecCodeCopyGuestWithAttributes(NULL, a, kSecCSDefaultFlags, &c)) { fprintf(stderr, "no code for pid %d\n", pid); return 2; }
        code = (SecStaticCodeRef)c;
        r = req(argv[4]);
        st = SecCodeCheckValidity(c, kSecCSDefaultFlags, r);
    } else {
        if (argc < 6) return 2;
        code = file(argv[3], argv[4]);
        r = req(argv[5]);
        st = !strcmp(argv[2], "status") && argc > 6 ? (OSStatus)atoi(argv[6]) : SecStaticCodeCheckValidityWithErrors(code, kSecCSDefaultFlags, r, NULL);
    }
    CFErrorRef err = CFErrorCreate(NULL, kCFErrorDomainOSStatus, st, NULL);
    OSStatus d = hao_codeid_decide(code, r, st, &err);
    printf("original=%d decided=%d errors=%s\n", (int)st, (int)d, err ? "kept" : "cleared");
    return 0;
}
