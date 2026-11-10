/* Signature-path and variadic bridge cases.
 * Each line must match native arm64. Run by tests/guest/bridgefix.sh.
 *  - libm log(): Kernel.framework's variadic log() made the generated tables trap it
 *  - long double results (strtold, sqrtl): x87 st0 was not captured; then
 *    computed in x87 precision and rounded twice (nexttowardl, strtold just
 *    above a halfway point) and modfl's 16-byte store: arm64 runs the double
 *    functions (src/libm.c)
 *  - a two-eightbyte struct after 5 integer registers (SysV does not split it)
 *  - an HFA spilling to the stack in a native->guest call with a signature
 *  - a guest long double result returned to native code
 *  - private variadics: CFLog, os_log_with_args, sandbox_check
 *  - the no-split method called natively (native -> guest)
 *  - aggregates over 64 bytes as arguments and over 256 bytes as results,
 *    guest -> native (a native IMP) and native -> guest (block, method)
 *  - CATransform3D (128 bytes) and CGAffineTransform (48) by value: sigdb
 *  - 5000 distinct struct-returning method encodings (the signature cache
 *    once held 4096 and then treated every encoding as register-exact) */
#import <Foundation/Foundation.h>
#import <QuartzCore/CATransform3D.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <math.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#import "bridgehelper.h"

void CFLog(int32_t level, CFStringRef format, ...);
void os_log_with_args(os_log_t oslog, os_log_type_t type, const char *format, va_list args, void *ret_addr);
int sandbox_check(pid_t pid, const char *operation, int type, ...);
#define SANDBOX_FILTER_PATH 1
#define SANDBOX_CHECK_NO_REPORT 0x40000000

static bh_big72 make72(long seed)
{
    bh_big72 a;
    for (int k = 0; k < 9; k++) a.v[k] = seed * 100 + k;
    return a;
}

static bh_big304 big_compute(bh_big72 a, long i)    /* bh_big_imp's formula, in guest code */
{
    bh_big304 r;
    for (int k = 0; k < 38; k++) r.v[k] = a.v[k % 9] * (k + 1) + i;
    return r;
}

@interface BFObj : NSObject
- (bh_big304)bigFrom:(bh_big72)a n:(long)i;
- (NSUInteger)count:(id)a with:(id)b options:(NSUInteger)o range:(NSRange)r;
@end
@implementation BFObj
- (bh_big304)bigFrom:(bh_big72)a n:(long)i { return big_compute(a, i); }
- (NSUInteger)count:(id)a with:(id)b options:(NSUInteger)o range:(NSRange)r
{
    return r.location * 1000 + r.length * 10 + o + (a == self ? 7000000 : 0) + (b == nil ? 500000 : 0);
}
@end

/* x86_64 and arm64 QuartzCore may differ in the sign of a zero: +0.0 drops it */
#define Z(x) ((x) + 0.0)
static void print_t3d(const char *what, CATransform3D t)
{
    printf("%s %.4f %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f %.4f\n", what, Z(t.m11), Z(t.m12), Z(t.m13),
           Z(t.m14), Z(t.m22), Z(t.m23), Z(t.m33), Z(t.m41), Z(t.m42), Z(t.m43), Z(t.m44));
}

static void logv(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    os_log_with_args(OS_LOG_DEFAULT, OS_LOG_TYPE_DEBUG, fmt, ap, __builtin_return_address(0));
    va_end(ap);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* a crash in one case keeps the lines before it */
    @autoreleasepool {
        volatile double e = 2.718281828459045, two = 2;
        printf("log(e) %.6f log(100) %.6f\n", log(e), log(100 * e / e));
        printf("strtold(\"1.5e3\") %.3f\n", (double)strtold("1.5e3", NULL));
        printf("sqrtl(2) %.12f\n", (double)sqrtl(two));
        /* long double arguments and results of sigdb functions (built with -fno-builtin: real calls) */
        int ex = 0;
        long double fr = frexpl(48, &ex);
        printf("powl %.6f fmal %.6f ldexpl %.6f frexpl %.6f %d hypotl %.6f cbrtl %.9f atan2l %.9f\n",
               (double)powl(two, 10), (double)fmal(two, 3, 4), (double)ldexpl(3, 4), (double)fr, ex,
               (double)hypotl(3, 4), (double)cbrtl(27), (double)atan2l(1, 1));
        long double ip, fp = modfl(-2.75, &ip);
        volatile double canary[2] = {7, 7};
        long double *slot = (long double *)&canary[0];
        modfl(5.5, slot);
        printf("nexttowardl(1, 2) %a nextafterl(1, 0) %a modfl %.3f %.3f canary %.1f %.1f\n", (double)nexttowardl(1, two),
               (double)nextafterl(1, 0), (double)fp, (double)ip, canary[0], canary[1]);
        /* 1 + 2^-53 + 10^-81: just above halfway between 1 and 1 + 2^-52 */
        const char *halfway = "1.000000000000000111022302462515654042363166809082031250000000000000000000000000001";
        printf("strtold(just above halfway) %a strtod %a\n", (double)strtold(halfway, NULL), strtod(halfway, NULL));
        printf("nexttowardl > 1 %d remainderl %.6f fabsl %.6f scalbnl %.6f truncl %.6f lgammal %.6f expl %.9f\n",
               nexttowardl(1, two) > 1, (double)remainderl(10, 3), (double)fabsl(-two), (double)scalbnl(1.5, 3),
               (double)truncl(-2.75), (double)lgammal(5), (double)expl(1));

        /* -replaceOccurrencesOfString:withString:options:range: = self, _cmd, 2 objects, options, NSRange */
        NSMutableString *m = [NSMutableString stringWithString:@"a-b-c-d-e"];
        NSUInteger n = [m replaceOccurrencesOfString:@"-" withString:@"+" options:0 range:NSMakeRange(2, 5)];
        printf("replaceOccurrences %lu %s\n", (unsigned long)n, m.UTF8String);
        NSRange r = [@"hello world" rangeOfString:@"o" options:NSBackwardsSearch range:NSMakeRange(0, 6)];
        printf("rangeOfString:options:range: %lu %lu\n", (unsigned long)r.location, (unsigned long)r.length);

        long got = bh_call_rects(^long(CGRect a, CGRect b, CGRect c, long i) {
            printf("rects %.0f %.0f %.0f %.0f / %.0f %.0f / %.0f %.0f %.0f %.0f i=%ld\n", a.origin.x, a.origin.y,
                   a.size.width, a.size.height, b.origin.x, b.size.height, c.origin.x, c.origin.y, c.size.width,
                   c.size.height, i);
            return i + (long)c.size.height;
        });
        printf("rects returned %ld\n", got);
        printf("long double block result doubled %.3f\n", bh_call_ld(^long double(double x) { return x + 0.25L; }, 1.5));

        CFLog(3, CFSTR("hao bridgefix CFLog %d %@"), 42, @"ok");
        printf("CFLog returned\n");
        logv("hao bridgefix os_log_with_args %d %s", 7, "seven");
        printf("os_log_with_args returned\n");
        int sb = sandbox_check(getpid(), "file-read-data", SANDBOX_FILTER_PATH | SANDBOX_CHECK_NO_REPORT, "/etc/hosts");
        printf("sandbox_check(file-read-data, /etc/hosts) %d\n", sb);

        BFObj *o = [BFObj new];
        printf("native -> guest no-split %lu\n", bh_send_nosplit((__bridge void *)o, "count:with:options:range:"));

        /* aggregates past the old fixed buffers, both directions */
        bh_big304 ref = big_compute(make72(7), 1000);
        printf("big: native -> guest block %ld (guest %ld)\n", bh_call_big(^bh_big304(bh_big72 a, long i) {
                   return big_compute(a, i);
               }), bh_sum304(&ref));
        ref = big_compute(make72(9), 2000);
        printf("big: native -> guest method %ld (guest %ld)\n", bh_send_big((__bridge void *)o, "bigFrom:n:"),
               bh_sum304(&ref));
        char types[128];
        snprintf(types, sizeof types, "%s@:%sq", @encode(bh_big304), @encode(bh_big72));
        class_addMethod([BFObj class], @selector(nativeBig:n:), (IMP)bh_big_imp, types);
        bh_big304 nb = ((bh_big304(*)(id, SEL, bh_big72, long))objc_msgSend)(o, @selector(nativeBig:n:), make72(5), 3000);
        ref = big_compute(make72(5), 3000);
        printf("big: guest -> native IMP %ld (guest %ld) last %ld\n", bh_sum304(&nb), bh_sum304(&ref), nb.v[37]);

        /* CATransform3D by value (sigdb) */
        CATransform3D s = CATransform3DMakeScale(2, 3, 4), tr = CATransform3DMakeTranslation(5, 6, 7);
        CATransform3D st = CATransform3DConcat(s, tr);
        print_t3d("CATransform3DConcat", st);
        print_t3d("CATransform3DInvert", CATransform3DInvert(st));
        print_t3d("CATransform3DTranslate", CATransform3DTranslate(st, 1, 1, 1));
        printf("CATransform3DIsAffine %d %d EqualToTransform %d %d IsIdentity %d\n", CATransform3DIsAffine(st),
               CATransform3DIsAffine(CATransform3DRotate(st, 0.5, 1, 0, 0)), CATransform3DEqualToTransform(st, st),
               CATransform3DEqualToTransform(st, tr), CATransform3DIsIdentity(CATransform3DIdentity));
        CGAffineTransform at = CATransform3DGetAffineTransform(CATransform3DMakeAffineTransform(
            CGAffineTransformMake(1, 2, 3, 4, 5, 6)));
        printf("CATransform3D <-> CGAffineTransform %.1f %.1f %.1f %.1f %.1f %.1f\n", at.a, at.b, at.c, at.d, at.tx, at.ty);

        /* more distinct encodings than the old fixed signature cache held */
        NSValue *val = [NSValue valueWithRect:NSMakeRect(1.5, 2.5, 3.5, 4.5)];
        IMP rimp = method_getImplementation(class_getInstanceMethod([val class], @selector(rectValue)));
        int bad = 0;
        for (int k = 0; k < 5000; k++) {
            char name[32];
            snprintf(types, sizeof types, "{CGRect%04d={CGPoint=dd}{CGSize=dd}}16@0:8", k);
            snprintf(name, sizeof name, "haoRect%04d", k);
            SEL sel = sel_registerName(name);
            class_addMethod([val class], sel, rimp, types);
            CGRect r = ((CGRect(*)(id, SEL))objc_msgSend)(val, sel);
            if (r.origin.x != 1.5 || r.origin.y != 2.5 || r.size.width != 3.5 || r.size.height != 4.5) bad++;
        }
        printf("5000 rect method encodings: %d wrong\n", bad);
    }
    return 0;
}
