/* tests/guest/objcimp.sh: guest code calling native ObjC methods through
 * IMPs it got from the runtime, and native blocks, with arguments and results
 * the two ABIs pass differently (MenuBarAgent: the captured -[CALayer
 * setBounds:] and setTransform: IMPs). One section per argument;
 * each prints what the native object then holds. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import "objcimphelper.h"
#include <stdio.h>
#include <string.h>

typedef void (*SetRect)(id, SEL, HRect);
typedef void (*SetXf)(id, SEL, HTransform);
typedef void (*SetPt)(id, SEL, HPoint);
typedef HRect (*GetRect)(id, SEL);
typedef HTransform (*GetXf)(id, SEL);
typedef BOOL (*GetBool)(id, SEL);
typedef HTriple (*GetTriple)(id, SEL);
typedef float (*Scale)(id, SEL, float, HRect);

static const HRect R = {{1.5, 2.25}, {300.125, 40.5}};
static HTransform X(void)
{
    HTransform t;
    for (int k = 0; k < 16; k++) t.m[k] = k * 1.25 - 3;
    return t;
}
static void show(const char *what, HaoLayer *l)
{
    HRect b = [l bounds];
    HTransform t = [l transform];
    HPoint p = [l position];
    double ts = 0;
    for (int k = 0; k < 16; k++) ts += t.m[k] * (k + 1);
    printf("%-10s bounds {%.17g %.17g %.17g %.17g} transform %.17g position {%.17g %.17g}\n", what, b.origin.x, b.origin.y,
           b.size.w, b.size.h, ts, p.x, p.y);
}
/* the IMPs every section calls, from one route each */
static void call_all(const char *what, HaoLayer *l, IMP sb, IMP st, IMP sp, IMP gb, IMP gt, IMP wide, IMP tri, IMP sc)
{
    ((SetRect)sb)(l, @selector(setBounds:), R);
    ((SetXf)st)(l, @selector(setTransform:), X());
    ((SetPt)sp)(l, @selector(setPosition:), (HPoint){5.5, -6.75});
    show(what, l);
    HRect b = ((GetRect)gb)(l, @selector(bounds));
    HTransform t = ((GetXf)gt)(l, @selector(transform));
    BOOL w = ((GetBool)wide)(l, @selector(isWide));
    HTriple tr = ((GetTriple)tri)(l, @selector(triple));
    float f = ((Scale)sc)(l, @selector(scale:rect:), 0.5f, R);
    printf("%-10s via IMP: bounds.w %.17g transform[15] %.17g wide %d triple {%ld %ld %ld} scale %.9g\n", what, b.size.w, t.m[15],
           (int)w, tr.a, tr.b, tr.c, (double)f);
}
#define SELS @selector(setBounds:), @selector(setTransform:), @selector(setPosition:), @selector(bounds), \
             @selector(transform), @selector(isWide), @selector(triple), @selector(scale:rect:)

static void gate(id self, SEL _cmd, HRect r) { (void)self; (void)_cmd; (void)r; }   /* MenuBarAgent's `ret` override */

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "";
    Class C = [HaoLayer class];
    SEL sels[] = {SELS};
    IMP imp[8];
    HaoLayer *l = [HaoLayer new];
    if (!strcmp(s, "getimp")) {
        for (int k = 0; k < 8; k++) imp[k] = class_getMethodImplementation(C, sels[k]);
        call_all("getimp", l, imp[0], imp[1], imp[2], imp[3], imp[4], imp[5], imp[6], imp[7]);
    } else if (!strcmp(s, "method")) {
        for (int k = 0; k < 8; k++) imp[k] = method_getImplementation(class_getInstanceMethod(C, sels[k]));
        call_all("method", l, imp[0], imp[1], imp[2], imp[3], imp[4], imp[5], imp[6], imp[7]);
    } else if (!strcmp(s, "methodfor")) {
        for (int k = 0; k < 8; k++) imp[k] = k & 1 ? [l methodForSelector:sels[k]] : [HaoLayer instanceMethodForSelector:sels[k]];
        call_all("methodfor", l, imp[0], imp[1], imp[2], imp[3], imp[4], imp[5], imp[6], imp[7]);
    } else if (!strcmp(s, "replace")) {
        /* the old IMP class_replaceMethod returns, called from the replacement */
        Method m = class_getInstanceMethod(C, @selector(setBounds:));
        IMP old = class_replaceMethod(C, @selector(setBounds:), (IMP)gate, method_getTypeEncoding(m));
        ((SetRect)old)(l, @selector(setBounds:), R);
        show("replace", l);
        class_replaceMethod(C, @selector(setBounds:), old, method_getTypeEncoding(m));
    } else if (!strcmp(s, "gated")) {
        /* MenuBarAgent: a runtime subclass whose setBounds:/setTransform: are no-ops, and whose
         * setIntentional*: blocks tail-call the IMPs captured from the native class */
        Class G = objc_allocateClassPair(C, "HaoGatedLayer", 0);
        Method mb = class_getInstanceMethod(C, @selector(setBounds:)), mt = class_getInstanceMethod(C, @selector(setTransform:));
        IMP nb = class_getMethodImplementation(C, @selector(setBounds:)), nt = class_getMethodImplementation(C, @selector(setTransform:));
        class_addMethod(G, @selector(setBounds:), (IMP)gate, method_getTypeEncoding(mb));
        class_addMethod(G, @selector(setTransform:), (IMP)gate, method_getTypeEncoding(mt));
        class_addMethod(G, sel_registerName("setIntentionalBounds:"),
                        imp_implementationWithBlock(^(id me, HRect r) { ((SetRect)nb)(me, @selector(setBounds:), r); }),
                        method_getTypeEncoding(mb));
        class_addMethod(G, sel_registerName("setIntentionalTransform:"),
                        imp_implementationWithBlock(^(id me, HTransform t) { ((SetXf)nt)(me, @selector(setTransform:), t); }),
                        method_getTypeEncoding(mt));
        objc_registerClassPair(G);
        HaoLayer *g = [G new];
        [g setBounds:(HRect){{9, 9}, {9, 9}}];   /* gated: no effect */
        ((void (*)(id, SEL, HRect))objc_msgSend)(g, sel_registerName("setIntentionalBounds:"), R);
        ((void (*)(id, SEL, HTransform))objc_msgSend)(g, sel_registerName("setIntentionalTransform:"), X());
        show("gated", g);
    } else if (!strcmp(s, "nilfirst")) {
        /* the IMP's first call has a nil receiver (the method does not touch self), the next a real one */
        IMP sc = class_getMethodImplementation(C, @selector(scale:rect:));
        float f0 = ((Scale)sc)(nil, @selector(scale:rect:), 0.25f, R);
        float f1 = ((Scale)sc)(l, @selector(scale:rect:), 0.5f, R);
        printf("nilfirst   scale %.9g %.9g\n", (double)f0, (double)f1);
    } else if (!strcmp(s, "exchange")) {
        /* an IMP captured before method_exchangeImplementations, called after: it now sits under the other selector */
        IMP sb = class_getMethodImplementation(C, @selector(setBounds:));
        Method a = class_getInstanceMethod(C, @selector(setBounds:));
        SEL alt = sel_registerName("setOtherBounds:");
        class_addMethod(C, alt, (IMP)gate, method_getTypeEncoding(a));
        method_exchangeImplementations(a, class_getInstanceMethod(C, alt));
        ((SetRect)sb)(l, @selector(setBounds:), R);
        show("exchange", l);
    } else if (!strcmp(s, "block")) {
        HRectBlock b = [HaoLayer rectBlock];   /* a method's block result ('@?') */
        b(R);
        printf("block      method's block: %.17g\n", hao_last_sum());
        HRectBlock c = hao_rect_block();       /* a C function's: nothing types it but the block itself */
        c(R);
        printf("block      C function's block: %.17g\n", hao_last_sum());
    } else if (!strcmp(s, "blocksig")) {
        /* native blocks whose signature crosses a page or is long: typed wherever it lies */
        for (int i = 0; i < HAO_SPLIT_CASES; i++) {
            HRectBlock b = hao_split_block(i);
            if (!b) { printf("blocksig   case %d: no block\n", i); continue; }
            b(R);
            printf("blocksig   case %d: %.17g\n", i, hao_last_sum());
        }
    } else {
        fprintf(stderr, "usage: objcimp getimp|method|methodfor|replace|gated|nilfirst|exchange|block|blocksig\n");
        return 98;
    }
    return 0;
}
