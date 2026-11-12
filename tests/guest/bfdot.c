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
CASE(c0, "bfdot v0.2s, v0.4h, v1.4h")
CASE(c1, "bfdot v0.2s, v0.4h, v1.2h[0]")
CASE(c2, "bfdot v0.2s, v0.4h, v1.2h[1]")
CASE(c3, "bfdot v0.2s, v0.4h, v1.2h[2]")
CASE(c4, "bfdot v0.2s, v0.4h, v1.2h[3]")
CASE(c5, "bfdot v0.2s, v1.4h, v0.4h")
CASE(c6, "bfdot v0.2s, v1.4h, v0.2h[0]")
CASE(c7, "bfdot v0.2s, v1.4h, v0.2h[1]")
CASE(c8, "bfdot v0.2s, v1.4h, v0.2h[2]")
CASE(c9, "bfdot v0.2s, v1.4h, v0.2h[3]")
CASE(c10, "bfdot v0.2s, v1.4h, v1.4h")
CASE(c11, "bfdot v0.2s, v1.4h, v1.2h[0]")
CASE(c12, "bfdot v0.2s, v1.4h, v1.2h[1]")
CASE(c13, "bfdot v0.2s, v1.4h, v1.2h[2]")
CASE(c14, "bfdot v0.2s, v1.4h, v1.2h[3]")
CASE(c15, "bfdot v0.4s, v0.8h, v1.8h")
CASE(c16, "bfdot v0.4s, v0.8h, v1.2h[0]")
CASE(c17, "bfdot v0.4s, v0.8h, v1.2h[1]")
CASE(c18, "bfdot v0.4s, v0.8h, v1.2h[2]")
CASE(c19, "bfdot v0.4s, v0.8h, v1.2h[3]")
CASE(c20, "bfdot v0.4s, v1.8h, v0.8h")
CASE(c21, "bfdot v0.4s, v1.8h, v0.2h[0]")
CASE(c22, "bfdot v0.4s, v1.8h, v0.2h[1]")
CASE(c23, "bfdot v0.4s, v1.8h, v0.2h[2]")
CASE(c24, "bfdot v0.4s, v1.8h, v0.2h[3]")
CASE(c25, "bfdot v0.4s, v1.8h, v1.8h")
CASE(c26, "bfdot v0.4s, v1.8h, v1.2h[0]")
CASE(c27, "bfdot v0.4s, v1.8h, v1.2h[1]")
CASE(c28, "bfdot v0.4s, v1.8h, v1.2h[2]")
CASE(c29, "bfdot v0.4s, v1.8h, v1.2h[3]")
CASE(c30, "bfmmla v0.4s, v0.8h, v1.8h")
CASE(c31, "bfmmla v0.4s, v1.8h, v0.8h")
CASE(c32, "bfmmla v0.4s, v1.8h, v1.8h")
static void (*const cases[])(vec,vec,uint64_t,uint64_t)={c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,c20,c21,c22,c23,c24,c25,c26,c27,c28,c29,c30,c31,c32};
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
        0x8000000000000000ULL,0xffffffffffffffffULL,0x7f817fc180018001ULL,0x3f813fff7f7f007fULL,0x3f80800000808080ULL};
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        for (unsigned cr=0;cr<32;cr++)
            for (unsigned a=0;a<sizeof inputs/sizeof *inputs;a++)
                for (unsigned b=0;b<sizeof inputs/sizeof *inputs;b++) {
                    vec x={inputs[a],~inputs[a]},y={inputs[b],~inputs[b]};
                    uint64_t fpcr=((cr&3)<<22)|((cr&4)<<22)|((cr&8)<<22)|((cr&16)<<9);
                    cases[c](x,y,fpcr,0);cases[c](x,y,fpcr,0x0800009f);
                }
        printf("%u %016llx\n",c,(unsigned long long)hash);
    }
    return 0;
}
