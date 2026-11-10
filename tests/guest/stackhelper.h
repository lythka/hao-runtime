/* Native helper for tests/guest/stackargs.m: a method and a block call with
 * more than 8 integer (and 8 floating-point) arguments of mixed widths, so
 * some are passed on the stack, packed at their natural size on arm64 and
 * in 8-byte slots on x86_64. Built universal (x86_64 + arm64 + arm64e). */
#ifndef STACKHELPER_H
#define STACKHELPER_H
#import <Foundation/Foundation.h>

typedef long (^sh_mix12_t)(char a, short b, int c, long d, unsigned char e, unsigned short f, signed char g, short h,
                           int i, char j, unsigned short k, long l);

@interface SHStack : NSObject
/* writes the arguments as text to buf, returns a checksum */
- (long)mix12:(char)a b:(short)b c:(int)c d:(long)d e:(unsigned char)e f:(unsigned short)f g:(signed char)g
            h:(short)h i:(int)i j:(char)j k:(unsigned short)k l:(long)l buf:(char *)buf;
- (double)fp10:(float)a b:(double)b c:(float)c d:(double)d e:(float)e f:(double)f g:(float)g h:(double)h
             i:(float)i j:(double)j n:(int)n s:(short)s buf:(char *)buf;
@end

/* calls b(-5, -300, 70000, -1099511627776L, 200, 65000, -7, -2, -123456, 'z', 60000, 99) natively */
long sh_call_mix12(sh_mix12_t b);
#endif
