#import "objcvascope.h"
@implementation HVScope
- (NSString *)localizedStringWithFormat:(NSString *)format { return [@"instance:" stringByAppendingString:format]; }
+ (NSString *)localizedStringWithFormat:(NSString *)format { return [@"class:" stringByAppendingString:format]; }
- (NSString *)stringWithFormat:(NSString *)format { return [@"instance-string:" stringByAppendingString:format]; }
+ (NSString *)stringWithFormat:(NSString *)format { return [@"class-string:" stringByAppendingString:format]; }
- (NSString *)arrayWithObjects:(NSString *)object { return [@"object:" stringByAppendingString:object]; }
- (NSString *)appendFormat:(NSString *)format { return [@"append:" stringByAppendingString:format]; }
@end
@implementation HVScopeChild
- (NSString *)localizedStringWithFormat:(NSString *)format { return [@"override:" stringByAppendingString:format]; }
@end
@implementation HVStringOverride
+ (NSString *)localizedStringWithFormat:(NSString *)format, ... { return (id)[@"string-override:" stringByAppendingString:format]; }
@end
