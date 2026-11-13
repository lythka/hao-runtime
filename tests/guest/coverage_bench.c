/* Per-instruction loops for helper statistics and development-host ratios.
 * Sixteen operations per iteration, constant ordinary inputs, accumulator
 * reset each iteration. Loop/reset/entry costs are included in both builds. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
typedef struct { uint64_t lo, hi; } vec;
static const vec tail_a = {0x3f0000003f000000ull, 0x3f0000003f000000ull};
static const vec tail_b = {0x3e8000003e800000ull, 0x3e8000003e800000ull};
static const vec single_a = {0x400000003f800000ull, 0x4080000040400000ull};
static const vec single_b = {0x3f0000003e800000ull, 0x3e0000003f800000ull};
static const vec double_a = {0x3ff0000000000000ull, 0x4000000000000000ull};
static const vec double_b = {0x3fe0000000000000ull, 0x3fd0000000000000ull};
static const vec shift32_a = {0x8123456789abcdefull, 0x0123456776543210ull};
static const vec shift32_b = {0x00000001000000fcull, 0x0000008000000020ull};
static const vec shift64_a = {0x8123456789abcdefull, 0x0123456776543210ull};
static const vec shift64_b = {0x01000000000000f4ull, 0x8000000000000007ull};
static const vec integer_a = {1, 2}, integer_b = {3, 4};
static const vec half_integer_a = {0x0004000300020001ull, 0x0008000700060005ull}, half_integer_b = {0,0};
static const vec half_a = {0x4400420040003c00ull, 0x4800470046004500ull}, half_b = {0x3800380038003800ull,0x3800380038003800ull};
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec; }
#define BENCH(name, insn, kind) \
static __attribute__((noinline)) vec name(unsigned long n) { \
    vec out; \
    __asm__ volatile("ldr q0, [%[a]]\nldr q1, [%[b]]\n" \
        "1: movi v2.4s, #0\n.rept 16\n" insn "\n.endr\n" \
        "subs %[n], %[n], #1\nb.ne 1b\nstr q2, [%[out]]\n" \
        : [n] "+&r"(n) : [a] "r"(&kind##_a), [b] "r"(&kind##_b), [out] "r"(&out) \
        : "v0", "v1", "v2", "x9", "cc", "memory"); \
    return out; \
}
/* Reset the addend before each fused instruction so every measured lane
 * retains its ordinary/subnormal/wide-span shape. Include exact FPSR in
 * the checksum, and restore the caller's default control state. */
#define HALF_FMA_BENCH(name, insn, a, b, c, control) \
static __attribute__((noinline)) vec name(unsigned long n) { \
    static const uint16_t data[3][8] = {{a,a,a,a,a,a,a,a},{b,b,b,b,b,b,b,b},{c,c,c,c,c,c,c,c}}; \
    vec out; uint64_t sr; \
    __asm__ volatile("ldp q0,q1,[%[data]]\nldr q3,[%[data],#32]\n" \
        "msr fpcr,%[cr]\nmsr fpsr,xzr\n1:\n.rept 16\nmov v2.16b,v3.16b\n" insn "\n.endr\n" \
        "subs %[n],%[n],#1\nb.ne 1b\nstr q2,[%[out]]\nmrs %[sr],fpsr\nmsr fpcr,xzr\n" \
        : [n] "+&r"(n), [sr] "=&r"(sr) : [data] "r"(data), [out] "r"(&out), [cr] "r"((uint64_t)(control)) \
        : "v0","v1","v2","v3","cc","memory"); \
    out.hi ^= sr; return out; \
}
HALF_FMA_BENCH(f16_fmla_4h_tinyproduct, "fmla v2.4h,v0.4h,v1.h[7]", 0x0001, 0x0001, 0x7bff, 0)
HALF_FMA_BENCH(f16_fmls_4h_tinyproduct, "fmls v2.4h,v0.4h,v1.h[7]", 0x0001, 0x0001, 0x7bff, 0)
HALF_FMA_BENCH(f16_fmla_8h_tinyproduct, "fmla v2.8h,v0.8h,v1.h[7]", 0x0001, 0x0001, 0x7bff, 0)
HALF_FMA_BENCH(f16_fmls_8h_tinyproduct, "fmls v2.8h,v0.8h,v1.h[7]", 0x0001, 0x0001, 0x7bff, 0)
HALF_FMA_BENCH(f16_fmla_4h_normal, "fmla v2.4h,v0.4h,v1.h[7]", 0x3c01, 0x1001, 0x3c01, 0x0)
HALF_FMA_BENCH(f16_fmla_4h_subnormal, "fmla v2.4h,v0.4h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x0)
HALF_FMA_BENCH(f16_fmla_4h_wide, "fmla v2.4h,v0.4h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x0)
HALF_FMA_BENCH(f16_fmla_4h_wide_rp, "fmla v2.4h,v0.4h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x400000)
HALF_FMA_BENCH(f16_fmla_4h_fz16, "fmla v2.4h,v0.4h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x80000)
HALF_FMA_BENCH(f16_fmla_4h_nan, "fmla v2.4h,v0.4h,v1.h[7]", 0x7c01, 0x3c00, 0x3c00, 0x0)
HALF_FMA_BENCH(f16_fmls_4h_normal, "fmls v2.4h,v0.4h,v1.h[7]", 0x3c01, 0x1001, 0x3c01, 0x0)
HALF_FMA_BENCH(f16_fmls_4h_subnormal, "fmls v2.4h,v0.4h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x0)
HALF_FMA_BENCH(f16_fmls_4h_wide, "fmls v2.4h,v0.4h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x0)
HALF_FMA_BENCH(f16_fmls_4h_wide_rp, "fmls v2.4h,v0.4h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x400000)
HALF_FMA_BENCH(f16_fmls_4h_fz16, "fmls v2.4h,v0.4h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x80000)
HALF_FMA_BENCH(f16_fmls_4h_nan, "fmls v2.4h,v0.4h,v1.h[7]", 0x7c01, 0x3c00, 0x3c00, 0x0)
HALF_FMA_BENCH(f16_fmla_8h_normal, "fmla v2.8h,v0.8h,v1.h[7]", 0x3c01, 0x1001, 0x3c01, 0x0)
HALF_FMA_BENCH(f16_fmla_8h_subnormal, "fmla v2.8h,v0.8h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x0)
HALF_FMA_BENCH(f16_fmla_8h_wide, "fmla v2.8h,v0.8h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x0)
HALF_FMA_BENCH(f16_fmla_8h_wide_rp, "fmla v2.8h,v0.8h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x400000)
HALF_FMA_BENCH(f16_fmla_8h_fz16, "fmla v2.8h,v0.8h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x80000)
HALF_FMA_BENCH(f16_fmla_8h_nan, "fmla v2.8h,v0.8h,v1.h[7]", 0x7c01, 0x3c00, 0x3c00, 0x0)
HALF_FMA_BENCH(f16_fmls_8h_normal, "fmls v2.8h,v0.8h,v1.h[7]", 0x3c01, 0x1001, 0x3c01, 0x0)
HALF_FMA_BENCH(f16_fmls_8h_subnormal, "fmls v2.8h,v0.8h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x0)
HALF_FMA_BENCH(f16_fmls_8h_wide, "fmls v2.8h,v0.8h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x0)
HALF_FMA_BENCH(f16_fmls_8h_wide_rp, "fmls v2.8h,v0.8h,v1.h[7]", 0x0401, 0x0401, 0x7bff, 0x400000)
HALF_FMA_BENCH(f16_fmls_8h_fz16, "fmls v2.8h,v0.8h,v1.h[7]", 0x0001, 0x3c00, 0x0400, 0x80000)
HALF_FMA_BENCH(f16_fmls_8h_nan, "fmls v2.8h,v0.8h,v1.h[7]", 0x7c01, 0x3c00, 0x3c00, 0x0)

BENCH(nar_shrn_v8_0_1, "shrn v2.8b,v0.8h,#1", shift64)
BENCH(nar_shrn_v8_0_8, "shrn v2.8b,v0.8h,#8", shift64)
BENCH(nar_shrn_v8_1_1, "shrn2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_shrn_v8_1_8, "shrn2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_shrn_v16_0_1, "shrn v2.4h,v0.4s,#1", shift64)
BENCH(nar_shrn_v16_0_16, "shrn v2.4h,v0.4s,#16", shift64)
BENCH(nar_shrn_v16_1_1, "shrn2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_shrn_v16_1_16, "shrn2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_shrn_v32_0_1, "shrn v2.2s,v0.2d,#1", shift64)
BENCH(nar_shrn_v32_0_32, "shrn v2.2s,v0.2d,#32", shift64)
BENCH(nar_shrn_v32_1_1, "shrn2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_shrn_v32_1_32, "shrn2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_rshrn_v8_0_1, "rshrn v2.8b,v0.8h,#1", shift64)
BENCH(nar_rshrn_v8_0_8, "rshrn v2.8b,v0.8h,#8", shift64)
BENCH(nar_rshrn_v8_1_1, "rshrn2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_rshrn_v8_1_8, "rshrn2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_rshrn_v16_0_1, "rshrn v2.4h,v0.4s,#1", shift64)
BENCH(nar_rshrn_v16_0_16, "rshrn v2.4h,v0.4s,#16", shift64)
BENCH(nar_rshrn_v16_1_1, "rshrn2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_rshrn_v16_1_16, "rshrn2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_rshrn_v32_0_1, "rshrn v2.2s,v0.2d,#1", shift64)
BENCH(nar_rshrn_v32_0_32, "rshrn v2.2s,v0.2d,#32", shift64)
BENCH(nar_rshrn_v32_1_1, "rshrn2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_rshrn_v32_1_32, "rshrn2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_sqshrun_v8_0_1, "sqshrun v2.8b,v0.8h,#1", shift64)
BENCH(nar_sqshrun_v8_0_8, "sqshrun v2.8b,v0.8h,#8", shift64)
BENCH(nar_sqshrun_v8_1_1, "sqshrun2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_sqshrun_v8_1_8, "sqshrun2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_sqshrun_v16_0_1, "sqshrun v2.4h,v0.4s,#1", shift64)
BENCH(nar_sqshrun_v16_0_16, "sqshrun v2.4h,v0.4s,#16", shift64)
BENCH(nar_sqshrun_v16_1_1, "sqshrun2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_sqshrun_v16_1_16, "sqshrun2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_sqshrun_v32_0_1, "sqshrun v2.2s,v0.2d,#1", shift64)
BENCH(nar_sqshrun_v32_0_32, "sqshrun v2.2s,v0.2d,#32", shift64)
BENCH(nar_sqshrun_v32_1_1, "sqshrun2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_sqshrun_v32_1_32, "sqshrun2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_sqrshrun_v8_0_1, "sqrshrun v2.8b,v0.8h,#1", shift64)
BENCH(nar_sqrshrun_v8_0_8, "sqrshrun v2.8b,v0.8h,#8", shift64)
BENCH(nar_sqrshrun_v8_1_1, "sqrshrun2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_sqrshrun_v8_1_8, "sqrshrun2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_sqrshrun_v16_0_1, "sqrshrun v2.4h,v0.4s,#1", shift64)
BENCH(nar_sqrshrun_v16_0_16, "sqrshrun v2.4h,v0.4s,#16", shift64)
BENCH(nar_sqrshrun_v16_1_1, "sqrshrun2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_sqrshrun_v16_1_16, "sqrshrun2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_sqrshrun_v32_0_1, "sqrshrun v2.2s,v0.2d,#1", shift64)
BENCH(nar_sqrshrun_v32_0_32, "sqrshrun v2.2s,v0.2d,#32", shift64)
BENCH(nar_sqrshrun_v32_1_1, "sqrshrun2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_sqrshrun_v32_1_32, "sqrshrun2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_sqshrn_v8_0_1, "sqshrn v2.8b,v0.8h,#1", shift64)
BENCH(nar_sqshrn_v8_0_8, "sqshrn v2.8b,v0.8h,#8", shift64)
BENCH(nar_sqshrn_v8_1_1, "sqshrn2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_sqshrn_v8_1_8, "sqshrn2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_sqshrn_v16_0_1, "sqshrn v2.4h,v0.4s,#1", shift64)
BENCH(nar_sqshrn_v16_0_16, "sqshrn v2.4h,v0.4s,#16", shift64)
BENCH(nar_sqshrn_v16_1_1, "sqshrn2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_sqshrn_v16_1_16, "sqshrn2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_sqshrn_v32_0_1, "sqshrn v2.2s,v0.2d,#1", shift64)
BENCH(nar_sqshrn_v32_0_32, "sqshrn v2.2s,v0.2d,#32", shift64)
BENCH(nar_sqshrn_v32_1_1, "sqshrn2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_sqshrn_v32_1_32, "sqshrn2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_sqrshrn_v8_0_1, "sqrshrn v2.8b,v0.8h,#1", shift64)
BENCH(nar_sqrshrn_v8_0_8, "sqrshrn v2.8b,v0.8h,#8", shift64)
BENCH(nar_sqrshrn_v8_1_1, "sqrshrn2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_sqrshrn_v8_1_8, "sqrshrn2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_sqrshrn_v16_0_1, "sqrshrn v2.4h,v0.4s,#1", shift64)
BENCH(nar_sqrshrn_v16_0_16, "sqrshrn v2.4h,v0.4s,#16", shift64)
BENCH(nar_sqrshrn_v16_1_1, "sqrshrn2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_sqrshrn_v16_1_16, "sqrshrn2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_sqrshrn_v32_0_1, "sqrshrn v2.2s,v0.2d,#1", shift64)
BENCH(nar_sqrshrn_v32_0_32, "sqrshrn v2.2s,v0.2d,#32", shift64)
BENCH(nar_sqrshrn_v32_1_1, "sqrshrn2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_sqrshrn_v32_1_32, "sqrshrn2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_uqshrn_v8_0_1, "uqshrn v2.8b,v0.8h,#1", shift64)
BENCH(nar_uqshrn_v8_0_8, "uqshrn v2.8b,v0.8h,#8", shift64)
BENCH(nar_uqshrn_v8_1_1, "uqshrn2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_uqshrn_v8_1_8, "uqshrn2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_uqshrn_v16_0_1, "uqshrn v2.4h,v0.4s,#1", shift64)
BENCH(nar_uqshrn_v16_0_16, "uqshrn v2.4h,v0.4s,#16", shift64)
BENCH(nar_uqshrn_v16_1_1, "uqshrn2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_uqshrn_v16_1_16, "uqshrn2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_uqshrn_v32_0_1, "uqshrn v2.2s,v0.2d,#1", shift64)
BENCH(nar_uqshrn_v32_0_32, "uqshrn v2.2s,v0.2d,#32", shift64)
BENCH(nar_uqshrn_v32_1_1, "uqshrn2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_uqshrn_v32_1_32, "uqshrn2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_uqrshrn_v8_0_1, "uqrshrn v2.8b,v0.8h,#1", shift64)
BENCH(nar_uqrshrn_v8_0_8, "uqrshrn v2.8b,v0.8h,#8", shift64)
BENCH(nar_uqrshrn_v8_1_1, "uqrshrn2 v2.16b,v0.8h,#1", shift64)
BENCH(nar_uqrshrn_v8_1_8, "uqrshrn2 v2.16b,v0.8h,#8", shift64)
BENCH(nar_uqrshrn_v16_0_1, "uqrshrn v2.4h,v0.4s,#1", shift64)
BENCH(nar_uqrshrn_v16_0_16, "uqrshrn v2.4h,v0.4s,#16", shift64)
BENCH(nar_uqrshrn_v16_1_1, "uqrshrn2 v2.8h,v0.4s,#1", shift64)
BENCH(nar_uqrshrn_v16_1_16, "uqrshrn2 v2.8h,v0.4s,#16", shift64)
BENCH(nar_uqrshrn_v32_0_1, "uqrshrn v2.2s,v0.2d,#1", shift64)
BENCH(nar_uqrshrn_v32_0_32, "uqrshrn v2.2s,v0.2d,#32", shift64)
BENCH(nar_uqrshrn_v32_1_1, "uqrshrn2 v2.4s,v0.2d,#1", shift64)
BENCH(nar_uqrshrn_v32_1_32, "uqrshrn2 v2.4s,v0.2d,#32", shift64)
BENCH(nar_sqshrun_s8_0_1, "sqshrun b2,h0,#1", shift64)
BENCH(nar_sqshrun_s8_0_8, "sqshrun b2,h0,#8", shift64)
BENCH(nar_sqshrun_s16_0_1, "sqshrun h2,s0,#1", shift64)
BENCH(nar_sqshrun_s16_0_16, "sqshrun h2,s0,#16", shift64)
BENCH(nar_sqshrun_s32_0_1, "sqshrun s2,d0,#1", shift64)
BENCH(nar_sqshrun_s32_0_32, "sqshrun s2,d0,#32", shift64)
BENCH(nar_sqrshrun_s8_0_1, "sqrshrun b2,h0,#1", shift64)
BENCH(nar_sqrshrun_s8_0_8, "sqrshrun b2,h0,#8", shift64)
BENCH(nar_sqrshrun_s16_0_1, "sqrshrun h2,s0,#1", shift64)
BENCH(nar_sqrshrun_s16_0_16, "sqrshrun h2,s0,#16", shift64)
BENCH(nar_sqrshrun_s32_0_1, "sqrshrun s2,d0,#1", shift64)
BENCH(nar_sqrshrun_s32_0_32, "sqrshrun s2,d0,#32", shift64)
BENCH(nar_sqshrn_s8_0_1, "sqshrn b2,h0,#1", shift64)
BENCH(nar_sqshrn_s8_0_8, "sqshrn b2,h0,#8", shift64)
BENCH(nar_sqshrn_s16_0_1, "sqshrn h2,s0,#1", shift64)
BENCH(nar_sqshrn_s16_0_16, "sqshrn h2,s0,#16", shift64)
BENCH(nar_sqshrn_s32_0_1, "sqshrn s2,d0,#1", shift64)
BENCH(nar_sqshrn_s32_0_32, "sqshrn s2,d0,#32", shift64)
BENCH(nar_sqrshrn_s8_0_1, "sqrshrn b2,h0,#1", shift64)
BENCH(nar_sqrshrn_s8_0_8, "sqrshrn b2,h0,#8", shift64)
BENCH(nar_sqrshrn_s16_0_1, "sqrshrn h2,s0,#1", shift64)
BENCH(nar_sqrshrn_s16_0_16, "sqrshrn h2,s0,#16", shift64)
BENCH(nar_sqrshrn_s32_0_1, "sqrshrn s2,d0,#1", shift64)
BENCH(nar_sqrshrn_s32_0_32, "sqrshrn s2,d0,#32", shift64)
BENCH(nar_uqshrn_s8_0_1, "uqshrn b2,h0,#1", shift64)
BENCH(nar_uqshrn_s8_0_8, "uqshrn b2,h0,#8", shift64)
BENCH(nar_uqshrn_s16_0_1, "uqshrn h2,s0,#1", shift64)
BENCH(nar_uqshrn_s16_0_16, "uqshrn h2,s0,#16", shift64)
BENCH(nar_uqshrn_s32_0_1, "uqshrn s2,d0,#1", shift64)
BENCH(nar_uqshrn_s32_0_32, "uqshrn s2,d0,#32", shift64)
BENCH(nar_uqrshrn_s8_0_1, "uqrshrn b2,h0,#1", shift64)
BENCH(nar_uqrshrn_s8_0_8, "uqrshrn b2,h0,#8", shift64)
BENCH(nar_uqrshrn_s16_0_1, "uqrshrn h2,s0,#1", shift64)
BENCH(nar_uqrshrn_s16_0_16, "uqrshrn h2,s0,#16", shift64)
BENCH(nar_uqrshrn_s32_0_1, "uqrshrn s2,d0,#1", shift64)
BENCH(nar_uqrshrn_s32_0_32, "uqrshrn s2,d0,#32", shift64)
BENCH(abd2s, "fabd v2.2s,v0.2s,v1.2s", single)
BENCH(abd4s, "fabd v2.4s,v0.4s,v1.4s", single)
BENCH(abd2d, "fabd v2.2d,v0.2d,v1.2d", double)
BENCH(abds, "fabd s2,s0,s1", single)
BENCH(abdd, "fabd d2,d0,d1", double)
BENCH(sqadd_8b, "sqadd v2.8b,v0.8b,v1.8b", single)
BENCH(sqadd_16b, "sqadd v2.16b,v0.16b,v1.16b", single)
BENCH(sqadd_4h, "sqadd v2.4h,v0.4h,v1.4h", single)
BENCH(sqadd_8h, "sqadd v2.8h,v0.8h,v1.8h", single)
BENCH(sqadd_2s, "sqadd v2.2s,v0.2s,v1.2s", single)
BENCH(sqadd_4s, "sqadd v2.4s,v0.4s,v1.4s", single)
BENCH(sqadd_2d, "sqadd v2.2d,v0.2d,v1.2d", single)
BENCH(sqadd_b, "sqadd b2,b0,b1", single)
BENCH(sqadd_h, "sqadd h2,h0,h1", single)
BENCH(sqadd_s, "sqadd s2,s0,s1", single)
BENCH(sqadd_d, "sqadd d2,d0,d1", single)
BENCH(uqadd_8b, "uqadd v2.8b,v0.8b,v1.8b", single)
BENCH(uqadd_16b, "uqadd v2.16b,v0.16b,v1.16b", single)
BENCH(uqadd_4h, "uqadd v2.4h,v0.4h,v1.4h", single)
BENCH(uqadd_8h, "uqadd v2.8h,v0.8h,v1.8h", single)
BENCH(uqadd_2s, "uqadd v2.2s,v0.2s,v1.2s", single)
BENCH(uqadd_4s, "uqadd v2.4s,v0.4s,v1.4s", single)
BENCH(uqadd_2d, "uqadd v2.2d,v0.2d,v1.2d", single)
BENCH(uqadd_b, "uqadd b2,b0,b1", single)
BENCH(uqadd_h, "uqadd h2,h0,h1", single)
BENCH(uqadd_s, "uqadd s2,s0,s1", single)
BENCH(uqadd_d, "uqadd d2,d0,d1", single)
BENCH(sqsub_8b, "sqsub v2.8b,v0.8b,v1.8b", single)
BENCH(sqsub_16b, "sqsub v2.16b,v0.16b,v1.16b", single)
BENCH(sqsub_4h, "sqsub v2.4h,v0.4h,v1.4h", single)
BENCH(sqsub_8h, "sqsub v2.8h,v0.8h,v1.8h", single)
BENCH(sqsub_2s, "sqsub v2.2s,v0.2s,v1.2s", single)
BENCH(sqsub_4s, "sqsub v2.4s,v0.4s,v1.4s", single)
BENCH(sqsub_2d, "sqsub v2.2d,v0.2d,v1.2d", single)
BENCH(sqsub_b, "sqsub b2,b0,b1", single)
BENCH(sqsub_h, "sqsub h2,h0,h1", single)
BENCH(sqsub_s, "sqsub s2,s0,s1", single)
BENCH(sqsub_d, "sqsub d2,d0,d1", single)
BENCH(uqsub_8b, "uqsub v2.8b,v0.8b,v1.8b", single)
BENCH(uqsub_16b, "uqsub v2.16b,v0.16b,v1.16b", single)
BENCH(uqsub_4h, "uqsub v2.4h,v0.4h,v1.4h", single)
BENCH(uqsub_8h, "uqsub v2.8h,v0.8h,v1.8h", single)
BENCH(uqsub_2s, "uqsub v2.2s,v0.2s,v1.2s", single)
BENCH(uqsub_4s, "uqsub v2.4s,v0.4s,v1.4s", single)
BENCH(uqsub_2d, "uqsub v2.2d,v0.2d,v1.2d", single)
BENCH(uqsub_b, "uqsub b2,b0,b1", single)
BENCH(uqsub_h, "uqsub h2,h0,h1", single)
BENCH(uqsub_s, "uqsub s2,s0,s1", single)
BENCH(uqsub_d, "uqsub d2,d0,d1", single)
BENCH(sabd_8b, "sabd v2.8b,v0.8b,v1.8b", single)
BENCH(sabd_4h, "sabd v2.4h,v0.4h,v1.4h", single)
BENCH(sabd_2s, "sabd v2.2s,v0.2s,v1.2s", single)
BENCH(sabd_16b, "sabd v2.16b,v0.16b,v1.16b", single)
BENCH(sabd_8h, "sabd v2.8h,v0.8h,v1.8h", single)
BENCH(sabd_4s, "sabd v2.4s,v0.4s,v1.4s", single)
BENCH(uabd_8b, "uabd v2.8b,v0.8b,v1.8b", single)
BENCH(uabd_4h, "uabd v2.4h,v0.4h,v1.4h", single)
BENCH(uabd_2s, "uabd v2.2s,v0.2s,v1.2s", single)
BENCH(uabd_16b, "uabd v2.16b,v0.16b,v1.16b", single)
BENCH(uabd_8h, "uabd v2.8h,v0.8h,v1.8h", single)
BENCH(uabd_4s, "uabd v2.4s,v0.4s,v1.4s", single)
BENCH(saba_8b, "saba v2.8b,v0.8b,v1.8b", single)
BENCH(saba_4h, "saba v2.4h,v0.4h,v1.4h", single)
BENCH(saba_2s, "saba v2.2s,v0.2s,v1.2s", single)
BENCH(saba_16b, "saba v2.16b,v0.16b,v1.16b", single)
BENCH(saba_8h, "saba v2.8h,v0.8h,v1.8h", single)
BENCH(saba_4s, "saba v2.4s,v0.4s,v1.4s", single)
BENCH(uaba_8b, "uaba v2.8b,v0.8b,v1.8b", single)
BENCH(uaba_4h, "uaba v2.4h,v0.4h,v1.4h", single)
BENCH(uaba_2s, "uaba v2.2s,v0.2s,v1.2s", single)
BENCH(uaba_16b, "uaba v2.16b,v0.16b,v1.16b", single)
BENCH(uaba_8h, "uaba v2.8h,v0.8h,v1.8h", single)
BENCH(uaba_4s, "uaba v2.4s,v0.4s,v1.4s", single)
BENCH(sabdl_8h, "sabdl v2.8h,v0.8b,v1.8b", single)
BENCH(sabdl_4s, "sabdl v2.4s,v0.4h,v1.4h", single)
BENCH(sabdl_2d, "sabdl v2.2d,v0.2s,v1.2s", single)
BENCH(sabdl2_8h, "sabdl2 v2.8h,v0.16b,v1.16b", single)
BENCH(sabdl2_4s, "sabdl2 v2.4s,v0.8h,v1.8h", single)
BENCH(sabdl2_2d, "sabdl2 v2.2d,v0.4s,v1.4s", single)
BENCH(uabdl_8h, "uabdl v2.8h,v0.8b,v1.8b", single)
BENCH(uabdl_4s, "uabdl v2.4s,v0.4h,v1.4h", single)
BENCH(uabdl_2d, "uabdl v2.2d,v0.2s,v1.2s", single)
BENCH(uabdl2_8h, "uabdl2 v2.8h,v0.16b,v1.16b", single)
BENCH(uabdl2_4s, "uabdl2 v2.4s,v0.8h,v1.8h", single)
BENCH(uabdl2_2d, "uabdl2 v2.2d,v0.4s,v1.4s", single)
BENCH(sabal_8h, "sabal v2.8h,v0.8b,v1.8b", single)
BENCH(sabal_4s, "sabal v2.4s,v0.4h,v1.4h", single)
BENCH(sabal_2d, "sabal v2.2d,v0.2s,v1.2s", single)
BENCH(sabal2_8h, "sabal2 v2.8h,v0.16b,v1.16b", single)
BENCH(sabal2_4s, "sabal2 v2.4s,v0.8h,v1.8h", single)
BENCH(sabal2_2d, "sabal2 v2.2d,v0.4s,v1.4s", single)
BENCH(uabal_8h, "uabal v2.8h,v0.8b,v1.8b", single)
BENCH(uabal_4s, "uabal v2.4s,v0.4h,v1.4h", single)
BENCH(uabal_2d, "uabal v2.2d,v0.2s,v1.2s", single)
BENCH(uabal2_8h, "uabal2 v2.8h,v0.16b,v1.16b", single)
BENCH(uabal2_4s, "uabal2 v2.4s,v0.8h,v1.8h", single)
BENCH(uabal2_2d, "uabal2 v2.2d,v0.4s,v1.4s", single)
BENCH(addhn_8b, "addhn v2.8b,v0.8h,v1.8h", single)
BENCH(addhn_4h, "addhn v2.4h,v0.4s,v1.4s", single)
BENCH(addhn_2s, "addhn v2.2s,v0.2d,v1.2d", single)
BENCH(addhn2_16b, "addhn2 v2.16b,v0.8h,v1.8h", single)
BENCH(addhn2_8h, "addhn2 v2.8h,v0.4s,v1.4s", single)
BENCH(addhn2_4s, "addhn2 v2.4s,v0.2d,v1.2d", single)
BENCH(raddhn_8b, "raddhn v2.8b,v0.8h,v1.8h", single)
BENCH(raddhn_4h, "raddhn v2.4h,v0.4s,v1.4s", single)
BENCH(raddhn_2s, "raddhn v2.2s,v0.2d,v1.2d", single)
BENCH(raddhn2_16b, "raddhn2 v2.16b,v0.8h,v1.8h", single)
BENCH(raddhn2_8h, "raddhn2 v2.8h,v0.4s,v1.4s", single)
BENCH(raddhn2_4s, "raddhn2 v2.4s,v0.2d,v1.2d", single)
BENCH(subhn_8b, "subhn v2.8b,v0.8h,v1.8h", single)
BENCH(subhn_4h, "subhn v2.4h,v0.4s,v1.4s", single)
BENCH(subhn_2s, "subhn v2.2s,v0.2d,v1.2d", single)
BENCH(subhn2_16b, "subhn2 v2.16b,v0.8h,v1.8h", single)
BENCH(subhn2_8h, "subhn2 v2.8h,v0.4s,v1.4s", single)
BENCH(subhn2_4s, "subhn2 v2.4s,v0.2d,v1.2d", single)
BENCH(rsubhn_8b, "rsubhn v2.8b,v0.8h,v1.8h", single)
BENCH(rsubhn_4h, "rsubhn v2.4h,v0.4s,v1.4s", single)
BENCH(rsubhn_2s, "rsubhn v2.2s,v0.2d,v1.2d", single)
BENCH(rsubhn2_16b, "rsubhn2 v2.16b,v0.8h,v1.8h", single)
BENCH(rsubhn2_8h, "rsubhn2 v2.8h,v0.4s,v1.4s", single)
BENCH(rsubhn2_4s, "rsubhn2 v2.4s,v0.2d,v1.2d", single)
BENCH(addp_8b, "addp v2.8b,v0.8b,v1.8b", single)
BENCH(addp_16b, "addp v2.16b,v0.16b,v1.16b", single)
BENCH(addp_4h, "addp v2.4h,v0.4h,v1.4h", single)
BENCH(addp_8h, "addp v2.8h,v0.8h,v1.8h", single)
BENCH(addp_2s, "addp v2.2s,v0.2s,v1.2s", single)
BENCH(addp_4s, "addp v2.4s,v0.4s,v1.4s", single)
BENCH(addp_2d, "addp v2.2d,v0.2d,v1.2d", single)
BENCH(fmul_2d_e0, "fmul v2.2d,v0.2d,v1.d[0]", double)
BENCH(fmul_2d_e1, "fmul v2.2d,v0.2d,v1.d[1]", double)
BENCH(fcmeq_2d, "fcmeq v2.2d,v0.2d,v1.2d", double)
BENCH(fcmge_2d, "fcmge v2.2d,v0.2d,v1.2d", double)
BENCH(fcmgt_2d, "fcmgt v2.2d,v0.2d,v1.2d", double)
BENCH(fcmeq_2d_zero, "fcmeq v2.2d,v0.2d,#0.0", double)
BENCH(fcmge_2d_zero, "fcmge v2.2d,v0.2d,#0.0", double)
BENCH(fcmgt_2d_zero, "fcmgt v2.2d,v0.2d,#0.0", double)
BENCH(fcmle_2d_zero, "fcmle v2.2d,v0.2d,#0.0", double)
BENCH(fcmlt_2d_zero, "fcmlt v2.2d,v0.2d,#0.0", double)
BENCH(sdot0_0, ".inst 0x0e819402", single)
BENCH(sdot1_0, ".inst 0x4e819402", single)
BENCH(udot0_0, ".inst 0x2e819402", single)
BENCH(udot1_0, ".inst 0x6e819402", single)
BENCH(usdot0_0, ".inst 0x0e819c02", single)
BENCH(usdot1_0, ".inst 0x4e819c02", single)
BENCH(sdot_e0_0, ".inst 0x0f81e002", single)
BENCH(sdot_e0_1, ".inst 0x0fa1e002", single)
BENCH(sdot_e0_2, ".inst 0x0f81e802", single)
BENCH(sdot_e0_3, ".inst 0x0fa1e802", single)
BENCH(sdot_e1_0, ".inst 0x4f81e002", single)
BENCH(sdot_e1_1, ".inst 0x4fa1e002", single)
BENCH(sdot_e1_2, ".inst 0x4f81e802", single)
BENCH(sdot_e1_3, ".inst 0x4fa1e802", single)
BENCH(udot_e0_0, ".inst 0x2f81e002", single)
BENCH(udot_e0_1, ".inst 0x2fa1e002", single)
BENCH(udot_e0_2, ".inst 0x2f81e802", single)
BENCH(udot_e0_3, ".inst 0x2fa1e802", single)
BENCH(udot_e1_0, ".inst 0x6f81e002", single)
BENCH(udot_e1_1, ".inst 0x6fa1e002", single)
BENCH(udot_e1_2, ".inst 0x6f81e802", single)
BENCH(udot_e1_3, ".inst 0x6fa1e802", single)
BENCH(usdot_e0_0, ".inst 0x0f81f002", single)
BENCH(usdot_e0_1, ".inst 0x0fa1f002", single)
BENCH(usdot_e0_2, ".inst 0x0f81f802", single)
BENCH(usdot_e0_3, ".inst 0x0fa1f802", single)
BENCH(usdot_e1_0, ".inst 0x4f81f002", single)
BENCH(usdot_e1_1, ".inst 0x4fa1f002", single)
BENCH(usdot_e1_2, ".inst 0x4f81f802", single)
BENCH(usdot_e1_3, ".inst 0x4fa1f802", single)
BENCH(sudot_e0_0, ".inst 0x0f01f002", single)
BENCH(sudot_e0_1, ".inst 0x0f21f002", single)
BENCH(sudot_e0_2, ".inst 0x0f01f802", single)
BENCH(sudot_e0_3, ".inst 0x0f21f802", single)
BENCH(sudot_e1_0, ".inst 0x4f01f002", single)
BENCH(sudot_e1_1, ".inst 0x4f21f002", single)
BENCH(sudot_e1_2, ".inst 0x4f01f802", single)
BENCH(sudot_e1_3, ".inst 0x4f21f802", single)
BENCH(smmla1_0, ".inst 0x4e81a402", single)
BENCH(ummla1_0, ".inst 0x6e81a402", single)
BENCH(usmmla1_0, ".inst 0x4e81ac02", single)
BENCH(tail_fcvtn_4h, "fcvtn v2.4h,v0.4s", single)
BENCH(tail_fcvtn_2s, "fcvtn v2.2s,v0.2d", double)
BENCH(tail_fcvtn2_8h, "fcvtn2 v2.8h,v0.4s", single)
BENCH(tail_fcvtn2_4s, "fcvtn2 v2.4s,v0.2d", double)
BENCH(tail_fcvtl_4s, "fcvtl v2.4s,v0.4h", half)
BENCH(tail_fcvtl_2d, "fcvtl v2.2d,v0.2s", single)
BENCH(tail_fcvtl2_4s, "fcvtl2 v2.4s,v0.8h", half)
BENCH(tail_fcvtl2_2d, "fcvtl2 v2.2d,v0.4s", single)
BENCH(tail_fmul_4h, "fmul v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fmul_8h, "fmul v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fmul_h, "fmul h2,h0,h1", half)
BENCH(tail_fadd_4h, "fadd v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fadd_8h, "fadd v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fadd_h, "fadd h2,h0,h1", half)
BENCH(tail_fsub_4h, "fsub v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fsub_8h, "fsub v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fsub_h, "fsub h2,h0,h1", half)
BENCH(tail_fmax_4h, "fmax v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fmax_8h, "fmax v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fmax_h, "fmax h2,h0,h1", half)
BENCH(tail_fmin_4h, "fmin v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fmin_8h, "fmin v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fmin_h, "fmin h2,h0,h1", half)
BENCH(tail_fmaxnm_4h, "fmaxnm v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fmaxnm_8h, "fmaxnm v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fmaxnm_h, "fmaxnm h2,h0,h1", half)
BENCH(tail_fminnm_4h, "fminnm v2.4h,v0.4h,v1.4h", half)
BENCH(tail_fminnm_8h, "fminnm v2.8h,v0.8h,v1.8h", half)
BENCH(tail_fminnm_h, "fminnm h2,h0,h1", half)
BENCH(tail_fmul_4h_e0, "fmul v2.4h,v0.4h,v1.h[0]", half)
BENCH(tail_fmul_8h_e0, "fmul v2.8h,v0.8h,v1.h[0]", half)
BENCH(tail_fmul_4h_e1, "fmul v2.4h,v0.4h,v1.h[1]", half)
BENCH(tail_fmul_8h_e1, "fmul v2.8h,v0.8h,v1.h[1]", half)
BENCH(tail_fmul_4h_e2, "fmul v2.4h,v0.4h,v1.h[2]", half)
BENCH(tail_fmul_8h_e2, "fmul v2.8h,v0.8h,v1.h[2]", half)
BENCH(tail_fmul_4h_e3, "fmul v2.4h,v0.4h,v1.h[3]", half)
BENCH(tail_fmul_8h_e3, "fmul v2.8h,v0.8h,v1.h[3]", half)
BENCH(tail_fmul_4h_e4, "fmul v2.4h,v0.4h,v1.h[4]", half)
BENCH(tail_fmul_8h_e4, "fmul v2.8h,v0.8h,v1.h[4]", half)
BENCH(tail_fmul_4h_e5, "fmul v2.4h,v0.4h,v1.h[5]", half)
BENCH(tail_fmul_8h_e5, "fmul v2.8h,v0.8h,v1.h[5]", half)
BENCH(tail_fmul_4h_e6, "fmul v2.4h,v0.4h,v1.h[6]", half)
BENCH(tail_fmul_8h_e6, "fmul v2.8h,v0.8h,v1.h[6]", half)
BENCH(tail_fmul_4h_e7, "fmul v2.4h,v0.4h,v1.h[7]", half)
BENCH(tail_fmul_8h_e7, "fmul v2.8h,v0.8h,v1.h[7]", half)
BENCH(tail_fcvtzu_4s, "fcvtzu v2.4s,v0.4s", single)
BENCH(tail_fcvtns_4s, "fcvtns v2.4s,v0.4s", single)
BENCH(tail_fcvtps_4s, "fcvtps v2.4s,v0.4s", single)
BENCH(tail_fcvtms_4s, "fcvtms v2.4s,v0.4s", single)
BENCH(tail_scvtf_s, "scvtf s2,s0", integer)
BENCH(tail_scvtf_d, "scvtf d2,d0", integer)
BENCH(tail_scvtf_2d, "scvtf v2.2d,v0.2d", integer)
BENCH(tail_scvtf_8h, "scvtf v2.8h,v0.8h", half_integer)
BENCH(tail_ucvtf_s, "ucvtf s2,s0", integer)
BENCH(tail_ucvtf_d, "ucvtf d2,d0", integer)
BENCH(tail_ucvtf_2d, "ucvtf v2.2d,v0.2d", integer)
BENCH(tail_ucvtf_8h, "ucvtf v2.8h,v0.8h", half_integer)
BENCH(tail_fcvtzu_8h, "fcvtzu v2.8h,v0.8h", half)
BENCH(tail_fcvtns_8h, "fcvtns v2.8h,v0.8h", half)
BENCH(tail_fcvt_hs, "fcvt h2,s0", single)
BENCH(tail_fcvt_hd, "fcvt h2,d0", double)
BENCH(tail_fcvt_sh, "fcvt s2,h0", half)
BENCH(tail_fcvt_dh, "fcvt d2,h0", half)
BENCH(zip1_8b, "zip1 v2.8b, v0.8b, v1.8b", single)
BENCH(zip1_16b, "zip1 v2.16b, v0.16b, v1.16b", single)
BENCH(zip1_4h, "zip1 v2.4h, v0.4h, v1.4h", single)
BENCH(zip1_8h, "zip1 v2.8h, v0.8h, v1.8h", single)
BENCH(zip1_2s, "zip1 v2.2s, v0.2s, v1.2s", single)
BENCH(zip1_4s, "zip1 v2.4s, v0.4s, v1.4s", single)
BENCH(zip1_2d, "zip1 v2.2d, v0.2d, v1.2d", single)
BENCH(zip2_8b, "zip2 v2.8b, v0.8b, v1.8b", single)
BENCH(zip2_16b, "zip2 v2.16b, v0.16b, v1.16b", single)
BENCH(zip2_4h, "zip2 v2.4h, v0.4h, v1.4h", single)
BENCH(zip2_8h, "zip2 v2.8h, v0.8h, v1.8h", single)
BENCH(zip2_2s, "zip2 v2.2s, v0.2s, v1.2s", single)
BENCH(zip2_4s, "zip2 v2.4s, v0.4s, v1.4s", single)
BENCH(zip2_2d, "zip2 v2.2d, v0.2d, v1.2d", single)
BENCH(uzp1_8b, "uzp1 v2.8b, v0.8b, v1.8b", single)
BENCH(uzp1_16b, "uzp1 v2.16b, v0.16b, v1.16b", single)
BENCH(uzp1_4h, "uzp1 v2.4h, v0.4h, v1.4h", single)
BENCH(uzp1_8h, "uzp1 v2.8h, v0.8h, v1.8h", single)
BENCH(uzp1_2s, "uzp1 v2.2s, v0.2s, v1.2s", single)
BENCH(uzp1_4s, "uzp1 v2.4s, v0.4s, v1.4s", single)
BENCH(uzp1_2d, "uzp1 v2.2d, v0.2d, v1.2d", single)
BENCH(uzp2_8b, "uzp2 v2.8b, v0.8b, v1.8b", single)
BENCH(uzp2_16b, "uzp2 v2.16b, v0.16b, v1.16b", single)
BENCH(uzp2_4h, "uzp2 v2.4h, v0.4h, v1.4h", single)
BENCH(uzp2_8h, "uzp2 v2.8h, v0.8h, v1.8h", single)
BENCH(uzp2_2s, "uzp2 v2.2s, v0.2s, v1.2s", single)
BENCH(uzp2_4s, "uzp2 v2.4s, v0.4s, v1.4s", single)
BENCH(uzp2_2d, "uzp2 v2.2d, v0.2d, v1.2d", single)
BENCH(trn1_8b, "trn1 v2.8b, v0.8b, v1.8b", single)
BENCH(trn1_16b, "trn1 v2.16b, v0.16b, v1.16b", single)
BENCH(trn1_4h, "trn1 v2.4h, v0.4h, v1.4h", single)
BENCH(trn1_8h, "trn1 v2.8h, v0.8h, v1.8h", single)
BENCH(trn1_2s, "trn1 v2.2s, v0.2s, v1.2s", single)
BENCH(trn1_4s, "trn1 v2.4s, v0.4s, v1.4s", single)
BENCH(trn1_2d, "trn1 v2.2d, v0.2d, v1.2d", single)
BENCH(trn2_8b, "trn2 v2.8b, v0.8b, v1.8b", single)
BENCH(trn2_16b, "trn2 v2.16b, v0.16b, v1.16b", single)
BENCH(trn2_4h, "trn2 v2.4h, v0.4h, v1.4h", single)
BENCH(trn2_8h, "trn2 v2.8h, v0.8h, v1.8h", single)
BENCH(trn2_2s, "trn2 v2.2s, v0.2s, v1.2s", single)
BENCH(trn2_4s, "trn2 v2.4s, v0.4s, v1.4s", single)
BENCH(trn2_2d, "trn2 v2.2d, v0.2d, v1.2d", single)
BENCH(addv_8b, "addv b2, v0.8b", single)
BENCH(addv_16b, "addv b2, v0.16b", single)
BENCH(addv_4h, "addv h2, v0.4h", single)
BENCH(addv_8h, "addv h2, v0.8h", single)
BENCH(addv_4s, "addv s2, v0.4s", single)
BENCH(uaddlp_4h, "uaddlp v2.4h, v0.8b", single)
BENCH(uaddlp_8h, "uaddlp v2.8h, v0.16b", single)
BENCH(uaddlp_2s, "uaddlp v2.2s, v0.4h", single)
BENCH(uaddlp_4s, "uaddlp v2.4s, v0.8h", single)
BENCH(uaddlp_1d, "uaddlp v2.1d, v0.2s", single)
BENCH(uaddlp_2d, "uaddlp v2.2d, v0.4s", single)
BENCH(saddlp_4h, "saddlp v2.4h, v0.8b", single)
BENCH(saddlp_8h, "saddlp v2.8h, v0.16b", single)
BENCH(saddlp_2s, "saddlp v2.2s, v0.4h", single)
BENCH(saddlp_4s, "saddlp v2.4s, v0.8h", single)
BENCH(saddlp_1d, "saddlp v2.1d, v0.2s", single)
BENCH(saddlp_2d, "saddlp v2.2d, v0.4s", single)
BENCH(uadalp_4h, "uadalp v2.4h, v0.8b", single)
BENCH(uadalp_8h, "uadalp v2.8h, v0.16b", single)
BENCH(uadalp_2s, "uadalp v2.2s, v0.4h", single)
BENCH(uadalp_4s, "uadalp v2.4s, v0.8h", single)
BENCH(uadalp_1d, "uadalp v2.1d, v0.2s", single)
BENCH(uadalp_2d, "uadalp v2.2d, v0.4s", single)
BENCH(sadalp_4h, "sadalp v2.4h, v0.8b", single)
BENCH(sadalp_8h, "sadalp v2.8h, v0.16b", single)
BENCH(sadalp_2s, "sadalp v2.2s, v0.4h", single)
BENCH(sadalp_4s, "sadalp v2.4s, v0.8h", single)
BENCH(sadalp_1d, "sadalp v2.1d, v0.2s", single)
BENCH(sadalp_2d, "sadalp v2.2d, v0.4s", single)
BENCH(faddp_4s, "faddp v2.4s, v0.4s, v1.4s", single)
BENCH(faddp_2s, "faddp v2.2s, v0.2s, v1.2s", single)
BENCH(faddp_s, "faddp s2, v0.2s", single)
BENCH(faddp_2d, "faddp v2.2d, v0.2d, v1.2d", double)
BENCH(faddp_d, "faddp d2, v0.2d", double)
BENCH(fmaxv_4s, "fmaxv s2, v0.4s", single)
BENCH(fminv_4s, "fminv s2, v0.4s", single)
BENCH(fmaxnmv_4s, "fmaxnmv s2, v0.4s", single)
BENCH(fminnmv_4s, "fminnmv s2, v0.4s", single)
BENCH(fdiv_2s, "fdiv v2.2s, v0.2s, v1.2s", single)
BENCH(fdiv_4s, "fdiv v2.4s, v0.4s, v1.4s", single)
BENCH(fcmeq_zero_2s, "fcmeq v2.2s, v0.2s, #0", single)
BENCH(fcmeq_zero_4s, "fcmeq v2.4s, v0.4s, #0", single)
BENCH(fcmge_zero_2s, "fcmge v2.2s, v0.2s, #0", single)
BENCH(fcmge_zero_4s, "fcmge v2.4s, v0.4s, #0", single)
BENCH(fcmgt_zero_2s, "fcmgt v2.2s, v0.2s, #0", single)
BENCH(fcmgt_zero_4s, "fcmgt v2.4s, v0.4s, #0", single)
BENCH(fcmle_zero_2s, "fcmle v2.2s, v0.2s, #0", single)
BENCH(fcmle_zero_4s, "fcmle v2.4s, v0.4s, #0", single)
BENCH(fcmlt_zero_2s, "fcmlt v2.2s, v0.2s, #0", single)
BENCH(fcmlt_zero_4s, "fcmlt v2.4s, v0.4s, #0", single)
BENCH(ushl_2s, "ushl v2.2s, v0.2s, v1.2s", shift32)
BENCH(ushl_4s, "ushl v2.4s, v0.4s, v1.4s", shift32)
BENCH(ushl_2d, "ushl v2.2d, v0.2d, v1.2d", shift64)
BENCH(sshl_2s, "sshl v2.2s, v0.2s, v1.2s", shift32)
BENCH(sshl_4s, "sshl v2.4s, v0.4s, v1.4s", shift32)
BENCH(sshl_2d, "sshl v2.2d, v0.2d, v1.2d", shift64)
BENCH(fmul_s0, "fmul s2, s0, v1.s[0]", single)
BENCH(fmul_s1, "fmul s2, s0, v1.s[1]", single)
BENCH(fmul_s2, "fmul s2, s0, v1.s[2]", single)
BENCH(fmul_s3, "fmul s2, s0, v1.s[3]", single)
BENCH(fmla_s0, "fmla s2, s0, v1.s[0]", single)
BENCH(fmla_s1, "fmla s2, s0, v1.s[1]", single)
BENCH(fmla_s2, "fmla s2, s0, v1.s[2]", single)
BENCH(fmla_s3, "fmla s2, s0, v1.s[3]", single)
BENCH(fmls_s0, "fmls s2, s0, v1.s[0]", single)
BENCH(fmls_s1, "fmls s2, s0, v1.s[1]", single)
BENCH(fmls_s2, "fmls s2, s0, v1.s[2]", single)
BENCH(fmls_s3, "fmls s2, s0, v1.s[3]", single)
BENCH(fmul_d0, "fmul d2, d0, v1.d[0]", double)
BENCH(fmul_d1, "fmul d2, d0, v1.d[1]", double)
BENCH(tail_fmadd, "fmadd s2, s0, s1, s0", tail)
BENCH(tail_fmsub, "fmsub s2, s0, s1, s0", tail)
BENCH(tail_fnmadd, "fnmadd s2, s0, s1, s0", tail)
BENCH(tail_fnmsub, "fnmsub s2, s0, s1, s0", tail)
BENCH(tail_fnmul, "fnmul s2, s0, s1", tail)
BENCH(tail_frecpss, "frecps s2, s0, s1", tail)
BENCH(tail_frecps2s, "frecps v2.2s, v0.2s, v1.2s", tail)
BENCH(tail_frecps4s, "frecps v2.4s, v0.4s, v1.4s", tail)
BENCH(tail_frsqrtss, "frsqrts s2, s0, s1", tail)
BENCH(tail_frsqrts2s, "frsqrts v2.2s, v0.2s, v1.2s", tail)
BENCH(tail_frsqrts4s, "frsqrts v2.4s, v0.4s, v1.4s", tail)
BENCH(tail_saddl0_0, "saddl v2.8h, v0.8b, v1.8b", single)
BENCH(tail_saddl0_1, "saddl v2.4s, v0.4h, v1.4h", single)
BENCH(tail_saddl0_2, "saddl v2.2d, v0.2s, v1.2s", single)
BENCH(tail_saddl1_0, "saddl2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_saddl1_1, "saddl2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_saddl1_2, "saddl2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_uaddl0_0, "uaddl v2.8h, v0.8b, v1.8b", single)
BENCH(tail_uaddl0_1, "uaddl v2.4s, v0.4h, v1.4h", single)
BENCH(tail_uaddl0_2, "uaddl v2.2d, v0.2s, v1.2s", single)
BENCH(tail_uaddl1_0, "uaddl2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_uaddl1_1, "uaddl2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_uaddl1_2, "uaddl2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_saddw0_0, "saddw v2.8h, v0.8h, v1.8b", single)
BENCH(tail_saddw0_1, "saddw v2.4s, v0.4s, v1.4h", single)
BENCH(tail_saddw0_2, "saddw v2.2d, v0.2d, v1.2s", single)
BENCH(tail_saddw1_0, "saddw2 v2.8h, v0.8h, v1.16b", single)
BENCH(tail_saddw1_1, "saddw2 v2.4s, v0.4s, v1.8h", single)
BENCH(tail_saddw1_2, "saddw2 v2.2d, v0.2d, v1.4s", single)
BENCH(tail_uaddw0_0, "uaddw v2.8h, v0.8h, v1.8b", single)
BENCH(tail_uaddw0_1, "uaddw v2.4s, v0.4s, v1.4h", single)
BENCH(tail_uaddw0_2, "uaddw v2.2d, v0.2d, v1.2s", single)
BENCH(tail_uaddw1_0, "uaddw2 v2.8h, v0.8h, v1.16b", single)
BENCH(tail_uaddw1_1, "uaddw2 v2.4s, v0.4s, v1.8h", single)
BENCH(tail_uaddw1_2, "uaddw2 v2.2d, v0.2d, v1.4s", single)
BENCH(tail_ssubl0_0, "ssubl v2.8h, v0.8b, v1.8b", single)
BENCH(tail_ssubl0_1, "ssubl v2.4s, v0.4h, v1.4h", single)
BENCH(tail_ssubl0_2, "ssubl v2.2d, v0.2s, v1.2s", single)
BENCH(tail_ssubl1_0, "ssubl2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_ssubl1_1, "ssubl2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_ssubl1_2, "ssubl2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_usubl0_0, "usubl v2.8h, v0.8b, v1.8b", single)
BENCH(tail_usubl0_1, "usubl v2.4s, v0.4h, v1.4h", single)
BENCH(tail_usubl0_2, "usubl v2.2d, v0.2s, v1.2s", single)
BENCH(tail_usubl1_0, "usubl2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_usubl1_1, "usubl2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_usubl1_2, "usubl2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_ssubw0_0, "ssubw v2.8h, v0.8h, v1.8b", single)
BENCH(tail_ssubw0_1, "ssubw v2.4s, v0.4s, v1.4h", single)
BENCH(tail_ssubw0_2, "ssubw v2.2d, v0.2d, v1.2s", single)
BENCH(tail_ssubw1_0, "ssubw2 v2.8h, v0.8h, v1.16b", single)
BENCH(tail_ssubw1_1, "ssubw2 v2.4s, v0.4s, v1.8h", single)
BENCH(tail_ssubw1_2, "ssubw2 v2.2d, v0.2d, v1.4s", single)
BENCH(tail_usubw0_0, "usubw v2.8h, v0.8h, v1.8b", single)
BENCH(tail_usubw0_1, "usubw v2.4s, v0.4s, v1.4h", single)
BENCH(tail_usubw0_2, "usubw v2.2d, v0.2d, v1.2s", single)
BENCH(tail_usubw1_0, "usubw2 v2.8h, v0.8h, v1.16b", single)
BENCH(tail_usubw1_1, "usubw2 v2.4s, v0.4s, v1.8h", single)
BENCH(tail_usubw1_2, "usubw2 v2.2d, v0.2d, v1.4s", single)
BENCH(tail_smull0_0, "smull v2.8h, v0.8b, v1.8b", single)
BENCH(tail_smull0_1, "smull v2.4s, v0.4h, v1.4h", single)
BENCH(tail_smull0_2, "smull v2.2d, v0.2s, v1.2s", single)
BENCH(tail_smull1_0, "smull2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_smull1_1, "smull2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_smull1_2, "smull2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_umull0_0, "umull v2.8h, v0.8b, v1.8b", single)
BENCH(tail_umull0_1, "umull v2.4s, v0.4h, v1.4h", single)
BENCH(tail_umull0_2, "umull v2.2d, v0.2s, v1.2s", single)
BENCH(tail_umull1_0, "umull2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_umull1_1, "umull2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_umull1_2, "umull2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_smlal0_0, "smlal v2.8h, v0.8b, v1.8b", single)
BENCH(tail_smlal0_1, "smlal v2.4s, v0.4h, v1.4h", single)
BENCH(tail_smlal0_2, "smlal v2.2d, v0.2s, v1.2s", single)
BENCH(tail_smlal1_0, "smlal2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_smlal1_1, "smlal2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_smlal1_2, "smlal2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_umlal0_0, "umlal v2.8h, v0.8b, v1.8b", single)
BENCH(tail_umlal0_1, "umlal v2.4s, v0.4h, v1.4h", single)
BENCH(tail_umlal0_2, "umlal v2.2d, v0.2s, v1.2s", single)
BENCH(tail_umlal1_0, "umlal2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_umlal1_1, "umlal2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_umlal1_2, "umlal2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_smlsl0_0, "smlsl v2.8h, v0.8b, v1.8b", single)
BENCH(tail_smlsl0_1, "smlsl v2.4s, v0.4h, v1.4h", single)
BENCH(tail_smlsl0_2, "smlsl v2.2d, v0.2s, v1.2s", single)
BENCH(tail_smlsl1_0, "smlsl2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_smlsl1_1, "smlsl2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_smlsl1_2, "smlsl2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_umlsl0_0, "umlsl v2.8h, v0.8b, v1.8b", single)
BENCH(tail_umlsl0_1, "umlsl v2.4s, v0.4h, v1.4h", single)
BENCH(tail_umlsl0_2, "umlsl v2.2d, v0.2s, v1.2s", single)
BENCH(tail_umlsl1_0, "umlsl2 v2.8h, v0.16b, v1.16b", single)
BENCH(tail_umlsl1_1, "umlsl2 v2.4s, v0.8h, v1.8h", single)
BENCH(tail_umlsl1_2, "umlsl2 v2.2d, v0.4s, v1.4s", single)
BENCH(tail_sqxtn0_0, "sqxtn v2.8b, v0.8h", single)
BENCH(tail_sqxtn0_1, "sqxtn v2.4h, v0.4s", single)
BENCH(tail_sqxtn0_2, "sqxtn v2.2s, v0.2d", single)
BENCH(tail_sqxtn1_0, "sqxtn2 v2.16b, v0.8h", single)
BENCH(tail_sqxtn1_1, "sqxtn2 v2.8h, v0.4s", single)
BENCH(tail_sqxtn1_2, "sqxtn2 v2.4s, v0.2d", single)
BENCH(tail_uqxtn0_0, "uqxtn v2.8b, v0.8h", single)
BENCH(tail_uqxtn0_1, "uqxtn v2.4h, v0.4s", single)
BENCH(tail_uqxtn0_2, "uqxtn v2.2s, v0.2d", single)
BENCH(tail_uqxtn1_0, "uqxtn2 v2.16b, v0.8h", single)
BENCH(tail_uqxtn1_1, "uqxtn2 v2.8h, v0.4s", single)
BENCH(tail_uqxtn1_2, "uqxtn2 v2.4s, v0.2d", single)
BENCH(tail_sqxtun0_0, "sqxtun v2.8b, v0.8h", single)
BENCH(tail_sqxtun0_1, "sqxtun v2.4h, v0.4s", single)
BENCH(tail_sqxtun0_2, "sqxtun v2.2s, v0.2d", single)
BENCH(tail_sqxtun1_0, "sqxtun2 v2.16b, v0.8h", single)
BENCH(tail_sqxtun1_1, "sqxtun2 v2.8h, v0.4s", single)
BENCH(tail_sqxtun1_2, "sqxtun2 v2.4s, v0.2d", single)
BENCH(tail_shll0_0, "shll v2.8h, v0.8b, #8", single)
BENCH(tail_shll0_1, "shll v2.4s, v0.4h, #16", single)
BENCH(tail_shll0_2, "shll v2.2d, v0.2s, #32", single)
BENCH(tail_shll1_0, "shll2 v2.8h, v0.16b, #8", single)
BENCH(tail_shll1_1, "shll2 v2.4s, v0.8h, #16", single)
BENCH(tail_shll1_2, "shll2 v2.2d, v0.4s, #32", single)
BENCH(tail_sri0_0_1, "sri v2.8b, v0.8b, #1", single)
BENCH(tail_sri0_0_4, "sri v2.8b, v0.8b, #4", single)
BENCH(tail_sri0_0_8, "sri v2.8b, v0.8b, #8", single)
BENCH(tail_sli0_0_0, "sli v2.8b, v0.8b, #0", single)
BENCH(tail_sli0_0_4, "sli v2.8b, v0.8b, #4", single)
BENCH(tail_sli0_0_7, "sli v2.8b, v0.8b, #7", single)
BENCH(tail_rev160_0, "rev16 v2.8b, v0.8b", single)
BENCH(tail_rev320_0, "rev32 v2.8b, v0.8b", single)
BENCH(tail_rev640_0, "rev64 v2.8b, v0.8b", single)
BENCH(tail_sri0_1_1, "sri v2.4h, v0.4h, #1", single)
BENCH(tail_sri0_1_8, "sri v2.4h, v0.4h, #8", single)
BENCH(tail_sri0_1_16, "sri v2.4h, v0.4h, #16", single)
BENCH(tail_sli0_1_0, "sli v2.4h, v0.4h, #0", single)
BENCH(tail_sli0_1_8, "sli v2.4h, v0.4h, #8", single)
BENCH(tail_sli0_1_15, "sli v2.4h, v0.4h, #15", single)
BENCH(tail_rev320_1, "rev32 v2.4h, v0.4h", single)
BENCH(tail_rev640_1, "rev64 v2.4h, v0.4h", single)
BENCH(tail_sqdmulh0_1, "sqdmulh v2.4h, v0.4h, v1.4h", single)
BENCH(tail_sqdmulh0_1_0, "sqdmulh v2.4h, v0.4h, v1.h[0]", single)
BENCH(tail_sqdmulh0_1_1, "sqdmulh v2.4h, v0.4h, v1.h[1]", single)
BENCH(tail_sqdmulh0_1_2, "sqdmulh v2.4h, v0.4h, v1.h[2]", single)
BENCH(tail_sqdmulh0_1_3, "sqdmulh v2.4h, v0.4h, v1.h[3]", single)
BENCH(tail_sqdmulh0_1_4, "sqdmulh v2.4h, v0.4h, v1.h[4]", single)
BENCH(tail_sqdmulh0_1_5, "sqdmulh v2.4h, v0.4h, v1.h[5]", single)
BENCH(tail_sqdmulh0_1_6, "sqdmulh v2.4h, v0.4h, v1.h[6]", single)
BENCH(tail_sqdmulh0_1_7, "sqdmulh v2.4h, v0.4h, v1.h[7]", single)
BENCH(tail_sqrdmulh0_1, "sqrdmulh v2.4h, v0.4h, v1.4h", single)
BENCH(tail_sqrdmulh0_1_0, "sqrdmulh v2.4h, v0.4h, v1.h[0]", single)
BENCH(tail_sqrdmulh0_1_1, "sqrdmulh v2.4h, v0.4h, v1.h[1]", single)
BENCH(tail_sqrdmulh0_1_2, "sqrdmulh v2.4h, v0.4h, v1.h[2]", single)
BENCH(tail_sqrdmulh0_1_3, "sqrdmulh v2.4h, v0.4h, v1.h[3]", single)
BENCH(tail_sqrdmulh0_1_4, "sqrdmulh v2.4h, v0.4h, v1.h[4]", single)
BENCH(tail_sqrdmulh0_1_5, "sqrdmulh v2.4h, v0.4h, v1.h[5]", single)
BENCH(tail_sqrdmulh0_1_6, "sqrdmulh v2.4h, v0.4h, v1.h[6]", single)
BENCH(tail_sqrdmulh0_1_7, "sqrdmulh v2.4h, v0.4h, v1.h[7]", single)
BENCH(tail_sri0_2_1, "sri v2.2s, v0.2s, #1", single)
BENCH(tail_sri0_2_16, "sri v2.2s, v0.2s, #16", single)
BENCH(tail_sri0_2_32, "sri v2.2s, v0.2s, #32", single)
BENCH(tail_sli0_2_0, "sli v2.2s, v0.2s, #0", single)
BENCH(tail_sli0_2_16, "sli v2.2s, v0.2s, #16", single)
BENCH(tail_sli0_2_31, "sli v2.2s, v0.2s, #31", single)
BENCH(tail_rev640_2, "rev64 v2.2s, v0.2s", single)
BENCH(tail_sqdmulh0_2, "sqdmulh v2.2s, v0.2s, v1.2s", single)
BENCH(tail_sqdmulh0_2_0, "sqdmulh v2.2s, v0.2s, v1.s[0]", single)
BENCH(tail_sqdmulh0_2_1, "sqdmulh v2.2s, v0.2s, v1.s[1]", single)
BENCH(tail_sqdmulh0_2_2, "sqdmulh v2.2s, v0.2s, v1.s[2]", single)
BENCH(tail_sqdmulh0_2_3, "sqdmulh v2.2s, v0.2s, v1.s[3]", single)
BENCH(tail_sqrdmulh0_2, "sqrdmulh v2.2s, v0.2s, v1.2s", single)
BENCH(tail_sqrdmulh0_2_0, "sqrdmulh v2.2s, v0.2s, v1.s[0]", single)
BENCH(tail_sqrdmulh0_2_1, "sqrdmulh v2.2s, v0.2s, v1.s[1]", single)
BENCH(tail_sqrdmulh0_2_2, "sqrdmulh v2.2s, v0.2s, v1.s[2]", single)
BENCH(tail_sqrdmulh0_2_3, "sqrdmulh v2.2s, v0.2s, v1.s[3]", single)
BENCH(tail_sri1_0_1, "sri v2.16b, v0.16b, #1", single)
BENCH(tail_sri1_0_4, "sri v2.16b, v0.16b, #4", single)
BENCH(tail_sri1_0_8, "sri v2.16b, v0.16b, #8", single)
BENCH(tail_sli1_0_0, "sli v2.16b, v0.16b, #0", single)
BENCH(tail_sli1_0_4, "sli v2.16b, v0.16b, #4", single)
BENCH(tail_sli1_0_7, "sli v2.16b, v0.16b, #7", single)
BENCH(tail_rev161_0, "rev16 v2.16b, v0.16b", single)
BENCH(tail_rev321_0, "rev32 v2.16b, v0.16b", single)
BENCH(tail_rev641_0, "rev64 v2.16b, v0.16b", single)
BENCH(tail_sri1_1_1, "sri v2.8h, v0.8h, #1", single)
BENCH(tail_sri1_1_8, "sri v2.8h, v0.8h, #8", single)
BENCH(tail_sri1_1_16, "sri v2.8h, v0.8h, #16", single)
BENCH(tail_sli1_1_0, "sli v2.8h, v0.8h, #0", single)
BENCH(tail_sli1_1_8, "sli v2.8h, v0.8h, #8", single)
BENCH(tail_sli1_1_15, "sli v2.8h, v0.8h, #15", single)
BENCH(tail_rev321_1, "rev32 v2.8h, v0.8h", single)
BENCH(tail_rev641_1, "rev64 v2.8h, v0.8h", single)
BENCH(tail_sqdmulh1_1, "sqdmulh v2.8h, v0.8h, v1.8h", single)
BENCH(tail_sqdmulh1_1_0, "sqdmulh v2.8h, v0.8h, v1.h[0]", single)
BENCH(tail_sqdmulh1_1_1, "sqdmulh v2.8h, v0.8h, v1.h[1]", single)
BENCH(tail_sqdmulh1_1_2, "sqdmulh v2.8h, v0.8h, v1.h[2]", single)
BENCH(tail_sqdmulh1_1_3, "sqdmulh v2.8h, v0.8h, v1.h[3]", single)
BENCH(tail_sqdmulh1_1_4, "sqdmulh v2.8h, v0.8h, v1.h[4]", single)
BENCH(tail_sqdmulh1_1_5, "sqdmulh v2.8h, v0.8h, v1.h[5]", single)
BENCH(tail_sqdmulh1_1_6, "sqdmulh v2.8h, v0.8h, v1.h[6]", single)
BENCH(tail_sqdmulh1_1_7, "sqdmulh v2.8h, v0.8h, v1.h[7]", single)
BENCH(tail_sqrdmulh1_1, "sqrdmulh v2.8h, v0.8h, v1.8h", single)
BENCH(tail_sqrdmulh1_1_0, "sqrdmulh v2.8h, v0.8h, v1.h[0]", single)
BENCH(tail_sqrdmulh1_1_1, "sqrdmulh v2.8h, v0.8h, v1.h[1]", single)
BENCH(tail_sqrdmulh1_1_2, "sqrdmulh v2.8h, v0.8h, v1.h[2]", single)
BENCH(tail_sqrdmulh1_1_3, "sqrdmulh v2.8h, v0.8h, v1.h[3]", single)
BENCH(tail_sqrdmulh1_1_4, "sqrdmulh v2.8h, v0.8h, v1.h[4]", single)
BENCH(tail_sqrdmulh1_1_5, "sqrdmulh v2.8h, v0.8h, v1.h[5]", single)
BENCH(tail_sqrdmulh1_1_6, "sqrdmulh v2.8h, v0.8h, v1.h[6]", single)
BENCH(tail_sqrdmulh1_1_7, "sqrdmulh v2.8h, v0.8h, v1.h[7]", single)
BENCH(tail_sri1_2_1, "sri v2.4s, v0.4s, #1", single)
BENCH(tail_sri1_2_16, "sri v2.4s, v0.4s, #16", single)
BENCH(tail_sri1_2_32, "sri v2.4s, v0.4s, #32", single)
BENCH(tail_sli1_2_0, "sli v2.4s, v0.4s, #0", single)
BENCH(tail_sli1_2_16, "sli v2.4s, v0.4s, #16", single)
BENCH(tail_sli1_2_31, "sli v2.4s, v0.4s, #31", single)
BENCH(tail_rev641_2, "rev64 v2.4s, v0.4s", single)
BENCH(tail_sqdmulh1_2, "sqdmulh v2.4s, v0.4s, v1.4s", single)
BENCH(tail_sqdmulh1_2_0, "sqdmulh v2.4s, v0.4s, v1.s[0]", single)
BENCH(tail_sqdmulh1_2_1, "sqdmulh v2.4s, v0.4s, v1.s[1]", single)
BENCH(tail_sqdmulh1_2_2, "sqdmulh v2.4s, v0.4s, v1.s[2]", single)
BENCH(tail_sqdmulh1_2_3, "sqdmulh v2.4s, v0.4s, v1.s[3]", single)
BENCH(tail_sqrdmulh1_2, "sqrdmulh v2.4s, v0.4s, v1.4s", single)
BENCH(tail_sqrdmulh1_2_0, "sqrdmulh v2.4s, v0.4s, v1.s[0]", single)
BENCH(tail_sqrdmulh1_2_1, "sqrdmulh v2.4s, v0.4s, v1.s[1]", single)
BENCH(tail_sqrdmulh1_2_2, "sqrdmulh v2.4s, v0.4s, v1.s[2]", single)
BENCH(tail_sqrdmulh1_2_3, "sqrdmulh v2.4s, v0.4s, v1.s[3]", single)
BENCH(tail_sri1_3_1, "sri v2.2d, v0.2d, #1", single)
BENCH(tail_sri1_3_32, "sri v2.2d, v0.2d, #32", single)
BENCH(tail_sri1_3_64, "sri v2.2d, v0.2d, #64", single)
BENCH(tail_sli1_3_0, "sli v2.2d, v0.2d, #0", single)
BENCH(tail_sli1_3_32, "sli v2.2d, v0.2d, #32", single)
BENCH(tail_sli1_3_63, "sli v2.2d, v0.2d, #63", single)
BENCH(tail_addpd, "addp d2, v0.2d", single)




BENCH(tail_aese, "aese v2.16b, v0.16b", single)
BENCH(tail_aesd, "aesd v2.16b, v0.16b", single)
BENCH(tail_aesmc, "aesmc v2.16b, v0.16b", single)
BENCH(tail_aesimc, "aesimc v2.16b, v0.16b", single)
BENCH(wide_fadd, "fadd v2.2d, v0.2d, v1.2d", double)
BENCH(wide_fsub, "fsub v2.2d, v0.2d, v1.2d", double)
BENCH(wide_fmul, "fmul v2.2d, v0.2d, v1.2d", double)
BENCH(wide_fdiv, "fdiv v2.2d, v0.2d, v1.2d", double)
BENCH(wide_sqrtd, "fsqrt v2.2d, v0.2d", double)
BENCH(wide_sqrts, "fsqrt v2.4s, v0.4s", single)
BENCH(wide_sqrt2s, "fsqrt v2.2s, v0.2s", single)
/* Element-interleaved memory forms use a private 64-byte buffer. */
#define MEMBENCH(name, insn) \
static __attribute__((noinline)) vec name(unsigned long n) { \
    uint64_t data[8] = {1,2,3,4,5,6,7,8}; \
    vec out; \
    __asm__ volatile("movi v2.16b, #2\nmovi v3.16b, #3\nmovi v4.16b, #4\nmovi v5.16b, #5\n" \
        "1:\n.rept 16\n" insn "\n.endr\n" \
        "subs %[n], %[n], #1\nb.ne 1b\nstr q2, [%[out]]\n" \
        : [n] "+&r"(n) : [p] "r"(data), [out] "r"(&out) \
        : "v2", "v3", "v4", "v5", "cc", "memory"); \
    out.lo ^= data[0] ^ data[2] ^ data[4] ^ data[6]; \
    out.hi ^= data[1] ^ data[3] ^ data[5] ^ data[7]; \
    return out; \
}
MEMBENCH(ld2_8b, "ld2 {v2.8b, v3.8b}, [%[p]]")
MEMBENCH(ld2_16b, "ld2 {v2.16b, v3.16b}, [%[p]]")
MEMBENCH(ld2_4h, "ld2 {v2.4h, v3.4h}, [%[p]]")
MEMBENCH(ld2_8h, "ld2 {v2.8h, v3.8h}, [%[p]]")
MEMBENCH(ld2_2s, "ld2 {v2.2s, v3.2s}, [%[p]]")
MEMBENCH(ld2_4s, "ld2 {v2.4s, v3.4s}, [%[p]]")
MEMBENCH(ld2_2d, "ld2 {v2.2d, v3.2d}, [%[p]]")
MEMBENCH(ld3_8b, "ld3 {v2.8b, v3.8b, v4.8b}, [%[p]]")
MEMBENCH(ld3_16b, "ld3 {v2.16b, v3.16b, v4.16b}, [%[p]]")
MEMBENCH(ld3_4h, "ld3 {v2.4h, v3.4h, v4.4h}, [%[p]]")
MEMBENCH(ld3_8h, "ld3 {v2.8h, v3.8h, v4.8h}, [%[p]]")
MEMBENCH(ld3_2s, "ld3 {v2.2s, v3.2s, v4.2s}, [%[p]]")
MEMBENCH(ld3_4s, "ld3 {v2.4s, v3.4s, v4.4s}, [%[p]]")
MEMBENCH(ld3_2d, "ld3 {v2.2d, v3.2d, v4.2d}, [%[p]]")
MEMBENCH(ld4_8b, "ld4 {v2.8b, v3.8b, v4.8b, v5.8b}, [%[p]]")
MEMBENCH(ld4_16b, "ld4 {v2.16b, v3.16b, v4.16b, v5.16b}, [%[p]]")
MEMBENCH(ld4_4h, "ld4 {v2.4h, v3.4h, v4.4h, v5.4h}, [%[p]]")
MEMBENCH(ld4_8h, "ld4 {v2.8h, v3.8h, v4.8h, v5.8h}, [%[p]]")
MEMBENCH(ld4_2s, "ld4 {v2.2s, v3.2s, v4.2s, v5.2s}, [%[p]]")
MEMBENCH(ld4_4s, "ld4 {v2.4s, v3.4s, v4.4s, v5.4s}, [%[p]]")
MEMBENCH(ld4_2d, "ld4 {v2.2d, v3.2d, v4.2d, v5.2d}, [%[p]]")
MEMBENCH(st2_8b, "st2 {v2.8b, v3.8b}, [%[p]]")
MEMBENCH(st2_16b, "st2 {v2.16b, v3.16b}, [%[p]]")
MEMBENCH(st2_4h, "st2 {v2.4h, v3.4h}, [%[p]]")
MEMBENCH(st2_8h, "st2 {v2.8h, v3.8h}, [%[p]]")
MEMBENCH(st2_2s, "st2 {v2.2s, v3.2s}, [%[p]]")
MEMBENCH(st2_4s, "st2 {v2.4s, v3.4s}, [%[p]]")
MEMBENCH(st2_2d, "st2 {v2.2d, v3.2d}, [%[p]]")
MEMBENCH(st3_8b, "st3 {v2.8b, v3.8b, v4.8b}, [%[p]]")
MEMBENCH(st3_16b, "st3 {v2.16b, v3.16b, v4.16b}, [%[p]]")
MEMBENCH(st3_4h, "st3 {v2.4h, v3.4h, v4.4h}, [%[p]]")
MEMBENCH(st3_8h, "st3 {v2.8h, v3.8h, v4.8h}, [%[p]]")
MEMBENCH(st3_2s, "st3 {v2.2s, v3.2s, v4.2s}, [%[p]]")
MEMBENCH(st3_4s, "st3 {v2.4s, v3.4s, v4.4s}, [%[p]]")
MEMBENCH(st3_2d, "st3 {v2.2d, v3.2d, v4.2d}, [%[p]]")
MEMBENCH(st4_8b, "st4 {v2.8b, v3.8b, v4.8b, v5.8b}, [%[p]]")
MEMBENCH(st4_16b, "st4 {v2.16b, v3.16b, v4.16b, v5.16b}, [%[p]]")
MEMBENCH(st4_4h, "st4 {v2.4h, v3.4h, v4.4h, v5.4h}, [%[p]]")
MEMBENCH(st4_8h, "st4 {v2.8h, v3.8h, v4.8h, v5.8h}, [%[p]]")
MEMBENCH(st4_2s, "st4 {v2.2s, v3.2s, v4.2s, v5.2s}, [%[p]]")
MEMBENCH(st4_4s, "st4 {v2.4s, v3.4s, v4.4s, v5.4s}, [%[p]]")
MEMBENCH(st4_2d, "st4 {v2.2d, v3.2d, v4.2d, v5.2d}, [%[p]]")

BENCH(tail_fccmp_s, "fccmp s0, s1, #0, al", single)
BENCH(tail_fccmpe_s, "fccmpe s0, s1, #0, al", single)
BENCH(tail_fccmp_d, "fccmp d0, d1, #0, al", double)
BENCH(tail_fccmpe_d, "fccmpe d0, d1, #0, al", double)
BENCH(tail_mrs_fpcr, "mrs x9, fpcr\nfmov d2, x9", single)
BENCH(tail_msr_fpcr, "msr fpcr, xzr", single)
BENCH(tail_mrs_fpsr, "msr fpsr, xzr\nmrs x9, fpsr\nfmov d2, x9", single)
BENCH(tail_msr_fpsr, "msr fpsr, xzr", single)

BENCH(tail_frecpe_2s, "frecpe v2.2s, v0.2s", single)
BENCH(tail_frecpe_4s, "frecpe v2.4s, v0.4s", single)
BENCH(tail_frecpe_2d, "frecpe v2.2d, v0.2d", double)
BENCH(tail_frecpe_s, "frecpe s2, s0", single)
BENCH(tail_frecpe_d, "frecpe d2, d0", double)
BENCH(tail_frsqrte_2s, "frsqrte v2.2s, v0.2s", single)
BENCH(tail_frsqrte_4s, "frsqrte v2.4s, v0.4s", single)
BENCH(tail_frsqrte_2d, "frsqrte v2.2d, v0.2d", double)
BENCH(tail_frsqrte_s, "frsqrte s2, s0", single)
BENCH(tail_frsqrte_d, "frsqrte d2, d0", double)

int main(int argc, char **argv)
{
    unsigned long n = argc > 1 ? strtoul(argv[1], 0, 0) : 50000;
    if (!n) return 2;
#define RUN(name) do { if (argc > 2 && strncmp(#name, argv[2], strlen(argv[2]))) break; uint64_t start = now(); vec v = name(n); uint64_t elapsed = now() - start; \
    printf("%-18s %016llx:%016llx\n", #name, (unsigned long long)v.hi, (unsigned long long)v.lo); \
    fprintf(stderr, "%-18s %.6f ns/op\n", #name, (double)elapsed / (16.0 * n)); } while (0)
    RUN(f16_fmla_4h_tinyproduct);
    RUN(f16_fmls_4h_tinyproduct);
    RUN(f16_fmla_8h_tinyproduct);
    RUN(f16_fmls_8h_tinyproduct);
    RUN(f16_fmla_4h_normal);
    RUN(f16_fmla_4h_subnormal);
    RUN(f16_fmla_4h_wide);
    RUN(f16_fmla_4h_wide_rp);
    RUN(f16_fmla_4h_fz16);
    RUN(f16_fmla_4h_nan);
    RUN(f16_fmls_4h_normal);
    RUN(f16_fmls_4h_subnormal);
    RUN(f16_fmls_4h_wide);
    RUN(f16_fmls_4h_wide_rp);
    RUN(f16_fmls_4h_fz16);
    RUN(f16_fmls_4h_nan);
    RUN(f16_fmla_8h_normal);
    RUN(f16_fmla_8h_subnormal);
    RUN(f16_fmla_8h_wide);
    RUN(f16_fmla_8h_wide_rp);
    RUN(f16_fmla_8h_fz16);
    RUN(f16_fmla_8h_nan);
    RUN(f16_fmls_8h_normal);
    RUN(f16_fmls_8h_subnormal);
    RUN(f16_fmls_8h_wide);
    RUN(f16_fmls_8h_wide_rp);
    RUN(f16_fmls_8h_fz16);
    RUN(f16_fmls_8h_nan);
    RUN(tail_fmadd); RUN(tail_fmsub); RUN(tail_fnmadd); RUN(tail_fnmsub); RUN(tail_fnmul); RUN(tail_frecpss); RUN(tail_frecps2s); RUN(tail_frecps4s); RUN(tail_frsqrtss); RUN(tail_frsqrts2s); RUN(tail_frsqrts4s);

    RUN(nar_shrn_v8_0_1);
    RUN(nar_shrn_v8_0_8);
    RUN(nar_shrn_v8_1_1);
    RUN(nar_shrn_v8_1_8);
    RUN(nar_shrn_v16_0_1);
    RUN(nar_shrn_v16_0_16);
    RUN(nar_shrn_v16_1_1);
    RUN(nar_shrn_v16_1_16);
    RUN(nar_shrn_v32_0_1);
    RUN(nar_shrn_v32_0_32);
    RUN(nar_shrn_v32_1_1);
    RUN(nar_shrn_v32_1_32);
    RUN(nar_rshrn_v8_0_1);
    RUN(nar_rshrn_v8_0_8);
    RUN(nar_rshrn_v8_1_1);
    RUN(nar_rshrn_v8_1_8);
    RUN(nar_rshrn_v16_0_1);
    RUN(nar_rshrn_v16_0_16);
    RUN(nar_rshrn_v16_1_1);
    RUN(nar_rshrn_v16_1_16);
    RUN(nar_rshrn_v32_0_1);
    RUN(nar_rshrn_v32_0_32);
    RUN(nar_rshrn_v32_1_1);
    RUN(nar_rshrn_v32_1_32);
    RUN(nar_sqshrun_v8_0_1);
    RUN(nar_sqshrun_v8_0_8);
    RUN(nar_sqshrun_v8_1_1);
    RUN(nar_sqshrun_v8_1_8);
    RUN(nar_sqshrun_v16_0_1);
    RUN(nar_sqshrun_v16_0_16);
    RUN(nar_sqshrun_v16_1_1);
    RUN(nar_sqshrun_v16_1_16);
    RUN(nar_sqshrun_v32_0_1);
    RUN(nar_sqshrun_v32_0_32);
    RUN(nar_sqshrun_v32_1_1);
    RUN(nar_sqshrun_v32_1_32);
    RUN(nar_sqrshrun_v8_0_1);
    RUN(nar_sqrshrun_v8_0_8);
    RUN(nar_sqrshrun_v8_1_1);
    RUN(nar_sqrshrun_v8_1_8);
    RUN(nar_sqrshrun_v16_0_1);
    RUN(nar_sqrshrun_v16_0_16);
    RUN(nar_sqrshrun_v16_1_1);
    RUN(nar_sqrshrun_v16_1_16);
    RUN(nar_sqrshrun_v32_0_1);
    RUN(nar_sqrshrun_v32_0_32);
    RUN(nar_sqrshrun_v32_1_1);
    RUN(nar_sqrshrun_v32_1_32);
    RUN(nar_sqshrn_v8_0_1);
    RUN(nar_sqshrn_v8_0_8);
    RUN(nar_sqshrn_v8_1_1);
    RUN(nar_sqshrn_v8_1_8);
    RUN(nar_sqshrn_v16_0_1);
    RUN(nar_sqshrn_v16_0_16);
    RUN(nar_sqshrn_v16_1_1);
    RUN(nar_sqshrn_v16_1_16);
    RUN(nar_sqshrn_v32_0_1);
    RUN(nar_sqshrn_v32_0_32);
    RUN(nar_sqshrn_v32_1_1);
    RUN(nar_sqshrn_v32_1_32);
    RUN(nar_sqrshrn_v8_0_1);
    RUN(nar_sqrshrn_v8_0_8);
    RUN(nar_sqrshrn_v8_1_1);
    RUN(nar_sqrshrn_v8_1_8);
    RUN(nar_sqrshrn_v16_0_1);
    RUN(nar_sqrshrn_v16_0_16);
    RUN(nar_sqrshrn_v16_1_1);
    RUN(nar_sqrshrn_v16_1_16);
    RUN(nar_sqrshrn_v32_0_1);
    RUN(nar_sqrshrn_v32_0_32);
    RUN(nar_sqrshrn_v32_1_1);
    RUN(nar_sqrshrn_v32_1_32);
    RUN(nar_uqshrn_v8_0_1);
    RUN(nar_uqshrn_v8_0_8);
    RUN(nar_uqshrn_v8_1_1);
    RUN(nar_uqshrn_v8_1_8);
    RUN(nar_uqshrn_v16_0_1);
    RUN(nar_uqshrn_v16_0_16);
    RUN(nar_uqshrn_v16_1_1);
    RUN(nar_uqshrn_v16_1_16);
    RUN(nar_uqshrn_v32_0_1);
    RUN(nar_uqshrn_v32_0_32);
    RUN(nar_uqshrn_v32_1_1);
    RUN(nar_uqshrn_v32_1_32);
    RUN(nar_uqrshrn_v8_0_1);
    RUN(nar_uqrshrn_v8_0_8);
    RUN(nar_uqrshrn_v8_1_1);
    RUN(nar_uqrshrn_v8_1_8);
    RUN(nar_uqrshrn_v16_0_1);
    RUN(nar_uqrshrn_v16_0_16);
    RUN(nar_uqrshrn_v16_1_1);
    RUN(nar_uqrshrn_v16_1_16);
    RUN(nar_uqrshrn_v32_0_1);
    RUN(nar_uqrshrn_v32_0_32);
    RUN(nar_uqrshrn_v32_1_1);
    RUN(nar_uqrshrn_v32_1_32);
    RUN(nar_sqshrun_s8_0_1);
    RUN(nar_sqshrun_s8_0_8);
    RUN(nar_sqshrun_s16_0_1);
    RUN(nar_sqshrun_s16_0_16);
    RUN(nar_sqshrun_s32_0_1);
    RUN(nar_sqshrun_s32_0_32);
    RUN(nar_sqrshrun_s8_0_1);
    RUN(nar_sqrshrun_s8_0_8);
    RUN(nar_sqrshrun_s16_0_1);
    RUN(nar_sqrshrun_s16_0_16);
    RUN(nar_sqrshrun_s32_0_1);
    RUN(nar_sqrshrun_s32_0_32);
    RUN(nar_sqshrn_s8_0_1);
    RUN(nar_sqshrn_s8_0_8);
    RUN(nar_sqshrn_s16_0_1);
    RUN(nar_sqshrn_s16_0_16);
    RUN(nar_sqshrn_s32_0_1);
    RUN(nar_sqshrn_s32_0_32);
    RUN(nar_sqrshrn_s8_0_1);
    RUN(nar_sqrshrn_s8_0_8);
    RUN(nar_sqrshrn_s16_0_1);
    RUN(nar_sqrshrn_s16_0_16);
    RUN(nar_sqrshrn_s32_0_1);
    RUN(nar_sqrshrn_s32_0_32);
    RUN(nar_uqshrn_s8_0_1);
    RUN(nar_uqshrn_s8_0_8);
    RUN(nar_uqshrn_s16_0_1);
    RUN(nar_uqshrn_s16_0_16);
    RUN(nar_uqshrn_s32_0_1);
    RUN(nar_uqshrn_s32_0_32);
    RUN(nar_uqrshrn_s8_0_1);
    RUN(nar_uqrshrn_s8_0_8);
    RUN(nar_uqrshrn_s16_0_1);
    RUN(nar_uqrshrn_s16_0_16);
    RUN(nar_uqrshrn_s32_0_1);
    RUN(nar_uqrshrn_s32_0_32);
    RUN(abd2s); RUN(abd4s); RUN(abd2d); RUN(abds); RUN(abdd);
    RUN(sqadd_8b);
    RUN(sqadd_16b);
    RUN(sqadd_4h);
    RUN(sqadd_8h);
    RUN(sqadd_2s);
    RUN(sqadd_4s);
    RUN(sqadd_2d);
    RUN(sqadd_b);
    RUN(sqadd_h);
    RUN(sqadd_s);
    RUN(sqadd_d);
    RUN(uqadd_8b);
    RUN(uqadd_16b);
    RUN(uqadd_4h);
    RUN(uqadd_8h);
    RUN(uqadd_2s);
    RUN(uqadd_4s);
    RUN(uqadd_2d);
    RUN(uqadd_b);
    RUN(uqadd_h);
    RUN(uqadd_s);
    RUN(uqadd_d);
    RUN(sqsub_8b);
    RUN(sqsub_16b);
    RUN(sqsub_4h);
    RUN(sqsub_8h);
    RUN(sqsub_2s);
    RUN(sqsub_4s);
    RUN(sqsub_2d);
    RUN(sqsub_b);
    RUN(sqsub_h);
    RUN(sqsub_s);
    RUN(sqsub_d);
    RUN(uqsub_8b);
    RUN(uqsub_16b);
    RUN(uqsub_4h);
    RUN(uqsub_8h);
    RUN(uqsub_2s);
    RUN(uqsub_4s);
    RUN(uqsub_2d);
    RUN(uqsub_b);
    RUN(uqsub_h);
    RUN(uqsub_s);
    RUN(uqsub_d);
    RUN(sabd_8b);
    RUN(sabd_4h);
    RUN(sabd_2s);
    RUN(sabd_16b);
    RUN(sabd_8h);
    RUN(sabd_4s);
    RUN(uabd_8b);
    RUN(uabd_4h);
    RUN(uabd_2s);
    RUN(uabd_16b);
    RUN(uabd_8h);
    RUN(uabd_4s);
    RUN(saba_8b);
    RUN(saba_4h);
    RUN(saba_2s);
    RUN(saba_16b);
    RUN(saba_8h);
    RUN(saba_4s);
    RUN(uaba_8b);
    RUN(uaba_4h);
    RUN(uaba_2s);
    RUN(uaba_16b);
    RUN(uaba_8h);
    RUN(uaba_4s);
    RUN(sabdl_8h);
    RUN(sabdl_4s);
    RUN(sabdl_2d);
    RUN(sabdl2_8h);
    RUN(sabdl2_4s);
    RUN(sabdl2_2d);
    RUN(uabdl_8h);
    RUN(uabdl_4s);
    RUN(uabdl_2d);
    RUN(uabdl2_8h);
    RUN(uabdl2_4s);
    RUN(uabdl2_2d);
    RUN(sabal_8h);
    RUN(sabal_4s);
    RUN(sabal_2d);
    RUN(sabal2_8h);
    RUN(sabal2_4s);
    RUN(sabal2_2d);
    RUN(uabal_8h);
    RUN(uabal_4s);
    RUN(uabal_2d);
    RUN(uabal2_8h);
    RUN(uabal2_4s);
    RUN(uabal2_2d);
    RUN(addhn_8b);
    RUN(addhn_4h);
    RUN(addhn_2s);
    RUN(addhn2_16b);
    RUN(addhn2_8h);
    RUN(addhn2_4s);
    RUN(raddhn_8b);
    RUN(raddhn_4h);
    RUN(raddhn_2s);
    RUN(raddhn2_16b);
    RUN(raddhn2_8h);
    RUN(raddhn2_4s);
    RUN(subhn_8b);
    RUN(subhn_4h);
    RUN(subhn_2s);
    RUN(subhn2_16b);
    RUN(subhn2_8h);
    RUN(subhn2_4s);
    RUN(rsubhn_8b);
    RUN(rsubhn_4h);
    RUN(rsubhn_2s);
    RUN(rsubhn2_16b);
    RUN(rsubhn2_8h);
    RUN(rsubhn2_4s);
    RUN(addp_8b);
    RUN(addp_16b);
    RUN(addp_4h);
    RUN(addp_8h);
    RUN(addp_2s);
    RUN(addp_4s);
    RUN(addp_2d);
    RUN(fmul_2d_e0);
    RUN(fmul_2d_e1);
    RUN(fcmeq_2d);
    RUN(fcmge_2d);
    RUN(fcmgt_2d);
    RUN(fcmeq_2d_zero);
    RUN(fcmge_2d_zero);
    RUN(fcmgt_2d_zero);
    RUN(fcmle_2d_zero);
    RUN(fcmlt_2d_zero);
    RUN(sdot0_0);
    RUN(sdot1_0);
    RUN(udot0_0);
    RUN(udot1_0);
    RUN(usdot0_0);
    RUN(usdot1_0);
    RUN(sdot_e0_0);
    RUN(sdot_e0_1);
    RUN(sdot_e0_2);
    RUN(sdot_e0_3);
    RUN(sdot_e1_0);
    RUN(sdot_e1_1);
    RUN(sdot_e1_2);
    RUN(sdot_e1_3);
    RUN(udot_e0_0);
    RUN(udot_e0_1);
    RUN(udot_e0_2);
    RUN(udot_e0_3);
    RUN(udot_e1_0);
    RUN(udot_e1_1);
    RUN(udot_e1_2);
    RUN(udot_e1_3);
    RUN(usdot_e0_0);
    RUN(usdot_e0_1);
    RUN(usdot_e0_2);
    RUN(usdot_e0_3);
    RUN(usdot_e1_0);
    RUN(usdot_e1_1);
    RUN(usdot_e1_2);
    RUN(usdot_e1_3);
    RUN(sudot_e0_0);
    RUN(sudot_e0_1);
    RUN(sudot_e0_2);
    RUN(sudot_e0_3);
    RUN(sudot_e1_0);
    RUN(sudot_e1_1);
    RUN(sudot_e1_2);
    RUN(sudot_e1_3);
    RUN(smmla1_0);
    RUN(ummla1_0);
    RUN(usmmla1_0);
    RUN(tail_fcvtn_4h);
    RUN(tail_fcvtn_2s);
    RUN(tail_fcvtn2_8h);
    RUN(tail_fcvtn2_4s);
    RUN(tail_fcvtl_4s);
    RUN(tail_fcvtl_2d);
    RUN(tail_fcvtl2_4s);
    RUN(tail_fcvtl2_2d);
    RUN(tail_fmul_4h);
    RUN(tail_fmul_8h);
    RUN(tail_fmul_h);
    RUN(tail_fadd_4h);
    RUN(tail_fadd_8h);
    RUN(tail_fadd_h);
    RUN(tail_fsub_4h);
    RUN(tail_fsub_8h);
    RUN(tail_fsub_h);
    RUN(tail_fmax_4h);
    RUN(tail_fmax_8h);
    RUN(tail_fmax_h);
    RUN(tail_fmin_4h);
    RUN(tail_fmin_8h);
    RUN(tail_fmin_h);
    RUN(tail_fmaxnm_4h);
    RUN(tail_fmaxnm_8h);
    RUN(tail_fmaxnm_h);
    RUN(tail_fminnm_4h);
    RUN(tail_fminnm_8h);
    RUN(tail_fminnm_h);
    RUN(tail_fmul_4h_e0);
    RUN(tail_fmul_8h_e0);
    RUN(tail_fmul_4h_e1);
    RUN(tail_fmul_8h_e1);
    RUN(tail_fmul_4h_e2);
    RUN(tail_fmul_8h_e2);
    RUN(tail_fmul_4h_e3);
    RUN(tail_fmul_8h_e3);
    RUN(tail_fmul_4h_e4);
    RUN(tail_fmul_8h_e4);
    RUN(tail_fmul_4h_e5);
    RUN(tail_fmul_8h_e5);
    RUN(tail_fmul_4h_e6);
    RUN(tail_fmul_8h_e6);
    RUN(tail_fmul_4h_e7);
    RUN(tail_fmul_8h_e7);
    RUN(tail_fcvtzu_4s);
    RUN(tail_fcvtns_4s);
    RUN(tail_fcvtps_4s);
    RUN(tail_fcvtms_4s);
    RUN(tail_scvtf_s);
    RUN(tail_scvtf_d);
    RUN(tail_scvtf_2d);
    RUN(tail_scvtf_8h);
    RUN(tail_ucvtf_s);
    RUN(tail_ucvtf_d);
    RUN(tail_ucvtf_2d);
    RUN(tail_ucvtf_8h);
    RUN(tail_fcvtzu_8h);
    RUN(tail_fcvtns_8h);
    RUN(tail_fcvt_hs);
    RUN(tail_fcvt_hd);
    RUN(tail_fcvt_sh);
    RUN(tail_fcvt_dh);
    RUN(zip1_8b);
    RUN(zip1_16b);
    RUN(zip1_4h);
    RUN(zip1_8h);
    RUN(zip1_2s);
    RUN(zip1_4s);
    RUN(zip1_2d);
    RUN(zip2_8b);
    RUN(zip2_16b);
    RUN(zip2_4h);
    RUN(zip2_8h);
    RUN(zip2_2s);
    RUN(zip2_4s);
    RUN(zip2_2d);
    RUN(uzp1_8b);
    RUN(uzp1_16b);
    RUN(uzp1_4h);
    RUN(uzp1_8h);
    RUN(uzp1_2s);
    RUN(uzp1_4s);
    RUN(uzp1_2d);
    RUN(uzp2_8b);
    RUN(uzp2_16b);
    RUN(uzp2_4h);
    RUN(uzp2_8h);
    RUN(uzp2_2s);
    RUN(uzp2_4s);
    RUN(uzp2_2d);
    RUN(trn1_8b);
    RUN(trn1_16b);
    RUN(trn1_4h);
    RUN(trn1_8h);
    RUN(trn1_2s);
    RUN(trn1_4s);
    RUN(trn1_2d);
    RUN(trn2_8b);
    RUN(trn2_16b);
    RUN(trn2_4h);
    RUN(trn2_8h);
    RUN(trn2_2s);
    RUN(trn2_4s);
    RUN(trn2_2d);
    RUN(addv_8b);
    RUN(addv_16b);
    RUN(addv_4h);
    RUN(addv_8h);
    RUN(addv_4s);
    RUN(uaddlp_4h);
    RUN(uaddlp_8h);
    RUN(uaddlp_2s);
    RUN(uaddlp_4s);
    RUN(uaddlp_1d);
    RUN(uaddlp_2d);
    RUN(saddlp_4h);
    RUN(saddlp_8h);
    RUN(saddlp_2s);
    RUN(saddlp_4s);
    RUN(saddlp_1d);
    RUN(saddlp_2d);
    RUN(uadalp_4h);
    RUN(uadalp_8h);
    RUN(uadalp_2s);
    RUN(uadalp_4s);
    RUN(uadalp_1d);
    RUN(uadalp_2d);
    RUN(sadalp_4h);
    RUN(sadalp_8h);
    RUN(sadalp_2s);
    RUN(sadalp_4s);
    RUN(sadalp_1d);
    RUN(sadalp_2d);
    RUN(faddp_4s);
    RUN(faddp_2s);
    RUN(faddp_s);
    RUN(faddp_2d);
    RUN(faddp_d);
    RUN(fmaxv_4s);
    RUN(fminv_4s);
    RUN(fmaxnmv_4s);
    RUN(fminnmv_4s);
    RUN(fdiv_2s);
    RUN(fdiv_4s);
    RUN(fcmeq_zero_2s);
    RUN(fcmeq_zero_4s);
    RUN(fcmge_zero_2s);
    RUN(fcmge_zero_4s);
    RUN(fcmgt_zero_2s);
    RUN(fcmgt_zero_4s);
    RUN(fcmle_zero_2s);
    RUN(fcmle_zero_4s);
    RUN(fcmlt_zero_2s);
    RUN(fcmlt_zero_4s);
    RUN(ushl_2s);
    RUN(ushl_4s);
    RUN(ushl_2d);
    RUN(sshl_2s);
    RUN(sshl_4s);
    RUN(sshl_2d);
    RUN(fmul_s0);
    RUN(fmul_s1);
    RUN(fmul_s2);
    RUN(fmul_s3);
    RUN(fmla_s0);
    RUN(fmla_s1);
    RUN(fmla_s2);
    RUN(fmla_s3);
    RUN(fmls_s0);
    RUN(fmls_s1);
    RUN(fmls_s2);
    RUN(fmls_s3);
    RUN(fmul_d0);
    RUN(fmul_d1);
    RUN(tail_saddl0_0);
    RUN(tail_saddl0_1);
    RUN(tail_saddl0_2);
    RUN(tail_saddl1_0);
    RUN(tail_saddl1_1);
    RUN(tail_saddl1_2);
    RUN(tail_uaddl0_0);
    RUN(tail_uaddl0_1);
    RUN(tail_uaddl0_2);
    RUN(tail_uaddl1_0);
    RUN(tail_uaddl1_1);
    RUN(tail_uaddl1_2);
    RUN(tail_saddw0_0);
    RUN(tail_saddw0_1);
    RUN(tail_saddw0_2);
    RUN(tail_saddw1_0);
    RUN(tail_saddw1_1);
    RUN(tail_saddw1_2);
    RUN(tail_uaddw0_0);
    RUN(tail_uaddw0_1);
    RUN(tail_uaddw0_2);
    RUN(tail_uaddw1_0);
    RUN(tail_uaddw1_1);
    RUN(tail_uaddw1_2);
    RUN(tail_ssubl0_0);
    RUN(tail_ssubl0_1);
    RUN(tail_ssubl0_2);
    RUN(tail_ssubl1_0);
    RUN(tail_ssubl1_1);
    RUN(tail_ssubl1_2);
    RUN(tail_usubl0_0);
    RUN(tail_usubl0_1);
    RUN(tail_usubl0_2);
    RUN(tail_usubl1_0);
    RUN(tail_usubl1_1);
    RUN(tail_usubl1_2);
    RUN(tail_ssubw0_0);
    RUN(tail_ssubw0_1);
    RUN(tail_ssubw0_2);
    RUN(tail_ssubw1_0);
    RUN(tail_ssubw1_1);
    RUN(tail_ssubw1_2);
    RUN(tail_usubw0_0);
    RUN(tail_usubw0_1);
    RUN(tail_usubw0_2);
    RUN(tail_usubw1_0);
    RUN(tail_usubw1_1);
    RUN(tail_usubw1_2);
    RUN(tail_smull0_0);
    RUN(tail_smull0_1);
    RUN(tail_smull0_2);
    RUN(tail_smull1_0);
    RUN(tail_smull1_1);
    RUN(tail_smull1_2);
    RUN(tail_umull0_0);
    RUN(tail_umull0_1);
    RUN(tail_umull0_2);
    RUN(tail_umull1_0);
    RUN(tail_umull1_1);
    RUN(tail_umull1_2);
    RUN(tail_smlal0_0);
    RUN(tail_smlal0_1);
    RUN(tail_smlal0_2);
    RUN(tail_smlal1_0);
    RUN(tail_smlal1_1);
    RUN(tail_smlal1_2);
    RUN(tail_umlal0_0);
    RUN(tail_umlal0_1);
    RUN(tail_umlal0_2);
    RUN(tail_umlal1_0);
    RUN(tail_umlal1_1);
    RUN(tail_umlal1_2);
    RUN(tail_smlsl0_0);
    RUN(tail_smlsl0_1);
    RUN(tail_smlsl0_2);
    RUN(tail_smlsl1_0);
    RUN(tail_smlsl1_1);
    RUN(tail_smlsl1_2);
    RUN(tail_umlsl0_0);
    RUN(tail_umlsl0_1);
    RUN(tail_umlsl0_2);
    RUN(tail_umlsl1_0);
    RUN(tail_umlsl1_1);
    RUN(tail_umlsl1_2);
    RUN(tail_sqxtn0_0);
    RUN(tail_sqxtn0_1);
    RUN(tail_sqxtn0_2);
    RUN(tail_sqxtn1_0);
    RUN(tail_sqxtn1_1);
    RUN(tail_sqxtn1_2);
    RUN(tail_uqxtn0_0);
    RUN(tail_uqxtn0_1);
    RUN(tail_uqxtn0_2);
    RUN(tail_uqxtn1_0);
    RUN(tail_uqxtn1_1);
    RUN(tail_uqxtn1_2);
    RUN(tail_sqxtun0_0);
    RUN(tail_sqxtun0_1);
    RUN(tail_sqxtun0_2);
    RUN(tail_sqxtun1_0);
    RUN(tail_sqxtun1_1);
    RUN(tail_sqxtun1_2);
    RUN(tail_shll0_0);
    RUN(tail_shll0_1);
    RUN(tail_shll0_2);
    RUN(tail_shll1_0);
    RUN(tail_shll1_1);
    RUN(tail_shll1_2);
    RUN(tail_sri0_0_1);
    RUN(tail_sri0_0_4);
    RUN(tail_sri0_0_8);
    RUN(tail_sli0_0_0);
    RUN(tail_sli0_0_4);
    RUN(tail_sli0_0_7);
    RUN(tail_rev160_0);
    RUN(tail_rev320_0);
    RUN(tail_rev640_0);
    RUN(tail_sri0_1_1);
    RUN(tail_sri0_1_8);
    RUN(tail_sri0_1_16);
    RUN(tail_sli0_1_0);
    RUN(tail_sli0_1_8);
    RUN(tail_sli0_1_15);
    RUN(tail_rev320_1);
    RUN(tail_rev640_1);
    RUN(tail_sqdmulh0_1);
    RUN(tail_sqdmulh0_1_0);
    RUN(tail_sqdmulh0_1_1);
    RUN(tail_sqdmulh0_1_2);
    RUN(tail_sqdmulh0_1_3);
    RUN(tail_sqdmulh0_1_4);
    RUN(tail_sqdmulh0_1_5);
    RUN(tail_sqdmulh0_1_6);
    RUN(tail_sqdmulh0_1_7);
    RUN(tail_sqrdmulh0_1);
    RUN(tail_sqrdmulh0_1_0);
    RUN(tail_sqrdmulh0_1_1);
    RUN(tail_sqrdmulh0_1_2);
    RUN(tail_sqrdmulh0_1_3);
    RUN(tail_sqrdmulh0_1_4);
    RUN(tail_sqrdmulh0_1_5);
    RUN(tail_sqrdmulh0_1_6);
    RUN(tail_sqrdmulh0_1_7);
    RUN(tail_sri0_2_1);
    RUN(tail_sri0_2_16);
    RUN(tail_sri0_2_32);
    RUN(tail_sli0_2_0);
    RUN(tail_sli0_2_16);
    RUN(tail_sli0_2_31);
    RUN(tail_rev640_2);
    RUN(tail_sqdmulh0_2);
    RUN(tail_sqdmulh0_2_0);
    RUN(tail_sqdmulh0_2_1);
    RUN(tail_sqdmulh0_2_2);
    RUN(tail_sqdmulh0_2_3);
    RUN(tail_sqrdmulh0_2);
    RUN(tail_sqrdmulh0_2_0);
    RUN(tail_sqrdmulh0_2_1);
    RUN(tail_sqrdmulh0_2_2);
    RUN(tail_sqrdmulh0_2_3);
    RUN(tail_sri1_0_1);
    RUN(tail_sri1_0_4);
    RUN(tail_sri1_0_8);
    RUN(tail_sli1_0_0);
    RUN(tail_sli1_0_4);
    RUN(tail_sli1_0_7);
    RUN(tail_rev161_0);
    RUN(tail_rev321_0);
    RUN(tail_rev641_0);
    RUN(tail_sri1_1_1);
    RUN(tail_sri1_1_8);
    RUN(tail_sri1_1_16);
    RUN(tail_sli1_1_0);
    RUN(tail_sli1_1_8);
    RUN(tail_sli1_1_15);
    RUN(tail_rev321_1);
    RUN(tail_rev641_1);
    RUN(tail_sqdmulh1_1);
    RUN(tail_sqdmulh1_1_0);
    RUN(tail_sqdmulh1_1_1);
    RUN(tail_sqdmulh1_1_2);
    RUN(tail_sqdmulh1_1_3);
    RUN(tail_sqdmulh1_1_4);
    RUN(tail_sqdmulh1_1_5);
    RUN(tail_sqdmulh1_1_6);
    RUN(tail_sqdmulh1_1_7);
    RUN(tail_sqrdmulh1_1);
    RUN(tail_sqrdmulh1_1_0);
    RUN(tail_sqrdmulh1_1_1);
    RUN(tail_sqrdmulh1_1_2);
    RUN(tail_sqrdmulh1_1_3);
    RUN(tail_sqrdmulh1_1_4);
    RUN(tail_sqrdmulh1_1_5);
    RUN(tail_sqrdmulh1_1_6);
    RUN(tail_sqrdmulh1_1_7);
    RUN(tail_sri1_2_1);
    RUN(tail_sri1_2_16);
    RUN(tail_sri1_2_32);
    RUN(tail_sli1_2_0);
    RUN(tail_sli1_2_16);
    RUN(tail_sli1_2_31);
    RUN(tail_rev641_2);
    RUN(tail_sqdmulh1_2);
    RUN(tail_sqdmulh1_2_0);
    RUN(tail_sqdmulh1_2_1);
    RUN(tail_sqdmulh1_2_2);
    RUN(tail_sqdmulh1_2_3);
    RUN(tail_sqrdmulh1_2);
    RUN(tail_sqrdmulh1_2_0);
    RUN(tail_sqrdmulh1_2_1);
    RUN(tail_sqrdmulh1_2_2);
    RUN(tail_sqrdmulh1_2_3);
    RUN(tail_sri1_3_1);
    RUN(tail_sri1_3_32);
    RUN(tail_sri1_3_64);
    RUN(tail_sli1_3_0);
    RUN(tail_sli1_3_32);
    RUN(tail_sli1_3_63);
    RUN(tail_addpd);



    RUN(tail_aese);
    RUN(tail_aesd);
    RUN(tail_aesmc);
    RUN(tail_aesimc);
    RUN(wide_fadd);
    RUN(wide_fsub);
    RUN(wide_fmul);
    RUN(wide_fdiv);
    RUN(wide_sqrtd);
    RUN(wide_sqrts);
    RUN(wide_sqrt2s);
    RUN(ld2_8b);
    RUN(ld2_16b);
    RUN(ld2_4h);
    RUN(ld2_8h);
    RUN(ld2_2s);
    RUN(ld2_4s);
    RUN(ld2_2d);
    RUN(ld3_8b);
    RUN(ld3_16b);
    RUN(ld3_4h);
    RUN(ld3_8h);
    RUN(ld3_2s);
    RUN(ld3_4s);
    RUN(ld3_2d);
    RUN(ld4_8b);
    RUN(ld4_16b);
    RUN(ld4_4h);
    RUN(ld4_8h);
    RUN(ld4_2s);
    RUN(ld4_4s);
    RUN(ld4_2d);
    RUN(st2_8b);
    RUN(st2_16b);
    RUN(st2_4h);
    RUN(st2_8h);
    RUN(st2_2s);
    RUN(st2_4s);
    RUN(st2_2d);
    RUN(st3_8b);
    RUN(st3_16b);
    RUN(st3_4h);
    RUN(st3_8h);
    RUN(st3_2s);
    RUN(st3_4s);
    RUN(st3_2d);
    RUN(st4_8b);
    RUN(st4_16b);
    RUN(st4_4h);
    RUN(st4_8h);
    RUN(st4_2s);
    RUN(st4_4s);
    RUN(st4_2d);

    RUN(tail_fccmp_s);
    RUN(tail_fccmpe_s);
    RUN(tail_fccmp_d);
    RUN(tail_fccmpe_d);
    RUN(tail_mrs_fpcr);
    RUN(tail_msr_fpcr);
    RUN(tail_mrs_fpsr);
    RUN(tail_msr_fpsr);
    RUN(tail_frecpe_2s);
    RUN(tail_frecpe_4s);
    RUN(tail_frecpe_2d);
    RUN(tail_frecpe_s);
    RUN(tail_frecpe_d);
    RUN(tail_frsqrte_2s);
    RUN(tail_frsqrte_4s);
    RUN(tail_frsqrte_2d);
    RUN(tail_frsqrte_s);
    RUN(tail_frsqrte_d);
    return 0;
}
