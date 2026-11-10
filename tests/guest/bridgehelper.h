/* Native helper for tests/guest/bridgefix.m: native code calling guest
 * blocks with signatures the bridge used to get wrong. Built universal. */
#ifndef BRIDGEHELPER_H
#define BRIDGEHELPER_H
#include <CoreGraphics/CGGeometry.h>

/* three CGRects (12 doubles: the third spills to the stack on arm64) then an integer */
typedef long (^bh_rects_t)(CGRect a, CGRect b, CGRect c, long i);
long bh_call_rects(bh_rects_t b);
/* a guest block returning long double, called natively (SysV returns it in x87 st0) */
typedef long double (^bh_ld_t)(double x);
double bh_call_ld(bh_ld_t b, double x);

/* aggregates past the signature paths' old fixed buffers: a 72-byte argument
 * (more than 64) and a 304-byte result (more than 256) */
typedef struct bh_big72 { long v[9]; } bh_big72;
typedef struct bh_big304 { long v[38]; } bh_big304;
long bh_sum304(const bh_big304 *r);
/* a native IMP the guest adds to a class: guest -> native by the method's encoding */
bh_big304 bh_big_imp(void *self, void *cmd, bh_big72 a, long i);
/* native -> guest: a guest block and a guest method with that signature */
typedef bh_big304 (^bh_big_t)(bh_big72 a, long i);
long bh_call_big(bh_big_t b);
long bh_send_big(void *obj, const char *sel);
/* native -> guest: (self, _cmd, id, id, NSUInteger, NSRange) puts the range
 * after 5 integer registers, where SysV passes it on the stack (no split) */
unsigned long bh_send_nosplit(void *obj, const char *sel);
#endif
