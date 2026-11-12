/* Per-instruction loops for helper statistics and development-host ratios.
 * Sixteen operations per iteration, constant ordinary inputs, accumulator
 * reset each iteration. Loop/reset/entry costs are included in both builds. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
typedef struct { uint64_t lo, hi; } vec;
static const vec single_a = {0x400000003f800000ull, 0x4080000040400000ull};
static const vec single_b = {0x3f0000003e800000ull, 0x3e0000003f800000ull};
static const vec double_a = {0x3ff0000000000000ull, 0x4000000000000000ull};
static const vec double_b = {0x3fe0000000000000ull, 0x3fd0000000000000ull};
static const vec shift32_a = {0x8123456789abcdefull, 0x0123456776543210ull};
static const vec shift32_b = {0x00000001000000fcull, 0x0000008000000020ull};
static const vec shift64_a = {0x8123456789abcdefull, 0x0123456776543210ull};
static const vec shift64_b = {0x01000000000000f4ull, 0x8000000000000007ull};
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec; }
#define BENCH(name, insn, kind) \
static __attribute__((noinline)) vec name(unsigned long n) { \
    vec out; \
    __asm__ volatile("ldr q0, [%[a]]\nldr q1, [%[b]]\n" \
        "1: movi v2.4s, #0\n.rept 16\n" insn "\n.endr\n" \
        "subs %[n], %[n], #1\nb.ne 1b\nstr q2, [%[out]]\n" \
        : [n] "+&r"(n) : [a] "r"(&kind##_a), [b] "r"(&kind##_b), [out] "r"(&out) \
        : "v0", "v1", "v2", "cc", "memory"); \
    return out; \
}
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
int main(int argc, char **argv)
{
    unsigned long n = argc > 1 ? strtoul(argv[1], 0, 0) : 50000;
    if (!n) return 2;
#define RUN(name) do { uint64_t start = now(); vec v = name(n); uint64_t elapsed = now() - start; \
    printf("%-18s %016llx:%016llx\n", #name, (unsigned long long)v.hi, (unsigned long long)v.lo); \
    fprintf(stderr, "%-18s %.6f ns/op\n", #name, (double)elapsed / (16.0 * n)); } while (0)
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
    return 0;
}
