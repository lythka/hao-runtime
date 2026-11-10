/* Guest C function pointers passed to native ObjC methods ('^?' parameters
 * in the x86_64 method's encoding), which the native method calls: each must
 * reach native code as an entry point (a thunk), not as the guest address,
 * whose every call would take the NX-fault path (dscl's comparator through
 * -[NSArray sortedArrayUsingFunction:context:]: thousands of faults).
 * Pointers the encoding does not type as code (void * contexts, values
 * handed back) keep their guest addresses, so comparisons with them hold.
 *
 *   msgfnptr SECTION     one section per run (tests/guest/msgfnptr.sh)
 *
 * Sections:
 *   sorted   -[NSArray sortedArrayUsingFunction:context:] and ...:hint: (x2)
 *   sort     -[NSMutableArray sortUsingFunction:context:] and ...:range: (x2,
 *            an NSRange after it)
 *   slots    the native helper (msgfnptrhelper.m): x6, the first stack slot,
 *            sp+8 after packed char/int stack arguments, x2 between doubles,
 *            a field of a 16-byte aggregate by value, of a 32-byte one (the
 *            stack and 32-byte cases take hao_call_native_sig, which thunked
 *            them before: regression guards, as is imp)
 *   super    a guest subclass of the helper: [super ...] (objc_msgSendSuper2)
 *            and objc_msgSendSuper called directly, a stack slot included
 *   imp      the native IMPs called directly: methodForSelector:,
 *            class_getMethodImplementation, method_getImplementation
 *   invoke   NSInvocation: the comparator set with setArgument:atIndex:,
 *            then -invoke and -invokeWithTarget:
 *   ptrfn    NSPointerFunctions' hash/isEqual/description functions, which an
 *            NSHashTable calls natively later
 *   ident    identity: a '^?' getter returns the guest's own function address
 *            (the helper's -fn, NSPointerFunctions' -hashFunction), a void *
 *            context holding a guest function address comes back unchanged
 *            (the comparator's context, the helper's -ctx, a KVO context)
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#import "msgfnptrhelper.h"

/* CoreFoundation's (no SDK header declares it): v48@0:8^?16^v24{_NSRange=QQ}32 */
@interface NSMutableArray (MFRange)
- (void)sortUsingFunction:(NSInteger (*)(id, id, void *))f context:(void *)c range:(NSRange)r;
@end

static long ncalls;

static NSInteger cmp_num(id a, id b, void *ctx)
{
    ncalls++;
    long dir = ctx ? *(long *)ctx : 1;
    NSInteger r = [(NSNumber *)a compare:(NSNumber *)b];
    return dir * r;
}

static void print_array(const char *what, NSArray *a)
{
    printf("%s:", what);
    for (NSNumber *n in a) printf(" %ld", n.longValue);
    printf(" (comparator %s)\n", ncalls ? "called" : "not called");
    ncalls = 0;
}

static NSArray *numbers(void)
{
    return @[@5, @3, @9, @1, @7, @2, @8, @6, @4, @0, @11, @10];
}

static void sec_sorted(void)
{
    long down = -1;
    print_array("sorted up", [numbers() sortedArrayUsingFunction:cmp_num context:NULL]);
    print_array("sorted down", [numbers() sortedArrayUsingFunction:cmp_num context:&down]);
    NSArray *s = [numbers() sortedArrayUsingFunction:cmp_num context:NULL];
    ncalls = 0;
    NSData *hint = [s sortedArrayHint];
    print_array("sorted hint", [numbers() sortedArrayUsingFunction:cmp_num context:NULL hint:hint]);
}

static void sec_sort(void)
{
    long down = -1;
    NSMutableArray *m = [numbers() mutableCopy];
    [m sortUsingFunction:cmp_num context:NULL];
    print_array("sort up", m);
    [m sortUsingFunction:cmp_num context:&down];
    print_array("sort down", m);
    m = [numbers() mutableCopy];
    [m sortUsingFunction:cmp_num context:NULL range:NSMakeRange(2, 7)];
    print_array("sort range 2..8", m);
}

static long add1000(long v) { ncalls++; return v + 1000; }
static double half(double v) { ncalls++; return v / 2; }

static void report(const char *what, long v)
{
    printf("%s = %ld (fn %s)\n", what, v, ncalls ? "called" : "not called");
    ncalls = 0;
}

static void run_slots(MFHelper *h)
{
    report("x6", [h call:1 b:2 c:3 d:4 fn:add1000]);
    report("sp+0", [h stack:1 b:2 c:3 d:4 e:5 f:6 fn:add1000]);
    report("sp+8 packed", [h packed:1 b:2 c:3 d:4 e:5 f:6 g:7 fn:add1000]);
    double d = [h fp:10 fn:half y:0.25];
    printf("x2 between d0/d1 = %g (fn %s)\n", d, ncalls ? "called" : "not called");
    ncalls = 0;
    report("rec", [h rec:(MFRec){add1000, 42}]);
    report("big", [h big:(MFBig){1, add1000, 2, 3}]);
}

static void sec_slots(void) { run_slots([MFHelper new]); }

@interface GHelper : MFHelper
@end
@implementation GHelper
- (long)call:(long)a b:(long)b c:(long)c d:(long)d fn:(mf_fn)fn { return [super call:a b:b c:c d:d fn:fn] + 1; }
- (long)stack:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(long)f fn:(mf_fn)fn
{
    return [super stack:a b:b c:c d:d e:e f:f fn:fn] + 1;
}
- (double)fp:(double)x fn:(mf_dfn)fn y:(double)y { return [super fp:x fn:fn y:y] + 1; }
@end

static void sec_super(void)
{
    GHelper *g = [GHelper new];
    report("super x6", [g call:1 b:2 c:3 d:4 fn:add1000]);
    report("super sp+0", [g stack:1 b:2 c:3 d:4 e:5 f:6 fn:add1000]);
    double d = [g fp:10 fn:half y:0.25];
    printf("super fp = %g (fn %s)\n", d, ncalls ? "called" : "not called");
    ncalls = 0;
    struct objc_super su = {g, [MFHelper class]};
    long r = ((long (*)(struct objc_super *, SEL, long, long, long, long, mf_fn))objc_msgSendSuper)(
        &su, @selector(call:b:c:d:fn:), 1, 2, 3, 4, add1000);
    report("objc_msgSendSuper x6", r);
    r = ((long (*)(struct objc_super *, SEL, long, long, long, long, long, long, mf_fn))objc_msgSendSuper)(
        &su, @selector(stack:b:c:d:e:f:fn:), 1, 2, 3, 4, 5, 6, add1000);
    report("objc_msgSendSuper sp+0", r);
    /* the inherited methods (no override: objc_msgSend to the native IMP with a guest receiver) */
    run_slots(g);
}

typedef NSArray *(*sorted_imp)(id, SEL, NSInteger (*)(id, id, void *), void *);
typedef long (*call_imp)(id, SEL, long, long, long, long, mf_fn);

static void sec_imp(void)
{
    NSArray *a = numbers();
    SEL s = @selector(sortedArrayUsingFunction:context:);
    sorted_imp f = (sorted_imp)[a methodForSelector:s];
    print_array("methodForSelector:", f(a, s, cmp_num, NULL));
    f = (sorted_imp)class_getMethodImplementation(object_getClass(a), s);
    print_array("class_getMethodImplementation", f(a, s, cmp_num, NULL));
    f = (sorted_imp)method_getImplementation(class_getInstanceMethod(object_getClass(a), s));
    print_array("method_getImplementation", f(a, s, cmp_num, NULL));
    MFHelper *h = [MFHelper new];
    SEL c = @selector(call:b:c:d:fn:);
    call_imp ci = (call_imp)[h methodForSelector:c];
    report("helper IMP x6", ci(h, c, 1, 2, 3, 4, add1000));
}

static void sec_invoke(void)
{
    NSArray *a = numbers();
    SEL s = @selector(sortedArrayUsingFunction:context:);
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[a methodSignatureForSelector:s]];
    inv.selector = s;
    inv.target = a;
    NSInteger (*fn)(id, id, void *) = cmp_num;
    void *ctx = NULL;
    [inv setArgument:&fn atIndex:2];
    [inv setArgument:&ctx atIndex:3];
    [inv invoke];
    __unsafe_unretained NSArray *r = nil;
    [inv getReturnValue:&r];
    print_array("invoke", r);
    long down = -1;
    void *dctx = &down;
    [inv setArgument:&dctx atIndex:3];
    [inv invokeWithTarget:@[@4, @1, @3]];
    [inv getReturnValue:&r];
    print_array("invokeWithTarget:", r);
    /* the helper through an invocation: fn in x6 */
    MFHelper *h = [MFHelper new];
    SEL c = @selector(call:b:c:d:fn:);
    NSInvocation *hi = [NSInvocation invocationWithMethodSignature:[h methodSignatureForSelector:c]];
    hi.selector = c;
    long v[4] = {1, 2, 3, 4};
    for (int k = 0; k < 4; k++) [hi setArgument:&v[k] atIndex:2 + k];
    mf_fn af = add1000;
    [hi setArgument:&af atIndex:6];
    [hi invokeWithTarget:h];
    long rv = 0;
    [hi getReturnValue:&rv];
    report("invoke helper x6", rv);
}

static long nhash, nequal, ndesc;
static NSUInteger pf_hash(const void *item, NSUInteger (*size)(const void *)) { (void)size; nhash++; return (uintptr_t)item % 7; }
static BOOL pf_equal(const void *a, const void *b, NSUInteger (*size)(const void *)) { (void)size; nequal++; return a == b; }
static NSString *pf_desc(const void *item) { ndesc++; return [NSString stringWithFormat:@"<%lu>", (unsigned long)(uintptr_t)item]; }

static void sec_ptrfn(void)
{
    NSPointerFunctions *pf = [NSPointerFunctions pointerFunctionsWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    pf.hashFunction = pf_hash;
    pf.isEqualFunction = pf_equal;
    pf.descriptionFunction = pf_desc;
    NSHashTable *t = [[NSHashTable alloc] initWithPointerFunctions:pf capacity:4];
    /* opaque pointers through the C interface (ARC would retain them as objects) */
    for (uintptr_t k = 1; k <= 20; k++) NSHashInsert(t, (const void *)(k * 8));
    NSHashInsert(t, (const void *)(uintptr_t)16);
    printf("hash table count %lu, member 24 %s, member 200 %s\n", (unsigned long)NSCountHashTable(t),
           NSHashGet(t, (const void *)(uintptr_t)24) ? "yes" : "no", NSHashGet(t, (const void *)(uintptr_t)200) ? "yes" : "no");
    NSString *d = NSStringFromHashTable(t);
    printf("description mentions <8>: %s\n", [d containsString:@"<8>"] ? "yes" : "no");
    printf("hash %s, isEqual %s, description %s\n", nhash ? "called" : "not called", nequal ? "called" : "not called",
           ndesc ? "called" : "not called");
}

@interface KVOTarget : NSObject
@property long value;
@end
@implementation KVOTarget
@end
@interface KVOObserver : NSObject
@property long seen, matched;
@end
@implementation KVOObserver
- (void)observeValueForKeyPath:(NSString *)kp ofObject:(id)o change:(NSDictionary *)ch context:(void *)ctx
{
    self.seen++;
    if (ctx == (void *)add1000) self.matched++;
}
@end

static NSInteger cmp_ctx(id a, id b, void *ctx)
{
    if (ctx == (void *)cmp_ctx) ncalls++;   /* the context is this function's own address */
    return [(NSNumber *)a compare:(NSNumber *)b];
}

static void sec_ident(void)
{
    MFHelper *h = [MFHelper new];
    h.fn = add1000;
    printf("helper -fn == add1000: %s\n", h.fn == add1000 ? "yes" : "no");
    report("helper callStored", [h callStored:5]);
    printf("helper -fn() = %ld\n", h.fn(6));
    ncalls = 0;
    h.ctx = (void *)add1000;
    printf("helper -ctx == add1000: %s\n", h.ctx == (void *)add1000 ? "yes" : "no");
    NSPointerFunctions *pf = [NSPointerFunctions pointerFunctionsWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    pf.hashFunction = pf_hash;
    printf("NSPointerFunctions -hashFunction == pf_hash: %s\n", pf.hashFunction == pf_hash ? "yes" : "no");
    NSArray *s = [numbers() sortedArrayUsingFunction:cmp_ctx context:(void *)cmp_ctx];
    printf("sorted with its own address as context: first %ld last %ld, context matched %s\n", [s.firstObject longValue],
           [s.lastObject longValue], ncalls ? "yes" : "no");
    ncalls = 0;
    KVOTarget *t = [KVOTarget new];
    KVOObserver *o = [KVOObserver new];
    [t addObserver:o forKeyPath:@"value" options:0 context:(void *)add1000];
    t.value = 1;
    t.value = 2;
    [t removeObserver:o forKeyPath:@"value" context:(void *)add1000];
    printf("KVO: %ld notifications, %ld with the function's address as context\n", o.seen, o.matched);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } secs[] = {
        {"sorted", sec_sorted}, {"sort", sec_sort}, {"slots", sec_slots}, {"super", sec_super},
        {"imp", sec_imp}, {"invoke", sec_invoke}, {"ptrfn", sec_ptrfn}, {"ident", sec_ident},
    };
    @autoreleasepool {
        for (size_t k = 0; k < sizeof secs / sizeof *secs; k++) {
            if (argc > 1 && strcmp(argv[1], secs[k].name)) continue;
            printf("== %s\n", secs[k].name);
            secs[k].fn();
        }
    }
    return 0;
}
