/* Native helper for tests/guest/stackargs.m: a method and a block call with
 * more than 8 integer (and 8 floating-point) arguments of mixed widths, so
 * some are passed on the stack, packed at their natural size on arm64 and
 * in 8-byte slots on x86_64. Built universal (x86_64 + arm64 + arm64e). */
#ifndef STACKHELPER_H
#define STACKHELPER_H
#import <Foundation/Foundation.h>

typedef long (^sh_mix12_t)(char a, short b, int c, long d, unsigned char e, unsigned short f, signed char g, short h,
                           int i, char j, unsigned short k, long l);

/* Float HFAs whose size is not a multiple of 8. With v0-v7 taken by eight
 * doubles they go to the stack, where Apple arm64 packs them at their natural
 * size and alignment (12 and 4 bytes), not in 8-byte multiples as AAPCS64
 * does; a float after them is packed in 4 bytes too. */
typedef struct { float x, y, z; } SHF3;
typedef struct { float v; } SHF1;
typedef double (^sh_hfa_t)(double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7,
                           SHF3 a, SHF3 b, SHF1 c, SHF1 d, float e, SHF3 f, int n);

@interface SHStack : NSObject
/* writes the arguments as text to buf, returns a checksum */
- (long)mix12:(char)a b:(short)b c:(int)c d:(long)d e:(unsigned char)e f:(unsigned short)f g:(signed char)g
            h:(short)h i:(int)i j:(char)j k:(unsigned short)k l:(long)l buf:(char *)buf;
- (double)fp10:(float)a b:(double)b c:(float)c d:(double)d e:(float)e f:(double)f g:(float)g h:(double)h
             i:(float)i j:(double)j n:(int)n s:(short)s buf:(char *)buf;
- (double)hfa:(double)d0 d1:(double)d1 d2:(double)d2 d3:(double)d3 d4:(double)d4 d5:(double)d5 d6:(double)d6
           d7:(double)d7 a:(SHF3)a b:(SHF3)b c:(SHF1)c d:(SHF1)d e:(float)e f:(SHF3)f n:(int)n buf:(char *)buf;
@end

/* calls b(-5, -300, 70000, -1099511627776L, 200, 65000, -7, -2, -123456, 'z', 60000, 99) natively */
long sh_call_mix12(sh_mix12_t b);
/* calls b(1..8, {1.25,2.25,3.25}, {4.5,5.5,6.5}, {7.75}, {8.75}, 9.5, {10.25,11.25,12.25}, -13) natively */
double sh_call_hfa(sh_hfa_t b);
#endif
