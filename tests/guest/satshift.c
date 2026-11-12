/* Register saturating shifts: all signed low-byte shift counts, every lane
 * width, scalar/vector forms, aliased registers and sticky QC. */
#include <stdint.h>
#include <stdio.h>
typedef uint64_t vec __attribute__((vector_size(16)));
static uint64_t hash = 0xcbf29ce484222325ULL;
static void mix(uint64_t x) { hash = (hash ^ x) * 0x100000001b3ULL; }
#define CASE(name, insn, dst, src, shift) \
static void name(vec a, vec b, uint32_t flags) { \
    vec out; uint64_t fpsr; \
    __asm__ volatile("msr fpsr, %x[f]\n\tldr q0, [%[a]]\n\tldr q1, [%[b]]\n\t" \
        insn " " dst ", " src ", " shift "\n\tstr q0, [%[out]]\n\tmrs %[fpsr], fpsr" \
        : [fpsr] "=r"(fpsr) : [a] "r"(&a), [b] "r"(&b), [out] "r"(&out), [f] "r"((uint64_t)flags) \
        : "v0", "v1", "memory"); \
    mix(out[0]); mix(out[1]); mix(fpsr); \
}
CASE(sqshl_8b, "sqshl", "v0.8b", "v0.8b", "v1.8b")
CASE(sqshl_8b_alias, "sqshl", "v0.8b", "v0.8b", "v0.8b")
CASE(sqshl_16b, "sqshl", "v0.16b", "v0.16b", "v1.16b")
CASE(sqshl_16b_alias, "sqshl", "v0.16b", "v0.16b", "v0.16b")
CASE(sqshl_4h, "sqshl", "v0.4h", "v0.4h", "v1.4h")
CASE(sqshl_4h_alias, "sqshl", "v0.4h", "v0.4h", "v0.4h")
CASE(sqshl_8h, "sqshl", "v0.8h", "v0.8h", "v1.8h")
CASE(sqshl_8h_alias, "sqshl", "v0.8h", "v0.8h", "v0.8h")
CASE(sqshl_2s, "sqshl", "v0.2s", "v0.2s", "v1.2s")
CASE(sqshl_2s_alias, "sqshl", "v0.2s", "v0.2s", "v0.2s")
CASE(sqshl_4s, "sqshl", "v0.4s", "v0.4s", "v1.4s")
CASE(sqshl_4s_alias, "sqshl", "v0.4s", "v0.4s", "v0.4s")
CASE(sqshl_2d, "sqshl", "v0.2d", "v0.2d", "v1.2d")
CASE(sqshl_2d_alias, "sqshl", "v0.2d", "v0.2d", "v0.2d")
CASE(sqshl_b, "sqshl", "b0", "b0", "b1")
CASE(sqshl_b_alias, "sqshl", "b0", "b0", "b0")
CASE(sqshl_h, "sqshl", "h0", "h0", "h1")
CASE(sqshl_h_alias, "sqshl", "h0", "h0", "h0")
CASE(sqshl_s, "sqshl", "s0", "s0", "s1")
CASE(sqshl_s_alias, "sqshl", "s0", "s0", "s0")
CASE(sqshl_d, "sqshl", "d0", "d0", "d1")
CASE(sqshl_d_alias, "sqshl", "d0", "d0", "d0")
CASE(uqshl_8b, "uqshl", "v0.8b", "v0.8b", "v1.8b")
CASE(uqshl_8b_alias, "uqshl", "v0.8b", "v0.8b", "v0.8b")
CASE(uqshl_16b, "uqshl", "v0.16b", "v0.16b", "v1.16b")
CASE(uqshl_16b_alias, "uqshl", "v0.16b", "v0.16b", "v0.16b")
CASE(uqshl_4h, "uqshl", "v0.4h", "v0.4h", "v1.4h")
CASE(uqshl_4h_alias, "uqshl", "v0.4h", "v0.4h", "v0.4h")
CASE(uqshl_8h, "uqshl", "v0.8h", "v0.8h", "v1.8h")
CASE(uqshl_8h_alias, "uqshl", "v0.8h", "v0.8h", "v0.8h")
CASE(uqshl_2s, "uqshl", "v0.2s", "v0.2s", "v1.2s")
CASE(uqshl_2s_alias, "uqshl", "v0.2s", "v0.2s", "v0.2s")
CASE(uqshl_4s, "uqshl", "v0.4s", "v0.4s", "v1.4s")
CASE(uqshl_4s_alias, "uqshl", "v0.4s", "v0.4s", "v0.4s")
CASE(uqshl_2d, "uqshl", "v0.2d", "v0.2d", "v1.2d")
CASE(uqshl_2d_alias, "uqshl", "v0.2d", "v0.2d", "v0.2d")
CASE(uqshl_b, "uqshl", "b0", "b0", "b1")
CASE(uqshl_b_alias, "uqshl", "b0", "b0", "b0")
CASE(uqshl_h, "uqshl", "h0", "h0", "h1")
CASE(uqshl_h_alias, "uqshl", "h0", "h0", "h0")
CASE(uqshl_s, "uqshl", "s0", "s0", "s1")
CASE(uqshl_s_alias, "uqshl", "s0", "s0", "s0")
CASE(uqshl_d, "uqshl", "d0", "d0", "d1")
CASE(uqshl_d_alias, "uqshl", "d0", "d0", "d0")
CASE(sqrshl_8b, "sqrshl", "v0.8b", "v0.8b", "v1.8b")
CASE(sqrshl_8b_alias, "sqrshl", "v0.8b", "v0.8b", "v0.8b")
CASE(sqrshl_16b, "sqrshl", "v0.16b", "v0.16b", "v1.16b")
CASE(sqrshl_16b_alias, "sqrshl", "v0.16b", "v0.16b", "v0.16b")
CASE(sqrshl_4h, "sqrshl", "v0.4h", "v0.4h", "v1.4h")
CASE(sqrshl_4h_alias, "sqrshl", "v0.4h", "v0.4h", "v0.4h")
CASE(sqrshl_8h, "sqrshl", "v0.8h", "v0.8h", "v1.8h")
CASE(sqrshl_8h_alias, "sqrshl", "v0.8h", "v0.8h", "v0.8h")
CASE(sqrshl_2s, "sqrshl", "v0.2s", "v0.2s", "v1.2s")
CASE(sqrshl_2s_alias, "sqrshl", "v0.2s", "v0.2s", "v0.2s")
CASE(sqrshl_4s, "sqrshl", "v0.4s", "v0.4s", "v1.4s")
CASE(sqrshl_4s_alias, "sqrshl", "v0.4s", "v0.4s", "v0.4s")
CASE(sqrshl_2d, "sqrshl", "v0.2d", "v0.2d", "v1.2d")
CASE(sqrshl_2d_alias, "sqrshl", "v0.2d", "v0.2d", "v0.2d")
CASE(sqrshl_b, "sqrshl", "b0", "b0", "b1")
CASE(sqrshl_b_alias, "sqrshl", "b0", "b0", "b0")
CASE(sqrshl_h, "sqrshl", "h0", "h0", "h1")
CASE(sqrshl_h_alias, "sqrshl", "h0", "h0", "h0")
CASE(sqrshl_s, "sqrshl", "s0", "s0", "s1")
CASE(sqrshl_s_alias, "sqrshl", "s0", "s0", "s0")
CASE(sqrshl_d, "sqrshl", "d0", "d0", "d1")
CASE(sqrshl_d_alias, "sqrshl", "d0", "d0", "d0")
CASE(uqrshl_8b, "uqrshl", "v0.8b", "v0.8b", "v1.8b")
CASE(uqrshl_8b_alias, "uqrshl", "v0.8b", "v0.8b", "v0.8b")
CASE(uqrshl_16b, "uqrshl", "v0.16b", "v0.16b", "v1.16b")
CASE(uqrshl_16b_alias, "uqrshl", "v0.16b", "v0.16b", "v0.16b")
CASE(uqrshl_4h, "uqrshl", "v0.4h", "v0.4h", "v1.4h")
CASE(uqrshl_4h_alias, "uqrshl", "v0.4h", "v0.4h", "v0.4h")
CASE(uqrshl_8h, "uqrshl", "v0.8h", "v0.8h", "v1.8h")
CASE(uqrshl_8h_alias, "uqrshl", "v0.8h", "v0.8h", "v0.8h")
CASE(uqrshl_2s, "uqrshl", "v0.2s", "v0.2s", "v1.2s")
CASE(uqrshl_2s_alias, "uqrshl", "v0.2s", "v0.2s", "v0.2s")
CASE(uqrshl_4s, "uqrshl", "v0.4s", "v0.4s", "v1.4s")
CASE(uqrshl_4s_alias, "uqrshl", "v0.4s", "v0.4s", "v0.4s")
CASE(uqrshl_2d, "uqrshl", "v0.2d", "v0.2d", "v1.2d")
CASE(uqrshl_2d_alias, "uqrshl", "v0.2d", "v0.2d", "v0.2d")
CASE(uqrshl_b, "uqrshl", "b0", "b0", "b1")
CASE(uqrshl_b_alias, "uqrshl", "b0", "b0", "b0")
CASE(uqrshl_h, "uqrshl", "h0", "h0", "h1")
CASE(uqrshl_h_alias, "uqrshl", "h0", "h0", "h0")
CASE(uqrshl_s, "uqrshl", "s0", "s0", "s1")
CASE(uqrshl_s_alias, "uqrshl", "s0", "s0", "s0")
CASE(uqrshl_d, "uqrshl", "d0", "d0", "d1")
CASE(uqrshl_d_alias, "uqrshl", "d0", "d0", "d0")
static void (*const cases[])(vec, vec, uint32_t) = {
sqshl_8b,
sqshl_8b_alias,
sqshl_16b,
sqshl_16b_alias,
sqshl_4h,
sqshl_4h_alias,
sqshl_8h,
sqshl_8h_alias,
sqshl_2s,
sqshl_2s_alias,
sqshl_4s,
sqshl_4s_alias,
sqshl_2d,
sqshl_2d_alias,
sqshl_b,
sqshl_b_alias,
sqshl_h,
sqshl_h_alias,
sqshl_s,
sqshl_s_alias,
sqshl_d,
sqshl_d_alias,
uqshl_8b,
uqshl_8b_alias,
uqshl_16b,
uqshl_16b_alias,
uqshl_4h,
uqshl_4h_alias,
uqshl_8h,
uqshl_8h_alias,
uqshl_2s,
uqshl_2s_alias,
uqshl_4s,
uqshl_4s_alias,
uqshl_2d,
uqshl_2d_alias,
uqshl_b,
uqshl_b_alias,
uqshl_h,
uqshl_h_alias,
uqshl_s,
uqshl_s_alias,
uqshl_d,
uqshl_d_alias,
sqrshl_8b,
sqrshl_8b_alias,
sqrshl_16b,
sqrshl_16b_alias,
sqrshl_4h,
sqrshl_4h_alias,
sqrshl_8h,
sqrshl_8h_alias,
sqrshl_2s,
sqrshl_2s_alias,
sqrshl_4s,
sqrshl_4s_alias,
sqrshl_2d,
sqrshl_2d_alias,
sqrshl_b,
sqrshl_b_alias,
sqrshl_h,
sqrshl_h_alias,
sqrshl_s,
sqrshl_s_alias,
sqrshl_d,
sqrshl_d_alias,
uqrshl_8b,
uqrshl_8b_alias,
uqrshl_16b,
uqrshl_16b_alias,
uqrshl_4h,
uqrshl_4h_alias,
uqrshl_8h,
uqrshl_8h_alias,
uqrshl_2s,
uqrshl_2s_alias,
uqrshl_4s,
uqrshl_4s_alias,
uqrshl_2d,
uqrshl_2d_alias,
uqrshl_b,
uqrshl_b_alias,
uqrshl_h,
uqrshl_h_alias,
uqrshl_s,
uqrshl_s_alias,
uqrshl_d,
uqrshl_d_alias
};
int main(void) {
    static const uint64_t inputs[] = {0, 1, ~0ULL, 0x8080808080808080ULL,
        0x7f7f7f7f7f7f7f7fULL, 0x8000800080008000ULL, 0x7fff7fff7fff7fffULL,
        0x8000000080000000ULL, 0x7fffffff7fffffffULL, 1ULL << 63, (1ULL << 63)-1};
    for (unsigned c=0; c<sizeof cases/sizeof *cases; c++) {
        for (unsigned v=0; v<sizeof inputs/sizeof *inputs; v++)
            for (unsigned shift=0; shift<256; shift++) {
                vec a={inputs[v],~inputs[v]};
                vec b={0x0101010101010101ULL*shift,0x0101010101010101ULL*shift};
                cases[c](a,b,0); cases[c](a,b,0x0800009f);
            }
        printf("%u %016llx\n",c,(unsigned long long)hash);
    }
    return 0;
}
