/* Directed checks of instructions the translator emits inline: each one
 * runs over a table of corner values with NZCV set by a preceding compare,
 * and the result, the memory cell (or vector register) and NZCV afterwards
 * are folded into one hash per instruction. The output must equal a native arm64 run's
 * (tests/guest/jitinsns.sh). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NOINL __attribute__((noinline))
static const uint64_t vals[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff,
    0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0xfffffffffffffffeull,
    0xffffffff80000000ull, 0xffffffffffffff80ull, 0xffffffffffff8000ull, 0x0123456789abcdefull, 0xfedcba9876543210ull,
    0xdeadbeefcafef00dull, 0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull, 0x00ff00ff00ff00ffull, 0x8000000080000000ull,
};
#define NV (sizeof vals / sizeof *vals)

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v; h *= 1099511628211ull; return h ^ (h >> 29); }

/* x0 = a, x1 = b on entry; the instruction leaves its result in x2 */
#define REG2(name, insn)                                                                \
    NOINL static uint64_t name(uint64_t a, uint64_t b, uint64_t *fl)                    \
    {                                                                                   \
        register uint64_t x0 __asm__("x0") = a, x1 __asm__("x1") = b;                   \
        register uint64_t x2 __asm__("x2") = 0x1122334455667788ull, x3 __asm__("x3");   \
        __asm__ volatile("cmp x0, x1\n" insn "\nmrs x3, nzcv"                           \
                         : "+r"(x2), "=r"(x3) : "r"(x0), "r"(x1) : "cc");               \
        *fl = x3;                                                                       \
        return x2;                                                                      \
    }
/* x0 = &cell (holding a), x1 = b; x2 = the instruction's register result */
#define MEM2(name, insn)                                                                \
    NOINL static uint64_t name(uint64_t a, uint64_t b, uint64_t *fl)                    \
    {                                                                                   \
        uint64_t cell[2] = {a, 0x0f0f0f0f0f0f0f0full};                                  \
        register uint64_t *x0 __asm__("x0") = cell;                                     \
        register uint64_t x1 __asm__("x1") = b;                                         \
        register uint64_t x2 __asm__("x2") = a ^ (b & 1), x3 __asm__("x3");             \
        __asm__ volatile("cmp x1, x2\n" insn "\nmrs x3, nzcv"                           \
                         : "+r"(x2), "=r"(x3) : "r"(x0), "r"(x1) : "cc", "memory");     \
        *fl = x3;                                                                       \
        return mix(mix(x2, cell[0]), cell[1]);                                          \
    }

REG2(t_udiv, "udiv x2, x0, x1")
REG2(t_udivw, "udiv w2, w0, w1")
REG2(t_sdiv, "sdiv x2, x0, x1")
REG2(t_sdivw, "sdiv w2, w0, w1")
REG2(t_adc, "adc x2, x0, x1")
REG2(t_adcw, "adc w2, w0, w1")
REG2(t_adcs, "adcs x2, x0, x1")
REG2(t_adcsw, "adcs w2, w0, w1")
REG2(t_sbc, "sbc x2, x0, x1")
REG2(t_sbcw, "sbc w2, w0, w1")
REG2(t_sbcs, "sbcs x2, x0, x1")
REG2(t_sbcsw, "sbcs w2, w0, w1")
REG2(t_ngcs, "ngcs x2, x1")
REG2(t_add128, "adds x2, x0, x1\nadcs x2, x2, x0\nadc x2, x2, x1")
REG2(t_rbit, "rbit x2, x0")
REG2(t_rbitw, "rbit w2, w1")
REG2(t_rev16, "rev16 x2, x0")
REG2(t_rev16w, "rev16 w2, w1")
REG2(t_rev32, "rev32 x2, x0")
REG2(t_ctz, "rbit x2, x0\nclz x2, x2")
REG2(t_bfi0, "mov x2, x0\nbfi x2, x1, #0, #1")
REG2(t_bfi1, "mov x2, x0\nbfi x2, x1, #8, #16")
REG2(t_bfi2, "mov x2, x0\nbfi x2, x1, #40, #24")
REG2(t_bfi3, "mov x2, x0\nbfi x2, x1, #63, #1")
REG2(t_bfi4, "mov x2, x0\nbfi w2, w1, #5, #27")
REG2(t_bfi5, "mov x2, x0\nbfi x2, x1, #32, #32")
REG2(t_bfxil0, "mov x2, x0\nbfxil x2, x1, #0, #64")
REG2(t_bfxil1, "mov x2, x0\nbfxil x2, x1, #16, #8")
REG2(t_bfxil2, "mov x2, x0\nbfxil x2, x1, #33, #31")
REG2(t_bfxil3, "mov x2, x0\nbfxil w2, w1, #31, #1")
REG2(t_bfxil4, "mov x2, x0\nbfxil w2, w1, #0, #32")
REG2(t_bfc, "mov x2, x0\nbfc x2, #12, #40")
REG2(t_bfcw, "mov x2, x1\nbfc w2, #0, #7")
REG2(t_movk, "mov x2, #0x1234\nmovk x2, #0x5678, lsl #16\nmovk x2, #0x9abc, lsl #32\nmovk x2, #0xdef0, lsl #48\nadd x2, x2, x0")
REG2(t_movkw, "mov w2, #0xffff\nmovk w2, #0x8000, lsl #16\neor x2, x2, x1")
REG2(t_movk2, "mov x2, x0\nmovk x2, #0xbeef, lsl #16\nmovk x2, #0xdead, lsl #48")
REG2(t_movn, "mov x2, #-0x1235\nmovk x2, #0x1, lsl #32\nsub x2, x2, x1")
/* MOVK runs without a MOVZ, with NZCV dead (a compare follows) and live */
REG2(t_movk3, "mov x2, x0\nmovk x2, #0xbeef, lsl #16\nmovk x2, #0xdead, lsl #48\ncmp x2, x1")
REG2(t_movk4, "mov x2, x1\nmovk w2, #0x1234, lsl #16\ncmn x2, x0")
REG2(t_movk5, "mov x2, x0\nmovk x2, #0x8000\nmovk x2, #0xffff, lsl #32\nmovk x2, #0x8000, lsl #16\ntst x2, x1")
REG2(t_movk6, "mov x2, x0\nmovk x2, #1\nmovk x2, #2, lsl #16\nmovk x2, #3, lsl #32\nmovk x2, #4, lsl #48\nmovk x2, #5")
REG2(t_movk7, "mov x2, x0\nmovk w2, #0xffff\nmovk x2, #0x7777, lsl #48\nmovk x3, #1\nmovk x2, #0x1111, lsl #16")
REG2(t_movz2, "mov w2, #0x10000\nmovk x2, #0xa, lsl #32\nmov x3, #0x20000\nmovk x3, #0xb\nadd x2, x2, x3\neor x2, x2, x0")

#define ATOM(n, mn)                                    \
    MEM2(t_##n##b, mn "b w1, w2, [x0]")                \
    MEM2(t_##n##h, mn "h w1, w2, [x0]")                \
    MEM2(t_##n##w, mn " w1, w2, [x0]")                 \
    MEM2(t_##n##x, mn " x1, x2, [x0]")
ATOM(ldadd, "ldaddal")
ATOM(ldclr, "ldclral")
ATOM(ldeor, "ldeoral")
ATOM(ldset, "ldsetal")
ATOM(ldsmax, "ldsmaxal")
ATOM(ldsmin, "ldsminal")
ATOM(ldumax, "ldumaxal")
ATOM(ldumin, "lduminal")
ATOM(swp, "swpal")
ATOM(ldaddr, "ldadda")
ATOM(swpl, "swpl")
ATOM(cas, "casal")     /* x2 = expected (a or a^1), x1 = new */
MEM2(t_stadd, "staddl x1, [x0]")
MEM2(t_stclrb, "stclrb w1, [x0]")
MEM2(t_casx2, "mov x2, x1\ncasa x2, x1, [x0]")
MEM2(t_ldapr, "ldapr x2, [x0]")
MEM2(t_ldaprb, "ldaprb w2, [x0]")
MEM2(t_ldraa, "ldraa x2, [x0, #8]")
MEM2(t_ldrab, "add x3, x0, #16\nldrab x2, [x3, #-16]!\nsub x3, x3, x0\nadd x2, x2, x3")
MEM2(t_stlr, "stlrh w1, [x0]\nldarb w2, [x0]")

/* v0 = {a, ~a rotated}, v1 = {b, b * odd}, v2 = {a ^ b, a + b}; x0 = a, x1 = b;
 * the instruction's result is v2 (and x2) */
#define VEC2(name, insn)                                                                \
    NOINL static uint64_t name(uint64_t a, uint64_t b, uint64_t *fl)                    \
    {                                                                                   \
        uint64_t in[6] = {a, ~((a << 13) | (a >> 51)), b, b * 0x9e3779b97f4a7c15ull, a ^ b, a + b}, out[2]; \
        register uint64_t x0 __asm__("x0") = a, x1 __asm__("x1") = b;                   \
        register uint64_t x2 __asm__("x2") = 0, x3 __asm__("x3");                       \
        register uint64_t *x4 __asm__("x4") = in, *x5 __asm__("x5") = out;              \
        __asm__ volatile("ldp q0, q1, [x4]\nldr q2, [x4, #32]\ncmp x0, x1\n" insn     \
                         "\nmrs x3, nzcv\nstr q2, [x5]"                                \
                         : "+r"(x2), "=r"(x3) : "r"(x0), "r"(x1), "r"(x4), "r"(x5)      \
                         : "cc", "memory", "v0", "v1", "v2");                           \
        *fl = x3;                                                                       \
        return mix(mix(x2, out[0]), out[1]);                                            \
    }
#define V4(n, mn)                                      \
    VEC2(t_##n##16b, mn " v2.16b, v0.16b, v1.16b")     \
    VEC2(t_##n##8h, mn " v2.8h, v0.8h, v1.8h")         \
    VEC2(t_##n##4s, mn " v2.4s, v0.4s, v1.4s")         \
    VEC2(t_##n##8b, mn " v2.8b, v0.8b, v1.8b")
#define V4Z(n, mn, z)                                  \
    VEC2(t_##n##16b, mn " v2.16b, v0.16b" z)           \
    VEC2(t_##n##8h, mn " v2.8h, v1.8h" z)              \
    VEC2(t_##n##4s, mn " v2.4s, v0.4s" z)              \
    VEC2(t_##n##8b, mn " v2.4h, v1.4h" z)
V4(vadd, "add") V4(vsub, "sub") V4(vcmeq, "cmeq") V4(vcmgt, "cmgt") V4(vcmge, "cmge") V4(vcmtst, "cmtst")
V4(vcmhi, "cmhi") V4(vcmhs, "cmhs") V4(vumax, "umax") V4(vumin, "umin") V4(vsmax, "smax") V4(vsmin, "smin") V4(vmul, "mul")
V4Z(vcmeqz, "cmeq", ", #0") V4Z(vcmgtz, "cmgt", ", #0") V4Z(vcmgez, "cmge", ", #0") V4Z(vcmlez, "cmle", ", #0")
V4Z(vcmltz, "cmlt", ", #0") V4Z(vneg, "neg", "") V4Z(vabs, "abs", "")
VEC2(t_vadd2d, "add v2.2d, v0.2d, v1.2d")
VEC2(t_vsub2d, "sub v2.2d, v0.2d, v1.2d")
VEC2(t_vcmeq2d, "cmeq v2.2d, v0.2d, v1.2d")
VEC2(t_vneg2d, "neg v2.2d, v0.2d")
VEC2(t_vaddd, "add d2, d0, d1")
VEC2(t_vand, "and v2.16b, v0.16b, v1.16b")
VEC2(t_vand8, "and v2.8b, v0.8b, v1.8b")
VEC2(t_vbic, "bic v2.16b, v0.16b, v1.16b")
VEC2(t_vorr, "orr v2.16b, v0.16b, v1.16b")
VEC2(t_vmov, "mov v2.16b, v1.16b")
VEC2(t_vmov8, "mov v2.8b, v0.8b")
VEC2(t_vorn, "orn v2.16b, v0.16b, v1.16b")
VEC2(t_veor, "eor v2.16b, v0.16b, v1.16b")
VEC2(t_vbsl, "bsl v2.16b, v0.16b, v1.16b")
VEC2(t_vbit, "bit v2.16b, v0.16b, v1.16b")
VEC2(t_vbif, "bif v2.8b, v0.8b, v1.8b")
VEC2(t_vbsl2, "bsl v0.16b, v2.16b, v1.16b\nmov v2.16b, v0.16b")
VEC2(t_vnot, "mvn v2.16b, v0.16b")
VEC2(t_vnot8, "mvn v2.8b, v1.8b")
VEC2(t_vcnt, "cnt v2.16b, v0.16b")
VEC2(t_vcnt8, "cnt v2.8b, v1.8b")
VEC2(t_popcnt, "fmov d2, x0\ncnt v2.8b, v2.8b\nuaddlv h2, v2.8b\nfmov w2, s2")
VEC2(t_uaddlv16, "uaddlv h2, v1.16b")
VEC2(t_movi0, "movi v2.2d, #0")
VEC2(t_movi1, "movi v2.16b, #0x5a")
VEC2(t_movi2, "movi v2.4s, #0x12, lsl #16")
VEC2(t_movi3, "movi v2.2d, #0xff00ff0000ffff00")
VEC2(t_movi4, "movi d2, #0xffffffff00000000")
VEC2(t_movi5, "movi v2.4h, #0x80, lsl #8")
VEC2(t_movi6, "movi v2.2s, #0x7f, msl #8")
VEC2(t_movi7, "movi v2.8b, #0xff")
VEC2(t_mvni0, "mvni v2.4s, #0")
VEC2(t_mvni1, "mvni v2.8h, #0x21")
VEC2(t_mvni2, "mvni v2.2s, #0x33, msl #16")
VEC2(t_fmovi0, "fmov v2.4s, #1.0")
VEC2(t_fmovi1, "fmov v2.2d, #-2.5")
VEC2(t_orri, "orr v2.4s, #0x81, lsl #24")
VEC2(t_orri2, "orr v2.4h, #0x18")
VEC2(t_bici, "bic v2.8h, #0xf0, lsl #8")
VEC2(t_bici2, "bic v2.2s, #0xff")
VEC2(t_dup0, "dup v2.16b, w0")
VEC2(t_dup1, "dup v2.8h, w1")
VEC2(t_dup2, "dup v2.4s, w0")
VEC2(t_dup3, "dup v2.2d, x1")
VEC2(t_dup4, "dup v2.8b, w1")
VEC2(t_dup5, "dup v2.4h, w0")
VEC2(t_dup6, "dup v2.2s, w1")
VEC2(t_dup7, "dup v2.2d, xzr")
VEC2(t_dupe0, "dup v2.16b, v0.b[13]")
VEC2(t_dupe1, "dup v2.4h, v1.h[5]")
VEC2(t_dupe2, "dup v2.4s, v0.s[3]")
VEC2(t_dupe3, "dup v2.2d, v1.d[1]")
VEC2(t_dups0, "mov d2, v0.d[1]")
VEC2(t_dups1, "mov s2, v1.s[2]")
VEC2(t_dups2, "mov h2, v0.h[7]")
VEC2(t_dups3, "mov b2, v1.b[9]")
VEC2(t_ins0, "mov v2.b[15], w0")
VEC2(t_ins1, "mov v2.h[3], w1")
VEC2(t_ins2, "mov v2.s[1], w0")
VEC2(t_ins3, "mov v2.d[1], x1")
VEC2(t_ins4, "mov v2.d[0], xzr")
VEC2(t_inse0, "mov v2.b[2], v0.b[11]")
VEC2(t_inse1, "mov v2.h[6], v1.h[1]")
VEC2(t_inse2, "mov v2.s[3], v0.s[0]")
VEC2(t_inse3, "mov v2.d[0], v1.d[1]")
VEC2(t_umov0, "umov w2, v0.b[14]")
VEC2(t_umov1, "umov w2, v1.h[6]")
VEC2(t_umov2, "mov w2, v0.s[3]")
VEC2(t_umov3, "mov x2, v1.d[1]")
VEC2(t_smov0, "smov w2, v0.b[3]")
VEC2(t_smov1, "smov w2, v1.h[2]")
VEC2(t_smov2, "smov x2, v0.b[15]")
VEC2(t_smov3, "smov x2, v1.h[7]")
VEC2(t_smov4, "smov x2, v0.s[1]")
VEC2(t_shl0, "shl v2.8h, v0.8h, #0")
VEC2(t_shl1, "shl v2.8h, v1.8h, #15")
VEC2(t_shl2, "shl v2.4s, v0.4s, #17")
VEC2(t_shl3, "shl v2.2d, v1.2d, #63")
VEC2(t_shl4, "shl v2.2s, v0.2s, #1")
VEC2(t_shl5, "shl d2, d1, #33")
VEC2(t_shl6, "shl v2.16b, v0.16b, #3")
VEC2(t_ushr0, "ushr v2.8h, v0.8h, #16")
VEC2(t_ushr1, "ushr v2.4s, v1.4s, #1")
VEC2(t_ushr2, "ushr v2.2d, v0.2d, #64")
VEC2(t_ushr3, "ushr v2.4h, v1.4h, #9")
VEC2(t_ushr4, "ushr d2, d0, #17")
VEC2(t_ushr5, "ushr v2.16b, v1.16b, #7")
VEC2(t_sshr0, "sshr v2.8h, v0.8h, #16")
VEC2(t_sshr1, "sshr v2.4s, v1.4s, #31")
VEC2(t_sshr2, "sshr v2.2s, v0.2s, #5")
VEC2(t_sshr3, "sshr v2.2d, v1.2d, #7")
VEC2(t_sshr4, "sshr d2, d0, #64")

typedef uint64_t (*fn2)(uint64_t, uint64_t, uint64_t *);
static void run(const char *name, fn2 f)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned i = 0; i < NV; i++)
        for (unsigned j = 0; j < NV; j++) {
            uint64_t fl = 0, r = f(vals[i], vals[j], &fl);
            h = mix(mix(h, r), fl);
        }
    printf("%-10s %016llx\n", name, (unsigned long long)h);
}

/* contended atomics: sums and extremes are the same in every interleaving */
#define NT 4
#define ROUNDS 200000
static uint64_t ctr_add, ctr_cas, ctr_max, ctr_or;
static uint32_t ctr_w;
static uint8_t ctr_b;
static void *worker(void *arg)
{
    uint64_t id = (uint64_t)(uintptr_t)arg;
    for (uint64_t k = 0; k < ROUNDS; k++) {
        __atomic_fetch_add(&ctr_add, k + id, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&ctr_w, 3, __ATOMIC_RELAXED);
        __atomic_fetch_add(&ctr_b, 1, __ATOMIC_RELAXED);
        uint64_t old = __atomic_load_n(&ctr_cas, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&ctr_cas, &old, old + 2, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) { }
        uint64_t m = k * NT + id;
        __asm__ volatile("ldumaxal %[m], %[m], [%[p]]" : [m] "+r"(m) : [p] "r"(&ctr_max) : "memory");
        __atomic_fetch_or(&ctr_or, 1ull << ((k + id) & 63), __ATOMIC_SEQ_CST);
    }
    return NULL;
}

#define R(n) run(#n, t_##n)
#define RA(n) R(n##b); R(n##h); R(n##w); R(n##x)
int main(void)
{
    R(udiv); R(udivw); R(sdiv); R(sdivw);
    R(adc); R(adcw); R(adcs); R(adcsw); R(sbc); R(sbcw); R(sbcs); R(sbcsw); R(ngcs); R(add128);
    R(rbit); R(rbitw); R(rev16); R(rev16w); R(rev32); R(ctz);
    R(bfi0); R(bfi1); R(bfi2); R(bfi3); R(bfi4); R(bfi5);
    R(bfxil0); R(bfxil1); R(bfxil2); R(bfxil3); R(bfxil4); R(bfc); R(bfcw);
    R(movk); R(movkw); R(movk2); R(movn); R(movk3); R(movk4); R(movk5); R(movk6); R(movk7); R(movz2);
    RA(ldadd); RA(ldclr); RA(ldeor); RA(ldset); RA(ldsmax); RA(ldsmin); RA(ldumax); RA(ldumin);
    RA(swp); RA(ldaddr); RA(swpl); RA(cas);
    R(stadd); R(stclrb); R(casx2); R(ldapr); R(ldaprb); R(ldraa); R(ldrab); R(stlr);
#define RV(n) R(n##16b); R(n##8h); R(n##4s); R(n##8b)
    RV(vadd); RV(vsub); RV(vcmeq); RV(vcmgt); RV(vcmge); RV(vcmtst); RV(vcmhi); RV(vcmhs);
    RV(vumax); RV(vumin); RV(vsmax); RV(vsmin); RV(vmul);
    RV(vcmeqz); RV(vcmgtz); RV(vcmgez); RV(vcmlez); RV(vcmltz); RV(vneg); RV(vabs);
    R(vadd2d); R(vsub2d); R(vcmeq2d); R(vneg2d); R(vaddd);
    R(vand); R(vand8); R(vbic); R(vorr); R(vmov); R(vmov8); R(vorn); R(veor); R(vbsl); R(vbit); R(vbif); R(vbsl2);
    R(vnot); R(vnot8); R(vcnt); R(vcnt8); R(popcnt); R(uaddlv16);
    R(movi0); R(movi1); R(movi2); R(movi3); R(movi4); R(movi5); R(movi6); R(movi7); R(mvni0); R(mvni1); R(mvni2);
    R(fmovi0); R(fmovi1); R(orri); R(orri2); R(bici); R(bici2);
    R(dup0); R(dup1); R(dup2); R(dup3); R(dup4); R(dup5); R(dup6); R(dup7); R(dupe0); R(dupe1); R(dupe2); R(dupe3);
    R(dups0); R(dups1); R(dups2); R(dups3); R(ins0); R(ins1); R(ins2); R(ins3); R(ins4);
    R(inse0); R(inse1); R(inse2); R(inse3); R(umov0); R(umov1); R(umov2); R(umov3);
    R(smov0); R(smov1); R(smov2); R(smov3); R(smov4);
    R(shl0); R(shl1); R(shl2); R(shl3); R(shl4); R(shl5); R(shl6);
    R(ushr0); R(ushr1); R(ushr2); R(ushr3); R(ushr4); R(ushr5); R(sshr0); R(sshr1); R(sshr2); R(sshr3); R(sshr4);
    pthread_t th[NT];
    for (uintptr_t k = 0; k < NT; k++) pthread_create(&th[k], NULL, worker, (void *)k);
    for (int k = 0; k < NT; k++) pthread_join(th[k], NULL);
    printf("threads    add=%llu w=%u b=%u cas=%llu max=%llu or=%016llx\n", (unsigned long long)ctr_add, ctr_w, ctr_b,
           (unsigned long long)ctr_cas, (unsigned long long)ctr_max, (unsigned long long)ctr_or);
    return 0;
}
