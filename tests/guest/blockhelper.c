/* See blockhelper.h. */
#include "blockhelper.h"
#include <Block.h>
#include <stddef.h>

void hb_rect(hb_rect_t b, const CGRect *in, double s, CGRect *out) { *out = b(*in, s); }

void hb_mixed(hb_mixed_t b, double d1, int i1, double d2, int i2, HMixed *out)
{
    HMixed x = {d1, i1}, y = {d2, i2};
    *out = b(x, y);
}

void hb_big(hb_big_t b, long base, int k, HBig *out)
{
    HBig in = {base, base + 1, base + 2, base + 3, base + 4};
    *out = b(in, k);
}

void hb_f3(hb_f3_t b, double x, double y, double z, double s, HF3 *out)
{
    HF3 v = {(float)x, (float)y, (float)z};
    *out = b(v, (float)s);
}

long hb_many(hb_many_t b, long base) { return b(base, base + 1, base + 2, base + 3, base + 4, base + 5, base + 6); }

double hb_fmix(hb_fmix_t b, int a, double x) { return b(a, x, (float)(x * 2), (long)a * 3, x / 4); }

hb_rect_t hb_copy_rect(hb_rect_t b, const CGRect *in, CGRect *out)
{
    hb_rect_t h = Block_copy(b);
    *out = h(*in, 1.5);
    return h;
}

void hb_outer(hb_outer_t b, const CGRect *in, int add, CGRect *out)
{
    int (^inner)(int) = ^int(int v) { return v * 10 + add; };
    *out = b(inner, *in);
}

int hb_outer2(hb_outer2_t b, int k)
{
    int (^inner)(int (^)(int), int) = ^int(int (^f)(int), int v) { return f(v) + f(v + 1) + k; };
    return b(inner, k);
}

static void (^stored)(int);
void hb_store(void (^b)(int)) { stored = Block_copy(b); }
void hb_fire(int v) { if (stored) stored(v); }
void hb_release(void) { if (stored) { Block_release(stored); stored = NULL; } }

long hb_arg9(long a, long b, long c, long d, long e, long f, long g, long h, long (^blk)(long))
{
    return blk(a + b + c + d + e + f + g + h);
}
