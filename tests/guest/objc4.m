/* libobjc's compiler-emitted C entry points, typed from objc4's own headers (P8b: objc-internal.h,
 * objc-abi.h; none of them is in the SDK), one difference kind per case (tests/guest/objc4.sh):
 *   none      id/Class/pointer arguments and results that hao's register bridge already places:
 *             objc_alloc, objc_alloc_init, objc_opt_new, objc_opt_class, objc_release, objc_autorelease,
 *             objc_retainAutorelease, objc_storeStrong, objc_copyWeak, objc_moveWeak, objc_destroyWeak,
 *             objc_getProperty, objc_setProperty_{atomic,nonatomic}{,_copy}, objc_copyStruct,
 *             objc_copyCppObjectAtomic (a guest function pointer argument). EXACT once typed.
 *   bool      BOOL results: bool on arm64, signed char on x86_64 (objc_opt_isKindOfClass,
 *             objc_opt_respondsToSelector): the whole x0 the guest gets is printed. TYPED B once typed.
 *   arv       objc_autoreleaseReturnValue / objc_retainAutoreleaseReturnValue called outside a return,
 *             so no handshake is involved (arguments and results only).
 *   personality  __objc_personality_v0: data in the unwind info, never called by guest code (control).
 *   control   libobjc functions P3 already typed (objc_retain, objc_initWeak, objc_loadWeakRetained,
 *             objc_opt_self, objc_getClass, object_getClass, class_getName).
 *   xN        objc_retain_xN / objc_release_xN (objc-abi.h, #if __arm64__ only: the object is in xN, the
 *             result in x0, only the AAPCS64 caller-saved registers may change). x86_64 libobjc has no
 *             such functions. Each case calls both with the object in xN from assembly that loads
 *             sentinels into x19-x28 and x0 first, and prints whether the result came from xN and
 *             which callee-saved register or sp changed; also nil, a tagged pointer, and a dealloc
 *             inside objc_release_xN.
 *   elision-* the autorelease-return handshake (objc-config.h HAS_RETURNADDR_AUTORELEASE_ELISION, arm64
 *             only; objc-object.h): a callee's tail call to objc_autoreleaseReturnValue and its caller's
 *             objc_claimAutoreleasedReturnValue (or mov x29, x29 + objc_retainAutoreleasedReturnValue)
 *             hand the object over at +1 without the pool, so it dies when the caller drops it. The
 *             sequences are the ones clang emits under ARC, written out in assembly so they are exact.
 *   objc4 list      the case names
 *   objc4 CASE      that case's lines ("CASE label: value")
 * Built without ARC: every retain/release is the program's own. No pointer is printed; each dealloc
 * prints the label of the line printed before it, so a changed lifetime shows as a changed line.
 * Every case runs in its own process, so a crash costs only its own lines. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* objc4 runtime/objc-internal.h and objc-abi.h (objc4-951.7); the SDK declares none of these. BOOL
 * results are declared uint64_t so that the whole x0 the callee leaves is printed. */
extern id objc_alloc(Class cls);
extern id objc_alloc_init(Class cls);
extern id objc_opt_new(Class cls);
extern Class objc_opt_class(id obj);
extern id objc_opt_self(id obj);
extern uint64_t objc_opt_isKindOfClass(id obj, Class cls);
extern uint64_t objc_opt_respondsToSelector(id obj, SEL sel);
extern id objc_retain(id obj);
extern void objc_release(id obj);
extern id objc_autorelease(id obj);
extern id objc_retainAutorelease(id obj);
extern id objc_autoreleaseReturnValue(id obj);
extern id objc_retainAutoreleaseReturnValue(id obj);
extern void objc_storeStrong(id *location, id obj);
extern id objc_initWeak(id *location, id val);
extern id objc_loadWeakRetained(id *location);
extern void objc_copyWeak(id *to, id *from);
extern void objc_moveWeak(id *to, id *from);
extern void objc_destroyWeak(id *location);
extern id objc_getProperty(id self, SEL _cmd, ptrdiff_t offset, BOOL atomic);
extern void objc_setProperty_atomic(id self, SEL _cmd, id newValue, ptrdiff_t offset);
extern void objc_setProperty_nonatomic(id self, SEL _cmd, id newValue, ptrdiff_t offset);
extern void objc_setProperty_atomic_copy(id self, SEL _cmd, id newValue, ptrdiff_t offset);
extern void objc_setProperty_nonatomic_copy(id self, SEL _cmd, id newValue, ptrdiff_t offset);
extern void objc_copyStruct(void *dest, const void *src, ptrdiff_t size, BOOL atomic, BOOL hasStrong);
extern void objc_copyCppObjectAtomic(void *dest, const void *src, void (*copyHelper)(void *dest, const void *source));

static const char *cur;
static char last[160] = "(start)";
#define P(label, ...) do { printf("%s %s: ", cur, label); printf(__VA_ARGS__); putchar('\n'); \
                           snprintf(last, sizeof last, "%s", label); } while (0)

typedef struct { double x, y, w, h; } Box;

@interface Probe : NSObject {
@public
    id _strong;
    id _copied;
    Box _box;
}
@property (nonatomic, copy) NSString *tag;
@end
@implementation Probe
- (void)dealloc
{
    char l[200];
    snprintf(l, sizeof l, "dealloc %s", _tag ? _tag.UTF8String : "?");
    printf("%s %s: after=%s\n", cur, l, last);
    [_tag release];
    [_strong release];
    [_copied release];
    [super dealloc];
}
@end
@interface SubProbe : Probe @end
@implementation SubProbe @end

/* a class with its own isKindOfClass:/respondsToSelector:/allocWithZone: (custom core: libobjc's fast
 * paths call these through objc_msgSend) */
static int custom_alloc;
@interface Custom : NSObject @end
@implementation Custom
+ (id)allocWithZone:(NSZone *)z { custom_alloc++; return [super allocWithZone:z]; }
- (BOOL)isKindOfClass:(Class)c { return c == [NSString class] ? YES : [super isKindOfClass:c]; }
- (BOOL)respondsToSelector:(SEL)s { return s == @selector(length) ? YES : [super respondsToSelector:s]; }
@end

static Probe *probe(const char *tag)
{
    Probe *p = [Probe new];
    p.tag = [NSString stringWithUTF8String:tag];
    return p;
}

static unsigned long rc(id o) { return (unsigned long)[o retainCount]; }
static const char *cname(id o) { return o ? class_getName(object_getClass(o)) : "nil"; }
static ptrdiff_t ivoff(Class c, const char *n) { return ivar_getOffset(class_getInstanceVariable(c, n)); }

/* ------------------------------------------------------------------ none */

static void c_alloc(void)
{
    @autoreleasepool {
        id a = objc_alloc([Probe class]);
        P("objc_alloc(Probe)", "%s rc=%lu", cname(a), rc(a));
        a = [a init];
        [a setTag:@"a"];
        id b = objc_alloc_init([SubProbe class]);
        [b setTag:@"b"];
        P("objc_alloc_init(SubProbe)", "%s rc=%lu", cname(b), rc(b));
        id n = objc_opt_new([Probe class]);
        [n setTag:@"n"];
        P("objc_opt_new(Probe)", "%s rc=%lu", cname(n), rc(n));
        P("objc_alloc(nil)", "%s", cname(objc_alloc(Nil)));
        P("objc_alloc_init(nil)", "%s", cname(objc_alloc_init(Nil)));
        P("objc_opt_new(nil)", "%s", cname(objc_opt_new(Nil)));
        id c = objc_alloc([Custom class]);
        id d = objc_alloc_init([Custom class]);
        id e = objc_opt_new([Custom class]);
        P("custom allocWithZone", "calls=%d %s %s", custom_alloc, cname(d), cname(e));
        [[c init] release]; [d release]; [e release];
        objc_release(a); objc_release(b); objc_release(n);
        P("released", "ok");
    }
}

static void c_opt(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        SubProbe *s = [SubProbe new];
        s.tag = @"s";
        P("objc_opt_class(Probe)", "%s", class_getName(objc_opt_class(p)));
        P("objc_opt_class(SubProbe)", "%s", class_getName(objc_opt_class(s)));
        P("objc_opt_class(class)", "%d", objc_opt_class((id)[Probe class]) == [Probe class]);
        P("objc_opt_class(nil)", "%d", objc_opt_class(nil) == Nil);
        P("objc_opt_class(constant NSNumber)", "%s", class_getName(objc_opt_class(@3)));
        id tg = [NSNumber numberWithInt:3];
        P("objc_opt_class(tagged NSNumber)", "%d", objc_opt_class(tg) == object_getClass(tg));
        P("objc_opt_class(NSString)", "%d", objc_opt_class(@"x") == [@"x" class]);
        [p release]; [s release];
    }
}

static void c_retain(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        P("start", "rc=%lu", rc(p));
        id r = objc_retainAutorelease(p);
        P("objc_retainAutorelease", "same=%d rc=%lu", r == p, rc(p));
        r = objc_autorelease([p retain]);
        P("objc_autorelease", "same=%d rc=%lu", r == p, rc(p));
        objc_release([p retain]);
        P("objc_release", "rc=%lu", rc(p));
        objc_release(nil);
        P("objc_release(nil)", "ok");
        P("objc_autorelease(nil)", "%d", objc_autorelease(nil) == nil);
        id five = @5;
        P("objc_retainAutorelease(tagged)", "%d", objc_retainAutorelease(five) == five);
        objc_release(p);
        P("released, pool holds 2", "ok");
    }
    P("pool drained", "ok");
}

static void c_strong(void)
{
    @autoreleasepool {
        Probe *p = probe("p"), *q = probe("q");
        id slot = nil;
        objc_storeStrong(&slot, p);
        P("store p", "same=%d rc=%lu", slot == p, rc(p));
        objc_storeStrong(&slot, p);
        P("store p again", "rc=%lu", rc(p));
        objc_storeStrong(&slot, q);
        P("store q", "same=%d rc p=%lu q=%lu", slot == q, rc(p), rc(q));
        [p release];
        P("p released", "ok");
        objc_storeStrong(&slot, nil);
        P("store nil", "slot=%d rc q=%lu", slot == nil, rc(q));
        [q release];
        P("q released", "ok");
    }
}

static void c_weak(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        id w1 = nil, w2 = nil, w3 = nil;
        objc_initWeak(&w1, p);
        objc_copyWeak(&w2, &w1);
        id g = objc_loadWeakRetained(&w2);
        P("objc_copyWeak", "same=%d", g == p);
        [g release];
        objc_moveWeak(&w3, &w2);
        g = objc_loadWeakRetained(&w3);
        P("objc_moveWeak", "from=%d to=%d", w2 == nil, g == p);
        [g release];
        objc_destroyWeak(&w2);
        objc_destroyWeak(&w3);
        P("objc_destroyWeak", "w3=%d", w3 == nil);
        [p release];
        P("p released", "ok");
        g = objc_loadWeakRetained(&w1);
        P("w1 after dealloc", "nil=%d", g == nil);
        objc_destroyWeak(&w1);
        id e = nil;
        objc_copyWeak(&w2, &e);
        P("objc_copyWeak(nil)", "%d", objc_loadWeakRetained(&w2) == nil);
        objc_destroyWeak(&w2);
    }
}

static void c_props(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        ptrdiff_t so = ivoff([Probe class], "_strong"), co = ivoff([Probe class], "_copied");
        NSMutableString *m = [NSMutableString stringWithString:@"one"];
        objc_setProperty_atomic(p, @selector(setStrong:), m, so);
        objc_setProperty_atomic_copy(p, @selector(setCopied:), m, co);
        [m appendString:@"+"];
        P("setProperty_atomic / _atomic_copy", "%s %s", [p->_strong UTF8String], [p->_copied UTF8String]);
        objc_setProperty_nonatomic(p, @selector(setStrong:), @"two", so);
        objc_setProperty_nonatomic_copy(p, @selector(setCopied:), m, co);
        [m appendString:@"+"];
        P("setProperty_nonatomic / _nonatomic_copy", "%s %s", [p->_strong UTF8String], [p->_copied UTF8String]);
        id a = objc_getProperty(p, @selector(strong), so, YES);
        id n = objc_getProperty(p, @selector(copied), co, NO);
        P("getProperty atomic / nonatomic", "%s %s same=%d %d", [a UTF8String], [n UTF8String], a == p->_strong, n == p->_copied);
        Probe *q = probe("q");
        objc_setProperty_atomic(p, @selector(setStrong:), q, so);
        [q release];
        P("set q", "rc=%lu", rc(q));
        objc_setProperty_nonatomic(p, @selector(setStrong:), nil, so);
        P("set nil", "%d", p->_strong == nil);
        [p release];
        P("p released", "ok");
    }
}

static void cpp_copy(void *d, const void *s) { *(long *)d = *(const long *)s * 10 + 1; }

static void c_copystruct(void)
{
    Box src = {1.5, 2.5, 3.5, 4.5}, dst = {0};
    objc_copyStruct(&dst, &src, sizeof dst, YES, NO);
    P("objc_copyStruct atomic", "%.1f %.1f %.1f %.1f", dst.x, dst.y, dst.w, dst.h);
    src.x = 9.25;
    objc_copyStruct(&dst, &src, sizeof dst, NO, NO);
    P("objc_copyStruct nonatomic", "%.2f %.1f", dst.x, dst.h);
    char big[300], bd[300];
    for (int i = 0; i < 300; i++) big[i] = (char)i;
    memset(bd, 0, sizeof bd);
    objc_copyStruct(bd, big, sizeof bd, YES, YES);
    P("objc_copyStruct 300 bytes hasStrong", "%d", memcmp(bd, big, sizeof bd) == 0);
    long s = 41, d = 0;
    objc_copyCppObjectAtomic(&d, &s, cpp_copy);
    P("objc_copyCppObjectAtomic (guest helper)", "%ld", d);
}

/* ------------------------------------------------------------------ bool */

static void c_bool(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        Custom *c = [Custom new];
        P("isKindOfClass(Probe, NSObject)", "x0=%#llx", (unsigned long long)objc_opt_isKindOfClass(p, [NSObject class]));
        P("isKindOfClass(Probe, NSString)", "x0=%#llx", (unsigned long long)objc_opt_isKindOfClass(p, [NSString class]));
        P("isKindOfClass(SubProbe class meta)", "x0=%#llx",
          (unsigned long long)objc_opt_isKindOfClass((id)[SubProbe class], object_getClass([Probe class])));
        P("isKindOfClass(nil)", "x0=%#llx", (unsigned long long)objc_opt_isKindOfClass(nil, [NSObject class]));
        P("isKindOfClass(tagged, NSNumber)", "x0=%#llx",
          (unsigned long long)objc_opt_isKindOfClass([NSNumber numberWithInt:7], [NSNumber class]));
        P("isKindOfClass(custom, NSString)", "x0=%#llx", (unsigned long long)objc_opt_isKindOfClass(c, [NSString class]));
        P("isKindOfClass(custom, NSArray)", "x0=%#llx", (unsigned long long)objc_opt_isKindOfClass(c, [NSArray class]));
        P("respondsToSelector(Probe, tag)", "x0=%#llx", (unsigned long long)objc_opt_respondsToSelector(p, @selector(tag)));
        P("respondsToSelector(Probe, nosuch)", "x0=%#llx",
          (unsigned long long)objc_opt_respondsToSelector(p, sel_registerName("nosuch")));
        P("respondsToSelector(nil)", "x0=%#llx", (unsigned long long)objc_opt_respondsToSelector(nil, @selector(tag)));
        P("respondsToSelector(custom, length)", "x0=%#llx", (unsigned long long)objc_opt_respondsToSelector(c, @selector(length)));
        P("respondsToSelector(custom, count)", "x0=%#llx", (unsigned long long)objc_opt_respondsToSelector(c, @selector(count)));
        P("respondsToSelector(NSString, length)", "x0=%#llx",
          (unsigned long long)objc_opt_respondsToSelector(@"s", @selector(length)));
        /* the way compiled code tests them: BOOL in w0 */
        BOOL (*kind)(id, Class) = (BOOL (*)(id, Class))objc_opt_isKindOfClass;
        BOOL (*resp)(id, SEL) = (BOOL (*)(id, SEL))objc_opt_respondsToSelector;
        int k = 0, r = 0;
        for (int i = 0; i < 64; i++) { k += kind(i & 1 ? p : nil, [Probe class]) == YES; r += resp(i & 2 ? c : p, @selector(length)) == YES; }
        P("as BOOL == YES", "kind=%d resp=%d", k, r);
        [p release]; [c release];
    }
}

/* ------------------------------------------------------------------ arv */

static void c_arv(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        id r = objc_autoreleaseReturnValue([p retain]);
        P("objc_autoreleaseReturnValue (not returned)", "same=%d rc=%lu", r == p, rc(p));
        r = objc_retainAutoreleaseReturnValue(p);
        P("objc_retainAutoreleaseReturnValue (not returned)", "same=%d rc=%lu", r == p, rc(p));
        P("nil", "%d %d", objc_autoreleaseReturnValue(nil) == nil, objc_retainAutoreleaseReturnValue(nil) == nil);
        [p release];
        P("p released, pool holds 2", "ok");
    }
    P("pool drained", "ok");
}

/* ------------------------------------------------------------------ personality */

__attribute__((noinline)) static void thrower(int kind)
{
    if (kind == 0) @throw [NSException exceptionWithName:@"GuestException" reason:@"r" userInfo:nil];
    if (kind == 1) [[NSArray array] objectAtIndex:3];               /* raised by native Foundation */
    @throw (id)probe("thrown");
}

static void c_personality(void)
{
    for (int k = 0; k < 3; k++) {
        int fin = 0;
        @autoreleasepool {
            @try {
                @try { thrower(k); }
                @finally { fin++; }
            } @catch (NSException *e) {
                P(k ? "catch native NSException" : "catch guest NSException", "%s fin=%d", e.name.UTF8String, fin);
            } @catch (Probe *p) {
                P("catch Probe", "%s fin=%d", p.tag.UTF8String, fin);
                [p release];
            }
        }
    }
}

/* ------------------------------------------------------------------ control */

static void c_control(void)
{
    @autoreleasepool {
        Probe *p = probe("p");
        id r = objc_retain(p);
        P("objc_retain", "same=%d rc=%lu", r == p, rc(p));
        objc_release(p);
        P("objc_opt_self", "%d", objc_opt_self(p) == p);
        P("objc_getClass", "%s", class_getName(objc_getClass("SubProbe")));
        P("object_getClass", "%s", class_getName(object_getClass(p)));
        id w = nil;
        objc_initWeak(&w, p);
        id g = objc_loadWeakRetained(&w);
        P("objc_initWeak / loadWeakRetained", "%d", g == p);
        [g release];
        objc_destroyWeak(&w);
        [p release];
        P("p released", "ok");
    }
}

/* ------------------------------------------------------------------ register variants */

/* rr_{retain,release}_xN(obj, out): x19-x28 = 0x1900 + 0x101*(n-19) style sentinels, x0 = 0x7777 (unless
 * N is 0), xN = obj; bl objc_{retain,release}_xN; out[0] = x0, out[1..10] = x19..x28, out[11] = x29 - sp. */
#define RR_PRO \
    "sub sp, sp, #112\n stp x29, x30, [sp, #96]\n add x29, sp, #96\n" \
    "stp x19, x20, [sp, #0]\n stp x21, x22, [sp, #16]\n stp x23, x24, [sp, #32]\n" \
    "stp x25, x26, [sp, #48]\n stp x27, x28, [sp, #64]\n str x1, [sp, #80]\n mov x9, x0\n" \
    "mov x19, #0x1919\n mov x20, #0x2020\n mov x21, #0x2121\n mov x22, #0x2222\n mov x23, #0x2323\n" \
    "mov x24, #0x2424\n mov x25, #0x2525\n mov x26, #0x2626\n mov x27, #0x2727\n mov x28, #0x2828\n"
#define RR_EPI \
    "ldr x9, [sp, #80]\n str x0, [x9]\n stp x19, x20, [x9, #8]\n stp x21, x22, [x9, #24]\n" \
    "stp x23, x24, [x9, #40]\n stp x25, x26, [x9, #56]\n stp x27, x28, [x9, #72]\n" \
    "mov x10, sp\n sub x10, x29, x10\n str x10, [x9, #88]\n" \
    "ldp x19, x20, [sp, #0]\n ldp x21, x22, [sp, #16]\n ldp x23, x24, [sp, #32]\n" \
    "ldp x25, x26, [sp, #48]\n ldp x27, x28, [sp, #64]\n ldp x29, x30, [sp, #96]\n add sp, sp, #112\n ret\n"
#define RR_ONE(op, N, junk) \
    ".text\n .p2align 2\n .globl _rr_" #op "_x" #N "\n_rr_" #op "_x" #N ":\n" RR_PRO "mov x" #N ", x9\n" junk \
    "bl _objc_" #op "_x" #N "\n" RR_EPI
#define RR(N, junk) \
    asm(RR_ONE(retain, N, junk) RR_ONE(release, N, junk)); \
    extern void rr_retain_x##N(id, uint64_t *); \
    extern void rr_release_x##N(id, uint64_t *);
#define J "mov x0, #0x7777\n"
RR(0, "") RR(1, J) RR(2, J) RR(3, J) RR(4, J) RR(5, J) RR(6, J) RR(7, J) RR(8, J) RR(9, J) RR(10, J)
RR(11, J) RR(12, J) RR(13, J) RR(14, J) RR(15, J) RR(19, J) RR(20, J) RR(21, J) RR(22, J) RR(23, J)
RR(24, J) RR(25, J) RR(26, J) RR(27, J) RR(28, J)
#define RRE(N) {N, rr_retain_x##N, rr_release_x##N}
static const struct rre { int n; void (*retain)(id, uint64_t *), (*release)(id, uint64_t *); } rrs[] = {
    RRE(0), RRE(1), RRE(2), RRE(3), RRE(4), RRE(5), RRE(6), RRE(7), RRE(8), RRE(9), RRE(10), RRE(11),
    RRE(12), RRE(13), RRE(14), RRE(15), RRE(19), RRE(20), RRE(21), RRE(22), RRE(23), RRE(24), RRE(25),
    RRE(26), RRE(27), RRE(28),
};

/* which callee-saved registers (and the frame) the call changed */
static const char *rr_regs(const uint64_t *o, int n, id obj)
{
    static char b[200];
    b[0] = 0;
    for (int r = 19; r <= 28; r++) {
        uint64_t want = r == n ? (uint64_t)(uintptr_t)obj : (uint64_t)(0x101 * (r / 10 * 16 + r % 10));
        if (o[r - 18] != want) snprintf(b + strlen(b), sizeof b - strlen(b), "x%d ", r);
    }
    if (o[11] != 96) snprintf(b + strlen(b), sizeof b - strlen(b), "sp ");
    if (!b[0]) return "kept";
    b[strlen(b) - 1] = 0;
    return b;
}

static void c_xvar(const struct rre *e)
{
    uint64_t o[12];
    char l[64];
    @autoreleasepool {
        Probe *p = probe("p");
        e->retain(p, o);
        snprintf(l, sizeof l, "objc_retain_x%d", e->n);
        P(l, "result=%s rc=%lu callee-saved %s", (id)(uintptr_t)o[0] == p ? "obj" : o[0] == 0x7777 ? "x0 unchanged" : "other",
          rc(p), rr_regs(o, e->n, p));
        e->release(p, o);
        snprintf(l, sizeof l, "objc_release_x%d", e->n);
        P(l, "rc=%lu callee-saved %s", rc(p), rr_regs(o, e->n, p));
        e->retain(nil, o);
        P("nil", "result=%s", o[0] == 0 ? "nil" : "other");
        e->release(nil, o);
        id t = [NSNumber numberWithInt:11];
        e->retain(t, o);
        P("tagged", "result=%s", (id)(uintptr_t)o[0] == t ? "obj" : "other");
        e->release(t, o);
        P("last release", "next");
        e->release(p, o);
        P("after last release", "callee-saved %s", rr_regs(o, e->n, p));
    }
}

/* ------------------------------------------------------------------ elision */

/* el_make: [Probe new] returned through a tail call to objc_autoreleaseReturnValue (what clang emits for
 * `return [Probe new];` under ARC); el_pass(o): o returned through objc_retainAutoreleaseReturnValue.
 * The callers: bl + objc_claimAutoreleasedReturnValue (+1), bl + mov x29, x29 +
 * objc_retainAutoreleasedReturnValue (+1), bl + objc_unsafeClaimAutoreleasedReturnValue (+0). */
id el_new(void);
id el_new(void) { return probe("e"); }
asm(".text\n .p2align 2\n"
    "_el_make:\n stp x29, x30, [sp, #-16]!\n mov x29, sp\n bl _el_new\n ldp x29, x30, [sp], #16\n"
    " b _objc_autoreleaseReturnValue\n"
    "_el_pass:\n b _objc_retainAutoreleaseReturnValue\n"
    ".globl _el_claim\n_el_claim:\n stp x29, x30, [sp, #-16]!\n mov x29, sp\n bl _el_make\n"
    " bl _objc_claimAutoreleasedReturnValue\n ldp x29, x30, [sp], #16\n ret\n"
    ".globl _el_rarv\n_el_rarv:\n stp x29, x30, [sp, #-16]!\n mov x29, sp\n bl _el_make\n mov x29, x29\n"
    " bl _objc_retainAutoreleasedReturnValue\n ldp x29, x30, [sp], #16\n ret\n"
    ".globl _el_unsafe\n_el_unsafe:\n stp x29, x30, [sp, #-16]!\n mov x29, sp\n bl _el_make\n"
    " bl _objc_unsafeClaimAutoreleasedReturnValue\n ldp x29, x30, [sp], #16\n ret\n"
    ".globl _el_passclaim\n_el_passclaim:\n stp x29, x30, [sp, #-16]!\n mov x29, sp\n bl _el_pass\n"
    " bl _objc_claimAutoreleasedReturnValue\n ldp x29, x30, [sp], #16\n ret\n");
extern id el_claim(void), el_rarv(void), el_unsafe(void), el_passclaim(id);

static void c_elision(int k)
{
    @autoreleasepool {
        id o = nil;
        if (k == 0) o = el_claim();
        if (k == 1) o = el_rarv();
        if (k == 2) { id w = nil; o = el_unsafe(); objc_initWeak(&w, o); P("unsafe claim returned", "ok");
                      id g = objc_loadWeakRetained(&w); P("alive after return", "%d", g != nil); [g release];
                      objc_destroyWeak(&w); o = nil; }
        if (k == 3) { Probe *p = probe("e"); o = el_passclaim(p); [p release]; }
        if (o) {
            P("holding", "rc=%lu", rc(o));
            [o release];
            P("dropped", "ok");
        }
        P("pool drains", "next");
    }
    P("pool drained", "ok");
}

/* ------------------------------------------------------------------ driver */

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"alloc", c_alloc}, {"opt", c_opt}, {"retain", c_retain}, {"strong", c_strong}, {"weak", c_weak},
    {"props", c_props}, {"copystruct", c_copystruct}, {"bool", c_bool}, {"arv", c_arv},
    {"personality", c_personality}, {"control", c_control},
};
static const char *elision[] = {"elision-claim", "elision-rarv", "elision-unsafe", "elision-passclaim"};

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) printf("%s\n", cases[i].name);
        for (size_t i = 0; i < sizeof rrs / sizeof *rrs; i++) printf("x%d\n", rrs[i].n);
        for (size_t i = 0; i < 4; i++) printf("%s\n", elision[i]);
        return 0;
    }
    cur = argv[1];
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        if (!strcmp(argv[1], cases[i].name)) { cases[i].fn(); return 0; }
    for (size_t i = 0; i < sizeof rrs / sizeof *rrs; i++) {
        char n[8];
        snprintf(n, sizeof n, "x%d", rrs[i].n);
        if (!strcmp(argv[1], n)) { c_xvar(&rrs[i]); return 0; }
    }
    for (int i = 0; i < 4; i++)
        if (!strcmp(argv[1], elision[i])) { c_elision(i); return 0; }
    fprintf(stderr, "objc4: no case %s\n", argv[1]);
    return 2;
}
