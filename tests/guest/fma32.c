/*
 * Single-precision fused multiply-add against native arm64: FMLA/FMLS .4S and
 * .2S, by element, scalar by element, FMADD/FMSUB/FNMADD/FNMSUB, each with
 * distinct and aliased registers, on the operand classes of
 * tests/oracle/fma_cases.h, under all sixteen RMode/FZ/DN settings and with
 * clear and preset FPSR flags. Prints one line per form and operand class: a
 * hash of every result register and FPSR value. Run by tests/guest/fma32.sh.
 */
#include "../oracle/fma_cases.h"
#include <stdio.h>
#include <stdlib.h>

/* v0 = st[0] (destination/accumulator), v1 = st[1], v2 = st[2], v3 = st[3] */
#define RUN(INSN) \
    __asm__ volatile("ldp q0, q1, [%[st]]\n ldp q2, q3, [%[st], #32]\n msr fpcr, %[cr]\n msr fpsr, %[sr]\n" \
                     INSN "\n mrs %[so], fpsr\n msr fpcr, %[z]\n str q0, [%[st]]" \
                     : [so] "=&r"(so) : [st] "r"(st), [cr] "r"(cr), [sr] "r"(sr), [z] "r"(zero) \
                     : "v0", "v1", "v2", "v3", "memory")

/* lanes computed, register of a, register of b (element EL of it when >= 0), register of c */
static const struct { const char *name; int lanes, a, b, el, c; } forms[] = {
    {"fmla v0.4s,v1.4s,v2.4s", 4, 1, 2, -1, 0},     {"fmls v0.4s,v1.4s,v2.4s", 4, 1, 2, -1, 0},
    {"fmla v0.2s,v1.2s,v2.2s", 2, 1, 2, -1, 0},     {"fmls v0.2s,v1.2s,v2.2s", 2, 1, 2, -1, 0},
    {"fmla v0.4s,v0.4s,v2.4s", 4, 0, 2, -1, 0},     {"fmls v0.4s,v1.4s,v0.4s", 4, 1, 0, -1, 0},
    {"fmla v0.4s,v1.4s,v1.4s", 4, 1, 1, -1, 0},     {"fmls v0.4s,v0.4s,v0.4s", 4, 0, 0, -1, 0},
    {"fmla v0.4s,v1.4s,v2.s[3]", 4, 1, 2, 3, 0},    {"fmls v0.4s,v1.4s,v2.s[0]", 4, 1, 2, 0, 0},
    {"fmla v0.2s,v1.2s,v2.s[2]", 2, 1, 2, 2, 0},    {"fmls v0.2s,v1.2s,v2.s[1]", 2, 1, 2, 1, 0},
    {"fmla v0.4s,v1.4s,v0.s[2]", 4, 1, 0, 2, 0},    {"fmls v0.4s,v0.4s,v2.s[1]", 4, 0, 2, 1, 0},
    {"fmla s0,s1,v2.s[2]", 1, 1, 2, 2, 0},          {"fmls s0,s1,v2.s[3]", 1, 1, 2, 3, 0},
    {"fmla s0,s1,v0.s[1]", 1, 1, 0, 1, 0},
    {"fmadd s0,s1,s2,s3", 1, 1, 2, -1, 3},          {"fmsub s0,s1,s2,s3", 1, 1, 2, -1, 3},
    {"fnmadd s0,s1,s2,s3", 1, 1, 2, -1, 3},         {"fnmsub s0,s1,s2,s3", 1, 1, 2, -1, 3},
    {"fmadd s0,s1,s2,s0", 1, 1, 2, -1, 0},          {"fmsub s0,s0,s2,s3", 1, 0, 2, -1, 3},
    {"fnmadd s0,s1,s0,s3", 1, 1, 0, -1, 3},         {"fnmsub s0,s0,s0,s0", 1, 0, 0, -1, 0},
};

static uint32_t run(unsigned form, uint32_t (*st)[4], uint64_t cr, uint64_t sr)
{
    uint64_t so = 0, zero = 0;
    switch (form) {
    case 0: RUN("fmla v0.4s, v1.4s, v2.4s"); break;
    case 1: RUN("fmls v0.4s, v1.4s, v2.4s"); break;
    case 2: RUN("fmla v0.2s, v1.2s, v2.2s"); break;
    case 3: RUN("fmls v0.2s, v1.2s, v2.2s"); break;
    case 4: RUN("fmla v0.4s, v0.4s, v2.4s"); break;
    case 5: RUN("fmls v0.4s, v1.4s, v0.4s"); break;
    case 6: RUN("fmla v0.4s, v1.4s, v1.4s"); break;
    case 7: RUN("fmls v0.4s, v0.4s, v0.4s"); break;
    case 8: RUN("fmla v0.4s, v1.4s, v2.s[3]"); break;
    case 9: RUN("fmls v0.4s, v1.4s, v2.s[0]"); break;
    case 10: RUN("fmla v0.2s, v1.2s, v2.s[2]"); break;
    case 11: RUN("fmls v0.2s, v1.2s, v2.s[1]"); break;
    case 12: RUN("fmla v0.4s, v1.4s, v0.s[2]"); break;
    case 13: RUN("fmls v0.4s, v0.4s, v2.s[1]"); break;
    case 14: RUN("fmla s0, s1, v2.s[2]"); break;
    case 15: RUN("fmls s0, s1, v2.s[3]"); break;
    case 16: RUN("fmla s0, s1, v0.s[1]"); break;
    case 17: RUN("fmadd s0, s1, s2, s3"); break;
    case 18: RUN("fmsub s0, s1, s2, s3"); break;
    case 19: RUN("fnmadd s0, s1, s2, s3"); break;
    case 20: RUN("fnmsub s0, s1, s2, s3"); break;
    case 21: RUN("fmadd s0, s1, s2, s0"); break;
    case 22: RUN("fmsub s0, s0, s2, s3"); break;
    case 23: RUN("fnmadd s0, s1, s0, s3"); break;
    default: RUN("fnmsub s0, s0, s0, s0"); break;
    }
    return (uint32_t)so;
}

int main(int argc, char **argv)
{
    unsigned per = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    uint64_t rng = 0x9e3779b97f4a7c15ull, total = 0;
    for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
        for (unsigned cat = 0; cat < FC_N; cat++) {
            uint64_t h = 0xcbf29ce484222325ull;
            unsigned flagged = 0;
            for (unsigned k = 0; k < per; k++) {
                uint32_t st[4][4];
                /* registers and lanes the instruction does not compute: any bits, NaNs included */
                for (int r = 0; r < 4; r++)
                    for (int l = 0; l < 4; l++) st[r][l] = fc_rr(&rng, 4) ? (uint32_t)fc_rnd(&rng) : 0x7f800001u + fc_rr(&rng, 2) * 0x400000u;
                fma_ops first = fma_case(&rng, cat);
                for (int l = 0; l < forms[f].lanes; l++) {
                    fma_ops o = !l ? first : fma_case(&rng, fc_rr(&rng, 6) ? cat : fc_rr(&rng, FC_N));
                    if (forms[f].el >= 0 && l && fc_rr(&rng, 2)) o = first;
                    st[forms[f].c][l] = o.c;
                    st[forms[f].a][l] = o.a;
                    if (forms[f].el < 0) st[forms[f].b][l] = o.b;
                }
                if (forms[f].el >= 0) st[forms[f].b][forms[f].el] = first.b;
                uint64_t cr = (uint64_t)(k & 15) << 22;
                uint64_t sr = k & 16 ? 0 : fc_rnd(&rng) & 0x0800009f;
                uint32_t so = run(f, st, cr, sr);
                flagged += (so & ~(uint32_t)sr) != 0;
                for (int l = 0; l < 4; l++) h = (h ^ st[0][l]) * 0x100000001b3ull;
                h = (h ^ so) * 0x100000001b3ull;
                total++;
            }
            printf("%-26s %-8s %016llx new-flags %u\n", forms[f].name, fma_case_name[cat], (unsigned long long)h, flagged);
        }
    }
    printf("%llu instructions\n", (unsigned long long)total);
    return 0;
}
