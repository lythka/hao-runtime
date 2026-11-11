// Guest calls of native CoreUtils functions that take a va_list (src/interpose.c vlists):
// a guest variadic wrapper hands its arm64 va_list (a pointer to 8-byte stack slots) to
// VSNPrintF, VASPrintF, NSPrintV and NSErrorV. Without the conversion x86_64 CoreUtils reads
// the guest's slots as an x86_64 va_list and faults at the first pointer conversion.
// Output must equal native arm64 (tests/guest/cuvalist.sh).
#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int VSNPrintF(char *buf, size_t size, const char *fmt, va_list args);     /* CoreUtils */
int VASPrintF(char **out, const char *fmt, va_list args);
NSString *NSPrintV(const char *fmt, va_list args);
NSError *NSErrorV(NSErrorDomain domain, int32_t code, const char *fmt, va_list args);

static const unsigned char mac[6] = {0x00, 0x1b, 0x63, 0x84, 0x45, 0xe6};

__attribute__((noinline)) static void vsn(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = VSNPrintF(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("VSNPrintF %d [%s]\n", n, b);
}

__attribute__((noinline)) static void vas(const char *fmt, ...)
{
    char *s = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = VASPrintF(&s, fmt, ap);
    va_end(ap);
    printf("VASPrintF %d [%s]\n", n, s ? s : "(null)");
    free(s);
}

__attribute__((noinline)) static void nsp(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *s = NSPrintV(fmt, ap);
    va_end(ap);
    printf("NSPrintV [%s]\n", s.UTF8String);
}

__attribute__((noinline)) static void nse(int32_t code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSError *e = NSErrorV(NSOSStatusErrorDomain, code, fmt, ap);
    va_end(ap);
    printf("NSErrorV %s %ld [%s]\n", e.domain.UTF8String, (long)e.code, e.localizedDescription.UTF8String);
}

int main(void)
{
    @autoreleasepool {
        NSString *obj = @"object";
        vsn("s=%s d=%d f=%.2f mac=%.6a x=%#x", "str", -7, 2.5, mac, 0xbeef);
        vsn("@=%@ ld=%ld g=%g c=%c", obj, 1234567890123L, 0.125, 'z');
        vas("s=%s f=%.3f @=%@ d=%d", "two", 1.5, obj, 42);
        nsp("d=%d @=%@ f=%.1f s=%s", 3, obj, -0.5, "tail");
        nse(-6705, "failed %s after %d tries (%.1f s) %@", "connect", 3, 1.5, obj);
        nse(0, "no error %d", 1);
    }
    return 0;
}
