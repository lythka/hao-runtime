/* Protocols across images: libomproto (objcprotolib.m, a guest dylib)
 * registers inheriting protocols during its initialisation; this program
 * refers to the same protocols, inherits them in protocols of its own and
 * subclasses the dylib's class. tests/objc/run.sh compares it with native
 * arm64. Build without ARC. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@protocol OMLBase <NSObject>
- (int)lbase;
@end

@protocol OMLDerived <OMLBase, NSCopying>
- (int)lderived;
@optional
- (void)lreply:(void (^)(NSString *s))reply;
@property (readonly) int lcount;
@end

@protocol OMMProto <OMLDerived>
- (int)mown;
@end

@interface OMLImpl : NSObject <OMLDerived>
@end

@interface OMMSub : OMLImpl <OMMProto>
@end

@implementation OMMSub
+ (void)load { printf("load OMMSub conforms=%d\n", [self conformsToProtocol:@protocol(OMLDerived)]); }
- (int)mown { return 6; }
@end

void om_lib_report(void);
/* libobjc SPI (objc-private.h): the extended type encoding of a protocol method */
extern const char *_protocol_getMethodTypeEncoding(Protocol *p, SEL sel, BOOL required, BOOL instance);

static int cmpstr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void print_adopted(const char *what, Protocol *p)
{
    unsigned n = 0;
    Protocol *__unsafe_unretained *l = protocol_copyProtocolList(p, &n);
    const char *names[16];
    for (unsigned k = 0; k < n && k < 16; k++) names[k] = protocol_getName(l[k]);
    qsort(names, n < 16 ? n : 16, sizeof names[0], cmpstr);
    printf("%s adopts", what);
    for (unsigned k = 0; k < n && k < 16; k++) printf(" %s", names[k]);
    printf("\n");
    free(l);
}

int main(void)
{
    om_lib_report();
    Protocol *d = @protocol(OMLDerived), *m = @protocol(OMMProto);
    printf("main same=%d mproto inherits derived=%d base=%d copying=%d\n", d == objc_getProtocol("OMLDerived"),
           protocol_conformsToProtocol(m, d), protocol_conformsToProtocol(m, @protocol(OMLBase)),
           protocol_conformsToProtocol(m, @protocol(NSCopying)));
    print_adopted("OMLDerived", d);
    print_adopted("OMMProto", m);
    struct objc_method_description md = protocol_getMethodDescription(d, @selector(lreply:), NO, YES);
    printf("lreply types=%s ext=%s\n", md.types ? md.types : "(null)",
           _protocol_getMethodTypeEncoding(d, @selector(lreply:), NO, YES) ?: "(null)");
    objc_property_t pr = protocol_getProperty(d, "lcount", YES, YES);
    printf("lcount attrs=%s\n", pr ? property_getAttributes(pr) : "(null)");
    OMMSub *s = [OMMSub new];
    printf("sub conforms mproto=%d derived=%d values=%d %d %d %d\n", [s conformsToProtocol:m],
           [OMMSub conformsToProtocol:d], [s lbase], [s lderived], [s lcount], [s mown]);
    NSXPCInterface *xi = [NSXPCInterface interfaceWithProtocol:d];
    printf("xpc interface %s\n", xi ? "ok" : "nil");
    [s release];
    return 0;
}
