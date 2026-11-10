#import <Foundation/Foundation.h>
int hao_check_rect(id object);
@interface HaoNativeFrame : NSObject
@property(nonatomic, readonly) NSRect frame;
@end
int hao_check_frame(id object);
