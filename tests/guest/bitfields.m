/* Aggregates holding bitfields (NSDecimal and others) across the
 * guest/native boundary in both directions, with canary bytes around every
 * result returned through memory; plus two aggregates whose x86_64 classes
 * depend on nesting (NST, a union of floats). Run as arm64e under hao and
 * as native arm64; the outputs must be identical. tests/guest/bitfields.sh
 * builds and compares. */
#import "bfhelper.h"
#include <objc/message.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define STRIP(p) ptrauth_strip((void *)(p), ptrauth_key_function_pointer)
#else
#define STRIP(p) ((void *)(p))
#endif

/* fn(a0..a3) with x8 = ret, the AAPCS64 indirect result register: the
 * result is written straight into `ret`, whatever the compiler would do */
__attribute__((naked, noinline)) static void call_x8(void *ret, void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    __asm__ volatile("stp x29, x30, [sp, #-16]!\n\t"
                     "mov x29, sp\n\t"
                     "mov x8, x0\n\t"
                     "mov x16, x1\n\t"
                     "mov x0, x2\n\t"
                     "mov x1, x3\n\t"
                     "mov x2, x4\n\t"
                     "mov x3, x5\n\t"
                     "blr x16\n\t"
                     "ldp x29, x30, [sp], #16\n\t"
                     "ret");
}
#define U(x) ((uint64_t)(uintptr_t)(x))
static uint64_t S(SEL s) { uint64_t v; memcpy(&v, &s, sizeof v); return v; }

/* the guest-side twin of BFNative, with different arithmetic */
@interface BFGuest : NSObject
@end
@implementation BFGuest
- (NSDecimal)dec:(NSDecimal)d plus:(int)k { d._mantissa[0] += (unsigned short)(2 * k); d._isNegative = !d._isNegative; return d; }
- (NSString *)describe:(NSDecimal)a tag:(int)t other:(NSDecimal)b
{
    return [NSString stringWithFormat:@"guest[%@ | %d | %@]", NSDecimalString(&a, nil), t, NSDecimalString(&b, nil)];
}
- (BF3)bf3:(BF3)x k:(int)k { BF3 r = {x.b + k, x.a + 1, x.f - k}; return r; }
- (int)sumBF3:(BF3)x :(BF3)y k:(int)k { return (int)((x.a * 10 + y.b) * k + x.f * y.f * 100); }
- (BFF)bff:(BFF)x { BFF r = {x.a * 2, x.b + 1, x.f * 4}; return r; }
- (BFC)bfc:(BFC)x k:(int)k { BFC r = {x.b, x.a, x.c - k}; return r; }
- (BNEST)nest:(BNEST)n k:(int)k
{
    BNEST r;
    memset(&r, 0, sizeof r);
    r.s.a = n.s.a + 1; r.s.b = n.s.b + k; r.s.c = n.s.c;
    snprintf(r.tail, sizeof r.tail, "g:%.17s", n.tail);
    return r;
}
- (int)nestSum:(BNEST)a :(BNEST)b k:(int)k { return (a.s.a * 100 + a.tail[0]) * 10000 + b.s.b * 100 + b.tail[1] + k; }
- (NST)nst:(NST)x { NST r = {x.s.b, {x.a, x.s.c * 3}}; return r; }
- (UF2)uf:(UF2)u k:(int)k { UF2 r; r.g[0] = u.g[0] - k; r.g[1] = u.g[1] * u.g[0]; return r; }
@end

static void print_bnest(const char *what, const BNEST *n)
{
    printf("%s: {%u,%u,%d} %.20s\n", what, n->s.a, n->s.b, n->s.c, n->tail);
}

static void foundation(void)
{
    NSDecimalNumber *n = [NSDecimalNumber decimalNumberWithString:@"12.5"];
    NSDecimal d = n.decimalValue;
    bfh_print_dec("decimalValue", &d);

    /* the result slot is exactly 20 bytes, canaries on both sides */
    BFDecBox box;
    memset(&box, 0xa5, sizeof box);
    call_x8(&box.d, STRIP(objc_msgSend), U(n), S(@selector(decimalValue)), 0, 0);
    bfh_print_dec("decimalValue (x8 box)", &box.d);
    bfh_check_box("decimalValue result", &box, sizeof box, &box.d, sizeof box.d);

    memset(&box, 0xa5, sizeof box);
    call_x8(&box.d, STRIP(objc_msgSend), U(@(-42)), S(@selector(decimalValue)), 0, 0);
    bfh_print_dec("NSNumber decimalValue (x8 box)", &box.d);
    bfh_check_box("NSNumber decimalValue result", &box, sizeof box, &box.d, sizeof box.d);

    /* NSDecimal arguments (by reference on arm64, on the stack on x86_64) */
    NSDecimalNumber *m = [NSDecimalNumber decimalNumberWithDecimal:d];
    NSDecimalNumber *m2 = [[NSDecimalNumber alloc] initWithDecimal:box.d];
    NSDecimalNumber *sum = [m decimalNumberByAdding:m2];
    NSDecimal s = sum.decimalValue;
    printf("decimalNumberWithDecimal: %s initWithDecimal: %s sum: %s equal %d\n", m.description.UTF8String,
           m2.description.UTF8String, sum.description.UTF8String, [m isEqual:n]);
    bfh_print_dec("sum decimalValue", &s);
    NSDecimal big = [[NSDecimalNumber decimalNumberWithString:@"123456789012345678901234567890.125"] decimalValue];
    bfh_print_dec("big", &big);
    NSDecimalNumber *bn = [NSDecimalNumber decimalNumberWithDecimal:big];
    printf("big round trip: %s\n", bn.stringValue.UTF8String);
}

static void native_methods(void)
{
    BFNative *nat = [BFNative new];
    NSDecimal d = [[NSDecimalNumber decimalNumberWithString:@"99.5"] decimalValue];
    NSDecimal e = [[NSDecimalNumber decimalNumberWithString:@"-0.001"] decimalValue];

    BFDecBox box;
    memset(&box, 0xa5, sizeof box);
    NSDecimal darg = d;                                      /* arm64: passed by reference */
    call_x8(&box.d, STRIP(objc_msgSend), U(nat), S(@selector(dec:plus:)), U(&darg), 6);
    bfh_print_dec("guest->native dec:plus: (x8 box)", &box.d);
    bfh_check_box("guest->native dec:plus: result", &box, sizeof box, &box.d, sizeof box.d);
    NSDecimal r = [nat dec:e plus:3];
    bfh_print_dec("guest->native dec:plus:", &r);
    printf("guest->native describe: %s\n", [nat describe:d tag:77 other:e].UTF8String);

    BF3 x = {7, 9, 1.5f}, y = {3, 4, 2.5f};
    BF3 x3 = [nat bf3:x k:3];
    printf("guest->native bf3: {%u,%u,%g} sumBF3: %d\n", x3.a, x3.b, x3.f, [nat sumBF3:x :y k:2]);
    BFF ff = [nat bff:(BFF){1000, 3, 0.25f}];
    printf("guest->native bff: {%u,%u,%g}\n", ff.a, ff.b, ff.f);
    BFC c = [nat bfc:(BFC){3, 5, 'q'} k:1];
    printf("guest->native bfc: {%u,%u,%c}\n", c.a, c.b, c.c);

    BNEST n = {{2, 6, 'n'}, "guest-to-native"}, m = {{1, 1, 'm'}, "second"};
    BFNestBox nb;
    memset(&nb, 0xa5, sizeof nb);
    BNEST narg = n;
    call_x8(&nb.n, STRIP(objc_msgSend), U(nat), S(@selector(nest:k:)), U(&narg), 4);
    print_bnest("guest->native nest:k: (x8 box)", &nb.n);
    bfh_check_box("guest->native nest:k: result", &nb, sizeof nb, &nb.n, sizeof nb.n);
    printf("guest->native nestSum: %d\n", [nat nestSum:n :m k:3]);

    NST t = [nat nst:(NST){1.25f, {2.5f, 10}}];
    UF2 u = [nat uf:(UF2){.g = {0.5f, 1.5f}} k:4];
    printf("guest->native nst: {%g,{%g,%d}} uf: {%g,%g}\n", t.a, t.s.b, t.s.c, u.g[0], u.g[1]);
}

int main(void)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        printf("== Foundation NSDecimal\n");
        foundation();
        printf("== guest -> native (BFNative)\n");
        native_methods();
        printf("== native -> guest (BFGuest)\n");
        bfh_drive([BFGuest new]);
        printf("== native -> guest blocks\n");
        bfh_block(^NSDecimal(NSDecimal d, int k) { d._mantissa[0] *= (unsigned short)k; return d; },
                  ^BF3(BF3 v, int k) { BF3 r = {v.a * k, v.b + k, v.f * 2}; return r; });
    }
    return 0;
}
