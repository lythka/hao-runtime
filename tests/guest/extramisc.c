/* Complex arithmetic: rotations, indexed pairs, aliases and FPSR. */
#include <stdint.h>
#include <stdio.h>
typedef uint64_t vec __attribute__((vector_size(16)));
static uint64_t hash=0xcbf29ce484222325ULL;
static void mix(uint64_t x) { hash=(hash^x)*0x100000001b3ULL; }
#define CASE(name, instruction) \
static void name(vec a, vec b, uint64_t cr, uint64_t sr) { \
    vec out; uint64_t fpsr; \
    __asm__ volatile("msr fpcr, %[cr]\n\tmsr fpsr, %[sr]\n\tldr q0, [%[a]]\n\tldr q1, [%[b]]\n\t" \
        instruction "\n\tstr q0, [%[out]]\n\tmrs %[fpsr], fpsr\n\tmsr fpcr, xzr" \
        : [fpsr] "=r"(fpsr) : [a] "r"(&a), [b] "r"(&b), [out] "r"(&out), [cr] "r"(cr), [sr] "r"(sr) \
        : "v0", "v1", "memory"); \
    mix(out[0]); mix(out[1]); mix(fpsr); \
}
CASE(c0, "suqadd v0.8b, v0.8b")
CASE(c1, "suqadd v0.8b, v1.8b")
CASE(c2, "suqadd v0.16b, v0.16b")
CASE(c3, "suqadd v0.16b, v1.16b")
CASE(c4, "suqadd v0.4h, v0.4h")
CASE(c5, "suqadd v0.4h, v1.4h")
CASE(c6, "suqadd v0.8h, v0.8h")
CASE(c7, "suqadd v0.8h, v1.8h")
CASE(c8, "suqadd v0.2s, v0.2s")
CASE(c9, "suqadd v0.2s, v1.2s")
CASE(c10, "suqadd v0.4s, v0.4s")
CASE(c11, "suqadd v0.4s, v1.4s")
CASE(c12, "suqadd v0.2d, v0.2d")
CASE(c13, "suqadd v0.2d, v1.2d")
CASE(c14, "suqadd b0, b0")
CASE(c15, "suqadd b0, b1")
CASE(c16, "suqadd h0, h0")
CASE(c17, "suqadd h0, h1")
CASE(c18, "suqadd s0, s0")
CASE(c19, "suqadd s0, s1")
CASE(c20, "suqadd d0, d0")
CASE(c21, "suqadd d0, d1")
CASE(c22, "usqadd v0.8b, v0.8b")
CASE(c23, "usqadd v0.8b, v1.8b")
CASE(c24, "usqadd v0.16b, v0.16b")
CASE(c25, "usqadd v0.16b, v1.16b")
CASE(c26, "usqadd v0.4h, v0.4h")
CASE(c27, "usqadd v0.4h, v1.4h")
CASE(c28, "usqadd v0.8h, v0.8h")
CASE(c29, "usqadd v0.8h, v1.8h")
CASE(c30, "usqadd v0.2s, v0.2s")
CASE(c31, "usqadd v0.2s, v1.2s")
CASE(c32, "usqadd v0.4s, v0.4s")
CASE(c33, "usqadd v0.4s, v1.4s")
CASE(c34, "usqadd v0.2d, v0.2d")
CASE(c35, "usqadd v0.2d, v1.2d")
CASE(c36, "usqadd b0, b0")
CASE(c37, "usqadd b0, b1")
CASE(c38, "usqadd h0, h0")
CASE(c39, "usqadd h0, h1")
CASE(c40, "usqadd s0, s0")
CASE(c41, "usqadd s0, s1")
CASE(c42, "usqadd d0, d0")
CASE(c43, "usqadd d0, d1")
CASE(c44, "sqabs v0.8b, v0.8b")
CASE(c45, "sqabs v0.8b, v1.8b")
CASE(c46, "sqabs v0.16b, v0.16b")
CASE(c47, "sqabs v0.16b, v1.16b")
CASE(c48, "sqabs v0.4h, v0.4h")
CASE(c49, "sqabs v0.4h, v1.4h")
CASE(c50, "sqabs v0.8h, v0.8h")
CASE(c51, "sqabs v0.8h, v1.8h")
CASE(c52, "sqabs v0.2s, v0.2s")
CASE(c53, "sqabs v0.2s, v1.2s")
CASE(c54, "sqabs v0.4s, v0.4s")
CASE(c55, "sqabs v0.4s, v1.4s")
CASE(c56, "sqabs v0.2d, v0.2d")
CASE(c57, "sqabs v0.2d, v1.2d")
CASE(c58, "sqabs b0, b0")
CASE(c59, "sqabs b0, b1")
CASE(c60, "sqabs h0, h0")
CASE(c61, "sqabs h0, h1")
CASE(c62, "sqabs s0, s0")
CASE(c63, "sqabs s0, s1")
CASE(c64, "sqabs d0, d0")
CASE(c65, "sqabs d0, d1")
CASE(c66, "sqneg v0.8b, v0.8b")
CASE(c67, "sqneg v0.8b, v1.8b")
CASE(c68, "sqneg v0.16b, v0.16b")
CASE(c69, "sqneg v0.16b, v1.16b")
CASE(c70, "sqneg v0.4h, v0.4h")
CASE(c71, "sqneg v0.4h, v1.4h")
CASE(c72, "sqneg v0.8h, v0.8h")
CASE(c73, "sqneg v0.8h, v1.8h")
CASE(c74, "sqneg v0.2s, v0.2s")
CASE(c75, "sqneg v0.2s, v1.2s")
CASE(c76, "sqneg v0.4s, v0.4s")
CASE(c77, "sqneg v0.4s, v1.4s")
CASE(c78, "sqneg v0.2d, v0.2d")
CASE(c79, "sqneg v0.2d, v1.2d")
CASE(c80, "sqneg b0, b0")
CASE(c81, "sqneg b0, b1")
CASE(c82, "sqneg h0, h0")
CASE(c83, "sqneg h0, h1")
CASE(c84, "sqneg s0, s0")
CASE(c85, "sqneg s0, s1")
CASE(c86, "sqneg d0, d0")
CASE(c87, "sqneg d0, d1")
CASE(c88, "frint32x v0.2s, v0.2s")
CASE(c89, "frint32x v0.2s, v1.2s")
CASE(c90, "frint32x v0.4s, v0.4s")
CASE(c91, "frint32x v0.4s, v1.4s")
CASE(c92, "frint32x v0.2d, v0.2d")
CASE(c93, "frint32x v0.2d, v1.2d")
CASE(c94, "frint32x s0, s0")
CASE(c95, "frint32x s0, s1")
CASE(c96, "frint32x d0, d0")
CASE(c97, "frint32x d0, d1")
CASE(c98, "frint32z v0.2s, v0.2s")
CASE(c99, "frint32z v0.2s, v1.2s")
CASE(c100, "frint32z v0.4s, v0.4s")
CASE(c101, "frint32z v0.4s, v1.4s")
CASE(c102, "frint32z v0.2d, v0.2d")
CASE(c103, "frint32z v0.2d, v1.2d")
CASE(c104, "frint32z s0, s0")
CASE(c105, "frint32z s0, s1")
CASE(c106, "frint32z d0, d0")
CASE(c107, "frint32z d0, d1")
CASE(c108, "frint64x v0.2s, v0.2s")
CASE(c109, "frint64x v0.2s, v1.2s")
CASE(c110, "frint64x v0.4s, v0.4s")
CASE(c111, "frint64x v0.4s, v1.4s")
CASE(c112, "frint64x v0.2d, v0.2d")
CASE(c113, "frint64x v0.2d, v1.2d")
CASE(c114, "frint64x s0, s0")
CASE(c115, "frint64x s0, s1")
CASE(c116, "frint64x d0, d0")
CASE(c117, "frint64x d0, d1")
CASE(c118, "frint64z v0.2s, v0.2s")
CASE(c119, "frint64z v0.2s, v1.2s")
CASE(c120, "frint64z v0.4s, v0.4s")
CASE(c121, "frint64z v0.4s, v1.4s")
CASE(c122, "frint64z v0.2d, v0.2d")
CASE(c123, "frint64z v0.2d, v1.2d")
CASE(c124, "frint64z s0, s0")
CASE(c125, "frint64z s0, s1")
CASE(c126, "frint64z d0, d0")
CASE(c127, "frint64z d0, d1")
CASE(c128, "fmaxv h0, v1.4h")
CASE(c129, "fmaxv h0, v1.8h")
CASE(c130, "fminv h0, v1.4h")
CASE(c131, "fminv h0, v1.8h")
CASE(c132, "fmaxnmv h0, v1.4h")
CASE(c133, "fmaxnmv h0, v1.8h")
CASE(c134, "fminnmv h0, v1.4h")
CASE(c135, "fminnmv h0, v1.8h")
CASE(c136, "faddp h0, v1.2h")
CASE(c137, "fmaxp h0, v1.2h")
CASE(c138, "fminp h0, v1.2h")
CASE(c139, "fmaxnmp h0, v1.2h")
CASE(c140, "fminnmp h0, v1.2h")
static void (*const cases[])(vec,vec,uint64_t,uint64_t)={c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,c20,c21,c22,c23,c24,c25,c26,c27,c28,c29,c30,c31,c32,c33,c34,c35,c36,c37,c38,c39,c40,c41,c42,c43,c44,c45,c46,c47,c48,c49,c50,c51,c52,c53,c54,c55,c56,c57,c58,c59,c60,c61,c62,c63,c64,c65,c66,c67,c68,c69,c70,c71,c72,c73,c74,c75,c76,c77,c78,c79,c80,c81,c82,c83,c84,c85,c86,c87,c88,c89,c90,c91,c92,c93,c94,c95,c96,c97,c98,c99,c100,c101,c102,c103,c104,c105,c106,c107,c108,c109,c110,c111,c112,c113,c114,c115,c116,c117,c118,c119,c120,c121,c122,c123,c124,c125,c126,c127,c128,c129,c130,c131,c132,c133,c134,c135,c136,c137,c138,c139,c140};
int main(void) {
    static const uint64_t inputs[]={0,0x800000007c00fc00ULL,0x7d017e017fff7c01ULL,
        0x000103ff04000401ULL,0x3bff3c003c013e00ULL,0x7bfffbff3555b555ULL,
        0x800183ff84008401ULL,0x0001800100028002ULL,0x4000c0004c00cc00ULL,
        0x7f8000017fc00001ULL,0x3f80000040000000ULL,0x7ff0000000000001ULL,
        0x7ff8000000000001ULL,0x3ff0000000000000ULL,0x0010000000000001ULL,0x41dfffffffffffffULL,0x41e0000000000000ULL,
        0xc1e0000000000000ULL,0xc1e0000000100000ULL,0x43dfffffffffffffULL,
        0x43e0000000000000ULL,0xc3e0000000000000ULL,0x3ff8000000000000ULL,
        0xbff8000000000000ULL,0x4f0000004effffffULL,0xcf000001cf000000ULL,
        0x5f0000005effffffULL,0xdf000001df000000ULL,0x7fffffffffffffffULL,
        0x8000000000000000ULL,0xffffffffffffffffULL};
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        for (unsigned cr=0;cr<16;cr++)
            for (unsigned a=0;a<sizeof inputs/sizeof *inputs;a++)
                for (unsigned b=0;b<sizeof inputs/sizeof *inputs;b++) {
                    vec x={inputs[a],~inputs[a]},y={inputs[b],~inputs[b]};
                    uint64_t fpcr=((cr&3)<<22)|((cr&4)<<22)|((cr&8)<<22);
                    cases[c](x,y,fpcr,0);cases[c](x,y,fpcr,0x0800009f);
                }
        printf("%u %016llx\n",c,(unsigned long long)hash);
    }
    return 0;
}
