/* Native variadic ObjC methods reached through
 * objc_msgSend / objc_msgSendSuper / objc_msgSendSuper2 whose selector no hao
 * table names (neither src/objc.c va_sels nor the <sel>arguments: sibling
 * rule). Apple arm64 passes every variadic argument in an 8-byte stack slot;
 * the native x86_64 callee reads them from the registers left after its fixed
 * arguments (its va_start prologue) and only then from the stack.
 *
 *   objcva SECTION     one section per run, so a crash hides nothing else
 *
 * Sections (callee, shape; x86_64 encodings from the host's runtime):
 *   dumper     -[OSLogTermDumper format:] (LoggingSupport), /usr/bin/log's own
 *              call (213 sites): C format, 3 fixed GPRs, ints/pointers/%-*s
 *   dumpsub    the same reached the way log reaches it: a guest subclass of
 *              the native OSLogTermDumper sends [self format:] (objc_msgSend)
 *              and [super format:] (objc_msgSendSuper2)
 *   msgsuper   the same through objc_msgSendSuper called directly
 *   dumpimp    the same through a bare IMP (methodForSelector:): bound by
 *              varsniff on main already; a regression guard
 *   validated  +[NSString stringWithValidatedFormat:validFormatSpecifiers:error:]
 *              (SDK NS_FORMAT_FUNCTION(2,4)): 5 fixed GPRs, the format is not
 *              the last fixed argument, a double vararg
 *   attributed +[NSAttributedString localizedAttributedStringWithFormat:]
 *              (SDK): an NSAttributedString format, object/int varargs
 *   cifilter   +[CIFilter filterWithName:keysAndValues:] (SDK,
 *              NS_REQUIRES_NIL_TERMINATION): 4 fixed GPRs, no format, a
 *              nil-terminated object list
 *   mlerror    +[MLModelErrorUtils errorWithCode:format:] (CoreML): 4 fixed
 *              GPRs (an integer before the NSString format)
 *   msverror   +[NSError msv_errorWithDomain:code:debugDescription:]
 *              (MediaServices): 5 fixed GPRs, format last; also through
 *              [super ...] from a guest NSError subclass (class-method
 *              objc_msgSendSuper2)
 *   pql        -[PQLConnection execute:] / fetchObjectOfClass:sql: (libprequelite,
 *              revisiond's): SQL formats whose %lld/%@ become bound parameters,
 *              a BOOL result (char on x86_64)
 *   nsformat   NSString's +stringWithFormat:, +localizedStringWithFormat: and
 *              -stringByAppendingFormat:, NSMutableString's -appendFormat: and
 *              NSAssertionHandler's handleFailureIn...description:, which
 *              src/objc.c once named in va_sels: object, integer, double and
 *              %L arguments, a format made at run time, 22 and 70 arguments
 */
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOGGINGSUPPORT "/System/Library/PrivateFrameworks/LoggingSupport.framework/Versions/A/LoggingSupport"

/* LoggingSupport is linked (tests/guest/objcva.sh gives a .tbd) so that a guest
 * class can inherit from it, as log's OSLogCompactOutputFormatter does */
@interface OSLogTermDumper : NSObject
- (id)initWithFd:(int)fd colorMode:(int)mode;
- (unsigned)format:(const char *)fmt, ...;
- (void)writeln;
- (void)flush:(BOOL)b;
@end

@interface HaoDumper : OSLogTermDumper
- (void)printTimestamp:(const int *)t;
- (void)printHeaderSuper;
@end
@implementation HaoDumper
- (void)printTimestamp:(const int *)t
{
    [self format:"%04d-%02d-%02d %02d:%02d:%02d.%03d ", t[0], t[1], t[2], t[3], t[4], t[5], t[6]];
}
- (void)printHeaderSuper
{
    [super format:"%-*s%-*s%s", 24, "Timestamp", 3, "Ty", "Process[PID:TID]"];
    [super format:" %s[%d:%llx]", "audiomxd", 123, 0x1f2fULL];
}
@end

@interface NSObject (P6Private)
+ (NSError *)errorWithCode:(long long)code format:(NSString *)fmt, ...;
+ (NSError *)msv_errorWithDomain:(NSString *)d code:(long long)c debugDescription:(NSString *)fmt, ...;
- (BOOL)openAtURL:(NSURL *)u sharedCache:(BOOL)s error:(NSError **)e;
- (BOOL)execute:(NSString *)sql, ...;
- (id)fetchObjectOfClass:(Class)c sql:(NSString *)sql, ...;
- (BOOL)close:(NSError **)e;
@end

@interface HaoError : NSError
+ (NSError *)viaSuper;
@end
@implementation HaoError
+ (NSError *)viaSuper
{
    return [super msv_errorWithDomain:@"hao.super" code:9 debugDescription:@"%@ %d-%d-%d", @"super", 2026, 11, 4];
}
@end

static const int ts[7] = {2026, 11, 4, 12, 16, 37, 892};

static OSLogTermDumper *dumper(Class c)
{
    fflush(stdout);
    return [[c alloc] initWithFd:1 colorMode:0];
}
static void done(OSLogTermDumper *d)
{
    [d writeln];
    [d flush:YES];
}
static void err(NSError *e)
{
    printf("  %s %ld [%s]\n", e ? e.domain.UTF8String : "(nil)", (long)e.code,
           e ? e.userInfo.description.UTF8String : "");
}

static int section(const char *s)
{
    if (!strcmp(s, "dumper")) {
        OSLogTermDumper *d = dumper([OSLogTermDumper class]);
        unsigned n = [d format:"%04d-%02d-%02d %02d:%02d:%02d.%03d ", ts[0], ts[1], ts[2], ts[3], ts[4], ts[5], ts[6]];
        n += [d format:"%-*s%-*s%s", 24, "Timestamp", 3, "Ty", "Process[PID:TID]"];
        done(d);
        n += [d format:"%s[%d:%llx] %c%c", "audiomxd", 123, 0x1f2fULL, 'o', 'k'];
        done(d);
        printf("dumper: %u\n", n);
    } else if (!strcmp(s, "dumpsub")) {
        HaoDumper *d = (HaoDumper *)dumper([HaoDumper class]);
        [d printTimestamp:ts];
        [d printHeaderSuper];
        done(d);
        printf("dumpsub: done\n");
    } else if (!strcmp(s, "msgsuper")) {
        HaoDumper *d = (HaoDumper *)dumper([HaoDumper class]);
        struct objc_super sup = {d, [OSLogTermDumper class]};
        unsigned (*f)(struct objc_super *, SEL, const char *, ...) = (void *)objc_msgSendSuper;
        unsigned n = f(&sup, @selector(format:), "%04d-%02d-%02d %s %d", 2026, 11, 4, "msgSendSuper", -5);
        done(d);
        printf("msgsuper: %u\n", n);
    } else if (!strcmp(s, "dumpimp")) {
        OSLogTermDumper *d = dumper([OSLogTermDumper class]);
        unsigned (*imp)(id, SEL, const char *, ...) = (void *)[d methodForSelector:@selector(format:)];
        unsigned n = imp(d, @selector(format:), "%04d-%02d-%02d %s", 2026, 11, 4, "imp");
        done(d);
        printf("dumpimp: %u\n", n);
    } else if (!strcmp(s, "validated")) {
        NSError *e = nil;
        NSString *r = [NSString stringWithValidatedFormat:@"%@ has %d items at %.2f (%ld)"
                                    validFormatSpecifiers:@"%@ %d %f %ld" error:&e, @"cart", 3, 9.75, 1234567890123L];
        printf("validated: %s\n", r ? r.UTF8String : "(nil)");
        if (!r) err(e);
        e = nil;
        /* specifiers that do not match the format: nil and NSCocoaErrorDomain 2048, no argument read */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat"
#pragma clang diagnostic ignored "-Wformat-extra-args"
        r = [NSString stringWithValidatedFormat:@"%d-%d" validFormatSpecifiers:@"%@" error:&e, 1, 2];
#pragma clang diagnostic pop
        printf("validated mismatch: %s\n", r ? r.UTF8String : "(nil)");
        printf("  error %s %ld\n", e ? e.domain.UTF8String : "(nil)", (long)e.code);
    } else if (!strcmp(s, "attributed")) {
        NSAttributedString *f = [[NSAttributedString alloc] initWithString:@"%@ = %d, %@ = %ld"];
        NSAttributedString *r = [NSAttributedString localizedAttributedStringWithFormat:f, @"x", 42, @"y", 1234567890123L];
        printf("attributed: %s\n", r ? r.string.UTF8String : "(nil)");
    } else if (!strcmp(s, "cifilter")) {
        CIFilter *f = [CIFilter filterWithName:@"CIColorControls" keysAndValues:
                       @"inputSaturation", @1.25, @"inputBrightness", @0.5, @"inputContrast", @2, nil];
        printf("cifilter: %s", f ? f.name.UTF8String : "(nil)");
        for (NSString *k in @[@"inputSaturation", @"inputBrightness", @"inputContrast"])
            printf(" %s=%s", k.UTF8String, [[f valueForKey:k] description].UTF8String);
        printf("\n");
        f = [CIFilter filterWithName:@"CIGaussianBlur" keysAndValues:@"inputRadius", @3.5, nil];
        printf("cifilter: %s inputRadius=%s\n", f ? f.name.UTF8String : "(nil)",
               [[f valueForKey:@"inputRadius"] description].UTF8String);
    } else if (!strcmp(s, "mlerror")) {
        Class c = NSClassFromString(@"MLModelErrorUtils");
        if (!c) { printf("mlerror: no class\n"); return 1; }
        NSError *e = [c errorWithCode:3 format:@"model %@ has %d inputs, %ld outputs", @"hao", 7, 1234567890123L];
        printf("mlerror: %ld %s\n", (long)e.code, e.localizedDescription.UTF8String);
    } else if (!strcmp(s, "msverror")) {
        NSError *e = [NSError msv_errorWithDomain:@"hao.msv" code:77
                                debugDescription:@"%@ %d-%02d-%02d %ld", @"date", 2026, 11, 4, 1234567890123L];
        printf("msverror:\n");
        err(e);
        e = [HaoError viaSuper];
        printf("msverror super: %s\n", class_getName(object_getClass(e)));
        err(e);
    } else if (!strcmp(s, "pql")) {
        Class c = NSClassFromString(@"PQLConnection");
        if (!c) { printf("pql: no class\n"); return 1; }
        id db = [c new];
        NSError *e = nil;
        /* a fresh database file, removed afterwards: every run starts empty */
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/objcva-pql.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
        NSString *path = [NSString stringWithFormat:@"%s/t.db", dir];
        BOOL ok = [db openAtURL:[NSURL fileURLWithPath:path] sharedCache:NO error:&e];
        printf("pql open: %d\n", ok);
        if (!ok) { err(e); return 1; }
        printf("pql create: %d\n", [db execute:@"CREATE TABLE t (a INTEGER, b TEXT, c INTEGER)"]);
        printf("pql insert: %d\n", [db execute:@"INSERT INTO t VALUES (%lld, %@, %d)", 1234567890123LL, @"hello", 7]);
        printf("pql insert: %d\n", [db execute:@"INSERT INTO t VALUES (%lld, %@, %d)", 42LL, @"world", 8]);
        NSNumber *a = [db fetchObjectOfClass:[NSNumber class] sql:@"SELECT a FROM t WHERE b = %@ AND c = %d", @"hello", 7];
        NSString *b = [db fetchObjectOfClass:[NSString class] sql:@"SELECT b FROM t WHERE a = %lld", 42LL];
        NSNumber *n = [db fetchObjectOfClass:[NSNumber class] sql:@"SELECT count(*) FROM t WHERE c > %d", 7];
        printf("pql fetch: a=%s b=%s n=%s\n", a.description.UTF8String, b.description.UTF8String, n.description.UTF8String);
        [db close:NULL];
        for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(dir) error:NULL])
            unlink([NSString stringWithFormat:@"%s/%@", dir, f].fileSystemRepresentation);
        rmdir(dir);
    } else if (!strcmp(s, "nsformat")) {
        /* a format made at run time is typed per call from the SDK's NS_FORMAT_FUNCTION */
        NSString *dyn = [NSString stringWithUTF8String:"%d|%@|%.3f|%lld|%s|%c|%u|%g"];
        printf("class: %s\n", [NSString stringWithFormat:@"%d|%@|%.3f|%lld|%s|%c|%u|%g", -7, @"obj", 2.5, 1LL << 40, "cstr",
                               'q', 4000000000u, 1e-3].UTF8String);
        printf("classdyn: %s\n", [NSString stringWithFormat:dyn, -7, @"obj", 2.5, 1LL << 40, "cstr", 'q', 4000000000u, 1e-3].UTF8String);
        printf("many: %s\n", [NSString stringWithFormat:@"%d %d %d %d %d %d %d %d %d %d %f %f %f %f %f %f %f %f %f %f %@ %ld",
                               1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5, @"end",
                               123456789012L].UTF8String);
        /* 70 arguments: more than the 64 slots varsniff passed before */
        printf("huge: %s\n", [NSString stringWithFormat:@"%d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f",
                               0, 1.5, 2, 3.5, 4, 5.5, 6, 7.5, 8, 9.5, 10, 11.5, 12, 13.5, 14, 15.5, 16, 17.5, 18, 19.5, 20, 21.5, 22, 23.5, 24, 25.5, 26, 27.5, 28, 29.5, 30, 31.5, 32, 33.5, 34, 35.5, 36, 37.5, 38, 39.5, 40, 41.5, 42, 43.5, 44, 45.5, 46, 47.5, 48, 49.5, 50, 51.5, 52, 53.5, 54, 55.5, 56, 57.5, 58, 59.5, 60, 61.5, 62, 63.5, 64, 65.5, 66, 67.5, 68, 69.5].UTF8String);
        NSMutableString *h = [NSMutableString string];
        [h appendFormat:@"%d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f",
                        0, 1.5, 2, 3.5, 4, 5.5, 6, 7.5, 8, 9.5, 10, 11.5, 12, 13.5, 14, 15.5, 16, 17.5, 18, 19.5, 20, 21.5, 22, 23.5, 24, 25.5, 26, 27.5, 28, 29.5, 30, 31.5, 32, 33.5, 34, 35.5, 36, 37.5, 38, 39.5, 40, 41.5, 42, 43.5, 44, 45.5, 46, 47.5, 48, 49.5, 50, 51.5, 52, 53.5, 54, 55.5, 56, 57.5, 58, 59.5, 60, 61.5, 62, 63.5, 64, 65.5, 66, 67.5, 68, 69.5];
        printf("hugeappend: %s\n", h.UTF8String);
        printf("ldouble: %s\n", [NSString stringWithFormat:@"%Lf %La", (long double)1.25, (long double)0.5].UTF8String);
        printf("localized: %s\n", [NSString localizedStringWithFormat:@"%d|%@|%.2f|%ld", 42, @"o", 3.25, 99L].UTF8String);
        NSMutableString *m = [NSMutableString stringWithString:@"m:"];
        [m appendFormat:@"%d,%@,%.1f,%s", 5, @"x", 0.5, "y"];
        [m appendFormat:dyn, -7, @"obj", 2.5, 1LL << 40, "cstr", 'q', 4000000000u, 1e-3];
        printf("append: %s\n", m.UTF8String);
        printf("appending: %s\n", [@"base" stringByAppendingFormat:@"+%d+%@+%.3f", 9, @"z", 1.125].UTF8String);
        @try {
            [[NSAssertionHandler currentHandler] handleFailureInMethod:@selector(description) object:@"recv" file:@"f.m"
                                                            lineNumber:12 description:@"method %d %@ %.2f", 3, @"why", 0.75];
        } @catch (NSException *e) { printf("assertm: %s\n", e.reason.UTF8String); }
        @try {
            [[NSAssertionHandler currentHandler] handleFailureInFunction:@"fn" file:@"g.m" lineNumber:34
                                                             description:@"function %s %ld %.1f", "c", 77L, 2.5];
        } @catch (NSException *e) { printf("assertf: %s\n", e.reason.UTF8String); }
    } else {
        fprintf(stderr, "objcva: unknown section %s\n", s);
        return 2;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: objcva SECTION\n"); return 2; }
    int rc;
    @autoreleasepool {
        if (!dlopen(LOGGINGSUPPORT, RTLD_NOW)) return 3;
        dlopen("/System/Library/Frameworks/CoreML.framework/CoreML", RTLD_NOW);
        dlopen("/System/Library/PrivateFrameworks/MediaServices.framework/MediaServices", RTLD_NOW);
        dlopen("/usr/lib/libprequelite.dylib", RTLD_NOW);
        rc = section(argv[1]);
    }
    fflush(stdout);
    return rc;
}
