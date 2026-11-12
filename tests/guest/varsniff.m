// Variadic native functions that no interposer table names (src/varsniff.c):
// a native helper's private-style variadics (integer lists, C and CFString
// formats with doubles past the eight XMMs, a fixed FP argument, five fixed
// GPRs, guest function pointers among the variadic arguments), the 27.0
// logind call DBLoggingLogWithFormat (loginsupport, CFString format with
// %s/%d/%@), syslog, which the SDK binds to syslog$DARWIN_EXTSN, and
// CFCalendarDecomposeAbsoluteTime, whose fixed double the generated table
// counted as an integer argument. %a is a double to printf but a pointer to
// an address to CoreUtils' SNPrintF (%.6a Ethernet, %.4a IPv4, %##a sockaddr):
// both, and the zero cases.
// Output must match native arm64.
#import <Foundation/Foundation.h>
#include <netinet/in.h>
#include <syslog.h>
#include "varsniffhelper.h"

void DBLoggingLogWithFormat(uint64_t controller, CFStringRef fmt, ...);
int SNPrintF(void *buf, size_t size, const char *fmt, ...);   /* CoreUtils */

static long f0(long k) { return k + 1; }
static long f1(long k) { return 2 * k + 3; }

int main(void)
{
    char b[512];
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sum %ld\n", vs_sum(9, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L));
    vs_format(b, sizeof b, "%d %s %ld %c %x %p %d %d", 7, "seven", 1L << 40, 'Q', 0xbeef, (void *)0x1234, -1, 42);
    printf("ints  %s\n", b);
    vs_format(b, sizeof b, "%f %d %.2f %s %g %e %d %a %f %f %f %f %d", 1.5, 2, 3.25, "s", 4.5e10, 6.0e-3, 9, 0.5, 7.0,
              8.0, 9.0, 10.0, 11);
    printf("mixed %s\n", b);
    vs_format(b, sizeof b, "%Lf %d %Lg %*d|%.*f", (long double)2.5, 3, (long double)1e5, 6, 77, 3, 3.14159);
    printf("long  %s\n", b);
    vs_format(b, sizeof b, "%La %LA %Le %d %LG", (long double)1.5, (long double)-0.1, (long double)3e300, 4, (long double)7.0);
    printf("hexl  %s\n", b);
    vs_format(b, sizeof b, "%a %d %s %.3a %#a %A %f", 0.0, 5, "z", 1.5, -2.0, 0.1, 6.5);
    printf("hex   %s\n", b);
    static const unsigned char mac[6] = {0x74, 0x68, 0x1e, 0x19, 0xf3, 0x19}, ip4[4] = {10, 1, 2, 3};
    struct sockaddr_in sin = {sizeof sin, AF_INET, htons(80), {htonl(0x0a010203)}};
    SNPrintF(b, sizeof b, "mac %.6a ip %.4a sa %##a f %f s %s d %d", mac, ip4, &sin, 2.5, "str", 7);
    printf("cu    %s\n", b);
    SNPrintF(b, sizeof b, "null %.6a d %d s %s f %f", (void *)0, 5, "s", 1.25);
    printf("cu0   %s\n", b);
    SNPrintF(b, sizeof b, "a=%a s=%s d=%d", (void *)0, "tail", 7);      /* bare %a of NULL: CoreUtils by family */
    printf("cu0b  %s\n", b);
    vs_format(b, sizeof b, "%a %d %a", 0x1p-1074, 3, 0x1p-1022);        /* subnormal and minimum normal: printf */
    printf("hexsub %s\n", b);
    vs_scaled(0.25, b, sizeof b, "%s=%f,%d", "k", 1.75, 12);
    printf("fixfp %s\n", b);
    vs_five(1, 2, 3, 4, "%s %f %ld", "five", 5.5, 6L);
    printf("five  %s\n", vs_last_five());
    CFStringRef s = vs_cf(3, CFSTR("%@ %d %s %.3f %@"), @"obj", 17, "cstr", 2.0 / 3, @[ @1, @2 ].description);
    printf("cf    %s\n", [(__bridge NSString *)s UTF8String]);
    CFRelease(s);
    s = vs_cf(4, CFSTR("%Lf %d %Lg %@ %La"), (long double)0.25, 5, (long double)-6.5, @"o", (long double)1.5);
    printf("cfL   %s\n", [(__bridge NSString *)s UTF8String]);
    CFRelease(s);
    printf("call  %ld\n", vs_call(3, f0, f1, f0));
    CFCalendarRef cal = CFCalendarCreateWithIdentifier(NULL, kCFGregorianCalendar);
    CFTimeZoneRef utc = CFTimeZoneCreateWithTimeIntervalFromGMT(NULL, 0);
    CFCalendarSetTimeZone(cal, utc);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    Boolean ok = CFCalendarDecomposeAbsoluteTime(cal, 815443200.0 + 3723.0, "yMdHm", &y, &mo, &d, &h, &mi);
    printf("cal   %d %04d-%02d-%02d %02d:%02d\n", ok, y, mo, d, h, mi);
    CFRelease(utc);
    CFRelease(cal);
    DBLoggingLogWithFormat(0, CFSTR("%s: %d %@"), __func__, 535, CFSTR("logind"));
    printf("dblogging returned\n");
    openlog("varsniff", LOG_PERROR, LOG_USER);
    syslog(LOG_DEBUG, "syslog %s %d %ld", "args", 21, 1L << 33);
    closelog();
    printf("done\n");
    return 0;
}
