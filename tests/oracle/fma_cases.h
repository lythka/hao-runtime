/*
 * Single-precision fused multiply-add operands, shared by the oracle's fma
 * mode (native_arm64.c) and tests/guest/fma32.c. fma_case(s, cat) returns raw
 * bits of (a, b, c) for a * b + c:
 *   FC_ORDINARY  normal values of moderate exponent
 *   FC_ANY       any bit patterns
 *   FC_TIE       a * b = (k + 1/2) ulp(c) * (1 - i^2 2^-46): the exact sum is
 *                just off a midpoint of two floats by less than binary64 can
 *                hold, so a binary64 sum lands on the midpoint and a second
 *                rounding to float goes the wrong way
 *   FC_GAP       exponent of c from 70 below to 70 above the product's
 *   FC_SPAN      c six to ten binades above the product: the exact sum needs
 *                53 to 57 bits, around binary64's own rounding position
 *   FC_OVERFLOW  results around the largest finite float
 *   FC_TINY      results around the smallest normal float and below
 *   FC_EXACT     small integers: exact results and exact cancellation
 *   FC_SPECIAL   one operand replaced by a zero, subnormal, infinity or NaN
 */
#pragma once
#include <stdint.h>
#include <string.h>

enum { FC_ORDINARY, FC_ANY, FC_TIE, FC_GAP, FC_SPAN, FC_OVERFLOW, FC_TINY, FC_EXACT, FC_SPECIAL, FC_N };
static const char *const fma_case_name[FC_N] = {"ordinary", "any", "tie", "gap", "span", "overflow", "tiny", "exact", "special"};

typedef struct { uint32_t a, b, c; } fma_ops;

static inline uint64_t fc_rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static inline unsigned fc_rr(uint64_t *s, unsigned n) { return (unsigned)(fc_rnd(s) % n); }

/* (-1)^sign * 1.frac * 2^e, the exponent clamped to the normal range */
static inline uint32_t fc_f32(unsigned sign, int e, uint32_t frac)
{
    int be = e + 127;
    if (be < 1) be = 1;
    if (be > 254) be = 254;
    return (uint32_t)sign << 31 | (uint32_t)be << 23 | (frac & 0x7fffff);
}
static inline uint32_t fc_frac(uint64_t *s)
{
    switch (fc_rr(s, 6)) {
    case 0: return 0;
    case 1: return 0x7fffff;
    case 2: return (uint32_t)fc_rnd(s) & 0x7fffff & ~((1u << fc_rr(s, 23)) - 1);   /* trailing zeros */
    case 3: return (uint32_t)fc_rnd(s) | ~(0x7fffffu >> fc_rr(s, 23));              /* leading ones */
    default: return (uint32_t)fc_rnd(s) & 0x7fffff;
    }
}
static inline uint32_t fc_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static inline fma_ops fma_case(uint64_t *s, unsigned cat)
{
    static const uint32_t special[] = {0, 0x80000000u, 1, 0x80000001u, 0x007fffff, 0x807fffff, 0x00400000,
                                       0x7f800000, 0xff800000u, 0x7fc00000, 0xffc00001u, 0x7f800001, 0xffa00000u,
                                       0x00800000, 0x80800000u, 0x7f7fffff, 0xff7fffffu};
    fma_ops o;
    unsigned sa = fc_rr(s, 2), sb = fc_rr(s, 2), sc = fc_rr(s, 2);
    int ea = (int)fc_rr(s, 41) - 20, eb = (int)fc_rr(s, 41) - 20;
    switch (cat) {
    default:
    case FC_ORDINARY:
        o.a = fc_f32(sa, ea, fc_frac(s)); o.b = fc_f32(sb, eb, fc_frac(s));
        o.c = fc_f32(sc, ea + eb + (int)fc_rr(s, 13) - 6, fc_frac(s));
        break;
    case FC_ANY:
        o.a = (uint32_t)fc_rnd(s); o.b = (uint32_t)fc_rnd(s); o.c = (uint32_t)fc_rnd(s);
        break;
    case FC_TIE: {
        uint32_t i = 8 * (1 + fc_rr(s, 4)), mult = 2 * fc_rr(s, 4) + 1;
        uint32_t m = (0x800000u - i) * mult;       /* mult * (1 - i 2^-23) * 2^23, a multiple of 8 */
        int e = 0;
        while (m >> 24) { m >>= 1; e++; }
        while (!(m >> 23)) { m <<= 1; e--; }
        o.a = fc_f32(sa, ea, i);                    /* (1 + i 2^-23) * 2^ea */
        o.b = fc_f32(sb, e, m);
        /* the product is mult * 2^ea * (1 - i^2 2^-46) = (k + 1/2) ulp(c) * (...) */
        o.c = fc_f32(sc, ea + 24, fc_rr(s, 4) ? (uint32_t)fc_rnd(s) : fc_frac(s));
        break;
    }
    case FC_GAP:
        o.a = fc_f32(sa, ea, fc_frac(s)); o.b = fc_f32(sb, eb, fc_frac(s));
        o.c = fc_f32(sc, ea + eb + (int)fc_rr(s, 141) - 70, fc_frac(s));
        break;
    case FC_SPAN:
        o.a = fc_f32(sa, ea, fc_frac(s)); o.b = fc_f32(sb, eb, fc_frac(s));
        o.c = fc_f32(sc, ea + eb + 6 + (int)fc_rr(s, 5), fc_frac(s));
        break;
    case FC_OVERFLOW:
        ea = 60 + (int)fc_rr(s, 8); eb = 126 - ea + (int)fc_rr(s, 4);
        o.a = fc_f32(sa, ea, fc_frac(s)); o.b = fc_f32(sb, eb, fc_frac(s));
        o.c = fc_f32(sc, fc_rr(s, 3) ? 127 - (int)fc_rr(s, 4) : 100 - (int)fc_rr(s, 30), fc_frac(s));
        break;
    case FC_TINY:
        ea = -60 - (int)fc_rr(s, 8); eb = -128 - ea + (int)fc_rr(s, 8) - (fc_rr(s, 4) ? 0 : (int)fc_rr(s, 30));
        o.a = fc_f32(sa, ea, fc_frac(s)); o.b = fc_f32(sb, eb, fc_frac(s));
        switch (fc_rr(s, 4)) {
        case 0: o.c = fc_f32(sc, -126, fc_rr(s, 2) ? 0 : fc_rr(s, 4)); break;      /* at the boundary */
        case 1: o.c = fc_f32(sc, ea + eb, fc_frac(s)); break;                      /* near cancellation */
        default: o.c = fc_f32(sc, -126 + (int)fc_rr(s, 6), fc_frac(s)); break;
        }
        break;
    case FC_EXACT: {
        int x = (int)fc_rr(s, 4096) + 1, y = (int)fc_rr(s, 4096) + 1, d = (int)fc_rr(s, 5) - 2;
        if (sa) x = -x;
        o.a = fc_bits((float)x); o.b = fc_bits((float)y);
        o.c = fc_bits((float)(fc_rr(s, 2) ? -(x * y) + d : (int)fc_rr(s, 4096) - 2048));
        if (!(o.c & 0x7fffffff)) o.c = fc_bits((float)-(x * y));                   /* zero is FC_SPECIAL's */
        break;
    }
    case FC_SPECIAL: {
        o = fma_case(s, fc_rr(s, 2) ? FC_ORDINARY : FC_TINY);
        uint32_t v = special[fc_rr(s, sizeof special / sizeof *special)];
        switch (fc_rr(s, 4)) {
        case 0: o.a = v; break;
        case 1: o.b = v; break;
        case 2: o.c = v; break;
        default: o.a = v; o.c = special[fc_rr(s, sizeof special / sizeof *special)]; break;
        }
        break;
    }
    }
    return o;
}
