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
BENCH(zip1_4s, "zip1 v2.4s, v0.4s, v1.4s", single)
BENCH(zip2_4s, "zip2 v2.4s, v0.4s, v1.4s", single)
BENCH(uzp1_4s, "uzp1 v2.4s, v0.4s, v1.4s", single)
BENCH(uzp2_4s, "uzp2 v2.4s, v0.4s, v1.4s", single)
BENCH(trn1_4s, "trn1 v2.4s, v0.4s, v1.4s", single)
BENCH(trn2_4s, "trn2 v2.4s, v0.4s, v1.4s", single)
BENCH(addv_4s, "addv s2, v0.4s", single)
BENCH(uaddlp_4s, "uaddlp v2.4s, v0.8h", single)
BENCH(saddlp_4s, "saddlp v2.4s, v0.8h", single)
BENCH(uadalp_4s, "uadalp v2.4s, v0.8h", single)
BENCH(sadalp_4s, "sadalp v2.4s, v0.8h", single)
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
BENCH(fcmeq_zero_4s, "fcmeq v2.4s, v0.4s, #0", single)
BENCH(fcmge_zero_4s, "fcmge v2.4s, v0.4s, #0", single)
BENCH(fcmgt_zero_4s, "fcmgt v2.4s, v0.4s, #0", single)
BENCH(fcmle_zero_4s, "fcmle v2.4s, v0.4s, #0", single)
BENCH(fcmlt_zero_4s, "fcmlt v2.4s, v0.4s, #0", single)
BENCH(ushl_4s, "ushl v2.4s, v0.4s, v1.4s", shift32)
BENCH(ushl_2d, "ushl v2.2d, v0.2d, v1.2d", shift64)
BENCH(sshl_4s, "sshl v2.4s, v0.4s, v1.4s", shift32)
BENCH(sshl_2d, "sshl v2.2d, v0.2d, v1.2d", shift64)
BENCH(fmul_s, "fmul s2, s0, v1.s[3]", single)
BENCH(fmul_d, "fmul d2, d0, v1.d[1]", double)
BENCH(fmla_s, "fmla s2, s0, v1.s[3]", single)
BENCH(fmls_s, "fmls s2, s0, v1.s[3]", single)
int main(int argc, char **argv)
{
    unsigned long n = argc > 1 ? strtoul(argv[1], 0, 0) : 50000;
    if (!n) return 2;
#define RUN(name) do { uint64_t start = now(); vec v = name(n); uint64_t elapsed = now() - start; \
    printf("%-18s %016llx:%016llx\n", #name, (unsigned long long)v.hi, (unsigned long long)v.lo); \
    fprintf(stderr, "%-18s %.6f ns/op\n", #name, (double)elapsed / (16.0 * n)); } while (0)
    RUN(zip1_4s);
    RUN(zip2_4s);
    RUN(uzp1_4s);
    RUN(uzp2_4s);
    RUN(trn1_4s);
    RUN(trn2_4s);
    RUN(addv_4s);
    RUN(uaddlp_4s);
    RUN(saddlp_4s);
    RUN(uadalp_4s);
    RUN(sadalp_4s);
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
    RUN(fcmeq_zero_4s);
    RUN(fcmge_zero_4s);
    RUN(fcmgt_zero_4s);
    RUN(fcmle_zero_4s);
    RUN(fcmlt_zero_4s);
    RUN(ushl_4s);
    RUN(ushl_2d);
    RUN(sshl_4s);
    RUN(sshl_2d);
    RUN(fmul_s);
    RUN(fmul_d);
    RUN(fmla_s);
    RUN(fmls_s);
    return 0;
}
