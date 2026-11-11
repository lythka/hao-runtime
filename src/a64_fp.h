/*
 * hao — A64 floating-point semantics shared by the interpreter (a64_simd.c)
 * and, through it, the JIT's helper path. Values are raw bit patterns of
 * esize 2 (half), 4 (single) or 8 (double) bytes. Every operation follows
 * the Arm ARM pseudocode (FPUnpack, FPProcessNaNs, FPRound, FPMulAdd, ...)
 * under the guest FP environment described in cpu.h: rounding mode and FZ
 * come from MXCSR, DN from cpu->fpcr, and the cumulative exception flags are
 * raised in MXCSR.
 */
#ifndef HAO_A64_FP_H
#define HAO_A64_FP_H

#include "hao/cpu.h"

typedef struct {
    hao_cpu *c;
    unsigned rmode;   /* FPCR.RMode: 0 RN, 1 RP, 2 RM, 3 RZ */
    bool fz, dn;
} fpctx;

/* rounding selectors for fp_rint/fp_to_int: the four FPCR modes plus ties-away */
enum { FPR_N = 0, FPR_P = 1, FPR_M = 2, FPR_Z = 3, FPR_A = 4 };

fpctx fp_ctx(hao_cpu *c);
static inline int fp_cur(const fpctx *x) { return (int)x->rmode; }

/* exception flags (FPSR bit positions) */
enum { FPX_IOC = 1, FPX_DZC = 2, FPX_OFC = 4, FPX_UFC = 8, FPX_IXC = 16, FPX_IDC = 128 };
void fp_raise(fpctx *x, unsigned flags);

bool fp_isnan(uint64_t v, int es);
uint64_t fp_default_nan(int es);
uint64_t fp_neg(uint64_t v, int es);
uint64_t fp_abs(uint64_t v, int es);

/* arithmetic */
uint64_t fp_add(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_sub(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_mul(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_mulx(fpctx *x, int es, uint64_t a, uint64_t b);
/* Speculative packed-single arithmetic. False leaves the output and FP
 * environment untouched, so the caller can use the exact per-lane path. */
enum { FPV_ADD, FPV_SUB, FPV_MUL };
bool fp_vec32(unsigned op, const hao_vreg *a, const hao_vreg *b, int lanes, hao_vreg *out);
/* Finite normal/zero vector selection and predicates. Neither acceptance nor
 * rejection changes the FP environment; false also leaves output untouched. */
enum { FPVS_MAX, FPVS_MIN, FPVS_EQ, FPVS_GE, FPVS_GT };
bool fp_vec32_select(unsigned op, const hao_vreg *a, const hao_vreg *b, int lanes, hao_vreg *out);
uint64_t fp_div(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_sqrt(fpctx *x, int es, uint64_t a);
uint64_t fp_muladd(fpctx *x, int es, uint64_t addend, uint64_t a, uint64_t b);   /* addend + a*b, one rounding */
/* Half vector fused arithmetic: exact binary64 intermediates under a proven
 * significand-span bound, followed by the existing half rounding operation.
 * Like fp_ctx(), this internal API requires x->rmode to match the live guest
 * MXCSR mode; exact cancellation uses that mode to select the zero sign.
 * False leaves output and the complete FP environment unchanged. */
bool fp_vec16_muladd(fpctx *x, const hao_vreg *d, const hao_vreg *a,
                    const hao_vreg *b, int lanes, bool subtract, hao_vreg *out);
/* Single vector fused arithmetic (2 or 4 lanes): the exact binary64 product,
 * its sum with the addend rounded to odd, and one conversion to single under
 * the guest's MXCSR. Admits normal inputs whose results are not tiny or zero.
 * False leaves output and the complete FP environment unchanged. fp_muladd
 * applies the same path to one single-precision lane. */
bool fp_vec32_muladd(fpctx *x, const hao_vreg *d, const hao_vreg *a,
                     const hao_vreg *b, int lanes, bool subtract, hao_vreg *out);
uint64_t fp_max(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_min(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_maxnm(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_minnm(fpctx *x, int es, uint64_t a, uint64_t b);
uint64_t fp_recps(fpctx *x, int es, uint64_t a, uint64_t b);    /* 2 - a*b */
uint64_t fp_rsqrts(fpctx *x, int es, uint64_t a, uint64_t b);   /* (3 - a*b) / 2 */
uint64_t fp_recpe(fpctx *x, int es, uint64_t a);
uint64_t fp_rsqrte(fpctx *x, int es, uint64_t a);
uint64_t fp_recpx(fpctx *x, int es, uint64_t a);

/* comparisons: NZCV for FCMP (signal: FCMPE, quiet NaNs raise IOC too) and
 * the vector compare predicates (FCMEQ quiet; FCMGE/FCMGT signalling) */
unsigned fp_cmp(fpctx *x, int es, uint64_t a, uint64_t b, bool signal);
bool fp_cmp_eq(fpctx *x, int es, uint64_t a, uint64_t b);
bool fp_cmp_ge(fpctx *x, int es, uint64_t a, uint64_t b);
bool fp_cmp_gt(fpctx *x, int es, uint64_t a, uint64_t b);

/* conversions */
uint64_t fp_cvt(fpctx *x, uint64_t a, int from, int to);                        /* FCVT */
uint64_t fp_cvt_odd(fpctx *x, uint64_t a);                                      /* FCVTXN d -> s */
uint64_t fp_rint(fpctx *x, int es, uint64_t a, int mode, bool exact);           /* FRINT* */
uint64_t fp_to_int(fpctx *x, int es, uint64_t a, int mode, bool uns, int bits, int fbits);   /* FCVT*S/U */
uint64_t fp_from_int(fpctx *x, int es, uint64_t v, bool sgn, int bits, int fbits);          /* SCVTF/UCVTF */
uint32_t fp_jcvt(fpctx *x, uint64_t a, bool *exact);                            /* FJCVTZS */

#endif
