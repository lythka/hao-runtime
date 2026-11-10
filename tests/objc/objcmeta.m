/*
 * Objective-C metadata forms hao registers with the native runtime: relative
 * method lists, protocols (inherited, optional, class methods, properties),
 * ivars, instance and class properties, categories on guest and native
 * classes (with protocols), +load in classes and categories, and a class whose
 * superclass comes from a library that is missing at run time (with a guest
 * subclass, categories and a +load of its own). Prints om_dump() (objcdump.h)
 * then a behaviour section. tests/objc/run.sh compares it with native arm64.
 * Build without ARC.
 */
#import <Foundation/Foundation.h>
#include "objcdump.h"

typedef struct { double x, y, w, h; } OMRect;

@protocol OMBaseProto <NSObject>
- (int)baseReq;
+ (int)classReq;
@optional
- (int)baseOpt;
+ (int)baseClassOpt;
@property (readonly) int count;
@end

@protocol OMProto <OMBaseProto>
- (int)protoReq:(int)v;
@optional
@property (copy) NSString *name;
@end

@protocol OMCatProto
- (NSString *)catProtoMethod;
@end

@protocol OMNativeCatProto
- (NSString *)om_nativeCat;
@end

/* block parameters: NSXPCInterface needs the extended method types */
@protocol OMXPCProto <NSObject>
- (void)doThing:(NSString *)what reply:(void (^)(NSString *result, NSError *error))reply;
@optional
- (void)other:(NSArray<NSString *> *)list;
@end

@interface OMBase : NSObject <OMProto> {
@public
    int _i;
    id _o;
    double _d;
    char _c;
    long _l;
}
@property (copy) NSString *name;
@property (readonly) int count;
@property (assign) OMRect rect;
@property (class) int shared;
- (int)baseReq;
+ (int)classReq;
@end

static int om_shared;
static char om_loads[256];

@implementation OMBase
@synthesize name = _name, count = _count, rect = _rect;
+ (void)load { strlcat(om_loads, " OMBase", sizeof om_loads); }
+ (int)shared { return om_shared; }
+ (void)setShared:(int)v { om_shared = v; }
- (int)baseReq { return 10 + _count; }
+ (int)classReq { return 20; }
- (int)protoReq:(int)v { return v * 3; }
- (NSString *)description { return [NSString stringWithFormat:@"<OMBase %@>", _name]; }
@end

@interface OMDerived : OMBase {
    short _s;
    id _extra[3];
}
- (long)derivedOnly:(long)a with:(double)b;
@end

@implementation OMDerived
- (int)baseReq { return [super baseReq] + 100; }
- (long)derivedOnly:(long)a with:(double)b { return a + (long)b; }
- (NSString *)description { return [NSString stringWithFormat:@"<OMDerived %@ %@>", self.name, [super description]]; }
@end

@interface OMLeaf : OMDerived <NSCopying>
- (int)leaf;
@end

@implementation OMLeaf
- (int)leaf { return 7; }
- (id)copyWithZone:(NSZone *)z { (void)z; return [self retain]; }
@end

@interface OMBase (OMCat) <OMCatProto>
@property (readonly) int catProp;
- (NSString *)catMethod;
+ (NSString *)catClassMethod;
@end

@implementation OMBase (OMCat)
+ (void)load { strlcat(om_loads, " OMBase(OMCat)", sizeof om_loads); }
- (int)catProp { return 5; }
- (NSString *)catMethod { return [@"cat:" stringByAppendingString:NSStringFromClass([self class])]; }
+ (NSString *)catClassMethod { return [@"catclass:" stringByAppendingString:NSStringFromClass(self)]; }
- (NSString *)catProtoMethod { return @"catproto"; }
@end

@interface NSObject (OMNative) <OMNativeCatProto>
- (NSString *)om_nativeCat;
+ (NSString *)om_nativeCat;
@end

@implementation NSObject (OMNative)
- (NSString *)om_nativeCat { return [@"native-" stringByAppendingString:NSStringFromClass([self class])]; }
+ (NSString *)om_nativeCat { return [@"native+" stringByAppendingString:NSStringFromClass(self)]; }
@end

/* superclass from the absent library */
@interface OMMissingBase : NSObject
@end

@interface OMMissingSub : OMMissingBase <OMProto> {
    int _sub;
}
- (int)missingSub;
@end

@implementation OMMissingSub
+ (void)load { strlcat(om_loads, " OMMissingSub", sizeof om_loads); }
- (int)missingSub { return 2; }
- (int)baseReq { return 0; }
+ (int)classReq { return 0; }
- (int)protoReq:(int)v { return v; }
- (int)count { return 0; }
@end

@interface OMMissingSubSub : OMMissingSub
- (int)missingSubSub;
@end

@implementation OMMissingSubSub
- (int)missingSubSub { return 3; }
@end

@interface OMMissingSub (OMMissingCat)
- (int)om_missingSubCat;
@end

@implementation OMMissingSub (OMMissingCat)
+ (void)load { strlcat(om_loads, " OMMissingSub(OMMissingCat)", sizeof om_loads); }
- (int)om_missingSubCat { return 4; }
@end

@interface OMMissingBase (OMMissingBaseCat)
- (int)om_missingBaseCat;
@end

@implementation OMMissingBase (OMMissingBaseCat)
- (int)om_missingBaseCat { return 5; }
@end

/* methods added at run time with guest IMPs, called by native code (KVC) */
static OMRect om_dynRect(id self, SEL _cmd) { (void)self; (void)_cmd; OMRect r = {1, 2, 3, 4}; return r; }
static OMRect om_dynRect2(id self, SEL _cmd) { (void)self; (void)_cmd; OMRect r = {5, 6, 7, 8}; return r; }
static NSRange om_dynRange(id self, SEL _cmd) { (void)self; (void)_cmd; return NSMakeRange(10, 20); }
static void om_print_rect(const char *what, id obj, NSString *key)
{
    NSValue *v = [obj valueForKey:key];
    OMRect r = {0, 0, 0, 0};
    [v getValue:&r size:sizeof r];
    printf("%s=%g,%g,%g,%g\n", what, r.x, r.y, r.w, r.h);
}

@interface OMAsserter : NSObject
- (void)fail;
@end
@implementation OMAsserter
- (void)fail { NSAssert(0, @"method %d %@ %s %.2f", 42, @"obj", "cstr", 2.5); }
@end
static void om_cfail(void) { NSCAssert(0, @"function %d %@ %s %.2f", 43, @"obj", "cstr", 3.5); }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "assert")) [[OMAsserter new] fail];   /* uncaught: aborts with the reason */
    if (argc > 1 && !strcmp(argv[1], "cassert")) om_cfail();
    @autoreleasepool {
        om_dump();
        printf("--- behaviour\n");
        printf("loads:%s\n", om_loads);
        OMLeaf *l = [OMLeaf new];
        l.name = @"leaf";
        OMRect r = {1.5, 2.5, 3.5, 4.5};
        l.rect = r;
        OMRect q = l.rect;
        l->_i = 41;
        printf("leaf baseReq=%d protoReq=%d leaf=%d derivedOnly=%ld rect=%g,%g,%g,%g ivar=%d\n", [l baseReq],
               [l protoReq:4], [l leaf], [l derivedOnly:5 with:6.75], q.x, q.y, q.w, q.h, l->_i);
        printf("desc=%s\n", [[l description] UTF8String]);
        printf("cat=%s catclass=%s catproto=%s catprop=%d\n", [[l catMethod] UTF8String],
               [[OMDerived catClassMethod] UTF8String], [[l catProtoMethod] UTF8String], l.catProp);
        OMBase.shared = 9;
        printf("shared=%d classReq=%d\n", OMLeaf.shared, [OMLeaf classReq]);
        printf("native=%s native+=%s nsstring=%s\n", [[l om_nativeCat] UTF8String], [[OMLeaf om_nativeCat] UTF8String],
               [[@"x" om_nativeCat] UTF8String]);
        printf("respondsTo catMethod=%d om_nativeCat=%d copy=%d\n", [l respondsToSelector:@selector(catMethod)],
               [@"x" respondsToSelector:@selector(om_nativeCat)], [l conformsToProtocol:@protocol(NSCopying)]);
        printf("isKind OMBase=%d OMDerived=%d\n", [l isKindOfClass:[OMBase class]], [l isKindOfClass:[OMDerived class]]);
        printf("missing classref=%p missingsub classref=%p subsub classref=%p\n", (void *)[OMMissingBase class],
               (void *)[OMMissingSub class], (void *)[OMMissingSubSub class]);
        printf("missing new=%p\n", (void *)[OMMissingSubSub new]);

        NSXPCInterface *xi = [NSXPCInterface interfaceWithProtocol:@protocol(OMXPCProto)];
        printf("xpc interface %s\n", xi ? "ok" : "nil");

        /* variadic selectors */
        NSLocale *en = [NSLocale localeWithLocaleIdentifier:@"en_US"], *de = [NSLocale localeWithLocaleIdentifier:@"de_DE"];
        NSString *f1 = [[NSString alloc] initWithFormat:@"%d-%@-%s-%.1f" locale:de, 42, @"obj", "cstr", 2.5];
        NSString *f2 = [[NSString alloc] initWithFormat:@"%.2f|%@" locale:en, 1234.5, @"x"];
        printf("fmt-locale=%s %s\n", f1.UTF8String, f2.UTF8String);
        printf("expr=%s\n", [[[NSExpression expressionWithFormat:@"%d + %d * %d", 3, 4, 5] expressionValueWithObject:nil context:nil] description].UTF8String);
        printf("pred=%d\n", [[NSPredicate predicateWithFormat:@"SELF > %d AND SELF < %f", 3, 9.5] evaluateWithObject:@7]);
        NSString *loc = [NSString localizedStringWithFormat:@"%.3f %d", 1234.5678, 1234567];
        NSString *ref = [[NSString alloc] initWithFormat:@"%.3f %d" locale:[NSLocale currentLocale], 1234.5678, 1234567];
        printf("localized matches currentLocale=%d\n", [loc isEqualToString:ref]);
        NSMutableString *ms = [NSMutableString stringWithFormat:@"<%d %s>", 1, "a"];
        [ms appendFormat:@"[%@ %.1f %ld]", @"b", 2.5, 3L];
        printf("append=%s by=%s\n", ms.UTF8String, [ms stringByAppendingFormat:@"{%d %.2f %@}", 4, 5.25, @"c"].UTF8String);
        NSArray *big = [NSArray arrayWithObjects:@"a0", @"a1", @"a2", @"a3", @"a4", @"a5", @"a6", @"a7", @"a8", @"a9",
                        @"b0", @"b1", @"b2", @"b3", @"b4", @"b5", @"b6", @"b7", @"b8", @"b9", @"c0", @"c1", @"c2",
                        @"c3", @"c4", nil];
        NSDictionary *bd = [NSDictionary dictionaryWithObjectsAndKeys:@1, @"k1", @2, @"k2", @3, @"k3", @4, @"k4", @5, @"k5",
                            @6, @"k6", @7, @"k7", @8, @"k8", @9, @"k9", @10, @"k10", @11, @"k11", @12, @"k12", nil];
        /* "...Format:" keywords that are not variadic: an enum, a plain string with '%' */
        printf("plist valid=%d\n", [NSPropertyListSerialization propertyList:@[@1, @"two"] isValidForFormat:NSPropertyListXMLFormat_v1_0]);
        NSNumberFormatter *nf = [[NSNumberFormatter alloc] init];
        nf.locale = en;
        [nf setPositiveFormat:@"#,##0.0%"];
        printf("percent=%s format=%s\n", [nf stringFromNumber:@0.25].UTF8String, nf.positiveFormat.UTF8String);
        [nf release];
        NSSet *st = [NSSet setWithObjects:@"s1", @"s2", @"s1", nil];
        printf("objlist=%lu last=%s dict=%lu k12=%s set=%lu\n", (unsigned long)big.count, [big.lastObject UTF8String],
               (unsigned long)bd.count, [[bd[@"k12"] description] UTF8String], (unsigned long)st.count);

        /* guest IMPs installed at run time, called through KVC */
        class_addMethod([OMBase class], @selector(dynRect), (IMP)om_dynRect, "{?=dddd}16@0:8");
        class_addMethod([OMBase class], @selector(dynRange), (IMP)om_dynRange, "{_NSRange=QQ}16@0:8");
        OMBase *b = [OMBase new];
        om_print_rect("added rect", b, @"dynRect");
        printf("added range=%s\n", [[b valueForKey:@"dynRange"] description].UTF8String);
        method_setImplementation(class_getInstanceMethod([OMBase class], @selector(dynRect)), (IMP)om_dynRect2);
        om_print_rect("set rect", b, @"dynRect");
        class_replaceMethod([OMBase class], @selector(dynRect), (IMP)om_dynRect, "{?=dddd}16@0:8");
        om_print_rect("replaced rect", b, @"dynRect");
        [b release];
        [l release];
    }
    return 0;
}
