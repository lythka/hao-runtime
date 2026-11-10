/* Native helper for tests/guest/stackargs.m (see stackhelper.h). */
#import "stackhelper.h"
#include <stdio.h>

@implementation SHStack
- (long)mix12:(char)a b:(short)b c:(int)c d:(long)d e:(unsigned char)e f:(unsigned short)f g:(signed char)g
            h:(short)h i:(int)i j:(char)j k:(unsigned short)k l:(long)l buf:(char *)buf
{
    snprintf(buf, 256, "%d %d %d %ld %u %u %d %d %d %d %u %ld", a, b, c, d, e, f, g, h, i, j, k, l);
    return a + 3L * b + 5L * c + 7L * d + 11L * e + 13L * f + 17L * g + 19L * h + 23L * i + 29L * j + 31L * k + 37L * l;
}
- (double)fp10:(float)a b:(double)b c:(float)c d:(double)d e:(float)e f:(double)f g:(float)g h:(double)h
             i:(float)i j:(double)j n:(int)n s:(short)s buf:(char *)buf
{
    snprintf(buf, 256, "%g %g %g %g %g %g %g %g %g %g %d %d", a, b, c, d, e, f, g, h, i, j, n, s);
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * n + 12 * s;
}
@end

long sh_call_mix12(sh_mix12_t b)
{
    return b(-5, -300, 70000, -1099511627776L, 200, 65000, -7, -2, -123456, 'z', 60000, 99);
}
