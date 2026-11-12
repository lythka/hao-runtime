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
CASE(c0, "usdot v0.2s, v0.8b, v1.8b")
CASE(c1, "usdot v0.2s, v0.8b, v1.4b[0]")
CASE(c2, "usdot v0.2s, v0.8b, v1.4b[1]")
CASE(c3, "usdot v0.2s, v0.8b, v1.4b[2]")
CASE(c4, "usdot v0.2s, v0.8b, v1.4b[3]")
CASE(c5, "usdot v0.2s, v1.8b, v0.8b")
CASE(c6, "usdot v0.2s, v1.8b, v0.4b[0]")
CASE(c7, "usdot v0.2s, v1.8b, v0.4b[1]")
CASE(c8, "usdot v0.2s, v1.8b, v0.4b[2]")
CASE(c9, "usdot v0.2s, v1.8b, v0.4b[3]")
CASE(c10, "usdot v0.4s, v0.16b, v1.16b")
CASE(c11, "usdot v0.4s, v0.16b, v1.4b[0]")
CASE(c12, "usdot v0.4s, v0.16b, v1.4b[1]")
CASE(c13, "usdot v0.4s, v0.16b, v1.4b[2]")
CASE(c14, "usdot v0.4s, v0.16b, v1.4b[3]")
CASE(c15, "usdot v0.4s, v1.16b, v0.16b")
CASE(c16, "usdot v0.4s, v1.16b, v0.4b[0]")
CASE(c17, "usdot v0.4s, v1.16b, v0.4b[1]")
CASE(c18, "usdot v0.4s, v1.16b, v0.4b[2]")
CASE(c19, "usdot v0.4s, v1.16b, v0.4b[3]")
CASE(c20, "sudot v0.2s, v0.8b, v1.4b[0]")
CASE(c21, "sudot v0.2s, v0.8b, v1.4b[1]")
CASE(c22, "sudot v0.2s, v0.8b, v1.4b[2]")
CASE(c23, "sudot v0.2s, v0.8b, v1.4b[3]")
CASE(c24, "sudot v0.2s, v1.8b, v0.4b[0]")
CASE(c25, "sudot v0.2s, v1.8b, v0.4b[1]")
CASE(c26, "sudot v0.2s, v1.8b, v0.4b[2]")
CASE(c27, "sudot v0.2s, v1.8b, v0.4b[3]")
CASE(c28, "sudot v0.4s, v0.16b, v1.4b[0]")
CASE(c29, "sudot v0.4s, v0.16b, v1.4b[1]")
CASE(c30, "sudot v0.4s, v0.16b, v1.4b[2]")
CASE(c31, "sudot v0.4s, v0.16b, v1.4b[3]")
CASE(c32, "sudot v0.4s, v1.16b, v0.4b[0]")
CASE(c33, "sudot v0.4s, v1.16b, v0.4b[1]")
CASE(c34, "sudot v0.4s, v1.16b, v0.4b[2]")
CASE(c35, "sudot v0.4s, v1.16b, v0.4b[3]")
CASE(c36, "smmla v0.4s, v0.16b, v1.16b")
CASE(c37, "smmla v0.4s, v1.16b, v0.16b")
CASE(c38, "smmla v0.4s, v1.16b, v1.16b")
CASE(c39, "ummla v0.4s, v0.16b, v1.16b")
CASE(c40, "ummla v0.4s, v1.16b, v0.16b")
CASE(c41, "ummla v0.4s, v1.16b, v1.16b")
CASE(c42, "usmmla v0.4s, v0.16b, v1.16b")
CASE(c43, "usmmla v0.4s, v1.16b, v0.16b")
CASE(c44, "usmmla v0.4s, v1.16b, v1.16b")
static void (*const cases[])(vec,vec,uint64_t,uint64_t)={c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,c20,c21,c22,c23,c24,c25,c26,c27,c28,c29,c30,c31,c32,c33,c34,c35,c36,c37,c38,c39,c40,c41,c42,c43,c44};
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
