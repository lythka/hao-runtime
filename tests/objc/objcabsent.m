/* Base class of objcmeta.m's OMMissingSub. objcmeta links against this
 * library, which is then deleted: its imports are unresolved at run time,
 * as for a daemon whose framework has no x86_64 slice. */
#import <Foundation/Foundation.h>
@interface OMMissingBase : NSObject { int _missingIvar; }
- (int)missingBase;
@end
@implementation OMMissingBase
- (int)missingBase { return 1; }
@end
