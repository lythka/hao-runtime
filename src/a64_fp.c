/*
 * hao — A64 floating-point semantics (see a64_fp.h).
 *
 * Exact software arithmetic, written from the Arm ARM pseudocode:
 *   FPUnpack       inputs; FZ flushes single/double denormals (IDC)
 *   FPProcessNaNs  the first signalling NaN (quietened, IOC) wins, then the
 *                  first quiet NaN, operands in instruction order; FPCR.DN
 *                  replaces any NaN result with the default NaN
 *   FPRound        one rounding of the exact result in FPCR.RMode, FZ flush
 *                  before rounding (UFC), overflow per rounding mode, and
 *                  IXC/UFC/OFC as the architecture sets them
 * Single/double add, sub, mul, div and sqrt take the host SSE result when it
 * is a normal number (the host rounds in the same mode, as the JIT's SSE
 * code does). Single/double conversions to and from integers, round to
 * integral, max/min and comparisons of ordinary operands (normal numbers and
 * zeros; where it says so, infinities) are done on the bits, or by the host
 * integer conversion, when no exception other than inexact can arise. Single
 * fused multiply-add of normal numbers with a normal result goes through
 * binary64 with one round-to-odd step (see fma32). The rest (NaNs, denormals,
 * saturating conversions, other FMA, half precision, FCVT between formats) is
 * computed exactly here.
 * Half precision ignores FZ (FZ16 is not modelled) and AHP.
 */
#include "a64_fp.h"
#include <string.h>
#include <emmintrin.h>

#pragma STDC FENV_ACCESS ON

typedef unsigned __int128 u128;

/* ------------------------------------------------------------ environment */

static inline uint32_t mxcsr_get(void) { uint32_t m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static inline void mxcsr_set(uint32_t m) { __asm__ volatile("ldmxcsr %0" : : "m"(m)); }

fpctx fp_ctx(hao_cpu *c)
{
    uint32_t m = mxcsr_get();
    unsigned rc = (m >> 13) & 3;
    fpctx x = {c, (rc == 1 || rc == 2) ? 3 - rc : rc, (m >> 15) & 1, (c->fpcr >> 25) & 1};
    return x;
}

void fp_raise(fpctx *x, unsigned f)
{
    if (f & FPX_IDC) x->c->fpsr |= FPX_IDC;
    f &= 0x1f;
    if (!f) return;
    /* The flags are cumulative: writing MXCSR only when one is new keeps the
     * common already-inexact case to a read. */
    uint32_t m = mxcsr_get(), want = m | (f & 1) | ((f & 0x1e) << 1);
    if (want != m) mxcsr_set(want);
}

/* ------------------------------------------------------------ formats */

typedef struct { unsigned mbits, ebits; int bias; } fmt;
static fmt F(int es)
{
    switch (es) {
    case 2: return (fmt){10, 5, 15};
    case 4: return (fmt){23, 8, 127};
    default: return (fmt){52, 11, 1023};
    }
}
static inline uint64_t signbit_of(int es) { return 1ull << (es * 8 - 1); }
static inline uint64_t emask_of(int es) { return es == 8 ? ~0ull : (1ull << (es * 8)) - 1; }

/* Integer classification of a raw value; bits above the format are ignored. */
static inline uint64_t mag_of(uint64_t v, int es)       /* the value without its sign */
{
    return es == 4 ? v & 0x7fffffffu : es == 8 ? v & 0x7fffffffffffffffull : v & 0x7fffu;
}
static inline uint64_t inf_mag(int es) { return es == 4 ? 0x7f800000u : es == 8 ? 0x7ff0000000000000ull : 0x7c00u; }
static inline uint64_t min_normal_mag(int es) { return es == 4 ? 0x00800000u : es == 8 ? 0x0010000000000000ull : 0x0400u; }
static inline bool isnan_es(uint64_t v, int es) { return mag_of(v, es) > inf_mag(es); }
/* Zero, normal or infinite: neither a NaN nor a denormal, so FPUnpack takes
 * the value as it is whatever FZ says and raises nothing. */
static inline bool ordinary(uint64_t v, int es)
{
    uint64_t m = mag_of(v, es);
    return m <= inf_mag(es) && (m >= min_normal_mag(es) || !m);
}
/* Sign-magnitude as a two's complement key: for non-NaN values the keys order
 * as the values do, and both zeros have key 0. */
static inline int64_t order_key(uint64_t v, int es)
{
    int64_t m = (int64_t)mag_of(v, es);
    return (v >> (es * 8 - 1)) & 1 ? -m : m;
}

bool fp_isnan(uint64_t v, int es) { return isnan_es(v, es); }
static bool is_snan(uint64_t v, int es) { return fp_isnan(v, es) && !((v >> (F(es).mbits - 1)) & 1); }
static uint64_t quiet(uint64_t v, int es) { return v | 1ull << (F(es).mbits - 1); }
uint64_t fp_default_nan(int es) { fmt f = F(es); return ((1ull << f.ebits) - 1) << f.mbits | 1ull << (f.mbits - 1); }
uint64_t fp_neg(uint64_t v, int es) { return (v ^ signbit_of(es)) & emask_of(es); }
uint64_t fp_abs(uint64_t v, int es) { return v & ~signbit_of(es) & emask_of(es); }
static uint64_t inf_of(int es, int sign) { fmt f = F(es); return (sign ? signbit_of(es) : 0) | ((1ull << f.ebits) - 1) << f.mbits; }
static uint64_t zero_of(int es, int sign) { return sign ? signbit_of(es) : 0; }

enum { C_ZERO, C_NUM, C_INF, C_QNAN, C_SNAN };
typedef struct { int cls, sign, e; uint64_t m; } fpu;   /* value = (-1)^sign * m * 2^e */

static fpu unpack(fpctx *x, int es, uint64_t v)
{
    fmt f = F(es);
    fpu u;
    u.sign = (int)((v >> (es * 8 - 1)) & 1);
    uint64_t e = (v >> f.mbits) & ((1ull << f.ebits) - 1), m = v & ((1ull << f.mbits) - 1);
    u.e = 0; u.m = 0;
    if (e == (1ull << f.ebits) - 1) u.cls = m ? (((m >> (f.mbits - 1)) & 1) ? C_QNAN : C_SNAN) : C_INF;
    else if (e == 0) {
        if (!m) u.cls = C_ZERO;
        else if (x->fz && es != 2) { u.cls = C_ZERO; fp_raise(x, FPX_IDC); }
        else { u.cls = C_NUM; u.m = m; u.e = 1 - f.bias - (int)f.mbits; }
    } else { u.cls = C_NUM; u.m = m | 1ull << f.mbits; u.e = (int)e - f.bias - (int)f.mbits; }
    return u;
}

/* FPProcessNaN(s): returns true with *r set when an operand is a NaN. */
static uint64_t nan_result(fpctx *x, int es, uint64_t v)
{
    if (is_snan(v, es)) fp_raise(x, FPX_IOC);
    return x->dn ? fp_default_nan(es) : quiet(v, es);
}
/* FPUnpack observes every operand before FPProcessNaNs selects its result.
 * A NaN early return must therefore retain IDC from another flushed input. */
static void nan_input_flags(fpctx *x, int es, int n, const uint64_t *ops)
{
    if (!x->fz || es == 2) return;
    fmt f = F(es);
    uint64_t mantissa = (1ull << f.mbits) - 1;
    uint64_t exponent = ((1ull << f.ebits) - 1) << f.mbits;
    for (int k = 0; k < n; k++)
        if (!(ops[k] & exponent) && (ops[k] & mantissa)) { fp_raise(x, FPX_IDC); break; }
}
static bool nans(fpctx *x, int es, int n, const uint64_t *ops, uint64_t *r)
{
    /* Every arithmetic operation asks, and almost none has a NaN operand:
     * settle that with one comparison per operand before the two ordered
     * searches (signalling first, then quiet) classify each one again. */
    bool any = false;
    for (int k = 0; k < n; k++) any |= isnan_es(ops[k], es);
    if (!any) return false;
    for (int k = 0; k < n; k++) if (is_snan(ops[k], es)) { nan_input_flags(x, es, n, ops); *r = nan_result(x, es, ops[k]); return true; }
    for (int k = 0; k < n; k++) if (fp_isnan(ops[k], es)) { nan_input_flags(x, es, n, ops); *r = nan_result(x, es, ops[k]); return true; }
    return false;
}
static bool nan2(fpctx *x, int es, uint64_t a, uint64_t b, uint64_t *r) { uint64_t o[2] = {a, b}; return nans(x, es, 2, o, r); }
static uint64_t invalid(fpctx *x, int es) { fp_raise(x, FPX_IOC); return fp_default_nan(es); }

/* ------------------------------------------------------------ rounding */

enum { RND_ODD = 5 };

static inline int msb128(u128 v)
{
    uint64_t hi = (uint64_t)(v >> 64);
    return hi ? 127 - __builtin_clzll(hi) : 63 - __builtin_clzll((uint64_t)v);
}

/* FPRound of the exact value (-1)^sign * m * 2^e (+ a nonzero tail below m's
 * last bit when sticky) to format es in `mode`. */
static uint64_t round_pack_mode(fpctx *x, int es, int sign, int e, u128 m, bool sticky, int mode)
{
    fmt f = F(es);
    int p = (int)f.mbits + 1, emin = 1 - f.bias;          /* precision, minimum normal exponent */
    if (!m) {
        if (!sticky) return zero_of(es, sign);
        m = 1; e -= 200; sticky = false;                   /* a nonzero tail only: far below anything */
    }
    int L = msb128(m), E = e + L;                          /* value in [2^E, 2^(E+1)) */
    if (x->fz && es != 2 && E < emin) {                    /* flush to zero before rounding */
        fp_raise(x, FPX_UFC);
        return zero_of(es, sign);
    }
    bool tiny = E < emin;
    int lsb = tiny ? emin - (int)f.mbits : E - (p - 1);     /* exponent of the result's last bit */
    int sh = lsb - e;
    u128 rb = 0, half = 0;
    if (sh > 0) {
        if (sh > 127) { sticky = sticky || m; m = 0; }
        else {
            rb = m & (((u128)1 << sh) - 1);
            half = (u128)1 << (sh - 1);
            m >>= sh;
        }
    } else m <<= -sh;
    bool inexact = rb || sticky, up = false;
    switch (mode) {
    case FPR_N: up = rb > half || (rb == half && half && (sticky || (m & 1))); break;
    case FPR_P: up = inexact && !sign; break;
    case FPR_M: up = inexact && sign; break;
    case FPR_A: up = half && rb >= half; break;
    default: break;                                        /* RZ, odd */
    }
    if (mode == RND_ODD && inexact) m |= 1;
    m += up;
    int be;                                                /* biased exponent */
    if (m >> p) { m >>= 1; lsb++; }                        /* carry out of a normal mantissa */
    if (m >> (p - 1)) be = lsb + (int)f.mbits + f.bias;    /* normal (including subnormal rounded up) */
    else be = 0;
    if (inexact) {
        unsigned fl = FPX_IXC;
        if (tiny) fl |= FPX_UFC;
        fp_raise(x, fl);
    }
    if (be >= (1 << f.ebits) - 1) {                        /* overflow */
        fp_raise(x, FPX_OFC | FPX_IXC);
        bool to_inf = mode == FPR_N || mode == FPR_A || (mode == FPR_P && !sign) || (mode == FPR_M && sign);
        if (to_inf) return inf_of(es, sign);
        return (sign ? signbit_of(es) : 0) | (((1ull << f.ebits) - 2) << f.mbits) | ((1ull << f.mbits) - 1);
    }
    return (sign ? signbit_of(es) : 0) | (uint64_t)be << f.mbits | ((uint64_t)m & ((1ull << f.mbits) - 1));
}
static uint64_t round_pack(fpctx *x, int es, int sign, int e, u128 m, bool sticky)
{
    return round_pack_mode(x, es, sign, e, m, sticky, (int)x->rmode);
}
static uint64_t repack(fpctx *x, int es, fpu u)   /* an exactly representable value (FZ applies) */
{
    if (u.cls == C_ZERO) return zero_of(es, u.sign);
    if (u.cls == C_INF) return inf_of(es, u.sign);
    return round_pack(x, es, u.sign, u.e, u.m, false);
}

/* exact sum of two signed values, with result (sign, e, m, sticky) */
typedef struct { int sign, e; u128 m; bool sticky; } xval;
/* Keep caller-specific simplification when the narrow path grows this helper;
 * add/sub and reciprocal steps previously inlined the wide-only version. */
static inline __attribute__((always_inline)) xval add_exact(int s1, int e1, u128 m1, int s2, int e2, u128 m2, int mode)
{
    xval r = {0, 0, 0, false};
    if (!m1) { r.sign = s2; r.e = e2; r.m = m2; return r; }
    if (!m2) { r.sign = s1; r.e = e1; r.m = m1; return r; }
    int l1 = msb128(m1) + 1, l2 = msb128(m2) + 1;
    /* Each aligned magnitude needs at most 63 bits, leaving one carry bit.
     * The bound also proves both narrowing conversions and shifts are safe.
     * Keep the original wide path for larger or widely separated terms. */
    if (l1 <= 64 && l2 <= 64) {
        int e = e1 < e2 ? e1 : e2;
        int d1 = e1 - e, d2 = e2 - e;
        if (l1 + d1 <= 63 && l2 + d2 <= 63) {
            uint64_t a = (uint64_t)m1 << d1, b = (uint64_t)m2 << d2;
            r.e = e;
            if (s1 == s2) { r.sign = s1; r.m = a + b; }
            else if (a > b) { r.sign = s1; r.m = a - b; }
            else if (b > a) { r.sign = s2; r.m = b - a; }
            else r.sign = mode == FPR_M;
            return r;
        }
    }
    /* normalise both to msb at bit 125 */
    int n1 = 126 - l1, n2 = 126 - l2;
    m1 <<= n1; e1 -= n1; m2 <<= n2; e2 -= n2;
    if (e1 < e2 || (e1 == e2 && m1 < m2)) {                /* |op1| >= |op2| */
        int ts = s1; s1 = s2; s2 = ts;
        int te = e1; e1 = e2; e2 = te;
        u128 tm = m1; m1 = m2; m2 = tm;
    }
    int d = e1 - e2;
    bool st = false;
    if (d > 0) {
        if (d > 127) { st = m2 != 0; m2 = 0; }
        else { st = (m2 & (((u128)1 << d) - 1)) != 0; m2 >>= d; }
    }
    r.e = e1;
    r.sign = s1;
    if (s1 == s2) { r.m = m1 + m2; r.sticky = st; }
    else {
        r.m = m1 - m2;
        if (st) { r.m -= 1; r.sticky = true; }
        if (!r.m && !r.sticky) r.sign = mode == FPR_M;     /* exact cancellation */
    }
    return r;
}

/* ------------------------------------------------------------ host fast path */

static inline double d_of(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline float f_of(uint64_t v) { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); return f; }
static inline uint64_t bits_d(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static inline uint64_t bits_f(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

/* A host result is final when both inputs are normal or zero and the result
 * is a normal number: then the host's rounding in the same mode is FPRound. */
static bool plain(uint64_t v, int es)
{
    fmt f = F(es);
    uint64_t e = (v >> f.mbits) & ((1ull << f.ebits) - 1);
    return e != (1ull << f.ebits) - 1 && (e != 0 || !(v & ((1ull << f.mbits) - 1)));
}
static bool normal(uint64_t v, int es)
{
    fmt f = F(es);
    uint64_t e = (v >> f.mbits) & ((1ull << f.ebits) - 1);
    return e && e != (1ull << f.ebits) - 1;
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV };
static bool host2(int op, int es, uint64_t a, uint64_t b, uint64_t *r)
{
    if (es == 2 || !plain(a, es) || !plain(b, es)) return false;
    uint32_t saved = mxcsr_get();
    uint64_t v;
    if (es == 8) {
        double x = d_of(a), y = d_of(b), z;
        switch (op) { case OP_ADD: z = x + y; break; case OP_SUB: z = x - y; break; case OP_MUL: z = x * y; break; default: z = x / y; break; }
        v = bits_d(z);
    } else {
        float x = f_of(a), y = f_of(b), z;
        switch (op) { case OP_ADD: z = x + y; break; case OP_SUB: z = x - y; break; case OP_MUL: z = x * y; break; default: z = x / y; break; }
        v = bits_f(z);
    }
    if (!normal(v, es)) {
        /* A declined hardware result is not architectural state. In
         * particular FTZ can raise IXC, while ARM's FZ-before-rounding
         * fallback raises only UFC. Keep preexisting sticky flags. */
        mxcsr_set(saved);
        return false;
    }
    *r = v;
    return true;
}

/* Batched form of host2's conservative finite fast path. Classify with integer
 * bits so even inspecting an inactive signalling NaN cannot raise IOC. Only
 * normal/zero inputs and normal results away from the minimum-normal boundary
 * are admitted, plus zeros proven exact from the operands. Other results use
 * the existing exact lane path. No FMA or optional x86 feature is used. */
bool fp_vec32(unsigned op, const hao_vreg *a, const hao_vreg *b, int lanes, hao_vreg *out)
{
    if (op > FPV_MUL || (lanes != 2 && lanes != 4)) return false;
    __m128i x = _mm_loadu_si128((const __m128i *)a), y = _mm_loadu_si128((const __m128i *)b);
    __m128i z = _mm_setzero_si128(), mask = _mm_set1_epi32(0x7f800000);
    __m128i ax = _mm_and_si128(x, _mm_set1_epi32(0x7fffffff));
    __m128i ay = _mm_and_si128(y, _mm_set1_epi32(0x7fffffff));
    __m128i ex = _mm_and_si128(x, mask), ey = _mm_and_si128(y, mask);
    __m128i bad = _mm_or_si128(_mm_cmpeq_epi32(ex, mask), _mm_cmpeq_epi32(ey, mask));
    bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_cmpeq_epi32(ax, z), _mm_cmpeq_epi32(ex, z)));
    bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_cmpeq_epi32(ay, z), _mm_cmpeq_epi32(ey, z)));
    unsigned active = lanes == 4 ? 0xffff : 0xff;
    if ((unsigned)_mm_movemask_epi8(bad) & active) return false;
    if (lanes == 2) { x = _mm_unpacklo_epi64(x, z); y = _mm_unpacklo_epi64(y, z); }

    uint32_t saved = mxcsr_get();
    __m128 result;
    switch (op) {
    case FPV_ADD: result = _mm_add_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(y)); break;
    case FPV_SUB: result = _mm_sub_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(y)); break;
    default: result = _mm_mul_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(y)); break;
    }
    __m128i bits = _mm_castps_si128(result), exp = _mm_and_si128(bits, mask);
    bad = _mm_or_si128(_mm_cmpeq_epi32(exp, z), _mm_cmpeq_epi32(exp, _mm_set1_epi32(0x00800000)));
    bad = _mm_or_si128(bad, _mm_cmpeq_epi32(exp, mask));
    if ((unsigned)_mm_movemask_epi8(bad) & active) {
        /* A zero output alone is insufficient: nonzero inputs can underflow
         * or flush to zero with different ARM/SSE flags. Exact cancellation
         * and a zero multiply operand do not add exceptions. Keep the SSE
         * result's sign, including round-down cancellation's negative zero. */
        __m128i zx = _mm_cmpeq_epi32(ax, z), zy = _mm_cmpeq_epi32(ay, z), exact;
        if (op == FPV_MUL) exact = _mm_or_si128(zx, zy);
        else {
            __m128i cancel = op == FPV_ADD ?
                _mm_cmpeq_epi32(x, _mm_xor_si128(y, _mm_set1_epi32((int)0x80000000u))) :
                _mm_cmpeq_epi32(x, y);
            exact = _mm_or_si128(_mm_and_si128(zx, zy), cancel);
        }
        __m128i zero_result = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), z);
        bad = _mm_andnot_si128(_mm_and_si128(exact, zero_result), bad);
        if ((unsigned)_mm_movemask_epi8(bad) & active) {
            mxcsr_set(saved);
            return false;
        }
    }
    _mm_storeu_si128((__m128i *)out, bits);
    return true;
}

/* Normal/zero comparisons and selection are exact and add no exceptions.
 * Reject specials with integer masks before any FP instruction, so NaN policy
 * and flushed-input IDC stay with the reference path. Inactive lanes are
 * cleared before FP evaluation, independently of the destination's clearing. */
bool fp_vec32_select(unsigned op, const hao_vreg *a, const hao_vreg *b, int lanes, hao_vreg *out)
{
    if (op > FPVS_GT || (lanes != 2 && lanes != 4)) return false;
    __m128i x = _mm_loadu_si128((const __m128i *)a), y = _mm_loadu_si128((const __m128i *)b);
    __m128i z = _mm_setzero_si128(), mask = _mm_set1_epi32(0x7f800000);
    __m128i ax = _mm_and_si128(x, _mm_set1_epi32(0x7fffffff));
    __m128i ay = _mm_and_si128(y, _mm_set1_epi32(0x7fffffff));
    __m128i ex = _mm_and_si128(x, mask), ey = _mm_and_si128(y, mask);
    __m128i bad = _mm_or_si128(_mm_cmpeq_epi32(ex, mask), _mm_cmpeq_epi32(ey, mask));
    bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_cmpeq_epi32(ax, z), _mm_cmpeq_epi32(ex, z)));
    bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_cmpeq_epi32(ay, z), _mm_cmpeq_epi32(ey, z)));
    unsigned active = lanes == 4 ? 0xffff : 0xff;
    if ((unsigned)_mm_movemask_epi8(bad) & active) return false;
    if (lanes == 2) { x = _mm_unpacklo_epi64(x, z); y = _mm_unpacklo_epi64(y, z); }
    __m128 fx = _mm_castsi128_ps(x), fy = _mm_castsi128_ps(y);
    __m128i result;
    if (op == FPVS_MAX || op == FPVS_MIN) {
        result = _mm_castps_si128(op == FPVS_MAX ? _mm_max_ps(fx, fy) : _mm_min_ps(fx, fy));
        __m128i zeros = _mm_and_si128(_mm_cmpeq_epi32(ax, z), _mm_cmpeq_epi32(ay, z));
        __m128i signs = op == FPVS_MAX ? _mm_and_si128(x, y) : _mm_or_si128(x, y);
        /* ARM chooses +0 for max and -0 for min of opposite-signed zeros.
         * SSE chooses its second operand; repair only both-zero lanes. Keep
         * the select as SSE2 rather than Penryn's possible PBLENDVB fusion. */
        __asm__("pand %0, %1\n\tpandn %2, %0\n\tpor %1, %0"
                : "+&x"(zeros), "+&x"(signs) : "x"(result));
        result = zeros;
    } else {
        /* Strict-FP Clang can scalarize relational intrinsics and reconstruct
         * their masks with optional SSE3/SSE4 instructions. Keep predicates
         * packed SSE2 and volatile so no FP comparison can move ahead of the
         * integer admission check. Admitted inputs cannot raise exceptions. */
        __m128 predicate;
        if (op == FPVS_EQ) {
            predicate = fx;
            __asm__ volatile("cmpeqps %1, %0" : "+x"(predicate) : "x"(fy));
        } else if (op == FPVS_GE) {
            predicate = fy;
            __asm__ volatile("cmpleps %1, %0" : "+x"(predicate) : "x"(fx));
        } else {
            predicate = fy;
            __asm__ volatile("cmpltps %1, %0" : "+x"(predicate) : "x"(fx));
        }
        result = _mm_castps_si128(predicate);
    }
    _mm_storeu_si128((__m128i *)out, result);
    return true;
}

/* ------------------------------------------------------------ arithmetic */

static uint64_t addsub(fpctx *x, int es, uint64_t a, uint64_t b, bool sub)
{
    uint64_t r;
    if (nan2(x, es, a, b, &r)) return r;
    if (host2(sub ? OP_SUB : OP_ADD, es, a, b, &r)) return r;
    fpu u = unpack(x, es, a), v = unpack(x, es, b);
    if (sub) v.sign ^= 1;
    if (u.cls == C_INF && v.cls == C_INF && u.sign != v.sign) return invalid(x, es);
    if (u.cls == C_INF) return inf_of(es, u.sign);
    if (v.cls == C_INF) return inf_of(es, v.sign);
    if (u.cls == C_ZERO && v.cls == C_ZERO)
        return zero_of(es, u.sign == v.sign ? u.sign : x->rmode == FPR_M);
    xval s = add_exact(u.sign, u.e, u.m, v.sign, v.e, v.m, (int)x->rmode);
    return round_pack(x, es, s.sign, s.e, s.m, s.sticky);
}
uint64_t fp_add(fpctx *x, int es, uint64_t a, uint64_t b) { return addsub(x, es, a, b, false); }
uint64_t fp_sub(fpctx *x, int es, uint64_t a, uint64_t b) { return addsub(x, es, a, b, true); }

static uint64_t mul(fpctx *x, int es, uint64_t a, uint64_t b, bool mulx)
{
    uint64_t r;
    if (nan2(x, es, a, b, &r)) return r;
    if (host2(OP_MUL, es, a, b, &r)) return r;
    fpu u = unpack(x, es, a), v = unpack(x, es, b);
    int sign = u.sign ^ v.sign;
    if ((u.cls == C_INF && v.cls == C_ZERO) || (u.cls == C_ZERO && v.cls == C_INF)) {
        if (!mulx) return invalid(x, es);
        return round_pack(x, es, sign, 1, 1, false);       /* FMULX: +-2.0 */
    }
    if (u.cls == C_INF || v.cls == C_INF) return inf_of(es, sign);
    if (u.cls == C_ZERO || v.cls == C_ZERO) return zero_of(es, sign);
    return round_pack(x, es, sign, u.e + v.e, (u128)u.m * v.m, false);
}
uint64_t fp_mul(fpctx *x, int es, uint64_t a, uint64_t b) { return mul(x, es, a, b, false); }
uint64_t fp_mulx(fpctx *x, int es, uint64_t a, uint64_t b) { return mul(x, es, a, b, true); }

uint64_t fp_div(fpctx *x, int es, uint64_t a, uint64_t b)
{
    uint64_t r;
    if (nan2(x, es, a, b, &r)) return r;
    if (host2(OP_DIV, es, a, b, &r)) return r;
    fpu u = unpack(x, es, a), v = unpack(x, es, b);
    int sign = u.sign ^ v.sign;
    if ((u.cls == C_INF && v.cls == C_INF) || (u.cls == C_ZERO && v.cls == C_ZERO)) return invalid(x, es);
    if (u.cls == C_INF || v.cls == C_ZERO) {
        if (v.cls == C_ZERO && u.cls != C_INF) fp_raise(x, FPX_DZC);
        return inf_of(es, sign);
    }
    if (u.cls == C_ZERO || v.cls == C_INF) return zero_of(es, sign);
    int nu = 63 - msb128(u.m), nv = 63 - msb128(v.m);       /* both to 64-bit normalised */
    u128 n = (u128)(u.m << nu) << 64, d = (u128)(v.m << nv);
    u128 q = n / d;
    return round_pack(x, es, sign, (u.e - nu) - (v.e - nv) - 64, q, (n % d) != 0);
}

static u128 isqrt128(u128 n, bool *exact)
{
    u128 r = 0, bitv = (u128)1 << 126;
    while (bitv > n) bitv >>= 2;
    while (bitv) {
        if (n >= r + bitv) { n -= r + bitv; r = (r >> 1) + bitv; }
        else r >>= 1;
        bitv >>= 2;
    }
    *exact = n == 0;
    return r;
}
uint64_t fp_sqrt(fpctx *x, int es, uint64_t a)
{
    if (fp_isnan(a, es)) return nan_result(x, es, a);
    fpu u = unpack(x, es, a);
    if (u.cls == C_ZERO) return zero_of(es, u.sign);
    if (u.sign) return invalid(x, es);
    if (u.cls == C_INF) return inf_of(es, 0);
    if (es != 2 && plain(a, es)) {
        uint64_t v = es == 8 ? bits_d(__builtin_sqrt(d_of(a))) : bits_f(__builtin_sqrtf(f_of(a)));
        if (normal(v, es)) return v;
    }
    int sh = 120 - msb128(u.m);                            /* m * 2^sh has ~121 bits, e - sh even */
    if ((u.e - sh) & 1) sh++;
    bool exact;
    u128 r = isqrt128((u128)u.m << sh, &exact);
    return round_pack(x, es, 0, (u.e - sh) / 2, r, !exact);
}

/* FPMulAdd(addend, op1, op2) */
static inline __m128i half_nonordinary(const hao_vreg *v)
{
    __m128i raw;
    memcpy(&raw, v, sizeof raw);
    __m128i magnitude = _mm_and_si128(raw, _mm_set1_epi16(0x7fff));
    __m128i exponent = _mm_and_si128(raw, _mm_set1_epi16(0x7c00));
    __m128i zero = _mm_cmpeq_epi16(magnitude, _mm_setzero_si128());
    __m128i tiny = _mm_cmpeq_epi16(exponent, _mm_setzero_si128());
    return _mm_or_si128(_mm_cmpeq_epi16(exponent, _mm_set1_epi16(0x7c00)),
                        _mm_andnot_si128(zero, tiny));
}

bool fp_vec16_muladd(fpctx *x, const hao_vreg *d, const hao_vreg *a,
                    const hao_vreg *b, int lanes, bool subtract, hao_vreg *out)
{
    /* All controls outside the modelled rounding/FZ/DN contract retain the
     * reference path, including FZ16, AHP and alternate-handling controls. */
    if ((lanes != 4 && lanes != 8) || (x->c->fpcr & ~0x03c00000u)) return false;
    __m128i bad = _mm_or_si128(half_nonordinary(a),
                              _mm_or_si128(half_nonordinary(b), half_nonordinary(d)));
    if ((unsigned)_mm_movemask_epi8(bad) & ((1u << (lanes * 2)) - 1)) return false;
    uint64_t av[8], bv[8], dv[8], sum[8];
    for (int k = 0; k < lanes; k++) {
        uint16_t h[3] = {a->h[k], b->h[k], d->h[k]};
        if (subtract) h[0] ^= 0x8000;
        unsigned exponent[3], mantissa[3];
        for (int j = 0; j < 3; j++) {
            exponent[j] = h[j] >> 10 & 31;
            unsigned fraction = h[j] & 1023;
            mantissa[j] = exponent[j] ? fraction | 1024 : 0;
        }
        unsigned product = mantissa[0] * mantissa[1], acc = mantissa[2];
        if (product && acc) {
            int ep = (int)exponent[0] + (int)exponent[1] - 50;
            int ea = (int)exponent[2] - 25;
            int top_p = ep + 32 - __builtin_clz(product);
            int top_a = ea + 32 - __builtin_clz(acc);
            int top = top_p > top_a ? top_p : top_a;
            int low = ep < ea ? ep : ea;
            /* One extra bit covers a carry. Both terms and their sum then
             * fit exactly in binary64, so there can be no double rounding. */
            if (top + 1 - low > 53) return false;
        }
        uint64_t wide[3];
        for (int j = 0; j < 3; j++)
            wide[j] = (uint64_t)(h[j] & 0x8000) << 48 |
                (exponent[j] ? (uint64_t)(exponent[j] + 1008) << 52 |
                               (uint64_t)(h[j] & 1023) << 42 : 0);
        av[k] = wide[0]; bv[k] = wide[1]; dv[k] = wide[2];
    }
    /* Every active lane has been admitted before any floating evaluation.
     * All nonzero intermediates are normal binary64 and all arithmetic is
     * exact. The live guest rounding mode supplies the cancellation sign. */
    for (int k = 0; k < lanes; k += 2) {
        __m128d p, m, acc;
        memcpy(&p, av + k, sizeof p); memcpy(&m, bv + k, sizeof m);
        memcpy(&acc, dv + k, sizeof acc);
        __asm__ volatile("mulpd %1, %0\n\taddpd %2, %0"
                         : "+&x"(p) : "x"(m), "x"(acc));
        memcpy(sum + k, &p, sizeof p);
    }
    hao_vreg r = {0};
    for (int k = 0; k < lanes; k++) r.h[k] = (uint16_t)fp_cvt(x, sum[k], 8, 2);
    *out = r;
    return true;
}

/* Single-precision fused multiply-add without an FMA instruction, for two
 * lanes held in the low halves of a, b and c:
 *   p = a * b in binary64 is exact (24 + 24 significant bits);
 *   s = RN(p + c), and Knuth's TwoSum gives e = (p + c) - s exactly;
 *   if e != 0 and s is even, s moves one unit in the last place toward e.
 * The value returned is p + c rounded to odd at 53 bits. Rounding that once to
 * 24 bits in any mode equals rounding p + c itself (Boldo and Melquiond;
 * needs 53 >= 24 + 2), and it is inexact exactly when p + c is not a float.
 * Requires MXCSR round-to-nearest and normal inputs: every nonzero value here
 * is then a normal binary64 (at least 2^-298, below 2^257), so FTZ/DAZ and
 * binary64 overflow cannot apply. The only flag this can raise is PE, and
 * only when e != 0. Assembly keeps the operation order and SSE2 encodings
 * (the compiler otherwise selects SSE4.1 forms for the integer step). */
static inline __attribute__((always_inline)) __m128i fma32_odd(__m128i a, __m128i b, __m128i c)
{
    __m128i s, e;
    __asm__ volatile("cvtps2pd %[a], %[a]\n\t"
                     "cvtps2pd %[b], %[b]\n\t"
                     "cvtps2pd %[c], %[c]\n\t"
                     "mulpd %[b], %[a]\n\t"          /* p */
                     "movapd %[a], %[s]\n\t"
                     "addpd %[c], %[s]\n\t"          /* s = RN(p + c) */
                     "movapd %[s], %[b]\n\t"
                     "subpd %[a], %[b]\n\t"          /* c' = s - p */
                     "movapd %[s], %[e]\n\t"
                     "subpd %[b], %[e]\n\t"          /* p' = s - c' */
                     "subpd %[e], %[a]\n\t"          /* p - p' */
                     "subpd %[b], %[c]\n\t"          /* c - c' */
                     "addpd %[c], %[a]\n\t"          /* e */
                     "xorpd %[c], %[c]\n\t"
                     "movapd %[a], %[e]\n\t"
                     "cmpneqpd %[c], %[a]\n\t"      /* inexact = e != 0 */
                     /* The step is +1 when s and e have the same sign (away
                      * from zero), else -1. An even s has no carry out of its
                      * significand going up, and going down from a power of
                      * two reaches the all-ones significand below: odd. */
                     "movdqa %[s], %[b]\n\t"
                     "pxor %[e], %[b]\n\t"
                     "psrad $31, %[b]\n\t"
                     "pshufd $0xf5, %[b], %[b]\n\t" /* signs differ: -1, else 0 */
                     "pcmpeqd %[e], %[e]\n\t"
                     "psrlq $63, %[e]\n\t"
                     "por %[e], %[b]\n\t"           /* -1 or +1 */
                     "movdqa %[s], %[c]\n\t"
                     "psllq $63, %[c]\n\t"
                     "psrad $31, %[c]\n\t"
                     "pshufd $0xf5, %[c], %[c]\n\t" /* s odd */
                     "pandn %[a], %[c]\n\t"         /* inexact and s even */
                     "pand %[c], %[b]\n\t"
                     "paddq %[b], %[s]"
                     : [a] "+&x"(a), [b] "+&x"(b), [c] "+&x"(c), [s] "=&x"(s), [e] "=&x"(e));
    return s;
}

/* Speculative FPMulAdd(c, a, b) on 1, 2 or 4 single lanes. Admits only normal
 * inputs and results at or above 2^-125 (overflow included: the conversion
 * rounds and flags it as FPRound does). Zero and tiny results are left to the
 * exact path because ARM detects tininess before rounding, SSE after, and FZ
 * applies; so are NaNs, infinities, zeros and subnormals, checked on integer
 * bits before any FP instruction. The final conversion runs under the guest's
 * MXCSR, which supplies the rounding mode and accumulates PE/OE as IXC/OFC.
 * False leaves the FP environment as it was. */
static inline __attribute__((always_inline)) bool fma32(__m128i c, __m128i a, __m128i b, int lanes, __m128i *out)
{
    __m128i z = _mm_setzero_si128(), mask = _mm_set1_epi32(0x7f800000);
    __m128i ea = _mm_and_si128(a, mask), eb = _mm_and_si128(b, mask), ec = _mm_and_si128(c, mask);
    __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi32(ea, z), _mm_cmpeq_epi32(ea, mask)),
                               _mm_or_si128(_mm_cmpeq_epi32(eb, z), _mm_cmpeq_epi32(eb, mask)));
    bad = _mm_or_si128(bad, _mm_or_si128(_mm_cmpeq_epi32(ec, z), _mm_cmpeq_epi32(ec, mask)));
    unsigned active = (1u << (lanes * 4)) - 1;
    if ((unsigned)_mm_movemask_epi8(bad) & active) return false;
    if (lanes == 1) {
        /* 1 * 1 + 1 in the unused lane of the pair: exact, no flags */
        __m128i one = _mm_set1_epi32(0x3f800000);
        a = _mm_unpacklo_epi32(a, one);
        b = _mm_unpacklo_epi32(b, one);
        c = _mm_unpacklo_epi32(c, one);
    }
    uint32_t saved = mxcsr_get();
    bool nearest = !(saved & 0x6000);
    if (!nearest) mxcsr_set(saved & ~0x6000u);
    __m128i lo = fma32_odd(a, b, c), hi = z;
    if (lanes == 4)
        hi = fma32_odd(_mm_unpackhi_epi64(a, a), _mm_unpackhi_epi64(b, b), _mm_unpackhi_epi64(c, c));
    /* Under round-to-nearest the sums ran in the guest's MXCSR. PE from an
     * inexact sum is then already the result's IXC: the odd last bit makes
     * the conversion inexact as well. A decline restores MXCSR below. */
    if (!nearest) mxcsr_set(saved);
    __asm__ volatile("cvtpd2ps %0, %0" : "+x"(lo));
    if (lanes == 4) {
        __asm__ volatile("cvtpd2ps %0, %0" : "+x"(hi));
        lo = _mm_unpacklo_epi64(lo, hi);
    }
    __m128i er = _mm_and_si128(lo, mask);
    bad = _mm_or_si128(_mm_cmpeq_epi32(er, z), _mm_cmpeq_epi32(er, _mm_set1_epi32(0x00800000)));
    if ((unsigned)_mm_movemask_epi8(bad) & active) {
        mxcsr_set(saved);
        return false;
    }
    *out = lo;
    return true;
}

bool fp_vec32_muladd(fpctx *x, const hao_vreg *d, const hao_vreg *a,
                     const hao_vreg *b, int lanes, bool subtract, hao_vreg *out)
{
    /* The same FPCR contract as fp_vec16_muladd: only RMode, FZ and DN. */
    if ((lanes != 2 && lanes != 4) || (x->c->fpcr & ~0x03c00000u)) return false;
    __m128i va = _mm_loadu_si128((const __m128i *)a), r;
    if (subtract) va = _mm_xor_si128(va, _mm_set1_epi32((int)0x80000000u));
    if (!fma32(_mm_loadu_si128((const __m128i *)d), va, _mm_loadu_si128((const __m128i *)b), lanes, &r))
        return false;
    if (lanes == 2) r = _mm_unpacklo_epi64(r, _mm_setzero_si128());
    _mm_storeu_si128((__m128i *)out, r);
    return true;
}

uint64_t fp_muladd(fpctx *x, int es, uint64_t addend, uint64_t a, uint64_t b)
{
    if (es == 4 && !(x->c->fpcr & ~0x03c00000u)) {
        __m128i r;
        if (fma32(_mm_cvtsi32_si128((int)(uint32_t)addend), _mm_cvtsi32_si128((int)(uint32_t)a),
                  _mm_cvtsi32_si128((int)(uint32_t)b), 1, &r))
            return (uint32_t)_mm_cvtsi128_si32(r);
    }
    fpu c = unpack(x, es, addend), u = unpack(x, es, a), v = unpack(x, es, b);
    bool inf_zero = (u.cls == C_INF && v.cls == C_ZERO) || (u.cls == C_ZERO && v.cls == C_INF);
    uint64_t ops[3] = {addend, a, b}, r;
    if (c.cls == C_QNAN && inf_zero && !is_snan(a, es) && !is_snan(b, es)) return invalid(x, es);
    /* unpack has classified the operands: nans() has nothing to do, and no
     * effect, unless one of them is a NaN */
    if ((c.cls >= C_QNAN || u.cls >= C_QNAN || v.cls >= C_QNAN) && nans(x, es, 3, ops, &r)) return r;
    int sp = u.sign ^ v.sign;
    bool infp = u.cls == C_INF || v.cls == C_INF, zerop = u.cls == C_ZERO || v.cls == C_ZERO;
    if (inf_zero || (c.cls == C_INF && infp && c.sign != sp)) return invalid(x, es);
    if (c.cls == C_INF) return inf_of(es, c.sign);
    if (infp) return inf_of(es, sp);
    if (c.cls == C_ZERO && zerop) return zero_of(es, c.sign == sp ? sp : x->rmode == FPR_M);
    /* Input flush/NaN/invalid processing and the both-zero sign rule are
     * complete. A finite nonzero addend plus an exact zero product is already
     * representable; retain its bits without an identity round/pack. */
    if (zerop) return addend & emask_of(es);
    u128 pm = zerop ? 0 : (u128)u.m * v.m;
    xval s = add_exact(sp, u.e + v.e, pm, c.sign, c.e, c.cls == C_ZERO ? 0 : c.m, (int)x->rmode);
    if (!s.m && !s.sticky) return zero_of(es, s.sign);
    return round_pack(x, es, s.sign, s.e, s.m, s.sticky);
}

static int cmp3(fpctx *x, int es, uint64_t a, uint64_t b);

/* FPMax/FPMin (and the NM forms, which replace a single quiet NaN) */
static uint64_t maxmin(fpctx *x, int es, uint64_t a, uint64_t b, bool max, bool nm)
{
    /* Zeros, normals and infinities: FPMax/FPMin (and the NM forms, which
     * differ only for NaNs) return one operand unchanged and raise nothing.
     * Equal keys are identical values or zeros of either sign, where max
     * takes +0 (AND) and min -0 (OR). */
    if (es != 2 && ordinary(a, es) && ordinary(b, es)) {
        int64_t ka = order_key(a, es), kb = order_key(b, es);
        if (ka == kb) return (max ? a & b : a | b) & emask_of(es);
        return ((ka > kb) == max ? a : b) & emask_of(es);
    }
    if (nm) {
        bool qa = fp_isnan(a, es) && !is_snan(a, es), qb = fp_isnan(b, es) && !is_snan(b, es);
        if (qa && !qb) a = inf_of(es, max);
        else if (!qa && qb) b = inf_of(es, max);
    }
    uint64_t r;
    if (nan2(x, es, a, b, &r)) return r;
    fpu u = unpack(x, es, a), v = unpack(x, es, b);
    if (u.cls == C_ZERO && v.cls == C_ZERO) return zero_of(es, max ? (u.sign & v.sign) : (u.sign | v.sign));
    int cmp = cmp3(x, es, a, b);
    fpu w = (cmp > 0) == max ? u : v;
    return repack(x, es, w);
}
uint64_t fp_max(fpctx *x, int es, uint64_t a, uint64_t b) { return maxmin(x, es, a, b, true, false); }
uint64_t fp_min(fpctx *x, int es, uint64_t a, uint64_t b) { return maxmin(x, es, a, b, false, false); }
uint64_t fp_maxnm(fpctx *x, int es, uint64_t a, uint64_t b) { return maxmin(x, es, a, b, true, true); }
uint64_t fp_minnm(fpctx *x, int es, uint64_t a, uint64_t b) { return maxmin(x, es, a, b, false, true); }

/* FPRecipStepFused / FPRSqrtStepFused: op1 is negated before NaN processing */
static uint64_t step(fpctx *x, int es, uint64_t a, uint64_t b, bool rsqrt)
{
    a = fp_neg(a, es);
    uint64_t r;
    if (nan2(x, es, a, b, &r)) return r;
    fpu u = unpack(x, es, a), v = unpack(x, es, b);
    int sp = u.sign ^ v.sign;
    if ((u.cls == C_INF && v.cls == C_ZERO) || (u.cls == C_ZERO && v.cls == C_INF))
        return round_pack(x, es, 0, rsqrt ? -1 : 1, rsqrt ? 3 : 1, false);   /* 1.5 / 2.0 */
    if (u.cls == C_INF || v.cls == C_INF) return inf_of(es, sp);
    u128 pm = (u.cls == C_ZERO || v.cls == C_ZERO) ? 0 : (u128)u.m * v.m;
    /* 2 + p, or (3 + p) / 2 as 3*2^-1 + p*2^-1 */
    xval s = add_exact(sp, u.e + v.e - (rsqrt ? 1 : 0), pm, 0, rsqrt ? -1 : 1, rsqrt ? 3 : 1, (int)x->rmode);
    if (!s.m && !s.sticky) return zero_of(es, s.sign);
    return round_pack(x, es, s.sign, s.e, s.m, s.sticky);
}
uint64_t fp_recps(fpctx *x, int es, uint64_t a, uint64_t b) { return step(x, es, a, b, false); }
uint64_t fp_rsqrts(fpctx *x, int es, uint64_t a, uint64_t b) { return step(x, es, a, b, true); }

/* FPRecipEstimate / FPRSqrtEstimate (Arm ARM shared/functions/float) */
static unsigned recip_estimate(unsigned a)            /* a: 9 bits, 256..511 */
{
    a = a * 2 + 1;
    unsigned b = (1u << 19) / a;
    return (b + 1) / 2;                                   /* 256..511 */
}
static unsigned rsqrt_estimate(unsigned a)            /* a: 9 bits, 128..511 */
{
    if (a < 256) a = a * 2 + 1; else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    unsigned b = 512;
    while (a * (b + 1) * (b + 1) < (1u << 28)) b++;
    return (b + 1) / 2;
}
uint64_t fp_recpe(fpctx *x, int es, uint64_t a)
{
    fmt f = F(es);
    if (fp_isnan(a, es)) return nan_result(x, es, a);
    fpu u = unpack(x, es, a);
    if (u.cls == C_INF) return zero_of(es, u.sign);
    if (u.cls == C_ZERO) { fp_raise(x, FPX_DZC); return inf_of(es, u.sign); }
    uint64_t frac = a & ((1ull << f.mbits) - 1);
    int exp = (int)((a >> f.mbits) & ((1u << f.ebits) - 1));
    /* tiny inputs overflow */
    if (exp == 0 && !(frac >> (f.mbits - 2))) {
        fp_raise(x, FPX_OFC | FPX_IXC);
        bool to_inf = x->rmode == FPR_N || (x->rmode == FPR_P && !u.sign) || (x->rmode == FPR_M && u.sign);
        if (to_inf) return inf_of(es, u.sign);
        return (u.sign ? signbit_of(es) : 0) | (((1ull << f.ebits) - 2) << f.mbits) | ((1ull << f.mbits) - 1);
    }
    if (x->fz && es != 2 && exp >= 2 * f.bias - 1) {      /* result would be denormal: flushed */
        fp_raise(x, FPX_UFC);
        return zero_of(es, u.sign);
    }
    uint64_t fr = frac << (52 - f.mbits);                 /* as a 52-bit fraction */
    if (exp == 0) {
        if (!((fr >> 51) & 1)) { exp = -1; fr = (fr << 2) & ((1ull << 52) - 1); }
        else fr = (fr << 1) & ((1ull << 52) - 1);
    }
    unsigned sc = (unsigned)(1u << 8 | (unsigned)(fr >> 44));
    int rexp = 2 * f.bias - 1 - exp;
    unsigned est = recip_estimate(sc);
    uint64_t rf = (uint64_t)(est & 0xff) << 44;
    if (rexp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ull << 50) | (rf >> 2); rexp = 0; }
    return (u.sign ? signbit_of(es) : 0) | (uint64_t)rexp << f.mbits | (rf >> (52 - f.mbits));
}
uint64_t fp_rsqrte(fpctx *x, int es, uint64_t a)
{
    fmt f = F(es);
    if (fp_isnan(a, es)) return nan_result(x, es, a);
    fpu u = unpack(x, es, a);
    if (u.cls == C_ZERO) { fp_raise(x, FPX_DZC); return inf_of(es, u.sign); }
    if (u.sign) return invalid(x, es);
    if (u.cls == C_INF) return zero_of(es, 0);
    uint64_t frac = a & ((1ull << f.mbits) - 1);
    int exp = (int)((a >> f.mbits) & ((1u << f.ebits) - 1));
    uint64_t fr = frac << (52 - f.mbits);
    if (exp == 0) {
        while (!((fr >> 51) & 1)) { fr <<= 1; exp--; }
        fr = (fr << 1) & ((1ull << 52) - 1);
    }
    unsigned sc = (exp & 1) == 0 ? (unsigned)(1u << 8 | (unsigned)(fr >> 44)) : (unsigned)(1u << 7 | (unsigned)(fr >> 45));
    int rexp = (3 * f.bias - 1 - exp) / 2;
    if ((3 * f.bias - 1 - exp) < 0) rexp = -((1 - (3 * f.bias - 1 - exp)) / 2);
    unsigned est = rsqrt_estimate(sc);
    return (uint64_t)rexp << f.mbits | ((uint64_t)(est & 0xff) << (f.mbits - 8));
}

/* ------------------------------------------------------------ comparisons */

static int cmp3(fpctx *x, int es, uint64_t a, uint64_t b)  /* -1, 0, 1; NaNs excluded by caller */
{
    /* Without NaNs the integer keys order as the values do. A denormal needs
     * the unpacking path only when FZ flushes it (and raises IDC). */
    if (es != 2 && (!x->fz || (ordinary(a, es) && ordinary(b, es)))) {
        int64_t ka = order_key(a, es), kb = order_key(b, es);
        return ka > kb ? 1 : ka < kb ? -1 : 0;
    }
    fpu u = unpack(x, es, a), v = unpack(x, es, b);
    if (u.cls == C_ZERO && v.cls == C_ZERO) return 0;
    double du = u.cls == C_INF ? 2 : u.cls == C_ZERO ? 0 : 1, dv = v.cls == C_INF ? 2 : v.cls == C_ZERO ? 0 : 1;
    if (u.sign) du = -du;
    if (v.sign) dv = -dv;
    if (du != dv || u.cls != C_NUM) return du > dv ? 1 : du < dv ? -1 : 0;
    int lu = msb128(u.m) + u.e, lv = msb128(v.m) + v.e, c;
    if (lu != lv) c = lu > lv ? 1 : -1;
    else {
        int k = u.e < v.e ? u.e : v.e;
        u128 mu = (u128)u.m << (u.e - k), mv = (u128)v.m << (v.e - k);
        c = mu > mv ? 1 : mu < mv ? -1 : 0;
    }
    return u.sign ? -c : c;
}
/* Comparisons also unpack both operands before their NaN early return.
 * Preserve flushed-input IDC while retaining quiet versus signalling IOC. */
static bool compare_nan(fpctx *x, int es, uint64_t a, uint64_t b, bool signal)
{
    if (!fp_isnan(a, es) && !fp_isnan(b, es)) return false;
    uint64_t ops[2] = {a, b};
    nan_input_flags(x, es, 2, ops);
    if (signal || is_snan(a, es) || is_snan(b, es)) fp_raise(x, FPX_IOC);
    return true;
}
unsigned fp_cmp(fpctx *x, int es, uint64_t a, uint64_t b, bool signal)
{
    if (compare_nan(x, es, a, b, signal)) return 0x3;
    int c = cmp3(x, es, a, b);
    return c == 0 ? 0x6 : c < 0 ? 0x8 : 0x2;
}
bool fp_cmp_eq(fpctx *x, int es, uint64_t a, uint64_t b)
{
    if (compare_nan(x, es, a, b, false)) return false;
    return cmp3(x, es, a, b) == 0;
}
bool fp_cmp_ge(fpctx *x, int es, uint64_t a, uint64_t b)
{
    if (compare_nan(x, es, a, b, true)) return false;
    return cmp3(x, es, a, b) >= 0;
}
bool fp_cmp_gt(fpctx *x, int es, uint64_t a, uint64_t b)
{
    if (compare_nan(x, es, a, b, true)) return false;
    return cmp3(x, es, a, b) > 0;
}

/* ------------------------------------------------------------ conversions */

/* FPConvertNaN: keep sign and the top fraction bits, set the quiet bit */
static uint64_t convert_nan(fpctx *x, uint64_t a, int from, int to)
{
    if (is_snan(a, from)) fp_raise(x, FPX_IOC);
    if (x->dn) return fp_default_nan(to);
    fmt f = F(from), t = F(to);
    int sign = (int)((a >> (from * 8 - 1)) & 1);
    uint64_t frac = a & ((1ull << f.mbits) - 1);
    frac = t.mbits >= f.mbits ? frac << (t.mbits - f.mbits) : frac >> (f.mbits - t.mbits);
    return (sign ? signbit_of(to) : 0) | ((1ull << t.ebits) - 1) << t.mbits | frac | 1ull << (t.mbits - 1);
}
uint64_t fp_cvt(fpctx *x, uint64_t a, int from, int to)
{
    if (fp_isnan(a, from)) return convert_nan(x, a, from, to);
    fpu u = unpack(x, from, a);
    if (u.cls == C_ZERO) return zero_of(to, u.sign);
    if (u.cls == C_INF) return inf_of(to, u.sign);
    return round_pack(x, to, u.sign, u.e, u.m, false);
}
uint64_t fp_cvt_odd(fpctx *x, uint64_t a)
{
    if (fp_isnan(a, 8)) return convert_nan(x, a, 8, 4);
    fpu u = unpack(x, 8, a);
    if (u.cls == C_ZERO) return zero_of(4, u.sign);
    if (u.cls == C_INF) return inf_of(4, u.sign);
    return round_pack_mode(x, 4, u.sign, u.e, u.m, false, RND_ODD);
}

/* Round m * 2^e to an integer in `mode` (ties to even/away), exactly. */
static u128 round_int(int sign, int e, uint64_t m, int mode, bool *inexact)
{
    if (e >= 0) { *inexact = false; return e > 60 ? (u128)0 - 1 : (u128)m << e; }
    if (-e > 64) { *inexact = m != 0; u128 q = 0;
        bool up = mode == FPR_P ? !sign && m : mode == FPR_M ? sign && m : false;
        return q + up; }
    uint64_t q = -e == 64 ? 0 : m >> -e;
    uint64_t rb = -e == 64 ? m : m & ((1ull << -e) - 1), half = 1ull << (-e - 1);
    *inexact = rb != 0;
    bool up;
    switch (mode) {
    case FPR_N: up = rb > half || (rb == half && (q & 1)); break;
    case FPR_A: up = rb >= half; break;
    case FPR_P: up = rb && !sign; break;
    case FPR_M: up = rb && sign; break;
    default: up = false; break;
    }
    return (u128)q + up;
}
/* FPRoundInt of a zero or normal single or double on its bits: clear the
 * fraction below the units place and step the magnitude by one unit when the
 * mode rounds away from zero. The carry of that step runs into the exponent
 * field, which is the encoding of the next power of two. Only FRINTX (exact)
 * reports an inexact result. Denormals, infinities and NaNs decline. */
static bool rint_fast(fpctx *x, int es, uint64_t a, int mode, bool exact, uint64_t *r)
{
    if (es == 2) return false;
    fmt f = F(es);
    a &= emask_of(es);
    uint64_t mag = mag_of(a, es);
    if (!mag) { *r = a; return true; }
    if (mag < min_normal_mag(es) || mag >= inf_mag(es)) return false;
    int e = (int)(mag >> f.mbits) - f.bias;
    if (e >= (int)f.mbits) { *r = a; return true; }           /* already integral */
    bool sign = a != mag, up;
    if (e < 0) {                                               /* 0 < |a| < 1: +-0 or +-1 */
        switch (mode) {
        case FPR_N: up = e == -1 && (mag & ((1ull << f.mbits) - 1)); break;   /* above one half */
        case FPR_A: up = e == -1; break;
        case FPR_P: up = !sign; break;
        case FPR_M: up = sign; break;
        default: up = false; break;
        }
        if (exact) fp_raise(x, FPX_IXC);
        *r = (a ^ mag) | (up ? (uint64_t)f.bias << f.mbits : 0);
        return true;
    }
    int fb = (int)f.mbits - e;                                 /* fraction bits, 1..mbits */
    uint64_t unit = 1ull << fb, rb = mag & (unit - 1), half = unit >> 1;
    if (!rb) { *r = a; return true; }
    bool odd = (((mag & ((1ull << f.mbits) - 1)) | 1ull << f.mbits) >> fb) & 1;
    switch (mode) {
    case FPR_N: up = rb > half || (rb == half && odd); break;
    case FPR_A: up = rb >= half; break;
    case FPR_P: up = !sign; break;
    case FPR_M: up = sign; break;
    default: up = false; break;
    }
    if (exact) fp_raise(x, FPX_IXC);
    *r = (a ^ mag) | ((mag - rb) + (up ? unit : 0));
    return true;
}

/* FPToFixed of a normal single or double whose rounded magnitude fits one
 * word and the destination: round_int's rounding on 64 bits. Every other
 * input (zero, denormal, infinity, NaN, out of range) takes the reference
 * path, which also owns IOC, IDC and saturation. */
static bool to_int_fast(fpctx *x, int es, uint64_t a, int mode, bool uns, int bits, int fbits, uint64_t *r)
{
    if (es == 2) return false;
    fmt f = F(es);
    uint64_t mag = mag_of(a, es);
    if (mag < min_normal_mag(es) || mag >= inf_mag(es)) return false;
    bool sign = (a >> (es * 8 - 1)) & 1, inexact = false;
    uint64_t m = (mag & ((1ull << f.mbits) - 1)) | 1ull << f.mbits, n;
    int e = (int)(mag >> f.mbits) - f.bias - (int)f.mbits + fbits;
    if (e >= 0) {
        if (e > 63 - (int)f.mbits) return false;               /* m << e would leave the word */
        n = m << e;
    } else {
        bool up;
        inexact = true;
        if (e <= -64) {                                        /* far below one half: only directed modes step */
            n = 0;
            up = mode == FPR_P ? !sign : mode == FPR_M ? sign : false;
        } else {
            uint64_t rb = m & ((1ull << -e) - 1), half = 1ull << (-e - 1);
            n = m >> -e;
            inexact = rb != 0;
            switch (mode) {
            case FPR_N: up = rb > half || (rb == half && (n & 1)); break;
            case FPR_A: up = rb >= half; break;
            case FPR_P: up = rb && !sign; break;
            case FPR_M: up = rb && sign; break;
            default: up = false; break;
            }
        }
        n += up;
    }
    uint64_t minmag = 1ull << (bits - 1), maxv = uns ? (minmag - 1) * 2 + 1 : minmag - 1;
    if (!sign ? n > maxv : uns ? n != 0 : n > minmag) return false;
    if (inexact) fp_raise(x, FPX_IXC);
    *r = sign ? 0 - n : n;
    return true;
}

uint64_t fp_rint(fpctx *x, int es, uint64_t a, int mode, bool exact)
{
    uint64_t fast;
    if (rint_fast(x, es, a, mode, exact, &fast)) return fast;
    if (fp_isnan(a, es)) return nan_result(x, es, a);
    fpu u = unpack(x, es, a);
    if (u.cls == C_ZERO) return zero_of(es, u.sign);
    if (u.cls == C_INF) return inf_of(es, u.sign);
    if (u.e >= 0) return repack(x, es, u);
    bool inex;
    u128 n = round_int(u.sign, u.e, u.m, mode, &inex);
    if (inex && exact) fp_raise(x, FPX_IXC);
    if (!n) return zero_of(es, u.sign);
    return round_pack(x, es, u.sign, 0, n, false);
}
uint64_t fp_to_int(fpctx *x, int es, uint64_t a, int mode, bool uns, int bits, int fbits)
{
    uint64_t fast;
    if (to_int_fast(x, es, a, mode, uns, bits, fbits, &fast)) return fast;
    if (fp_isnan(a, es)) { fp_raise(x, FPX_IOC); return 0; }
    fpu u = unpack(x, es, a);
    u128 maxv = uns ? ((u128)1 << bits) - 1 : ((u128)1 << (bits - 1)) - 1, minmag = uns ? 0 : (u128)1 << (bits - 1);
    uint64_t lim_hi = (uint64_t)maxv, lim_lo = uns ? 0 : (uint64_t)(0 - (uint64_t)minmag);
    if (u.cls == C_ZERO) return 0;
    if (u.cls == C_INF) { fp_raise(x, FPX_IOC); return u.sign ? lim_lo : lim_hi; }
    bool inex;
    u128 n = round_int(u.sign, u.e + fbits, u.m, mode, &inex);
    if (!u.sign) {
        if (n > maxv) { fp_raise(x, FPX_IOC); return lim_hi; }
    } else {
        if (uns ? n != 0 : n > minmag) { fp_raise(x, FPX_IOC); return lim_lo; }
    }
    if (inex) fp_raise(x, FPX_IXC);
    uint64_t r = (uint64_t)n;
    return u.sign ? 0 - r : r;
}
uint64_t fp_from_int(fpctx *x, int es, uint64_t v, bool sgn, int bits, int fbits)
{
    if (bits < 64) v &= (1ull << bits) - 1;
    int sign = 0;
    if (sgn && bits < 64 && ((v >> (bits - 1)) & 1)) v |= ~0ull << bits;
    if (sgn && (int64_t)v < 0) { sign = 1; v = 0 - v; }
    if (!v) return 0;
    /* An integer that fits int64: the host conversion rounds once in the
     * live MXCSR mode and raises PE exactly when FPRound raises IXC. The
     * result is a normal number, so FZ and the other flags do not arise. */
    if (es != 2 && !fbits && v <= (uint64_t)INT64_MAX) {
        int64_t s = sign ? -(int64_t)v : (int64_t)v;
        if (es == 4) {
            __m128 z = _mm_setzero_ps();
            __asm__ volatile("cvtsi2ssq %1, %0" : "+x"(z) : "r"(s));
            return (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(z));
        }
        __m128d z = _mm_setzero_pd();
        __asm__ volatile("cvtsi2sdq %1, %0" : "+x"(z) : "r"(s));
        return (uint64_t)_mm_cvtsi128_si64(_mm_castpd_si128(z));
    }
    return round_pack(x, es, sign, -fbits, v, false);
}
uint32_t fp_jcvt(fpctx *x, uint64_t a, bool *exact)
{
    *exact = false;
    if (fp_isnan(a, 8)) { fp_raise(x, FPX_IOC); return 0; }
    fpu u = unpack(x, 8, a);
    bool denormal = !((a >> 52) & 0x7ff) && (a & ((1ull << 52) - 1));
    if (u.cls == C_ZERO) { *exact = !u.sign && !denormal; return 0; }   /* a flushed denormal was not an integer */
    if (u.cls == C_INF) { fp_raise(x, FPX_IOC); return 0; }
    uint64_t n;
    bool frac = false;
    if (u.e >= 64) n = 0;
    else if (u.e >= 0) n = u.m << u.e;
    else if (-u.e >= 64) { n = 0; frac = true; }
    else { n = u.m >> -u.e; frac = (u.m & ((1ull << -u.e) - 1)) != 0; }
    /* FPToFixedJS judges the range on the integer truncated toward zero, so
     * -2^31 - 0.5 is in range (and inexact). */
    bool big = msb128(u.m) + u.e >= 64 || (u.sign ? n > 0x80000000ull : n > 0x7fffffffull);
    uint32_t r = (uint32_t)(u.sign ? 0 - n : n);
    if (big) fp_raise(x, FPX_IOC);
    else if (frac) fp_raise(x, FPX_IXC);
    *exact = !big && !frac;
    return r;
}

/* FPRecpX: exponent complemented, fraction zeroed */
uint64_t fp_recpx(fpctx *x, int es, uint64_t a)
{
    if (fp_isnan(a, es)) return nan_result(x, es, a);
    fmt f = F(es);
    uint64_t emax = (1ull << f.ebits) - 1, e = (a >> f.mbits) & emax, sign = a & signbit_of(es);
    if (e == 0 || (x->fz && es != 2 && e == 0)) return sign | (emax - 1) << f.mbits;
    return sign | ((~e & emax) << f.mbits);
}
