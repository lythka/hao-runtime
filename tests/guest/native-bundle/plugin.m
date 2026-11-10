#import <Foundation/Foundation.h>
#ifndef CLASS
#define CLASS GuestBundleFixture
#endif
static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized++; }
@interface CLASS : NSObject
+ (long)value;
@end
@implementation CLASS
+ (long)value { return 40 + initialized; }
@end
int untypedExport(void) { return 7; }
int bundleData = 19;
const char bundleSignatureMarker[] = "hao-bundle-signature-marker-83721";
