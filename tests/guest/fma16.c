/* Native result and full-FPSR comparison for half vector fused forms. */
#include "../oracle/fma16_cases.h"
#include <stdio.h>
#define RUN(I) __asm__ volatile("ldp q0,q1,[%[st]]\n ldr q2,[%[st],#32]\n msr fpcr,%[cr]\n msr fpsr,%[sr]\n" I "\n mrs %[so],fpsr\n msr fpcr,xzr\n str q0,[%[st]]" : [so] "=&r"(so) : [st] "r"(st), [cr] "r"(cr), [sr] "r"(sr) : "v0","v1","v2","memory")
static unsigned run(unsigned f, uint16_t st[3][8], uint64_t cr, uint64_t sr)
{
    uint64_t so;
    switch(f) {
    case 0: RUN("fmla v0.8h,v1.8h,v2.8h"); break;
    case 1: RUN("fmla v0.8h,v1.8h,v2.h[0]"); break;
    case 2: RUN("fmla v0.8h,v1.8h,v2.h[1]"); break;
    case 3: RUN("fmla v0.8h,v1.8h,v2.h[2]"); break;
    case 4: RUN("fmla v0.8h,v1.8h,v2.h[3]"); break;
    case 5: RUN("fmla v0.8h,v1.8h,v2.h[4]"); break;
    case 6: RUN("fmla v0.8h,v1.8h,v2.h[5]"); break;
    case 7: RUN("fmla v0.8h,v1.8h,v2.h[6]"); break;
    case 8: RUN("fmla v0.8h,v1.8h,v2.h[7]"); break;
    case 9: RUN("fmls v0.8h,v1.8h,v2.8h"); break;
    case 10: RUN("fmls v0.8h,v1.8h,v2.h[0]"); break;
    case 11: RUN("fmls v0.8h,v1.8h,v2.h[1]"); break;
    case 12: RUN("fmls v0.8h,v1.8h,v2.h[2]"); break;
    case 13: RUN("fmls v0.8h,v1.8h,v2.h[3]"); break;
    case 14: RUN("fmls v0.8h,v1.8h,v2.h[4]"); break;
    case 15: RUN("fmls v0.8h,v1.8h,v2.h[5]"); break;
    case 16: RUN("fmls v0.8h,v1.8h,v2.h[6]"); break;
    case 17: RUN("fmls v0.8h,v1.8h,v2.h[7]"); break;
    case 18: RUN("fmla v0.4h,v1.4h,v2.4h"); break;
    case 19: RUN("fmla v0.4h,v1.4h,v2.h[0]"); break;
    case 20: RUN("fmla v0.4h,v1.4h,v2.h[1]"); break;
    case 21: RUN("fmla v0.4h,v1.4h,v2.h[2]"); break;
    case 22: RUN("fmla v0.4h,v1.4h,v2.h[3]"); break;
    case 23: RUN("fmla v0.4h,v1.4h,v2.h[4]"); break;
    case 24: RUN("fmla v0.4h,v1.4h,v2.h[5]"); break;
    case 25: RUN("fmla v0.4h,v1.4h,v2.h[6]"); break;
    case 26: RUN("fmla v0.4h,v1.4h,v2.h[7]"); break;
    case 27: RUN("fmls v0.4h,v1.4h,v2.4h"); break;
    case 28: RUN("fmls v0.4h,v1.4h,v2.h[0]"); break;
    case 29: RUN("fmls v0.4h,v1.4h,v2.h[1]"); break;
    case 30: RUN("fmls v0.4h,v1.4h,v2.h[2]"); break;
    case 31: RUN("fmls v0.4h,v1.4h,v2.h[3]"); break;
    case 32: RUN("fmls v0.4h,v1.4h,v2.h[4]"); break;
    case 33: RUN("fmls v0.4h,v1.4h,v2.h[5]"); break;
    case 34: RUN("fmls v0.4h,v1.4h,v2.h[6]"); break;
    case 35: RUN("fmls v0.4h,v1.4h,v2.h[7]"); break;
    case 36: RUN("fmla v0.8h,v0.8h,v2.8h"); break;
    case 37: RUN("fmls v0.8h,v1.8h,v0.8h"); break;
    case 38: RUN("fmla v0.8h,v1.8h,v1.8h"); break;
    case 39: RUN("fmls v0.8h,v0.8h,v0.8h"); break;
    case 40: RUN("fmla v0.4h,v1.4h,v0.h[7]"); break;
    case 41: RUN("fmls v0.8h,v0.8h,v2.h[5]"); break;
    default: return 0;
    }
    return so;
}
int main(void)
{
    uint64_t rng=0x657721b39a1355ull;
    for (unsigned f=0; f<42; f++) {
        uint64_t h=0xcbf29ce484222325ull;
        for (unsigned control=0; control<16; control++)
            for (unsigned k=0; k<512; k++) {
                uint16_t st[3][8];
                for (unsigned l=0; l<8; l++) {
                    fma16_ops o=fma16_case(&rng, k<22 ? k : k+l);
                    st[0][l]=o.c; st[1][l]=o.a; st[2][l]=o.b;
                }
                /* Indexed lanes must be distinguishable in ordinary cases. */
                if (k>=22 && !(k%4)) for(unsigned l=0;l<8;l++) st[2][l]=0x3000+l*0x101;
                unsigned sr=k&1 ? 0x0800009f : 0;
                unsigned flags=run(f,st,(uint64_t)control<<22,sr);
                for(unsigned l=0;l<8;l++) h=(h^st[0][l])*0x100000001b3ull;
                h=(h^flags)*0x100000001b3ull;
            }
        printf("form %u %016llx\n",f,(unsigned long long)h);
    }
    return 0;
}
