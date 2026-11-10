#import <Foundation/Foundation.h>
@interface HVScope : NSObject
- (NSString *)localizedStringWithFormat:(NSString *)format;
+ (NSString *)localizedStringWithFormat:(NSString *)format;
- (NSString *)stringWithFormat:(NSString *)format;
+ (NSString *)stringWithFormat:(NSString *)format;
- (NSString *)arrayWithObjects:(NSString *)object;
- (NSString *)appendFormat:(NSString *)format;
@end
@interface HVScopeChild : HVScope
@end
@interface HVStringOverride : NSString
+ (NSString *)localizedStringWithFormat:(NSString *)format, ...;
@end
