/* See bfhelper.h. */
#import "bfhelper.h"
#include <objc/runtime.h>
#include <stdio.h>

void bfh_print_dec(const char *what, const NSDecimal *d)
{
    printf("%s: %s (exp %d len %u neg %u mant %u %u %u)\n", what, NSDecimalString(d, nil).UTF8String,
           d->_exponent, d->_length, d->_isNegative, d->_mantissa[0], d->_mantissa[1], d->_mantissa[2]);
}

/* every byte of the box outside the result slot must still be 0xa5 */
void bfh_check_box(const char *what, const void *box, size_t size, const void *slot, size_t slot_size)
{
    const uint8_t *b = box, *s = slot;
    int bad = 0, first = -1;
    for (size_t i = 0; i < size; i++) {
        if (b + i >= s && b + i < s + slot_size) continue;
        if (b[i] != 0xa5) { bad++; if (first < 0) first = (int)(b + i - (s + slot_size)); }
    }
    if (bad) printf("%s: OVERRUN: %d canary bytes overwritten, first at slot end %+d\n", what, bad, first);
    else printf("%s: canaries intact\n", what);
}

static void print_bnest(const char *what, const BNEST *n)
{
    printf("%s: {%u,%u,%d} %.20s\n", what, n->s.a, n->s.b, n->s.c, n->tail);
}

@implementation BFNative
- (NSDecimal)dec:(NSDecimal)d plus:(int)k { d._mantissa[0] += (unsigned short)k; return d; }
- (NSString *)describe:(NSDecimal)a tag:(int)t other:(NSDecimal)b
{
    return [NSString stringWithFormat:@"%@ #%d %@", NSDecimalString(&a, nil), t, NSDecimalString(&b, nil)];
}
- (BF3)bf3:(BF3)x k:(int)k { BF3 r = {x.a + k, x.b * 2, x.f * k}; return r; }
- (int)sumBF3:(BF3)x :(BF3)y k:(int)k { return (int)(x.a + x.b + y.a + y.b + x.f + y.f) * k; }
- (BFF)bff:(BFF)x { BFF r = {x.a + 1, x.b + 2, x.f + 3}; return r; }
- (BFC)bfc:(BFC)x k:(int)k { BFC r = {x.a + 1, x.b + 2, x.c + k}; return r; }
- (BNEST)nest:(BNEST)n k:(int)k
{
    BNEST r = n;
    r.s.a = n.s.b; r.s.b = n.s.a; r.s.c = n.s.c + k;
    for (int i = 0; i < 19 && r.tail[i]; i++) r.tail[i] = (char)(r.tail[i] - 'a' + 'A');
    return r;
}
- (int)nestSum:(BNEST)a :(BNEST)b k:(int)k { return (a.s.a + a.s.b + a.s.c + a.tail[0]) * 1000 + (b.s.a + b.s.b + b.s.c + b.tail[0]) + k; }
- (NST)nst:(NST)x { NST r = {x.a * 2, {x.s.b + 1, x.s.c - 1}}; return r; }
- (UF2)uf:(UF2)u k:(int)k { UF2 r; r.g[0] = u.g[1] * k; r.g[1] = u.g[0] + k; return r; }
@end

static IMP imp_of(id o, SEL s) { return method_getImplementation(class_getInstanceMethod(object_getClass(o), s)); }

/* Results returned through memory are written into a canary box. On
 * x86_64 the IMP is called with the box slot as the explicit hidden first
 * argument (SysV passes a MEMORY-class result's buffer that way), so the
 * callee writes the slot itself; arm64 needs no check (no hao there). */
void bfh_drive(id g)
{
    NSDecimal d = [[NSDecimalNumber decimalNumberWithString:@"-7.25"] decimalValue];
    NSDecimal e = [[NSDecimalNumber decimalNumberWithString:@"1000.5"] decimalValue];
    BFDecBox box;
    memset(&box, 0xa5, sizeof box);
    SEL sel = @selector(dec:plus:);
#if defined(__x86_64__)
    ((void (*)(NSDecimal *, id, SEL, NSDecimal, int))imp_of(g, sel))(&box.d, g, sel, d, 5);
#else
    box.d = ((NSDecimal (*)(id, SEL, NSDecimal, int))imp_of(g, sel))(g, sel, d, 5);
#endif
    bfh_print_dec("native->guest dec:plus:", &box.d);
    bfh_check_box("native->guest dec:plus: result", &box, sizeof box, &box.d, sizeof box.d);
    NSDecimal r = [g dec:e plus:7];
    bfh_print_dec("native->guest dec:plus: (msgSend)", &r);
    printf("native->guest describe: %s\n", [[g describe:d tag:9 other:e] UTF8String]);

    BF3 x = {7, 9, 1.5f}, y = {3, 4, 2.5f};
    BF3 x3 = [g bf3:x k:3];
    printf("native->guest bf3: {%u,%u,%g} sumBF3: %d\n", x3.a, x3.b, x3.f, [g sumBF3:x :y k:2]);
    BFF ff = [g bff:(BFF){1000, 3, 0.25f}];
    printf("native->guest bff: {%u,%u,%g}\n", ff.a, ff.b, ff.f);
    BFC c = [g bfc:(BFC){3, 5, 'q'} k:1];
    printf("native->guest bfc: {%u,%u,%c}\n", c.a, c.b, c.c);

    BNEST n = {{2, 6, 'n'}, "native-to-guest"}, m = {{1, 1, 'm'}, "second"};
    BFNestBox nb;
    memset(&nb, 0xa5, sizeof nb);
    sel = @selector(nest:k:);
#if defined(__x86_64__)
    ((void (*)(BNEST *, id, SEL, BNEST, int))imp_of(g, sel))(&nb.n, g, sel, n, 4);
#else
    nb.n = ((BNEST (*)(id, SEL, BNEST, int))imp_of(g, sel))(g, sel, n, 4);
#endif
    print_bnest("native->guest nest:k:", &nb.n);
    bfh_check_box("native->guest nest:k: result", &nb, sizeof nb, &nb.n, sizeof nb.n);
    printf("native->guest nestSum: %d\n", [g nestSum:n :m k:3]);

    NST t = [g nst:(NST){1.25f, {2.5f, 10}}];
    UF2 u = [g uf:(UF2){.g = {0.5f, 1.5f}} k:4];
    printf("native->guest nst: {%g,{%g,%d}} uf: {%g,%g}\n", t.a, t.s.b, t.s.c, u.g[0], u.g[1]);
}

void bfh_block(bfh_dec_block_t b, bfh_bf3_block_t b3)
{
    NSDecimal d = [[NSDecimalNumber decimalNumberWithString:@"3.75"] decimalValue];
    BFDecBox box;
    memset(&box, 0xa5, sizeof box);
#if defined(__x86_64__)
    /* a block literal's invoke takes the block itself as its first argument */
    struct lit { void *isa; int flags, reserved; void *invoke; } *lit = (__bridge void *)b;
    ((void (*)(NSDecimal *, id, NSDecimal, int))lit->invoke)(&box.d, b, d, 11);
#else
    box.d = b(d, 11);
#endif
    bfh_print_dec("native->guest block", &box.d);
    bfh_check_box("native->guest block result", &box, sizeof box, &box.d, sizeof box.d);
    BF3 x = b3((BF3){1, 2, 0.5f}, 6);
    printf("native->guest bf3 block: {%u,%u,%g}\n", x.a, x.b, x.f);
}
