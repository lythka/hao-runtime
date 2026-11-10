/* Objective-C metadata of a library that exists only in the arm64e shared
 * cache (/usr/lib/libCoreRepairState.dylib: 16 classes, a category on
 * NSString, a protocol). The cache builder pre-optimises cache images: direct
 * relative method lists (selector and type offsets from the cache's
 * selector buffer), canonical protocols, objc_msgSend stubs that build the
 * selector with ADRP+ADD, [super init] with the selector string computed in
 * place. Under hao the library is guest code; every result here is compared
 * with native arm64.
 *   crobjc            metadata dump, then behaviour */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CRDeviceComponent and CRDevice are not exported: looked up by name */
@protocol CRState <NSObject>
- (id)initWithType:(int)type locKey:(id)locKey spc:(id)spc fdrKeys:(id)fdrKeys superModule:(id)sm;
- (int)type;
- (id)locKey;
- (id)spc;
- (id)fdrKeys;
- (id)superModule;
- (void)setLocKey:(id)k;
- (BOOL)hasStrongIdentity;
- (id)initWithComponents:(id)components;
- (id)components;
- (id<CRState>)getComponentByType:(int)type;
- (id<CRState>)getComponentBySPC:(id)spc;
@end
@interface CRRepairHistoryItem : NSObject
- (id)initWithComponentName:(id)name CRComponentType:(int)type isRepaired:(BOOL)rep isUsed:(BOOL)used repairDate:(id)date;
- (id)componentName;
- (int)componentType;
- (BOOL)isRepaired;
- (BOOL)isUsed;
- (id)repairDate;
@end
@interface NSString (Digest)
- (id)SHA256DigestString;
- (id)SHA256DigestData;
@end

/* a subclass of a cache-image class, defined in the main program */
@interface HaoItem : CRRepairHistoryItem
@end
@implementation HaoItem
- (int)componentType { return [super componentType] + 100; }
- (id)componentName { return [@"sub:" stringByAppendingString:[super componentName]]; }
@end

extern const char *_protocol_getMethodTypeEncoding(Protocol *p, SEL sel, BOOL required, BOOL instance);

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void dump_methods(Class c, const char *kind)
{
    unsigned n = 0;
    Method *ms = class_copyMethodList(c, &n);
    char **v = calloc(n + 1, sizeof *v);
    for (unsigned k = 0; k < n; k++) {
        const char *t = method_getTypeEncoding(ms[k]);
        size_t len = strlen(sel_getName(method_getName(ms[k]))) + (t ? strlen(t) : 4) + 4;
        v[k] = malloc(len);
        snprintf(v[k], len, "%s %s", sel_getName(method_getName(ms[k])), t ? t : "NULL");
        if (!method_getImplementation(ms[k])) printf("  %s %s: no IMP\n", kind, v[k]);
    }
    qsort(v, n, sizeof *v, cmpstr);
    for (unsigned k = 0; k < n; k++) { printf("  %s %s\n", kind, v[k]); free(v[k]); }
    free(v);
    free(ms);
}

static void dump_class(const char *name)
{
    Class c = objc_getClass(name);
    if (!c) { printf("class %s: missing\n", name); return; }
    printf("class %s super %s size %zu meta %s\n", class_getName(c), class_getName(class_getSuperclass(c)),
           class_getInstanceSize(c), class_isMetaClass(object_getClass(c)) ? "yes" : "no");
    dump_methods(c, "-");
    dump_methods(object_getClass(c), "+");
    unsigned n = 0;
    Ivar *iv = class_copyIvarList(c, &n);
    for (unsigned k = 0; k < n; k++)
        printf("  ivar %s %s @%td\n", ivar_getName(iv[k]), ivar_getTypeEncoding(iv[k]), ivar_getOffset(iv[k]));
    free(iv);
    objc_property_t *pr = class_copyPropertyList(c, &n);
    char **pv = calloc(n + 1, sizeof *pv);
    for (unsigned k = 0; k < n; k++) asprintf(&pv[k], "%s %s", property_getName(pr[k]), property_getAttributes(pr[k]));
    qsort(pv, n, sizeof *pv, cmpstr);
    for (unsigned k = 0; k < n; k++) { printf("  property %s\n", pv[k]); free(pv[k]); }
    free(pv);
    free(pr);
    Protocol *__unsafe_unretained *pl = class_copyProtocolList(c, &n);
    for (unsigned k = 0; k < n; k++) printf("  protocol %s\n", protocol_getName(pl[k]));
    free(pl);
}

static void dump_protocol(const char *name)
{
    Protocol *p = objc_getProtocol(name);
    if (!p) { printf("protocol %s: missing\n", name); return; }
    printf("protocol %s\n", protocol_getName(p));
    for (int req = 1; req >= 0; req--)
        for (int inst = 1; inst >= 0; inst--) {
            unsigned n = 0;
            struct objc_method_description *d = protocol_copyMethodDescriptionList(p, req, inst, &n);
            char **v = calloc(n + 1, sizeof *v);
            /* extended types (NSXPCInterface reads them for block parameters) */
            for (unsigned k = 0; k < n; k++)
                asprintf(&v[k], "%s %s | %s", sel_getName(d[k].name), d[k].types,
                         _protocol_getMethodTypeEncoding(p, d[k].name, req, inst) ?: "NULL");
            qsort(v, n, sizeof *v, cmpstr);
            for (unsigned k = 0; k < n; k++) { printf("  %s%c %s\n", req ? "required" : "optional", inst ? '-' : '+', v[k]); free(v[k]); }
            free(v);
            free(d);
        }
    unsigned n = 0;
    Protocol *__unsafe_unretained *pl = protocol_copyProtocolList(p, &n);
    for (unsigned k = 0; k < n; k++) printf("  adopts %s\n", protocol_getName(pl[k]));
    free(pl);
}

int main(void)
{
    @autoreleasepool {
        static const char *classes[] = {"CRDeviceMap", "CRDeviceComponent", "CRDevice", "CRInputDeviceController",
            "CRInputDeviceManager", "CRLASController", "CRLocalization", "CRFDRRCHL", "CRRepairHistoryItem", "CRRepairHistory",
            "CRRepairStateSnapshot", "CRSmcController", "CRUserDefaults", "CRFDRUtils", "CRFDRRetryController", "CRUtils",
            "HaoItem"};
        for (unsigned k = 0; k < sizeof classes / sizeof *classes; k++) dump_class(classes[k]);
        dump_protocol("CoreRepairCoreXPCServiceProtocol");
        printf("NSString(Digest):");
        static const char *cat[] = {"SHA256DigestString", "SHA256DigestData", "dataIdentifier", "dataKey", "dataTypeInfo"};
        for (unsigned k = 0; k < 5; k++)
            printf(" %s=%d", cat[k], [NSString instancesRespondToSelector:sel_registerName(cat[k])]);
        printf("\n");

        printf("--- behaviour\n");
        /* the library's code: objc_msgSend stubs, [super init], ARC, blocks */
        Class compc = NSClassFromString(@"CRDeviceComponent"), devc = NSClassFromString(@"CRDevice");
        id<CRState> c = [(id<CRState>)[compc alloc] initWithType:7 locKey:@"LOC_KEY" spc:@"SPC1" fdrKeys:@[@"bcfg", @"prpc"] superModule:nil];
        printf("component type=%d locKey=%s spc=%s fdrKeys=%s super=%s strong=%d\n", [c type], [[c locKey] UTF8String],
               [[c spc] UTF8String], [[[c fdrKeys] componentsJoinedByString:@","] UTF8String],
               [c superModule] ? "set" : "nil", [c hasStrongIdentity]);
        [c setLocKey:@"OTHER"];
        printf("setLocKey -> %s, -description has class name: %d\n", [[c locKey] UTF8String],
               [[c description] containsString:@"CRDeviceComponent"]);
        id<CRState> c2 = [(id<CRState>)[compc alloc] initWithType:9 locKey:@"K2" spc:@"SPC2" fdrKeys:@[] superModule:c];
        id<CRState> d = [(id<CRState>)[devc alloc] initWithComponents:@[c, c2]];
        printf("device components=%lu byType(9)=%s bySPC(SPC1) type=%d byType(5)=%s\n", (unsigned long)[[d components] count],
               [[[d getComponentByType:9] locKey] UTF8String], [[d getComponentBySPC:@"SPC1"] type],
               [d getComponentByType:5] ? "found" : "nil");
        /* a category of the cache library on a native class */
        NSString *str = @"hao reverse rosetta";
        printf("SHA256DigestString=%s\n", [[str SHA256DigestString] UTF8String]);
        printf("SHA256DigestData=%s\n", [[[str SHA256DigestData] description] UTF8String]);
        /* an exported cache class: bound by the main program, and subclassed */
        NSDate *date = [NSDate dateWithTimeIntervalSince1970:1700000000];
        CRRepairHistoryItem *it = [[CRRepairHistoryItem alloc] initWithComponentName:@"Display" CRComponentType:3
                                                                          isRepaired:YES isUsed:NO repairDate:date];
        printf("item %s type=%d repaired=%d used=%d date=%.0f\n", [[it componentName] UTF8String], [it componentType],
               [it isRepaired], [it isUsed], [[it repairDate] timeIntervalSince1970]);
        printf("item description has name: %d\n", [[it description] containsString:@"Display"]);
        HaoItem *h = [[HaoItem alloc] initWithComponentName:@"Battery" CRComponentType:4 isRepaired:NO isUsed:YES
                                                 repairDate:date];
        printf("subclass %s type=%d isKind=%d responds=%d super=%s\n", [[h componentName] UTF8String], [h componentType],
               [h isKindOfClass:[CRRepairHistoryItem class]], [h respondsToSelector:@selector(isUsed)],
               class_getName(class_getSuperclass([h class])));
        printf("selector identity: %d %d\n", @selector(initWithComponents:) == NSSelectorFromString(@"initWithComponents:"),
               [devc instancesRespondToSelector:NSSelectorFromString(@"populateLookupDictionary:")]);
        printf("class identity: %d %d\n", [CRRepairHistoryItem class] == NSClassFromString(@"CRRepairHistoryItem"),
               [c class] == objc_getClass("CRDeviceComponent"));
    }
    return 0;
}
