/* Guest dylib for objcproto.m: protocols that inherit protocols (its own and
 * a native one) and a class adopting them, registered while the dylib is
 * initialised, before the main executable's metadata. It has no x86_64
 * slice, so under hao it loads as a guest image. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>

@protocol OMLBase <NSObject>
- (int)lbase;
@end

@protocol OMLDerived <OMLBase, NSCopying>
- (int)lderived;
@optional
- (void)lreply:(void (^)(NSString *s))reply;
@property (readonly) int lcount;
@end

@interface OMLImpl : NSObject <OMLDerived>
@end

@implementation OMLImpl
+ (void)load { printf("load OMLImpl conforms=%d\n", [self conformsToProtocol:@protocol(OMLBase)]); }
- (int)lbase { return 3; }
- (int)lderived { return 4; }
- (int)lcount { return 5; }
- (id)copyWithZone:(NSZone *)z { (void)z; return [self retain]; }
@end

void om_lib_report(void)
{
    Protocol *d = objc_getProtocol("OMLDerived");
    printf("lib OMLDerived=%d same=%d inherits base=%d nsobject=%d copying=%d\n", d != NULL,
           d == @protocol(OMLDerived), protocol_conformsToProtocol(d, @protocol(OMLBase)),
           protocol_conformsToProtocol(d, @protocol(NSObject)), protocol_conformsToProtocol(d, @protocol(NSCopying)));
}
