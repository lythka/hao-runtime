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
CASE(c0, "fcmla v0.4h, v0.4h, v1.4h, #0")
CASE(c1, "fcmla v0.4h, v1.4h, v0.4h, #0")
CASE(c2, "fcmla v0.4h, v0.4h, v1.h[0], #0")
CASE(c3, "fcmla v0.4h, v1.4h, v0.h[0], #0")
CASE(c4, "fcmla v0.4h, v0.4h, v1.h[1], #0")
CASE(c5, "fcmla v0.4h, v1.4h, v0.h[1], #0")
CASE(c6, "fcmla v0.4h, v0.4h, v1.4h, #90")
CASE(c7, "fcmla v0.4h, v1.4h, v0.4h, #90")
CASE(c8, "fcmla v0.4h, v0.4h, v1.h[0], #90")
CASE(c9, "fcmla v0.4h, v1.4h, v0.h[0], #90")
CASE(c10, "fcmla v0.4h, v0.4h, v1.h[1], #90")
CASE(c11, "fcmla v0.4h, v1.4h, v0.h[1], #90")
CASE(c12, "fcmla v0.4h, v0.4h, v1.4h, #180")
CASE(c13, "fcmla v0.4h, v1.4h, v0.4h, #180")
CASE(c14, "fcmla v0.4h, v0.4h, v1.h[0], #180")
CASE(c15, "fcmla v0.4h, v1.4h, v0.h[0], #180")
CASE(c16, "fcmla v0.4h, v0.4h, v1.h[1], #180")
CASE(c17, "fcmla v0.4h, v1.4h, v0.h[1], #180")
CASE(c18, "fcmla v0.4h, v0.4h, v1.4h, #270")
CASE(c19, "fcmla v0.4h, v1.4h, v0.4h, #270")
CASE(c20, "fcmla v0.4h, v0.4h, v1.h[0], #270")
CASE(c21, "fcmla v0.4h, v1.4h, v0.h[0], #270")
CASE(c22, "fcmla v0.4h, v0.4h, v1.h[1], #270")
CASE(c23, "fcmla v0.4h, v1.4h, v0.h[1], #270")
CASE(c24, "fcadd v0.4h, v0.4h, v1.4h, #90")
CASE(c25, "fcadd v0.4h, v0.4h, v1.4h, #270")
CASE(c26, "fcmla v0.8h, v0.8h, v1.8h, #0")
CASE(c27, "fcmla v0.8h, v1.8h, v0.8h, #0")
CASE(c28, "fcmla v0.8h, v0.8h, v1.h[0], #0")
CASE(c29, "fcmla v0.8h, v1.8h, v0.h[0], #0")
CASE(c30, "fcmla v0.8h, v0.8h, v1.h[1], #0")
CASE(c31, "fcmla v0.8h, v1.8h, v0.h[1], #0")
CASE(c32, "fcmla v0.8h, v0.8h, v1.h[2], #0")
CASE(c33, "fcmla v0.8h, v1.8h, v0.h[2], #0")
CASE(c34, "fcmla v0.8h, v0.8h, v1.h[3], #0")
CASE(c35, "fcmla v0.8h, v1.8h, v0.h[3], #0")
CASE(c36, "fcmla v0.8h, v0.8h, v1.8h, #90")
CASE(c37, "fcmla v0.8h, v1.8h, v0.8h, #90")
CASE(c38, "fcmla v0.8h, v0.8h, v1.h[0], #90")
CASE(c39, "fcmla v0.8h, v1.8h, v0.h[0], #90")
CASE(c40, "fcmla v0.8h, v0.8h, v1.h[1], #90")
CASE(c41, "fcmla v0.8h, v1.8h, v0.h[1], #90")
CASE(c42, "fcmla v0.8h, v0.8h, v1.h[2], #90")
CASE(c43, "fcmla v0.8h, v1.8h, v0.h[2], #90")
CASE(c44, "fcmla v0.8h, v0.8h, v1.h[3], #90")
CASE(c45, "fcmla v0.8h, v1.8h, v0.h[3], #90")
CASE(c46, "fcmla v0.8h, v0.8h, v1.8h, #180")
CASE(c47, "fcmla v0.8h, v1.8h, v0.8h, #180")
CASE(c48, "fcmla v0.8h, v0.8h, v1.h[0], #180")
CASE(c49, "fcmla v0.8h, v1.8h, v0.h[0], #180")
CASE(c50, "fcmla v0.8h, v0.8h, v1.h[1], #180")
CASE(c51, "fcmla v0.8h, v1.8h, v0.h[1], #180")
CASE(c52, "fcmla v0.8h, v0.8h, v1.h[2], #180")
CASE(c53, "fcmla v0.8h, v1.8h, v0.h[2], #180")
CASE(c54, "fcmla v0.8h, v0.8h, v1.h[3], #180")
CASE(c55, "fcmla v0.8h, v1.8h, v0.h[3], #180")
CASE(c56, "fcmla v0.8h, v0.8h, v1.8h, #270")
CASE(c57, "fcmla v0.8h, v1.8h, v0.8h, #270")
CASE(c58, "fcmla v0.8h, v0.8h, v1.h[0], #270")
CASE(c59, "fcmla v0.8h, v1.8h, v0.h[0], #270")
CASE(c60, "fcmla v0.8h, v0.8h, v1.h[1], #270")
CASE(c61, "fcmla v0.8h, v1.8h, v0.h[1], #270")
CASE(c62, "fcmla v0.8h, v0.8h, v1.h[2], #270")
CASE(c63, "fcmla v0.8h, v1.8h, v0.h[2], #270")
CASE(c64, "fcmla v0.8h, v0.8h, v1.h[3], #270")
CASE(c65, "fcmla v0.8h, v1.8h, v0.h[3], #270")
CASE(c66, "fcadd v0.8h, v0.8h, v1.8h, #90")
CASE(c67, "fcadd v0.8h, v0.8h, v1.8h, #270")
CASE(c68, "fcmla v0.2s, v0.2s, v1.2s, #0")
CASE(c69, "fcmla v0.2s, v1.2s, v0.2s, #0")
CASE(c70, "fcmla v0.2s, v0.2s, v1.2s, #90")
CASE(c71, "fcmla v0.2s, v1.2s, v0.2s, #90")
CASE(c72, "fcmla v0.2s, v0.2s, v1.2s, #180")
CASE(c73, "fcmla v0.2s, v1.2s, v0.2s, #180")
CASE(c74, "fcmla v0.2s, v0.2s, v1.2s, #270")
CASE(c75, "fcmla v0.2s, v1.2s, v0.2s, #270")
CASE(c76, "fcadd v0.2s, v0.2s, v1.2s, #90")
CASE(c77, "fcadd v0.2s, v0.2s, v1.2s, #270")
CASE(c78, "fcmla v0.4s, v0.4s, v1.4s, #0")
CASE(c79, "fcmla v0.4s, v1.4s, v0.4s, #0")
CASE(c80, "fcmla v0.4s, v0.4s, v1.s[0], #0")
CASE(c81, "fcmla v0.4s, v1.4s, v0.s[0], #0")
CASE(c82, "fcmla v0.4s, v0.4s, v1.s[1], #0")
CASE(c83, "fcmla v0.4s, v1.4s, v0.s[1], #0")
CASE(c84, "fcmla v0.4s, v0.4s, v1.4s, #90")
CASE(c85, "fcmla v0.4s, v1.4s, v0.4s, #90")
CASE(c86, "fcmla v0.4s, v0.4s, v1.s[0], #90")
CASE(c87, "fcmla v0.4s, v1.4s, v0.s[0], #90")
CASE(c88, "fcmla v0.4s, v0.4s, v1.s[1], #90")
CASE(c89, "fcmla v0.4s, v1.4s, v0.s[1], #90")
CASE(c90, "fcmla v0.4s, v0.4s, v1.4s, #180")
CASE(c91, "fcmla v0.4s, v1.4s, v0.4s, #180")
CASE(c92, "fcmla v0.4s, v0.4s, v1.s[0], #180")
CASE(c93, "fcmla v0.4s, v1.4s, v0.s[0], #180")
CASE(c94, "fcmla v0.4s, v0.4s, v1.s[1], #180")
CASE(c95, "fcmla v0.4s, v1.4s, v0.s[1], #180")
CASE(c96, "fcmla v0.4s, v0.4s, v1.4s, #270")
CASE(c97, "fcmla v0.4s, v1.4s, v0.4s, #270")
CASE(c98, "fcmla v0.4s, v0.4s, v1.s[0], #270")
CASE(c99, "fcmla v0.4s, v1.4s, v0.s[0], #270")
CASE(c100, "fcmla v0.4s, v0.4s, v1.s[1], #270")
CASE(c101, "fcmla v0.4s, v1.4s, v0.s[1], #270")
CASE(c102, "fcadd v0.4s, v0.4s, v1.4s, #90")
CASE(c103, "fcadd v0.4s, v0.4s, v1.4s, #270")
CASE(c104, "fcmla v0.2d, v0.2d, v1.2d, #0")
CASE(c105, "fcmla v0.2d, v1.2d, v0.2d, #0")
CASE(c106, "fcmla v0.2d, v0.2d, v1.2d, #90")
CASE(c107, "fcmla v0.2d, v1.2d, v0.2d, #90")
CASE(c108, "fcmla v0.2d, v0.2d, v1.2d, #180")
CASE(c109, "fcmla v0.2d, v1.2d, v0.2d, #180")
CASE(c110, "fcmla v0.2d, v0.2d, v1.2d, #270")
CASE(c111, "fcmla v0.2d, v1.2d, v0.2d, #270")
CASE(c112, "fcadd v0.2d, v0.2d, v1.2d, #90")
CASE(c113, "fcadd v0.2d, v0.2d, v1.2d, #270")
static void (*const cases[])(vec,vec,uint64_t,uint64_t)={
c0,
c1,
c2,
c3,
c4,
c5,
c6,
c7,
c8,
c9,
c10,
c11,
c12,
c13,
c14,
c15,
c16,
c17,
c18,
c19,
c20,
c21,
c22,
c23,
c24,
c25,
c26,
c27,
c28,
c29,
c30,
c31,
c32,
c33,
c34,
c35,
c36,
c37,
c38,
c39,
c40,
c41,
c42,
c43,
c44,
c45,
c46,
c47,
c48,
c49,
c50,
c51,
c52,
c53,
c54,
c55,
c56,
c57,
c58,
c59,
c60,
c61,
c62,
c63,
c64,
c65,
c66,
c67,
c68,
c69,
c70,
c71,
c72,
c73,
c74,
c75,
c76,
c77,
c78,
c79,
c80,
c81,
c82,
c83,
c84,
c85,
c86,
c87,
c88,
c89,
c90,
c91,
c92,
c93,
c94,
c95,
c96,
c97,
c98,
c99,
c100,
c101,
c102,
c103,
c104,
c105,
c106,
c107,
c108,
c109,
c110,
c111,
c112,
c113
};
int main(void) {
    static const uint64_t inputs[]={0,0x800000007c00fc00ULL,0x7d017e017fff7c01ULL,
        0x000103ff04000401ULL,0x3bff3c003c013e00ULL,0x7bfffbff3555b555ULL,
        0x800183ff84008401ULL,0x0001800100028002ULL,0x4000c0004c00cc00ULL,
        0x7f8000017fc00001ULL,0x3f80000040000000ULL,0x7ff0000000000001ULL,
        0x7ff8000000000001ULL,0x3ff0000000000000ULL,0x0010000000000001ULL};
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
