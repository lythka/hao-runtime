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
- (double)hfa:(double)d0 d1:(double)d1 d2:(double)d2 d3:(double)d3 d4:(double)d4 d5:(double)d5 d6:(double)d6
           d7:(double)d7 a:(SHF3)a b:(SHF3)b c:(SHF1)c d:(SHF1)d e:(float)e f:(SHF3)f n:(int)n buf:(char *)buf
{
    snprintf(buf, 256, "%g %g %g %g %g %g %g %g | %g %g %g | %g %g %g | %g | %g | %g | %g %g %g | %d", d0, d1, d2, d3,
             d4, d5, d6, d7, a.x, a.y, a.z, b.x, b.y, b.z, c.v, d.v, e, f.x, f.y, f.z, n);
    return d0 + d7 + a.x + 2 * a.z + 3 * b.x + 4 * b.z + 5 * c.v + 6 * d.v + 7 * e + 8 * f.x + 9 * f.z + 10 * n;
}
@end

long sh_call_mix12(sh_mix12_t b)
{
    return b(-5, -300, 70000, -1099511627776L, 200, 65000, -7, -2, -123456, 'z', 60000, 99);
}

double sh_call_hfa(sh_hfa_t b)
{
    return b(1, 2, 3, 4, 5, 6, 7, 8, (SHF3){1.25f, 2.25f, 3.25f}, (SHF3){4.5f, 5.5f, 6.5f}, (SHF1){7.75f},
             (SHF1){8.75f}, 9.5f, (SHF3){10.25f, 11.25f, 12.25f}, -13);
}
