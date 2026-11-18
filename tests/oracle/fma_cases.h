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
 *   FC_SPECIAL   one or two operands replaced by a zero, subnormal, infinity
 *                or NaN
 * fma64_case(s, cat) gives double operands of the same classes, shared with
 * the oracle's fmad mode and tests/aot/jit_vector.c: TIE is just off a
 * midpoint by i^2 2^-104 of the product, below what a double-double sum
 * rounded once can resolve; SPAN puts c 50 to 60 binades above the product;
 * OVERFLOW and TINY also place exponents at the edges of the translator's
 * operand windows (a64_fp.c fma64, jit.c t_vfp_fma64).
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
        /* a third are zeros: the host path admits those among the specials */
        uint32_t v = fc_rr(s, 3) ? special[fc_rr(s, sizeof special / sizeof *special)] : (uint32_t)fc_rr(s, 2) << 31;
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

/* ---- double precision */
typedef struct { uint64_t a, b, c; } fma64_ops;

/* (-1)^sign * 1.frac * 2^e, the exponent clamped to the normal range */
static inline uint64_t fc_f64(unsigned sign, int e, uint64_t frac)
{
    int be = e + 1023;
    if (be < 1) be = 1;
    if (be > 2046) be = 2046;
    return (uint64_t)sign << 63 | (uint64_t)be << 52 | (frac & ((1ull << 52) - 1));
}
static inline uint64_t fc_frac64(uint64_t *s)
{
    const uint64_t m = (1ull << 52) - 1;
    switch (fc_rr(s, 6)) {
    case 0: return 0;
    case 1: return m;
    case 2: return fc_rnd(s) & m & ~((1ull << fc_rr(s, 52)) - 1);    /* trailing zeros */
    case 3: return (fc_rnd(s) | ~(m >> fc_rr(s, 52))) & m;           /* leading ones */
    default: return fc_rnd(s) & m;
    }
}
static inline uint64_t fc_bits64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static inline fma64_ops fma64_case(uint64_t *s, unsigned cat)
{
    static const uint64_t special[] = {0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000fffffffffffffull,
                                       0x800fffffffffffffull, 0x0008000000000000ull, 0x7ff0000000000000ull,
                                       0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000001ull,
                                       0x7ff0000000000001ull, 0xfff4000000000000ull, 0x0010000000000000ull,
                                       0x8010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull};
    /* the edges of the admitted exponent windows and one step outside */
    static const int edge[] = {-970, -971, -918, -919, -459, -460, 510, 511, 994, 995, 1020, 1021, 1022};
    fma64_ops o;
    unsigned sa = fc_rr(s, 2), sb = fc_rr(s, 2), sc = fc_rr(s, 2);
    int ea = (int)fc_rr(s, 121) - 60, eb = (int)fc_rr(s, 121) - 60;
    switch (cat) {
    default:
    case FC_ORDINARY:
        o.a = fc_f64(sa, ea, fc_frac64(s)); o.b = fc_f64(sb, eb, fc_frac64(s));
        o.c = fc_f64(sc, ea + eb + (int)fc_rr(s, 13) - 6, fc_frac64(s));
        break;
    case FC_ANY:
        o.a = fc_rnd(s); o.b = fc_rnd(s); o.c = fc_rnd(s);
        break;
    case FC_TIE: {
        /* a = (1 + i 2^-52) 2^ea, b = mult (1 - i 2^-52): the product is
         * mult 2^ea (1 - i^2 2^-104) = (k + 1/2) ulp(c) (1 - i^2 2^-104) */
        uint64_t i = 8 * (1 + (fc_rr(s, 2) ? fc_rr(s, 4) : fc_rr(s, 1u << 16)));
        uint64_t mult = 2 * fc_rr(s, 4) + 1;
        uint64_t m = ((1ull << 52) - i) * mult;      /* a multiple of 8 below 2^55 */
        int e = 0;
        while (m >> 53) { m >>= 1; e++; }
        while (!(m >> 52)) { m <<= 1; e--; }
        o.a = fc_f64(sa, ea, i);
        o.b = fc_f64(sb, e, m);
        o.c = fc_f64(sc, ea + 53, fc_rr(s, 4) ? fc_rnd(s) : fc_frac64(s));
        break;
    }
    case FC_GAP:
        o.a = fc_f64(sa, ea, fc_frac64(s)); o.b = fc_f64(sb, eb, fc_frac64(s));
        o.c = fc_f64(sc, ea + eb + (int)fc_rr(s, 241) - 120, fc_frac64(s));
        break;
    case FC_SPAN:
        o.a = fc_f64(sa, ea, fc_frac64(s)); o.b = fc_f64(sb, eb, fc_frac64(s));
        o.c = fc_f64(sc, ea + eb + 50 + (int)fc_rr(s, 11), fc_frac64(s));
        break;
    case FC_OVERFLOW:
        switch (fc_rr(s, 3)) {
        case 0:                                   /* the product and c near the largest double */
            ea = 300 + (int)fc_rr(s, 700); eb = 1021 - ea + (int)fc_rr(s, 4);
            o.c = fc_f64(sc, fc_rr(s, 2) ? 1023 - (int)fc_rr(s, 4) : 1021 - (int)fc_rr(s, 60), fc_frac64(s));
            break;
        case 1:                                   /* a at a window edge, b to match */
            ea = edge[6 + fc_rr(s, 7)]; eb = (int)fc_rr(s, 2) ? -ea + (int)fc_rr(s, 40) : 1021 - ea - (int)fc_rr(s, 3);
            o.c = fc_f64(sc, ea + eb + (int)fc_rr(s, 9) - 4, fc_frac64(s));
            break;
        default:                                  /* c at a window edge */
            o.c = fc_f64(sc, edge[10 + fc_rr(s, 3)], fc_frac64(s));
            eb = edge[10 + fc_rr(s, 3)] - ea - (int)fc_rr(s, 3);
            break;
        }
        if (fc_rr(s, 2)) { int t = ea; ea = eb; eb = t; }
        o.a = fc_f64(sa, ea, fc_frac64(s)); o.b = fc_f64(sb, eb, fc_frac64(s));
        break;
    case FC_TINY:
        switch (fc_rr(s, 3)) {
        case 0:                                   /* results around the smallest normal */
            ea = -400 - (int)fc_rr(s, 400); eb = -1022 - ea + (int)fc_rr(s, 8) - (fc_rr(s, 4) ? 0 : (int)fc_rr(s, 60));
            o.c = fc_rr(s, 2) ? fc_f64(sc, -1022, fc_rr(s, 2) ? 0 : fc_rr(s, 4)) : fc_f64(sc, ea + eb, fc_frac64(s));
            break;
        case 1:                                   /* exponent sum or one exponent at a window edge */
            ea = edge[fc_rr(s, 6)]; eb = fc_rr(s, 2) ? edge[2 + fc_rr(s, 2)] - ea : (int)fc_rr(s, 41) - 20;
            o.c = fc_f64(sc, ea + eb + (int)fc_rr(s, 121) - 60, fc_frac64(s));
            break;
        default:                                  /* c at the window edge */
            o.c = fc_f64(sc, edge[fc_rr(s, 2)], fc_frac64(s));
            eb = -900 - ea + (int)fc_rr(s, 80);
            break;
        }
        if (fc_rr(s, 2)) { int t = ea; ea = eb; eb = t; }
        o.a = fc_f64(sa, ea, fc_frac64(s)); o.b = fc_f64(sb, eb, fc_frac64(s));
        break;
    case FC_EXACT: {
        int64_t x = (int64_t)fc_rr(s, 1u << 26) + 1, y = (int64_t)fc_rr(s, 1u << 26) + 1, d = (int64_t)fc_rr(s, 5) - 2;
        if (sa) x = -x;
        o.a = fc_bits64((double)x); o.b = fc_bits64((double)y);
        o.c = fc_bits64(fc_rr(s, 2) ? (double)(-(x * y) + d) : (double)((int64_t)fc_rr(s, 1u << 30) - (1 << 29)));
        if (!(o.c << 1)) o.c = fc_bits64((double)-(x * y));                        /* zero is FC_SPECIAL's */
        if (fc_rr(s, 4) == 0) {                   /* c = -RN(a*b): the result is the product's error */
            double p = (double)x * (double)y;
            o.c = fc_bits64(-p) + (uint64_t)fc_rr(s, 3) - 1;
        }
        break;
    }
    case FC_SPECIAL: {
        o = fma64_case(s, fc_rr(s, 2) ? FC_ORDINARY : FC_TINY);
        uint64_t v = fc_rr(s, 3) ? special[fc_rr(s, sizeof special / sizeof *special)] : (uint64_t)fc_rr(s, 2) << 63;
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
