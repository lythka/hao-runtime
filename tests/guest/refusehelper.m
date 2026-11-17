/* Native side of tests/guest/refuse.sh (fat x86_64 + arm64 + arm64e dylib). */
#import "refusehelper.h"

/* a guest function returning 8 KB in memory, called through a plain thunk: x86_64 passes the buffer in
 * rdi and leaves the thunk in rax, so hao has no buffer to give the guest's x8 */
__attribute__((noinline)) long rf_call_mid(struct rf_mid (*f)(long), long k)
{
    struct rf_mid r = f(k);
    volatile long s = r.w[0];
    return s != -1;
}

__attribute__((noinline)) long rf_call_big(struct rf_big (*f)(long), long k)
{
    struct rf_big r = f(k);
    volatile long s = r.w[0];
    return s != -1;
}

/* a guest block whose parameter is too large to marshal */
__attribute__((noinline)) long rf_call_block(long (^b)(struct rf_huge), long k)
{
    static struct rf_huge v;
    v.b[0] = (char)k;
    v.b[sizeof v.b - 1] = (char)(k + 1);
    return b(v);
}

@implementation RFNative
- (long)take:(struct rf_huge)v { return v.b[0] + v.b[sizeof v.b - 1]; }
@end
