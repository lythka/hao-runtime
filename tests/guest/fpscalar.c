/* Scalar floating point, conversions and round-to-integral against native
 * arm64: every instruction below runs over a table of corner values (all
 * pairs of them for two-operand forms) and seeded random values, under each
 * FPCR rounding mode with FZ and DN off and on, with FPSR clear or full
 * beforehand. The result register(s), FPSR (cumulative flags, IDC, QC) and
 * NZCV are folded into one hash per instruction and FPCR setting. The output
 * must equal a native arm64 run's (tests/guest/fpscalar.sh).
 *
 *   fpscalar                 every instruction, one hash line per FPCR setting
 *   fpscalar NAME [CFG]      one instruction: every case on its own line
 *   fpscalar -b NAME COUNT   time a loop of 8 x COUNT executions of one instruction
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NOINL __attribute__((noinline))

/* in:  q0 q1 q2 (48 bytes), x10, nzcv
 * out: q0 (16 bytes), x11, fpsr, nzcv */
typedef struct { uint64_t v[6], x, nzcv; } fin;
typedef struct { uint64_t v[2], x, fpsr, nzcv; } fout;
typedef void opfn(const fin *in, fout *out, uint64_t fpcr, uint64_t fpsr);
typedef uint64_t benchfn(const fin *in, uint64_t count);

#define OP(name, insn)                                                                     \
    NOINL static void op_##name(const fin *in, fout *out, uint64_t fpcr, uint64_t fpsr)    \
    {                                                                                      \
        __asm__ volatile(                                                                  \
            "ldp q0, q1, [%[in]]\n ldr q2, [%[in], #32]\n"                                 \
            "ldp x10, x12, [%[in], #48]\n mov x11, #0x5a5a\n"                              \
            "mrs x9, fpcr\n msr fpcr, %[cr]\n msr fpsr, %[sr]\n msr nzcv, x12\n"           \
            insn "\n"                                                                      \
            "mrs x12, fpsr\n mrs x13, nzcv\n msr fpcr, x9\n"                               \
            "str q0, [%[out]]\n stp x11, x12, [%[out], #16]\n str x13, [%[out], #32]\n"    \
            : : [in] "r"(in), [out] "r"(out), [cr] "r"(fpcr), [sr] "r"(fpsr)               \
            : "x9", "x10", "x11", "x12", "x13", "v0", "v1", "v2", "cc", "memory");         \
    }                                                                                      \
    NOINL static uint64_t bench_##name(const fin *in, uint64_t count)                      \
    {                                                                                      \
        uint64_t r;                                                                        \
        __asm__ volatile(                                                                  \
            "ldp q0, q1, [%[in]]\n ldr q2, [%[in], #32]\n"                                 \
            "ldr x10, [%[in], #48]\n mov x11, #0\n"                                        \
            "1:\n" insn "\n" insn "\n" insn "\n" insn "\n"                                  \
            insn "\n" insn "\n" insn "\n" insn "\n"                                         \
            "subs %[n], %[n], #1\n b.ne 1b\n"                                              \
            "fmov x9, d0\n add %[r], x9, x11\n"                                            \
            : [r] "=&r"(r), [n] "+r"(count) : [in] "r"(in)                                 \
            : "x9", "x10", "x11", "v0", "v1", "v2", "cc", "memory");                       \
        return r;                                                                          \
    }

/* element kinds of the vector inputs */
enum { ES, ED, EH, EI32, EI64,
       EST,     /* singles, mostly ordinary values in the conversion range: whole vectors the inline paths accept */
       EIT };   /* 32-bit integers, mostly small and non-negative */

#define OPS(X)                                                                 \
    /* translated inline: FCVTZS/FCVTZU, UCVTF/SCVTF, FMOV #imm, max/min */    \
    X(fcvtzs_ws, "fcvtzs w11, s0", ES)   X(fcvtzs_xs, "fcvtzs x11, s0", ES)    \
    X(fcvtzs_wd, "fcvtzs w11, d0", ED)   X(fcvtzs_xd, "fcvtzs x11, d0", ED)    \
    X(fcvtzu_ws, "fcvtzu w11, s0", ES)   X(fcvtzu_xs, "fcvtzu x11, s0", ES)    \
    X(fcvtzu_wd, "fcvtzu w11, d0", ED)   X(fcvtzu_xd, "fcvtzu x11, d0", ED)    \
    X(ucvtf_sw, "ucvtf s0, w10", ES)     X(ucvtf_sx, "ucvtf s0, x10", ES)      \
    X(ucvtf_dw, "ucvtf d0, w10", ED)     X(ucvtf_dx, "ucvtf d0, x10", ED)      \
    X(scvtf_sw, "scvtf s0, w10", ES)     X(scvtf_sx, "scvtf s0, x10", ES)      \
    X(scvtf_dw, "scvtf d0, w10", ED)     X(scvtf_dx, "scvtf d0, x10", ED)      \
    X(ucvtf_szr, "ucvtf s0, wzr", ES)    X(fcvtzs_zrs, "fcvtzs wzr, s0", ES)   \
    X(fmov_s1, "fmov s0, #1.0", ES)      X(fmov_sm, "fmov s0, #-0.4375", ES)   \
    X(fmov_d31, "fmov d0, #31.0", ED)    X(fmov_dm, "fmov d0, #-0.125", ED)    \
    X(fmov_s2, "fmov s0, #2.0", ES)      X(fmov_d19, "fmov d0, #1.9375", ED)   \
    X(fmax_s, "fmax s0, s0, s1", ES)     X(fmin_s, "fmin s0, s0, s1", ES)      \
    X(fmaxnm_s, "fmaxnm s0, s0, s1", ES) X(fminnm_s, "fminnm s0, s0, s1", ES)  \
    X(fmax_d, "fmax d0, d0, d1", ED)     X(fmin_d, "fmin d0, d0, d1", ED)      \
    X(fmaxnm_d, "fmaxnm d0, d0, d1", ED) X(fminnm_d, "fminnm d0, d0, d1", ED)  \
    X(fmax_s21, "fmax s0, s2, s1", ES)   X(fminnm_d00, "fminnm d0, d0, d0", ED)\
    /* the other rounding modes and fixed point (helper) */                    \
    X(fcvtns_ws, "fcvtns w11, s0", ES)   X(fcvtnu_xd, "fcvtnu x11, d0", ED)    \
    X(fcvtas_xs, "fcvtas x11, s0", ES)   X(fcvtau_wd, "fcvtau w11, d0", ED)    \
    X(fcvtps_wd, "fcvtps w11, d0", ED)   X(fcvtpu_xs, "fcvtpu x11, s0", ES)    \
    X(fcvtms_xd, "fcvtms x11, d0", ED)   X(fcvtmu_ws, "fcvtmu w11, s0", ES)    \
    X(fcvtzs_fx_ws, "fcvtzs w11, s0, #7", ES)  X(fcvtzu_fx_xd, "fcvtzu x11, d0, #40", ED) \
    X(scvtf_fx_sw, "scvtf s0, w10, #9", ES)    X(ucvtf_fx_dx, "ucvtf d0, x10, #33", ED)   \
    X(fjcvtzs, "fjcvtzs w11, d0", ED)                                          \
    /* round to integral */                                                    \
    X(frintn_s, "frintn s0, s0", ES)     X(frintn_d, "frintn d0, d0", ED)      \
    X(frintp_s, "frintp s0, s0", ES)     X(frintp_d, "frintp d0, d0", ED)      \
    X(frintm_s, "frintm s0, s0", ES)     X(frintm_d, "frintm d0, d0", ED)      \
    X(frintz_s, "frintz s0, s0", ES)     X(frintz_d, "frintz d0, d0", ED)      \
    X(frinta_s, "frinta s0, s0", ES)     X(frinta_d, "frinta d0, d0", ED)      \
    X(frintx_s, "frintx s0, s0", ES)     X(frintx_d, "frintx d0, d0", ED)      \
    X(frinti_s, "frinti s0, s0", ES)     X(frinti_d, "frinti d0, d0", ED)      \
    X(frintn_4s, "frintn v0.4s, v0.4s", ES)  X(frintn_2d, "frintn v0.2d, v0.2d", ED) \
    X(frintp_4s, "frintp v0.4s, v0.4s", ES)  X(frintm_2d, "frintm v0.2d, v0.2d", ED) \
    X(frintz_4s, "frintz v0.4s, v0.4s", ES)  X(frinta_2d, "frinta v0.2d, v0.2d", ED) \
    X(frintx_4s, "frintx v0.4s, v0.4s", ES)  X(frinti_2d, "frinti v0.2d, v0.2d", ED) \
    X(frinta_2s, "frinta v0.2s, v0.2s", ES)  X(frintm_4s, "frintm v0.4s, v0.4s", ES) \
    /* vector and SIMD-scalar conversions */                                   \
    X(scvtf_4s, "scvtf v0.4s, v0.4s", EI32)  X(ucvtf_4s, "ucvtf v0.4s, v0.4s", EI32) \
    X(scvtf_2d, "scvtf v0.2d, v0.2d", EI64)  X(ucvtf_2d, "ucvtf v0.2d, v0.2d", EI64) \
    X(scvtf_2s, "scvtf v0.2s, v0.2s", EI32)  X(ucvtf_ss, "ucvtf s0, s0", EI32)        \
    X(scvtf_dd, "scvtf d0, d0", EI64)        X(scvtf_fx_4s, "scvtf v0.4s, v0.4s, #5", EI32) \
    X(fcvtzs_4s, "fcvtzs v0.4s, v0.4s", ES)  X(fcvtzu_4s, "fcvtzu v0.4s, v0.4s", ES)  \
    X(fcvtzs_2d, "fcvtzs v0.2d, v0.2d", ED)  X(fcvtzu_2d, "fcvtzu v0.2d, v0.2d", ED)  \
    X(fcvtns_4s, "fcvtns v0.4s, v0.4s", ES)  X(fcvtau_2d, "fcvtau v0.2d, v0.2d", ED)  \
    X(fcvtps_4s, "fcvtps v0.4s, v0.4s", ES)  X(fcvtmu_2d, "fcvtmu v0.2d, v0.2d", ED)  \
    X(fcvtzs_ss, "fcvtzs s0, s0", ES)        X(fcvtzs_fx_4s, "fcvtzs v0.4s, v0.4s, #3", ES) \
    /* packed single forms translated inline, on mostly ordinary vectors too */ \
    X(scvtf_4s_t, "scvtf v0.4s, v0.4s", EIT)   X(ucvtf_4s_t, "ucvtf v0.4s, v0.4s", EIT)   \
    X(scvtf_2s_t, "scvtf v0.2s, v0.2s", EIT)   X(ucvtf_2s_t, "ucvtf v0.2s, v0.2s", EIT)   \
    X(ucvtf_2s, "ucvtf v0.2s, v0.2s", EI32)    X(fcvtzs_2s, "fcvtzs v0.2s, v0.2s", ES)    \
    X(fcvtzs_4s_t, "fcvtzs v0.4s, v0.4s", EST) X(fcvtzs_2s_t, "fcvtzs v0.2s, v0.2s", EST) \
    X(fcmgt0_4s_t, "fcmgt v0.4s, v0.4s, #0.0", EST) X(fcmgt0_2s_t, "fcmgt v0.2s, v0.2s, #0.0", EST) \
    X(fcmgt0_2s, "fcmgt v0.2s, v0.2s, #0.0", ES)    X(fcvtzs_4s_21, "fcvtzs v0.4s, v2.4s", EST)     \
    X(frintn_4s_t, "frintn v0.4s, v0.4s", EST)      X(frintx_4s_t, "frintx v0.4s, v0.4s", EST)      \
    X(fcvtzu_4s_t, "fcvtzu v0.4s, v0.4s", EST)      X(fcvtas_4s_t, "fcvtas v0.4s, v0.4s", EST)      \
    /* comparisons that reach the helper */                                    \
    X(fcmgt0_4s, "fcmgt v0.4s, v0.4s, #0.0", ES)  X(fcmge0_2d, "fcmge v0.2d, v0.2d, #0.0", ED) \
    X(fcmeq0_4s, "fcmeq v0.4s, v0.4s, #0.0", ES)  X(fcmlt0_2d, "fcmlt v0.2d, v0.2d, #0.0", ED) \
    X(fcmle0_4s, "fcmle v0.4s, v0.4s, #0.0", ES)  X(fcmgt_ss, "fcmgt s0, s0, s1", ES)          \
    X(fcmge_dd, "fcmge d0, d0, d1", ED)           X(fcmeq_ss, "fcmeq s0, s0, s1", ES)          \
    X(fcmgt_2d, "fcmgt v0.2d, v0.2d, v1.2d", ED)  X(facgt_4s, "facgt v0.4s, v0.4s, v1.4s", ES) \
    X(fccmp_s, "fccmp s0, s1, #4, ne", ES)        X(fccmpe_d, "fccmpe d0, d1, #9, ne", ED)     \
    X(fcmp_s, "fcmp s0, s1", ES)                  X(fcmpe_d0, "fcmpe d0, #0.0", ED)            \
    /* max/min and pairwise forms through the helper */                        \
    X(fmaxnm_2d, "fmaxnm v0.2d, v0.2d, v1.2d", ED)  X(fmin_2d, "fmin v0.2d, v0.2d, v1.2d", ED) \
    X(fmaxp_4s, "fmaxp v0.4s, v0.4s, v1.4s", ES)    X(fminnmv_4s, "fminnmv s0, v0.4s", ES)     \
    X(fmaxv_4s, "fmaxv s0, v0.4s", ES)              X(faddp_2s, "faddp v0.2s, v0.2s, v1.2s", ES) \
    X(faddp_d, "faddp d0, v0.2d", ED)               X(fabd_s, "fabd s0, s0, s1", ES)           \
    /* NaN selection with two and three operands; arithmetic already inline */ \
    X(fadd_d, "fadd d0, d0, d1", ED) \
    X(fsub_s, "fsub s0, s0, s1", ES) \
    X(fmul_d, "fmul d0, d0, d1", ED) \
    X(fdiv_s, "fdiv s0, s0, s1", ES) \
    X(fsqrt_d, "fsqrt d0, d0", ED) \
    X(fcmp_d, "fcmp d0, d1", ED) \
    X(fcmpe_s, "fcmpe s0, s1", ES) \
    X(fcmp_s0, "fcmp s0, #0.0", ES) \
    X(fcmp_d0, "fcmp d0, #0.0", ED) \
    X(fcmpe_d, "fcmpe d0, d1", ED) \
    X(fcmpe_s0, "fcmpe s0, #0.0", ES) \
    X(fadd_s, "fadd s0, s0, s1", ES)     X(fsub_d, "fsub d0, d0, d1", ED)      \
    X(fmul_s, "fmul s0, s0, s1", ES)     X(fdiv_d, "fdiv d0, d0, d1", ED)      \
    X(fnmul_s, "fnmul s0, s0, s1", ES)   X(fdiv_4s, "fdiv v0.4s, v0.4s, v1.4s", ES) \
    X(fmadd_s, "fmadd s0, s0, s1, s2", ES)   X(fnmsub_d, "fnmsub d0, d0, d1, d2", ED) \
    X(fmulx_s, "fmulx s0, s0, s1", ES)       X(frecps_d, "frecps d0, d0, d1", ED)     \
    X(fsqrt_s, "fsqrt s0, s0", ES)           X(fcvt_ds, "fcvt d0, s0", ES)            \
    X(fcvt_sd, "fcvt s0, d0", ED)                                              \
    /* half precision keeps the reference path */                              \
    X(fadd_h, "fadd h0, h0, h1", EH)     X(fmaxnm_h, "fmaxnm h0, h0, h1", EH)  \
    X(fcvtzs_wh, "fcvtzs w11, h0", EH)   X(frintn_h, "frintn h0, h0", EH)      \
    X(fcmp_h, "fcmp h0, h1", EH)         X(scvtf_hw, "scvtf h0, w10", EH)      \
    X(fmadd_h, "fmadd h0, h0, h1, h2", EH)

#define X(name, insn, kind) OP(name, insn)
OPS(X)
#undef X

static const struct { const char *name; opfn *fn; benchfn *bench; int kind; } ops[] = {
#define X(name, insn, kind) {#name, op_##name, bench_##name, kind},
    OPS(X)
#undef X
};
#define NOPS (sizeof ops / sizeof *ops)

/* ------------------------------------------------------------- values */

static const uint32_t s32[] = {
    0x0c7fffff, 0x0c800000, 0x0c800001, 0x8c800001,
    0x267fffff, 0x26800000, 0x26800001, 0x58800000, 0x58800001,
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x80800001, 0x7f7fffff, 0xff7fffff,   /* zeros, denormals, min/max normal */
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc12345, 0x7f800001, 0xffa00000, 0x7fbfffff,   /* inf, quiet and signalling NaNs */
    0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x3effffff, 0xbf000001, 0x3fc00000, 0xc0200000,   /* 1, .5, just below/above .5, 1.5, -2.5 */
    0x40600000, 0x4afffffe, 0xcb000001, 0x4b7fffff, 0x4b800000,                                       /* 3.5, 2^22.., 2^23+1, 2^24-1, 2^24 */
    0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001, 0x4f7fffff, 0x4f800000,                           /* around 2^31 and 2^32 */
    0x5effffff, 0x5f000000, 0xdf000000, 0xdf000001, 0x5f7fffff, 0x5f800000,                           /* around 2^63 and 2^64 */
    0x3e99999a, 0xbf7fffff, 0x33800000, 0x1e3ce508,                                                   /* .3, -(1-ulp), 2^-24, tiny */
};
static const uint64_t s64[] = {
    0x035fffffffffffff, 0x0360000000000000, 0x0360000000000001, 0x8360000000000001,
    0x20afffffffffffff, 0x20b0000000000000, 0x20b0000000000001, 0x5f30000000000000, 0x5f30000000000001,
    0x380fffffffffffff, 0x3810000000000000, 0x3810000000000001,
    0x0000000000000000, 0x8000000000000000, 0x0000000000000001, 0x800fffffffffffff, 0x0010000000000000, 0x8010000000000001,
    0x7fefffffffffffff, 0xffefffffffffffff, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000,
    0x7ff8000012345678, 0x7ff0000000000001, 0xfff4000000000000, 0x7ff7ffffffffffff,
    0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000, 0xbfe0000000000000, 0x3fdfffffffffffff, 0xbfe0000000000001,
    0x3ff8000000000000, 0xc004000000000000, 0x400c000000000000, 0x432ffffffffffffe, 0xc330000000000001, 0x433fffffffffffff,
    0x41dfffffffc00000, 0x41dfffffffe00000, 0x41e0000000000000, 0xc1e0000000000000, 0xc1e0000000100000, 0xc1e0000000200000,   /* 2^31-1, 2^31-.5, 2^31, -2^31, -2^31-.5, -2^31-1 */
    0x41efffffffe00000, 0x41effffffff00000, 0x41f0000000000000, 0x41f0000000080000,                                           /* 2^32-1, 2^32-.5, 2^32, 2^32+.5 */
    0x43dfffffffffffff, 0x43e0000000000000, 0xc3e0000000000000, 0xc3e0000000000001, 0x43efffffffffffff, 0x43f0000000000000,
    0x3fd3333333333333, 0xbfefffffffffffff, 0x47efffffe0000000, 0x47effffff0000000, 0x36a0000000000000, 0x3690000000000000,   /* .3, .., FLT_MAX, just over, 2^-149, 2^-150 */
};
static const uint16_t s16[] = {
    0x0000, 0x8000, 0x0001, 0x83ff, 0x0400, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01, 0xfd00,
    0x3c00, 0xbc00, 0x3800, 0xb800, 0x3e00, 0xc100, 0x4200, 0x77ff, 0x7800, 0xf800, 0x37ff,
};
static const uint64_t si[] = {
    0, 1, 2, 3, 0xffffffffffffffffull, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0x100000000ull, 0xfffffe, 0xffffff,
    0x1000000, 0x1000001, 0x1000002, 0x1000003, 0x2000003, 0xffffff7f, 0xffffff80, 0xffffff81, 0xff000001, 0x80000080,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0x8000000000000001ull, 0x1fffffffffffffull, 0x20000000000000ull,
    0x20000000000001ull, 0x20000000000003ull, 0x7ffffffffffffc00ull, 0x7ffffffffffffdffull, 0xfffffffffffff7ffull,
    0xfffffffffffff800ull, 0xfffffffffffffbffull, 0xfffffffffffffc00ull, 0x8000000000000400ull, 0x80000000000007ffull,
    0xffffff0000000000ull, 0xffffff8000000000ull, 0x7fffff4000000000ull, 0x0123456789abcdefull, 0xfedcba9876543210ull,
};
#define N(a) ((int)(sizeof a / sizeof *a))


static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static uint32_t rnd_s(void)
{
    uint64_t r = rnd();
    switch (r & 7) {
    case 0: return s32[(r >> 8) % N(s32)];
    case 1: return (uint32_t)(r >> 16);                                         /* any bits */
    case 2: case 3:                                                             /* conversion range, fraction bits */
        return (uint32_t)(r >> 63) << 31 | (uint32_t)(125 + (r >> 8) % 70) << 23 | ((uint32_t)(r >> 20) & 0x7fffff);
    case 4: {                                                                   /* k/2 and k/4: ties */
        float f = (float)(int32_t)((r >> 8) & 0xffff) / (r & 0x100000000ull ? 2.0f : 4.0f);
        uint32_t u; memcpy(&u, &f, 4);
        return u | (uint32_t)(r >> 63) << 31;
    }
    case 5: return (uint32_t)(r >> 63) << 31 | (uint32_t)((r >> 8) % 3) << 23 | ((uint32_t)(r >> 20) & 0x7fffff);   /* denormal .. tiny */
    case 6: return (uint32_t)(r >> 63) << 31 | (uint32_t)(252 + (r >> 8) % 4) << 23 | ((uint32_t)(r >> 20) & 0x7fffff & -(uint32_t)((r >> 50) & 1)); /* huge, inf, NaN */
    default: return (uint32_t)(r >> 63) << 31 | (uint32_t)(100 + (r >> 8) % 60) << 23 | ((uint32_t)(r >> 20) & 0x7fffff);
    }
}
static uint64_t rnd_d(void)
{
    uint64_t r = rnd(), m = rnd() & 0xfffffffffffffull;
    switch (r & 7) {
    case 0: return s64[(r >> 8) % N(s64)];
    case 1: return rnd();
    case 2: case 3: return (r >> 63) << 63 | (1021 + (r >> 8) % 70) << 52 | (r & 0x10000 ? m : m & ~0xffffffffull);
    case 4: {
        double f = (double)(int64_t)((r >> 8) & 0xffffffffffull) / (r & 0x1000000000000ull ? 2.0 : 4.0);
        uint64_t u; memcpy(&u, &f, 8);
        return u | (r >> 63) << 63;
    }
    case 5: return (r >> 63) << 63 | ((r >> 8) % 3) << 52 | m;
    case 6: return (r >> 63) << 63 | (2044 + (r >> 8) % 4) << 52 | (m & -((r >> 50) & 1));
    default: return (r >> 63) << 63 | (870 + (r >> 8) % 300) << 52 | m;       /* float range and its edges */
    }
}
static uint64_t rnd_i(void);
static uint32_t rnd_st(void)
{
    uint64_t r = rnd();
    if (!(r & 15)) return rnd_s();
    if (r & 16) {                                                               /* k/4 */
        float f = (float)(int32_t)((r >> 8) & 0xfffff) / 4.0f;
        uint32_t u; memcpy(&u, &f, 4);
        return u | (uint32_t)(r >> 63) << 31;
    }
    if (!(r & 0xe0)) return (uint32_t)(r >> 63) << 31;                          /* a zero */
    return (uint32_t)(r >> 63) << 31 | (uint32_t)(110 + (r >> 8) % 47) << 23 | ((uint32_t)(r >> 20) & 0x7fffff);
}
static uint32_t rnd_it(void)
{
    uint64_t r = rnd();
    if (!(r & 15)) return (uint32_t)rnd_i();
    return (uint32_t)(rnd() >> 33) >> ((r >> 8) & 31);                          /* 0 .. 2^31-1, every magnitude */
}
static uint16_t rnd_h(void)
{
    uint64_t r = rnd();
    return r & 3 ? (uint16_t)(r >> 16) : s16[(r >> 8) % N(s16)];
}
static uint64_t rnd_i(void)
{
    uint64_t r = rnd();
    switch (r & 3) {
    case 0: return si[(r >> 8) % N(si)];
    case 1: return rnd();
    default: { uint64_t v = rnd() >> ((r >> 8) & 63); return r & 4 ? 0 - v : v; }   /* every magnitude */
    }
}

/* Case n of an instruction: the corner values first (each alone, then every
 * pair in lane 0 of the two operands), random afterwards. */
static void fill(fin *in, int kind, int n)
{
    for (int k = 0; k < 6; k++) {
        switch (kind) {
        case ES: in->v[k] = (uint64_t)rnd_s() << 32 | rnd_s(); break;
        case ED: in->v[k] = rnd_d(); break;
        case EH: in->v[k] = (uint64_t)rnd_h() << 48 | (uint64_t)rnd_h() << 32 | (uint64_t)rnd_h() << 16 | rnd_h(); break;
        case EI32: in->v[k] = rnd_i() << 32 | (rnd_i() & 0xffffffffu); break;
        case EST: in->v[k] = (uint64_t)rnd_st() << 32 | rnd_st(); break;
        case EIT: in->v[k] = (uint64_t)rnd_it() << 32 | rnd_it(); break;
        default: in->v[k] = rnd_i(); break;
        }
    }
    in->x = rnd_i();
    in->nzcv = (rnd() & 0xf) << 28;
    if (kind >= EST) return;
    int ns = kind == ES ? N(s32) : kind == ED ? N(s64) : kind == EH ? N(s16) : N(si);
    if (n < ns * ns) {
        int a = n / ns, b = n % ns;
        switch (kind) {
        case ES: in->v[0] = (in->v[0] & ~0xffffffffull) | s32[b]; in->v[2] = (in->v[2] & ~0xffffffffull) | s32[a]; break;
        case ED: in->v[0] = s64[b]; in->v[2] = s64[a]; break;
        case EH: in->v[0] = (in->v[0] & ~0xffffull) | s16[b]; in->v[2] = (in->v[2] & ~0xffffull) | s16[a]; break;
        case EI32: in->v[0] = (in->v[0] & ~0xffffffffull) | (si[b] & 0xffffffffu); break;
        default: in->v[0] = si[b]; break;
        }
        if (kind >= EI32 || kind == EH) in->x = si[b % N(si)];
        else in->x = si[(a + b) % N(si)];
    }
}

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v; h *= 1099511628211ull; return h ^ (h >> 29); }

/* FPCR: RMode (bits 23:22), FZ (24), DN (25) */
static uint64_t cfg_fpcr(int cfg) { return (uint64_t)(cfg & 3) << 22 | (uint64_t)((cfg >> 2) & 1) << 24 | (uint64_t)((cfg >> 3) & 1) << 25; }
#define NCFG 16
#define FPSR_ALL 0x0800009full

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    int random_cases = 500;
    if (argc >= 4 && !strcmp(argv[1], "-b")) {
        /* COUNT iterations of a loop holding the instruction eight times on
         * fixed ordinary operands (FPCR as the process has it). */
        uint64_t count = strtoull(argv[3], NULL, 0);
        for (size_t o = 0; o < NOPS; o++) {
            if (strcmp(ops[o].name, argv[2])) continue;
            fin in;
            static const float fs[4] = {3.25f, -117.75f, 0.4375f, 90210.5f};
            static const double ds[2] = {3.25, -117.75};
            memset(&in, 0, sizeof in);
            for (int k = 0; k < 3; k++) {
                if (ops[o].kind == ES) memcpy(&in.v[2 * k], fs + (k & 1) * 2, 8), memcpy(&in.v[2 * k + 1], fs, 8);
                else if (ops[o].kind == ED) memcpy(&in.v[2 * k], ds, 16);
                else in.v[2 * k] = 0x0000303900000007ull + (uint64_t)k, in.v[2 * k + 1] = 0x00000011000186a0ull;
            }
            in.x = 123456;
            uint64_t t0 = now_ns();
            uint64_t h = ops[o].bench(&in, count);
            uint64_t t1 = now_ns();
            printf("%s %.2f ns/insn (%llu x 8) %016llx\n", ops[o].name, (double)(t1 - t0) / (8.0 * (double)count),
                   (unsigned long long)count, (unsigned long long)h);
            return 0;
        }
        return 2;
    }
    const char *only = argc > 1 ? argv[1] : NULL;
    int only_cfg = argc > 2 ? atoi(argv[2]) : -1;
    for (size_t o = 0; o < NOPS; o++) {
        if (only && strcmp(only, ops[o].name)) continue;
        int kind = ops[o].kind;
        int ns = kind == ES ? N(s32) : kind == ED ? N(s64) : kind == EH ? N(s16) : N(si);
        int cases = ns * ns + random_cases;
        if (kind >= EI32) cases = ns + random_cases;
        if (kind >= EST) cases = 4 * random_cases;
        for (int cfg = 0; cfg < NCFG; cfg++) {
            if (only_cfg >= 0 && cfg != only_cfg) continue;
            uint64_t h = 1469598103934665603ull;
            rs = 0x9e3779b97f4a7c15ull ^ (o * 1000003u + (unsigned)cfg);
            for (int n = 0; n < cases; n++) {
                fin in;
                fout out;
                fill(&in, kind, n);
                uint64_t pre = n & 1 ? FPSR_ALL : n & 2 ? 0x10 : 0;
                ops[o].fn(&in, &out, cfg_fpcr(cfg), pre);
                if (only)
                    printf("%s %d %d: %016llx %016llx | %016llx %016llx | %016llx %016llx x=%016llx nzcv=%llx pre=%llx -> %016llx %016llx x=%016llx fpsr=%08llx nzcv=%llx\n",
                           ops[o].name, cfg, n,
                           (unsigned long long)in.v[1], (unsigned long long)in.v[0], (unsigned long long)in.v[3], (unsigned long long)in.v[2],
                           (unsigned long long)in.v[5], (unsigned long long)in.v[4], (unsigned long long)in.x,
                           (unsigned long long)(in.nzcv >> 28), (unsigned long long)pre,
                           (unsigned long long)out.v[1], (unsigned long long)out.v[0], (unsigned long long)out.x,
                           (unsigned long long)out.fpsr, (unsigned long long)(out.nzcv >> 28));
                h = mix(h, out.v[0]); h = mix(h, out.v[1]); h = mix(h, out.x); h = mix(h, out.fpsr); h = mix(h, out.nzcv);
            }
            if (!only) printf("%-14s fpcr=%08llx %016llx\n", ops[o].name, (unsigned long long)cfg_fpcr(cfg), (unsigned long long)h);
        }
    }
    return 0;
}
