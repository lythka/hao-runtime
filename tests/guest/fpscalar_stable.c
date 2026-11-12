/* Scalar FP throughput on fixed normal sources. Destinations never alias
 * sources, so repeated multiply/divide cannot reach zero or infinity.
 * FPSR correctness is covered by fpscalar.c; this program reports timings
 * and a result checksum, with no machine-dependent performance threshold. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint64_t V __attribute__((vector_size(16)));
#define OP(name, insn, kind)                                                     \
static uint64_t name(const V *p, uint64_t count)                                  \
{                                                                                \
    uint64_t result;                                                             \
    __asm__ volatile(                                                            \
        "ldp q1, q2, [%[p]]\n movi v0.16b, #0\n msr fpcr, xzr\n1:\n"             \
        insn "\n" insn "\n" insn "\n" insn "\n"                                  \
        insn "\n" insn "\n" insn "\n" insn "\n"                                  \
        "subs %[n], %[n], #1\n b.ne 1b\n fmov %[r], d0"                            \
        : [r] "=r"(result), [n] "+r"(count) : [p] "r"(p)                          \
        : "v0", "v1", "v2", "memory", "cc");                                     \
    return result;                                                               \
}
#define OPS(X) \
X(fadd_s,"fadd s0,s1,s2",0) X(fadd_d,"fadd d0,d1,d2",1) \
X(fsub_s,"fsub s0,s1,s2",0) X(fsub_d,"fsub d0,d1,d2",1) \
X(fmul_s,"fmul s0,s1,s2",0) X(fmul_d,"fmul d0,d1,d2",1) \
X(fdiv_s,"fdiv s0,s1,s2",0) X(fdiv_d,"fdiv d0,d1,d2",1) \
X(fsqrt_s,"fsqrt s0,s1",0) X(fsqrt_d,"fsqrt d0,d1",1) \
X(fcvt_ds,"fcvt d0,s1",0) X(fcvt_sd,"fcvt s0,d1",1) \
X(fcmp_s,"fcmp s1,s2",0) X(fcmp_d,"fcmp d1,d2",1) \
X(fcmp_s0,"fcmp s1,#0.0",0) X(fcmp_d0,"fcmp d1,#0.0",1) \
X(fcmpe_s,"fcmpe s1,s2",0) X(fcmpe_d,"fcmpe d1,d2",1) \
X(fcmpe_s0,"fcmpe s1,#0.0",0) X(fcmpe_d0,"fcmpe d1,#0.0",1)
OPS(OP)
static const struct { const char *name;uint64_t(*fn)(const V*,uint64_t);int dbl; } ops[]={
#define E(n,s,k) {#n,n,k},
OPS(E)
};
static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    uint64_t n = strtoull(argv[2], 0, 0);
    if (!n) return 2;
    for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++) {
        if (strcmp(argv[1], ops[i].name)) continue;
        V p[2];
        float f[8] = {3.25f, 0, 0, 0, 1.25f, 0, 0, 0};
        double d[4] = {3.25, 0, 1.25, 0};
        memcpy(p, ops[i].dbl ? (void *)d : (void *)f, sizeof p);
        uint64_t start = now(), result = ops[i].fn(p, n), elapsed = now() - start;
        printf("%s %.2f ns/insn %016llx\n", ops[i].name,
               (double)elapsed / (8 * n), (unsigned long long)result);
        return 0;
    }
    return 2;
}
