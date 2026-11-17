/* tests/guest/refuse.sh: the native side (a fat dylib) */
#import <Foundation/Foundation.h>
struct rf_mid { long w[1024]; };          /* 8 KB: returned in memory on both sides */
struct rf_huge { char b[70000]; };        /* larger than any aggregate hao marshals */
struct rf_big { long w[12288]; };       /* 96 KB: more than hao's area for a result without a buffer */
long rf_call_mid(struct rf_mid (*f)(long), long k);
long rf_call_big(struct rf_big (*f)(long), long k);
long rf_call_block(long (^b)(struct rf_huge), long k);
@interface RFNative : NSObject
- (long)take:(struct rf_huge)v;
@end
