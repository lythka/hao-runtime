/* tests/guest/msgfnptr.m's native helper class (msgfnptrhelper.m): methods
 * that take C function pointers ('^?' in their encodings) in every arm64
 * argument place a method has, and call them natively, as
 * -[NSArray sortedArrayUsingFunction:context:] calls its comparator. */
#import <Foundation/Foundation.h>

typedef long (*mf_fn)(long);
typedef double (*mf_dfn)(double);

/* by value in x2, x3 (an integer-class aggregate of 16 bytes) */
typedef struct { mf_fn fn; long v; } MFRec;
/* by value, more than 16 bytes: by reference on arm64, in memory on x86_64 */
typedef struct { long a; mf_fn fn; long b; long c; } MFBig;

@interface MFHelper : NSObject
/* fn in x6 */
- (long)call:(long)a b:(long)b c:(long)c d:(long)d fn:(mf_fn)fn;
/* a..f in x2-x7, fn in the first stack slot */
- (long)stack:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(long)f fn:(mf_fn)fn;
/* f a char in x7, g an int at sp+0 (Apple packs it at its size), fn at sp+8 */
- (long)packed:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(char)f g:(int)g fn:(mf_fn)fn;
/* fn in x2 between doubles in d0 and d1 */
- (double)fp:(double)x fn:(mf_dfn)fn y:(double)y;
/* fn a field of an aggregate passed by value */
- (long)rec:(MFRec)r;
- (long)big:(MFBig)r;
/* stored and called later; the getter returns it */
- (void)setFn:(mf_fn)fn;
- (mf_fn)fn;
- (long)callStored:(long)v;
/* a void * that is handed back unchanged */
- (void)setCtx:(void *)p;
- (void *)ctx;
@end
