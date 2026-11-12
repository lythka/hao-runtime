/* Saturating multiply/accumulate: native results, QC and register aliases. */
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
CASE(c0, "sqrdmlah v0.4h, v0.4h, v1.4h")
CASE(c1, "sqrdmlah v0.4h, v1.4h, v0.4h")
CASE(c2, "sqrdmlah v0.4h, v0.4h, v1.h[0]")
CASE(c3, "sqrdmlah v0.4h, v0.4h, v1.h[1]")
CASE(c4, "sqrdmlah v0.4h, v0.4h, v1.h[2]")
CASE(c5, "sqrdmlah v0.4h, v0.4h, v1.h[3]")
CASE(c6, "sqrdmlah v0.4h, v0.4h, v1.h[4]")
CASE(c7, "sqrdmlah v0.4h, v0.4h, v1.h[5]")
CASE(c8, "sqrdmlah v0.4h, v0.4h, v1.h[6]")
CASE(c9, "sqrdmlah v0.4h, v0.4h, v1.h[7]")
CASE(c10, "sqrdmlah v0.8h, v0.8h, v1.8h")
CASE(c11, "sqrdmlah v0.8h, v1.8h, v0.8h")
CASE(c12, "sqrdmlah v0.8h, v0.8h, v1.h[0]")
CASE(c13, "sqrdmlah v0.8h, v0.8h, v1.h[1]")
CASE(c14, "sqrdmlah v0.8h, v0.8h, v1.h[2]")
CASE(c15, "sqrdmlah v0.8h, v0.8h, v1.h[3]")
CASE(c16, "sqrdmlah v0.8h, v0.8h, v1.h[4]")
CASE(c17, "sqrdmlah v0.8h, v0.8h, v1.h[5]")
CASE(c18, "sqrdmlah v0.8h, v0.8h, v1.h[6]")
CASE(c19, "sqrdmlah v0.8h, v0.8h, v1.h[7]")
CASE(c20, "sqrdmlah v0.2s, v0.2s, v1.2s")
CASE(c21, "sqrdmlah v0.2s, v1.2s, v0.2s")
CASE(c22, "sqrdmlah v0.2s, v0.2s, v1.s[0]")
CASE(c23, "sqrdmlah v0.2s, v0.2s, v1.s[1]")
CASE(c24, "sqrdmlah v0.2s, v0.2s, v1.s[2]")
CASE(c25, "sqrdmlah v0.2s, v0.2s, v1.s[3]")
CASE(c26, "sqrdmlah v0.4s, v0.4s, v1.4s")
CASE(c27, "sqrdmlah v0.4s, v1.4s, v0.4s")
CASE(c28, "sqrdmlah v0.4s, v0.4s, v1.s[0]")
CASE(c29, "sqrdmlah v0.4s, v0.4s, v1.s[1]")
CASE(c30, "sqrdmlah v0.4s, v0.4s, v1.s[2]")
CASE(c31, "sqrdmlah v0.4s, v0.4s, v1.s[3]")
CASE(c32, "sqrdmlah h0, h0, h1")
CASE(c33, "sqrdmlah h0, h1, h0")
CASE(c34, "sqrdmlah h0, h0, v1.h[0]")
CASE(c35, "sqrdmlah h0, h0, v1.h[1]")
CASE(c36, "sqrdmlah h0, h0, v1.h[2]")
CASE(c37, "sqrdmlah h0, h0, v1.h[3]")
CASE(c38, "sqrdmlah h0, h0, v1.h[4]")
CASE(c39, "sqrdmlah h0, h0, v1.h[5]")
CASE(c40, "sqrdmlah h0, h0, v1.h[6]")
CASE(c41, "sqrdmlah h0, h0, v1.h[7]")
CASE(c42, "sqrdmlah s0, s0, s1")
CASE(c43, "sqrdmlah s0, s1, s0")
CASE(c44, "sqrdmlah s0, s0, v1.s[0]")
CASE(c45, "sqrdmlah s0, s0, v1.s[1]")
CASE(c46, "sqrdmlah s0, s0, v1.s[2]")
CASE(c47, "sqrdmlah s0, s0, v1.s[3]")
CASE(c48, "sqrdmlsh v0.4h, v0.4h, v1.4h")
CASE(c49, "sqrdmlsh v0.4h, v1.4h, v0.4h")
CASE(c50, "sqrdmlsh v0.4h, v0.4h, v1.h[0]")
CASE(c51, "sqrdmlsh v0.4h, v0.4h, v1.h[1]")
CASE(c52, "sqrdmlsh v0.4h, v0.4h, v1.h[2]")
CASE(c53, "sqrdmlsh v0.4h, v0.4h, v1.h[3]")
CASE(c54, "sqrdmlsh v0.4h, v0.4h, v1.h[4]")
CASE(c55, "sqrdmlsh v0.4h, v0.4h, v1.h[5]")
CASE(c56, "sqrdmlsh v0.4h, v0.4h, v1.h[6]")
CASE(c57, "sqrdmlsh v0.4h, v0.4h, v1.h[7]")
CASE(c58, "sqrdmlsh v0.8h, v0.8h, v1.8h")
CASE(c59, "sqrdmlsh v0.8h, v1.8h, v0.8h")
CASE(c60, "sqrdmlsh v0.8h, v0.8h, v1.h[0]")
CASE(c61, "sqrdmlsh v0.8h, v0.8h, v1.h[1]")
CASE(c62, "sqrdmlsh v0.8h, v0.8h, v1.h[2]")
CASE(c63, "sqrdmlsh v0.8h, v0.8h, v1.h[3]")
CASE(c64, "sqrdmlsh v0.8h, v0.8h, v1.h[4]")
CASE(c65, "sqrdmlsh v0.8h, v0.8h, v1.h[5]")
CASE(c66, "sqrdmlsh v0.8h, v0.8h, v1.h[6]")
CASE(c67, "sqrdmlsh v0.8h, v0.8h, v1.h[7]")
CASE(c68, "sqrdmlsh v0.2s, v0.2s, v1.2s")
CASE(c69, "sqrdmlsh v0.2s, v1.2s, v0.2s")
CASE(c70, "sqrdmlsh v0.2s, v0.2s, v1.s[0]")
CASE(c71, "sqrdmlsh v0.2s, v0.2s, v1.s[1]")
CASE(c72, "sqrdmlsh v0.2s, v0.2s, v1.s[2]")
CASE(c73, "sqrdmlsh v0.2s, v0.2s, v1.s[3]")
CASE(c74, "sqrdmlsh v0.4s, v0.4s, v1.4s")
CASE(c75, "sqrdmlsh v0.4s, v1.4s, v0.4s")
CASE(c76, "sqrdmlsh v0.4s, v0.4s, v1.s[0]")
CASE(c77, "sqrdmlsh v0.4s, v0.4s, v1.s[1]")
CASE(c78, "sqrdmlsh v0.4s, v0.4s, v1.s[2]")
CASE(c79, "sqrdmlsh v0.4s, v0.4s, v1.s[3]")
CASE(c80, "sqrdmlsh h0, h0, h1")
CASE(c81, "sqrdmlsh h0, h1, h0")
CASE(c82, "sqrdmlsh h0, h0, v1.h[0]")
CASE(c83, "sqrdmlsh h0, h0, v1.h[1]")
CASE(c84, "sqrdmlsh h0, h0, v1.h[2]")
CASE(c85, "sqrdmlsh h0, h0, v1.h[3]")
CASE(c86, "sqrdmlsh h0, h0, v1.h[4]")
CASE(c87, "sqrdmlsh h0, h0, v1.h[5]")
CASE(c88, "sqrdmlsh h0, h0, v1.h[6]")
CASE(c89, "sqrdmlsh h0, h0, v1.h[7]")
CASE(c90, "sqrdmlsh s0, s0, s1")
CASE(c91, "sqrdmlsh s0, s1, s0")
CASE(c92, "sqrdmlsh s0, s0, v1.s[0]")
CASE(c93, "sqrdmlsh s0, s0, v1.s[1]")
CASE(c94, "sqrdmlsh s0, s0, v1.s[2]")
CASE(c95, "sqrdmlsh s0, s0, v1.s[3]")
CASE(c96, "sqdmlal v0.4s, v0.4h, v1.4h")
CASE(c97, "sqdmlal2 v0.4s, v0.8h, v1.8h")
CASE(c98, "sqdmlal v0.4s, v0.4h, v1.h[0]")
CASE(c99, "sqdmlal2 v0.4s, v0.8h, v1.h[0]")
CASE(c100, "sqdmlal v0.4s, v0.4h, v1.h[1]")
CASE(c101, "sqdmlal2 v0.4s, v0.8h, v1.h[1]")
CASE(c102, "sqdmlal v0.4s, v0.4h, v1.h[2]")
CASE(c103, "sqdmlal2 v0.4s, v0.8h, v1.h[2]")
CASE(c104, "sqdmlal v0.4s, v0.4h, v1.h[3]")
CASE(c105, "sqdmlal2 v0.4s, v0.8h, v1.h[3]")
CASE(c106, "sqdmlal v0.4s, v0.4h, v1.h[4]")
CASE(c107, "sqdmlal2 v0.4s, v0.8h, v1.h[4]")
CASE(c108, "sqdmlal v0.4s, v0.4h, v1.h[5]")
CASE(c109, "sqdmlal2 v0.4s, v0.8h, v1.h[5]")
CASE(c110, "sqdmlal v0.4s, v0.4h, v1.h[6]")
CASE(c111, "sqdmlal2 v0.4s, v0.8h, v1.h[6]")
CASE(c112, "sqdmlal v0.4s, v0.4h, v1.h[7]")
CASE(c113, "sqdmlal2 v0.4s, v0.8h, v1.h[7]")
CASE(c114, "sqdmlal v0.2d, v0.2s, v1.2s")
CASE(c115, "sqdmlal2 v0.2d, v0.4s, v1.4s")
CASE(c116, "sqdmlal v0.2d, v0.2s, v1.s[0]")
CASE(c117, "sqdmlal2 v0.2d, v0.4s, v1.s[0]")
CASE(c118, "sqdmlal v0.2d, v0.2s, v1.s[1]")
CASE(c119, "sqdmlal2 v0.2d, v0.4s, v1.s[1]")
CASE(c120, "sqdmlal v0.2d, v0.2s, v1.s[2]")
CASE(c121, "sqdmlal2 v0.2d, v0.4s, v1.s[2]")
CASE(c122, "sqdmlal v0.2d, v0.2s, v1.s[3]")
CASE(c123, "sqdmlal2 v0.2d, v0.4s, v1.s[3]")
CASE(c124, "sqdmlal s0, h0, h1")
CASE(c125, "sqdmlal s0, h0, v1.h[0]")
CASE(c126, "sqdmlal s0, h0, v1.h[1]")
CASE(c127, "sqdmlal s0, h0, v1.h[2]")
CASE(c128, "sqdmlal s0, h0, v1.h[3]")
CASE(c129, "sqdmlal s0, h0, v1.h[4]")
CASE(c130, "sqdmlal s0, h0, v1.h[5]")
CASE(c131, "sqdmlal s0, h0, v1.h[6]")
CASE(c132, "sqdmlal s0, h0, v1.h[7]")
CASE(c133, "sqdmlal d0, s0, s1")
CASE(c134, "sqdmlal d0, s0, v1.s[0]")
CASE(c135, "sqdmlal d0, s0, v1.s[1]")
CASE(c136, "sqdmlal d0, s0, v1.s[2]")
CASE(c137, "sqdmlal d0, s0, v1.s[3]")
CASE(c138, "sqdmlsl v0.4s, v0.4h, v1.4h")
CASE(c139, "sqdmlsl2 v0.4s, v0.8h, v1.8h")
CASE(c140, "sqdmlsl v0.4s, v0.4h, v1.h[0]")
CASE(c141, "sqdmlsl2 v0.4s, v0.8h, v1.h[0]")
CASE(c142, "sqdmlsl v0.4s, v0.4h, v1.h[1]")
CASE(c143, "sqdmlsl2 v0.4s, v0.8h, v1.h[1]")
CASE(c144, "sqdmlsl v0.4s, v0.4h, v1.h[2]")
CASE(c145, "sqdmlsl2 v0.4s, v0.8h, v1.h[2]")
CASE(c146, "sqdmlsl v0.4s, v0.4h, v1.h[3]")
CASE(c147, "sqdmlsl2 v0.4s, v0.8h, v1.h[3]")
CASE(c148, "sqdmlsl v0.4s, v0.4h, v1.h[4]")
CASE(c149, "sqdmlsl2 v0.4s, v0.8h, v1.h[4]")
CASE(c150, "sqdmlsl v0.4s, v0.4h, v1.h[5]")
CASE(c151, "sqdmlsl2 v0.4s, v0.8h, v1.h[5]")
CASE(c152, "sqdmlsl v0.4s, v0.4h, v1.h[6]")
CASE(c153, "sqdmlsl2 v0.4s, v0.8h, v1.h[6]")
CASE(c154, "sqdmlsl v0.4s, v0.4h, v1.h[7]")
CASE(c155, "sqdmlsl2 v0.4s, v0.8h, v1.h[7]")
CASE(c156, "sqdmlsl v0.2d, v0.2s, v1.2s")
CASE(c157, "sqdmlsl2 v0.2d, v0.4s, v1.4s")
CASE(c158, "sqdmlsl v0.2d, v0.2s, v1.s[0]")
CASE(c159, "sqdmlsl2 v0.2d, v0.4s, v1.s[0]")
CASE(c160, "sqdmlsl v0.2d, v0.2s, v1.s[1]")
CASE(c161, "sqdmlsl2 v0.2d, v0.4s, v1.s[1]")
CASE(c162, "sqdmlsl v0.2d, v0.2s, v1.s[2]")
CASE(c163, "sqdmlsl2 v0.2d, v0.4s, v1.s[2]")
CASE(c164, "sqdmlsl v0.2d, v0.2s, v1.s[3]")
CASE(c165, "sqdmlsl2 v0.2d, v0.4s, v1.s[3]")
CASE(c166, "sqdmlsl s0, h0, h1")
CASE(c167, "sqdmlsl s0, h0, v1.h[0]")
CASE(c168, "sqdmlsl s0, h0, v1.h[1]")
CASE(c169, "sqdmlsl s0, h0, v1.h[2]")
CASE(c170, "sqdmlsl s0, h0, v1.h[3]")
CASE(c171, "sqdmlsl s0, h0, v1.h[4]")
CASE(c172, "sqdmlsl s0, h0, v1.h[5]")
CASE(c173, "sqdmlsl s0, h0, v1.h[6]")
CASE(c174, "sqdmlsl s0, h0, v1.h[7]")
CASE(c175, "sqdmlsl d0, s0, s1")
CASE(c176, "sqdmlsl d0, s0, v1.s[0]")
CASE(c177, "sqdmlsl d0, s0, v1.s[1]")
CASE(c178, "sqdmlsl d0, s0, v1.s[2]")
CASE(c179, "sqdmlsl d0, s0, v1.s[3]")
CASE(c180, "sqdmull v0.4s, v0.4h, v1.4h")
CASE(c181, "sqdmull2 v0.4s, v0.8h, v1.8h")
CASE(c182, "sqdmull v0.4s, v0.4h, v1.h[0]")
CASE(c183, "sqdmull2 v0.4s, v0.8h, v1.h[0]")
CASE(c184, "sqdmull v0.4s, v0.4h, v1.h[1]")
CASE(c185, "sqdmull2 v0.4s, v0.8h, v1.h[1]")
CASE(c186, "sqdmull v0.4s, v0.4h, v1.h[2]")
CASE(c187, "sqdmull2 v0.4s, v0.8h, v1.h[2]")
CASE(c188, "sqdmull v0.4s, v0.4h, v1.h[3]")
CASE(c189, "sqdmull2 v0.4s, v0.8h, v1.h[3]")
CASE(c190, "sqdmull v0.4s, v0.4h, v1.h[4]")
CASE(c191, "sqdmull2 v0.4s, v0.8h, v1.h[4]")
CASE(c192, "sqdmull v0.4s, v0.4h, v1.h[5]")
CASE(c193, "sqdmull2 v0.4s, v0.8h, v1.h[5]")
CASE(c194, "sqdmull v0.4s, v0.4h, v1.h[6]")
CASE(c195, "sqdmull2 v0.4s, v0.8h, v1.h[6]")
CASE(c196, "sqdmull v0.4s, v0.4h, v1.h[7]")
CASE(c197, "sqdmull2 v0.4s, v0.8h, v1.h[7]")
CASE(c198, "sqdmull v0.2d, v0.2s, v1.2s")
CASE(c199, "sqdmull2 v0.2d, v0.4s, v1.4s")
CASE(c200, "sqdmull v0.2d, v0.2s, v1.s[0]")
CASE(c201, "sqdmull2 v0.2d, v0.4s, v1.s[0]")
CASE(c202, "sqdmull v0.2d, v0.2s, v1.s[1]")
CASE(c203, "sqdmull2 v0.2d, v0.4s, v1.s[1]")
CASE(c204, "sqdmull v0.2d, v0.2s, v1.s[2]")
CASE(c205, "sqdmull2 v0.2d, v0.4s, v1.s[2]")
CASE(c206, "sqdmull v0.2d, v0.2s, v1.s[3]")
CASE(c207, "sqdmull2 v0.2d, v0.4s, v1.s[3]")
CASE(c208, "sqdmull s0, h0, h1")
CASE(c209, "sqdmull s0, h0, v1.h[0]")
CASE(c210, "sqdmull s0, h0, v1.h[1]")
CASE(c211, "sqdmull s0, h0, v1.h[2]")
CASE(c212, "sqdmull s0, h0, v1.h[3]")
CASE(c213, "sqdmull s0, h0, v1.h[4]")
CASE(c214, "sqdmull s0, h0, v1.h[5]")
CASE(c215, "sqdmull s0, h0, v1.h[6]")
CASE(c216, "sqdmull s0, h0, v1.h[7]")
CASE(c217, "sqdmull d0, s0, s1")
CASE(c218, "sqdmull d0, s0, v1.s[0]")
CASE(c219, "sqdmull d0, s0, v1.s[1]")
CASE(c220, "sqdmull d0, s0, v1.s[2]")
CASE(c221, "sqdmull d0, s0, v1.s[3]")
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
c113,
c114,
c115,
c116,
c117,
c118,
c119,
c120,
c121,
c122,
c123,
c124,
c125,
c126,
c127,
c128,
c129,
c130,
c131,
c132,
c133,
c134,
c135,
c136,
c137,
c138,
c139,
c140,
c141,
c142,
c143,
c144,
c145,
c146,
c147,
c148,
c149,
c150,
c151,
c152,
c153,
c154,
c155,
c156,
c157,
c158,
c159,
c160,
c161,
c162,
c163,
c164,
c165,
c166,
c167,
c168,
c169,
c170,
c171,
c172,
c173,
c174,
c175,
c176,
c177,
c178,
c179,
c180,
c181,
c182,
c183,
c184,
c185,
c186,
c187,
c188,
c189,
c190,
c191,
c192,
c193,
c194,
c195,
c196,
c197,
c198,
c199,
c200,
c201,
c202,
c203,
c204,
c205,
c206,
c207,
c208,
c209,
c210,
c211,
c212,
c213,
c214,
c215,
c216,
c217,
c218,
c219,
c220,
c221
};
int main(void) {
    static const uint64_t inputs[]={0,1,~0ULL,0x8000800080008000ULL,
        0x7fff7fff7fff7fffULL,0x8000000080000000ULL,0x7fffffff7fffffffULL,
        1ULL<<63,(1ULL<<63)-1,0x4000400040004000ULL,0xc000c000c000c000ULL,
        0x4000000040000000ULL,0xc0000000c0000000ULL,0x80017ffe7fff8000ULL};
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        for (unsigned a=0;a<sizeof inputs/sizeof *inputs;a++)
            for (unsigned b=0;b<sizeof inputs/sizeof *inputs;b++) {
                vec x={inputs[a],~inputs[a]},y={inputs[b],~inputs[b]};
                cases[c](x,y,0,0);cases[c](x,y,0,0x0800009f);
            }
        printf("%u %016llx\n",c,(unsigned long long)hash);
    }
    return 0;
}
