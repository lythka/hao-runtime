/* Directed checks of instructions the translator emits inline: each one
 * runs over a table of corner values with NZCV set by a preceding compare,
 * and the result, the memory cell (or vector register) and NZCV afterwards
 * are folded into one hash per instruction. The output must equal a native arm64 run's
 * (tests/guest/jitinsns.sh). */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define SET_PC_RAW(uc, a) do { void (*f_)(void) = ptrauth_sign_unauthenticated((void (*)(void))(a), ptrauth_key_function_pointer, 0); \
        __darwin_arm_thread_state64_set_pc_fptr((uc)->uc_mcontext->__ss, f_); } while (0)
#else
#define SET_PC_RAW(uc, a) ((uc)->uc_mcontext->__ss.__pc = (a))
#endif
#define SS(uc) ((uc)->uc_mcontext->__ss)
#define GET_PC(uc) ((uintptr_t)__darwin_arm_thread_state64_get_pc(SS(uc)))

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

/* NZCV across block exits: live through B, CBZ and TBZ to a reader, dead
 * on one or both paths, rewritten in a loop */
REG2(t_fl1, "cmp x0, x1\nb 1f\n1: cbz x1, 2f\nadd x2, x2, #1\n2: tbz x0, #3, 3f\nadd x2, x2, #2\n3: cset w3, hi\nadd x2, x2, x3")
REG2(t_fl2, "adds x2, x0, x1\ncbz x1, 1f\nsubs x2, x2, #5\nb 2f\n1: ands x2, x2, x0\n2: nop")
REG2(t_fl3, "cmp x0, x1\ntbz x1, #0, 1f\ncmp x1, x0\nb 2f\n1: cinc x2, x2, lo\n2: nop")
REG2(t_fl4, "mov x2, #0\nand x9, x1, #7\n1: add x2, x2, x0\nsubs x9, x9, #1\nb.pl 1b")
REG2(t_fl5, "cmp x0, x1\ncbz x0, 1f\nccmp x1, #3, #4, ne\n1: cset w2, eq")
REG2(t_fl6, "cmp x0, x1\ntbnz x1, #63, 1f\nmul x2, x0, x1\nb 2f\n1: udiv x2, x0, x1\n2: csinc x2, x2, x0, ge")
REG2(t_fl7, "cmn x0, x1\ncbnz x0, 1f\nmov x2, #7\n1: adc x2, x2, x1")
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
                         : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5");         \
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
V4(vmla, "mla") V4(vmls, "mls")
VEC2(t_vcmgt2d, "cmgt v2.2d, v0.2d, v1.2d")
VEC2(t_vcmge2d, "cmge v2.2d, v1.2d, v0.2d")
VEC2(t_vcmhi2d, "cmhi v2.2d, v0.2d, v1.2d")
VEC2(t_vcmhs2d, "cmhs v2.2d, v1.2d, v0.2d")
VEC2(t_vcmhs2d2, "cmhs v2.2d, v0.2d, v0.2d")
VEC2(t_vcmtst2d, "cmtst v2.2d, v0.2d, v1.2d")
VEC2(t_vcmgtz2d, "cmgt v2.2d, v0.2d, #0")
VEC2(t_vcmgez2d, "cmge v2.2d, v1.2d, #0")
VEC2(t_vcmeqz2d, "cmeq v2.2d, v0.2d, #0")
VEC2(t_vcmlez2d, "cmle v2.2d, v1.2d, #0")
VEC2(t_vcmltz2d, "cmlt v2.2d, v0.2d, #0")
VEC2(t_vabs2d, "abs v2.2d, v1.2d")
VEC2(t_vabs2d2, "abs v2.2d, v0.2d")
VEC2(t_vcmhi4s2, "cmhi v2.4s, v1.4s, v0.4s")
VEC2(t_vcmhs2s, "cmhs v2.2s, v0.2s, v1.2s")
VEC2(t_vabs2s, "abs v2.2s, v0.2s")
VEC2(t_vabs8b2, "abs v2.8b, v1.8b")
VEC2(t_vmule0, "mul v2.4s, v0.4s, v1.s[3]")
VEC2(t_vmule1, "mul v2.2s, v1.2s, v0.s[1]")
VEC2(t_vmule2, "mul v2.8h, v0.8h, v1.h[5]")
VEC2(t_vmule3, "mul v2.4h, v1.4h, v0.h[7]")
VEC2(t_vmlae0, "mla v2.4s, v0.4s, v1.s[0]")
VEC2(t_vmlae1, "mla v2.8h, v1.8h, v0.h[2]")
VEC2(t_vmlse0, "mls v2.2s, v0.2s, v1.s[2]")
VEC2(t_vmlse1, "mls v2.4h, v1.4h, v0.h[6]")
VEC2(t_vmul16b2, "mul v2.16b, v1.16b, v1.16b")
VEC2(t_tbl1, "tbl v2.16b, {v0.16b}, v1.16b")
VEC2(t_tbl1m, "movi v3.16b, #0x1f\nand v3.16b, v1.16b, v3.16b\ntbl v2.16b, {v0.16b}, v3.16b")
VEC2(t_tbl1h, "movi v3.16b, #0x1f\nand v3.16b, v1.16b, v3.16b\ntbl v2.8b, {v1.16b}, v3.8b")
VEC2(t_tbl2, "movi v3.16b, #0x3f\nand v3.16b, v1.16b, v3.16b\ntbl v2.16b, {v0.16b, v1.16b}, v3.16b")
VEC2(t_tbl3, "movi v3.16b, #0x3f\nand v4.16b, v1.16b, v3.16b\neor v3.16b, v0.16b, v1.16b\n"
             "tbl v2.16b, {v1.16b, v2.16b, v3.16b}, v4.16b")
VEC2(t_tbl4, "movi v4.16b, #0x7f\nand v4.16b, v1.16b, v4.16b\neor v3.16b, v0.16b, v1.16b\n"
             "tbl v2.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v4.16b")
VEC2(t_tbl4h, "movi v4.16b, #0x3f\nand v4.16b, v1.16b, v4.16b\neor v3.16b, v0.16b, v1.16b\n"
              "tbl v2.8b, {v0.16b, v1.16b, v2.16b, v3.16b}, v4.8b")
VEC2(t_tbx1, "movi v3.16b, #0x1f\nand v3.16b, v1.16b, v3.16b\ntbx v2.16b, {v0.16b}, v3.16b")
VEC2(t_tbx2, "movi v3.16b, #0x3f\nand v3.16b, v1.16b, v3.16b\ntbx v2.8b, {v0.16b, v1.16b}, v3.8b")
VEC2(t_tbx3, "movi v3.16b, #0x3f\nand v3.16b, v1.16b, v3.16b\nmov v4.16b, v0.16b\nmov v5.16b, v1.16b\n"
             "tbx v2.16b, {v3.16b, v4.16b, v5.16b}, v3.16b")
VEC2(t_tbx4, "movi v4.16b, #0x7f\nand v4.16b, v1.16b, v4.16b\neor v3.16b, v0.16b, v1.16b\n"
             "tbx v2.16b, {v1.16b, v2.16b, v3.16b, v4.16b}, v4.16b")
VEC2(t_tbxr, "tbx v2.16b, {v0.16b, v1.16b}, v1.16b")
VEC2(t_ext0, "ext v2.16b, v0.16b, v1.16b, #0")
VEC2(t_ext1, "ext v2.16b, v0.16b, v1.16b, #5")
VEC2(t_ext2, "ext v2.16b, v1.16b, v0.16b, #15")
VEC2(t_ext3, "ext v2.8b, v0.8b, v1.8b, #0")
VEC2(t_ext4, "ext v2.8b, v1.8b, v0.8b, #3")
VEC2(t_ext5, "ext v2.8b, v0.8b, v1.8b, #7")
VEC2(t_ext6, "ext v2.16b, v2.16b, v2.16b, #8")
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
VEC2(t_shl7, "shl v2.8b, v1.8b, #7")
VEC2(t_shl8, "shl v2.16b, v1.16b, #0")
VEC2(t_ushr6, "ushr v2.8b, v0.8b, #1")
VEC2(t_ushr7, "ushr v2.16b, v0.16b, #8")
VEC2(t_sshr5, "sshr v2.16b, v0.16b, #1")
VEC2(t_sshr6, "sshr v2.16b, v1.16b, #7")
VEC2(t_sshr7, "sshr v2.8b, v0.8b, #8")
VEC2(t_sshr8, "sshr v2.16b, v1.16b, #3")
VEC2(t_sshr9, "sshr v2.2d, v0.2d, #64")
VEC2(t_sshr10, "sshr v2.2d, v1.2d, #1")
VEC2(t_sshr11, "sshr v2.2d, v0.2d, #63")
VEC2(t_sshr12, "sshr d2, d1, #33")
VEC2(t_sra0, "usra v2.16b, v0.16b, #3")
VEC2(t_sra1, "ssra v2.8b, v1.8b, #8")
VEC2(t_sra2, "ssra v2.8h, v0.8h, #15")
VEC2(t_sra3, "usra v2.2d, v1.2d, #64")
VEC2(t_sra4, "ssra d2, d0, #40")
VEC2(t_rshr0, "urshr v2.16b, v1.16b, #8")
VEC2(t_rshr1, "srshr v2.16b, v0.16b, #1")
VEC2(t_rshr2, "urshr v2.4h, v1.4h, #16")
VEC2(t_rshr3, "srshr v2.4s, v0.4s, #31")
VEC2(t_rshr4, "srshr v2.2d, v1.2d, #64")
VEC2(t_rshr5, "urshr d2, d0, #1")
VEC2(t_rsra0, "srsra v2.8h, v1.8h, #4")
VEC2(t_rsra1, "ursra v2.2d, v0.2d, #33")
VEC2(t_rsra2, "srsra v2.8b, v0.8b, #7")
VEC2(t_xtn0, "xtn v2.8b, v0.8h")
VEC2(t_xtn1, "xtn v2.4h, v1.4s")
VEC2(t_xtn2, "xtn v2.2s, v0.2d")
VEC2(t_xtn3, "xtn2 v2.16b, v1.8h")
VEC2(t_xtn4, "xtn2 v2.8h, v0.4s")
VEC2(t_xtn5, "xtn2 v2.4s, v1.2d")
VEC2(t_shrn0, "shrn v2.8b, v0.8h, #1")
VEC2(t_shrn1, "shrn v2.8b, v1.8h, #8")
VEC2(t_shrn2, "shrn v2.4h, v0.4s, #16")
VEC2(t_shrn3, "shrn v2.2s, v1.2d, #32")
VEC2(t_shrn4, "shrn2 v2.16b, v0.8h, #5")
VEC2(t_shrn5, "shrn2 v2.8h, v1.4s, #3")
VEC2(t_shrn6, "shrn2 v2.4s, v0.2d, #17")
VEC2(t_shll0, "ushll v2.8h, v0.8b, #0")
VEC2(t_shll1, "sshll v2.8h, v1.8b, #7")
VEC2(t_shll2, "ushll2 v2.4s, v0.8h, #15")
VEC2(t_shll3, "sshll2 v2.4s, v1.8h, #0")
VEC2(t_shll4, "ushll v2.2d, v1.2s, #31")
VEC2(t_shll5, "sshll2 v2.2d, v0.4s, #5")
VEC2(t_shll6, "sxtl2 v2.8h, v1.16b")
VEC2(t_shll7, "uxtl2 v2.2d, v0.4s")
VEC2(t_fabs0, "fabs v2.4s, v0.4s")
VEC2(t_fabs1, "fabs v2.2s, v1.2s")
VEC2(t_fabs2, "fabs v2.2d, v0.2d")
VEC2(t_fneg0, "fneg v2.4s, v1.4s")
VEC2(t_fneg1, "fneg v2.2s, v0.2s")
VEC2(t_fneg2, "fneg v2.2d, v1.2d")

/* Single-precision vector arithmetic over a table of corner values (zeros,
 * subnormals, the minimum normal, the largest finite, infinities, quiet and
 * signalling NaNs, values that overflow, underflow or cancel exactly) under
 * five FPCR settings: v0 and v1 are built from fv[i] and fv[j], v2 is the
 * result; FPSR (cleared before) and NZCV (set by a compare before) are read
 * after it. */
static const uint32_t fv[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x80800000, 0x00800001, 0x01000000,
    0x7f7fffff, 0xff7fffff, 0x7f000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0xffc00123,
    0x3f800000, 0xbf800000, 0x3f800001, 0x3fffffff, 0x1f800000, 0x5f800000, 0x20000000, 0x40490fdb,
};
#define NF (sizeof fv / sizeof *fv)
static const uint64_t fpcrs[] = {0, 3u << 22, 1u << 24, 1u << 25, 2u << 22 | 3u << 24, 1u << 22};
#define VECF(name, insn)                                                                \
    NOINL static uint64_t name(const uint32_t *in, uint64_t fpcr, uint64_t *fl)        \
    {                                                                                   \
        uint64_t out[2];                                                                \
        register const uint32_t *x4 __asm__("x4") = in;                                 \
        register uint64_t *x5 __asm__("x5") = out;                                      \
        register uint64_t x6 __asm__("x6") = fpcr, x3 __asm__("x3"), x7 __asm__("x7"); \
        __asm__ volatile("ldp q0, q1, [x4]\nldr q2, [x4, #32]\nmov v17.16b, v1.16b\n"  \
                         "msr fpcr, x6\nmsr fpsr, xzr\ncmp x4, x5\n" insn              \
                         "\nmrs x3, nzcv\nmrs x7, fpsr\nmsr fpcr, xzr\nstr q2, [x5]"     \
                         : "=r"(x3), "=r"(x7) : "r"(x4), "r"(x5), "r"(x6)               \
                         : "cc", "memory", "v0", "v1", "v2", "v17");                    \
        *fl = x3 ^ x7;                                                                  \
        return mix(out[0], out[1]);                                                     \
    }
#define F4(n, mn) VECF(t_##n##4s, mn " v2.4s, v0.4s, v1.4s") VECF(t_##n##2s, mn " v2.2s, v0.2s, v1.2s")
VECF(t_fmla4h, "fmla v2.4h,v0.4h,v1.4h")
VECF(t_fmla8h, "fmla v2.8h,v0.8h,v1.8h")
VECF(t_fmlah0, "fmla v2.4h,v0.4h,v1.h[0]")
VECF(t_fmlah1, "fmla v2.8h,v0.8h,v1.h[1]")
VECF(t_fmlah2, "fmla v2.4h,v0.4h,v1.h[2]")
VECF(t_fmlah3, "fmla v2.8h,v0.8h,v1.h[3]")
VECF(t_fmlah4, "fmla v2.4h,v0.4h,v1.h[4]")
VECF(t_fmlah5, "fmla v2.8h,v0.8h,v1.h[5]")
VECF(t_fmlah6, "fmla v2.4h,v0.4h,v1.h[6]")
VECF(t_fmlah7, "fmla v2.8h,v0.8h,v1.h[7]")
VECF(t_fmls4h, "fmls v2.4h,v0.4h,v1.4h")
VECF(t_fmls8h, "fmls v2.8h,v0.8h,v1.8h")
VECF(t_fmlsh0, "fmls v2.4h,v0.4h,v1.h[0]")
VECF(t_fmlsh1, "fmls v2.8h,v0.8h,v1.h[1]")
VECF(t_fmlsh2, "fmls v2.4h,v0.4h,v1.h[2]")
VECF(t_fmlsh3, "fmls v2.8h,v0.8h,v1.h[3]")
VECF(t_fmlsh4, "fmls v2.4h,v0.4h,v1.h[4]")
VECF(t_fmlsh5, "fmls v2.8h,v0.8h,v1.h[5]")
VECF(t_fmlsh6, "fmls v2.4h,v0.4h,v1.h[6]")
VECF(t_fmlsh7, "fmls v2.8h,v0.8h,v1.h[7]")
F4(fmla, "fmla") F4(fmls, "fmls")
F4(fadd, "fadd") F4(fsub, "fsub") F4(fmul, "fmul") F4(fmax, "fmax") F4(fmin, "fmin") F4(fmaxnm, "fmaxnm")
F4(fminnm, "fminnm") F4(fcmeq, "fcmeq") F4(fcmge, "fcmge") F4(fcmgt, "fcmgt")
VECF(t_fmule0, "fmul v2.4s, v0.4s, v1.s[0]")
VECF(t_fmule1, "fmul v2.4s, v0.4s, v17.s[3]")
VECF(t_fmule2, "fmul v2.2s, v1.2s, v0.s[1]")
VECF(t_fmule3, "fmul v2.2s, v0.2s, v17.s[2]")
VECF(t_fmlae0, "fmla v2.4s, v0.4s, v17.s[0]")
VECF(t_fmlae1, "fmla v2.4s, v0.4s, v17.s[1]")
VECF(t_fmlae2, "fmla v2.2s, v0.2s, v17.s[2]")
VECF(t_fmlae3, "fmla v2.2s, v0.2s, v17.s[3]")
VECF(t_fmlse0, "fmls v2.4s, v0.4s, v17.s[0]")
VECF(t_fmlse1, "fmls v2.4s, v0.4s, v17.s[1]")
VECF(t_fmlse2, "fmls v2.2s, v0.2s, v17.s[2]")
VECF(t_fmlse3, "fmls v2.2s, v0.2s, v17.s[3]")
VECF(t_fchain, "fmul v2.4s, v0.4s, v1.4s\nfadd v2.4s, v2.4s, v0.4s\nfsub v2.4s, v2.4s, v1.4s\nfmax v2.4s, v2.4s, v1.4s")
VECF(t_fadd2d, "fadd v2.2d, v0.2d, v1.2d")
/* round to integral: vector .4S/.2S/.2D and scalar S/D */
#define FR(n, mn)                                                                       \
    VECF(t_##n##4s, mn " v2.4s, v0.4s") VECF(t_##n##2s, mn " v2.2s, v1.2s")             \
    VECF(t_##n##2d, mn " v2.2d, v0.2d") VECF(t_##n##s, mn " s2, s1") VECF(t_##n##d, mn " d2, d0")
FR(frintn, "frintn") FR(frintm, "frintm") FR(frintp, "frintp") FR(frintz, "frintz") FR(frinta, "frinta")
FR(frintx, "frintx") FR(frinti, "frinti")
VECF(t_frchain, "frintm v2.4s, v0.4s\nfadd v2.4s, v2.4s, v1.4s\nfrintx v2.4s, v2.4s\nfmul v2.4s, v2.4s, v0.4s\nfrinta v2.4s, v2.4s")
typedef uint64_t (*fnf)(const uint32_t *, uint64_t, uint64_t *);
/* values to round: halves and quarters, the last fractional values below
 * 2^23 and 2^52, specials; single, and double (two words each) */
static const uint32_t rv[] = {
    0x3f000000, 0xbf000000, 0x3fc00000, 0x40200000, 0xc0200000, 0x3effffff, 0x3f7fffff, 0x4b000000,
    0x4affffff, 0xcafffffe, 0x3e800000, 0xbf400000, 0x00000001, 0x80000000, 0x7f800000, 0x7fc00000,
    0x7f800001, 0x3f800000, 0x42f6e979, 0xc2f6e979, 0x00800000, 0x7f7fffff, 0x3f400000, 0xc0600000,
};
static const uint64_t rvd[] = {
    0x3fe0000000000000ull, 0xbfe0000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull,
    0xc004000000000000ull, 0x3fdfffffffffffffull, 0x3fefffffffffffffull, 0x4330000000000000ull,
    0x432fffffffffffffull, 0xc32ffffffffffffeull, 0x3fd0000000000000ull, 0xbfe8000000000000ull,
    0x0000000000000001ull, 0x8000000000000000ull, 0x7ff0000000000000ull, 0x7ff8000000000000ull,
    0x7ff0000000000001ull, 0x405edd2f1a9fbe77ull, 0xc05edd2f1a9fbe77ull, 0x0010000000000000ull,
};
#define NR (sizeof rv / sizeof *rv)
#define NRD (sizeof rvd / sizeof *rvd)
static void runr(const char *name, fnf f)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned c = 0; c < sizeof fpcrs / sizeof *fpcrs; c++)
        for (unsigned i = 0; i < NR; i++)
            for (unsigned j = 0; j < NR; j++) {
                uint64_t a = rvd[i % NRD], b = rvd[j % NRD];
                uint32_t in[12] = {(uint32_t)a, (uint32_t)(a >> 32), (uint32_t)b, (uint32_t)(b >> 32),
                                   rv[i], rv[j], rv[i] ^ 0x80000000u, rv[(i + j) % NR],
                                   rv[(i + 5) % NR], 0x12345678, rv[j], 0x9abcdef0};
                if ((i + j) & 1) { in[0] = rv[j]; in[1] = rv[i]; in[2] = rv[(j + 3) % NR]; in[3] = rv[i] ^ 0x80000000u; }
                uint64_t fl = 0, r = f(in, fpcrs[c], &fl);
                h = mix(mix(h, r), fl);
            }
    printf("%-10s %016llx\n", name, (unsigned long long)h);
}
static void runf(const char *name, fnf f)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned c = 0; c < sizeof fpcrs / sizeof *fpcrs; c++)
        for (unsigned i = 0; i < NF; i++)
            for (unsigned j = 0; j < NF; j++) {
                uint32_t in[12] = {fv[i], fv[j], fv[(i + 5) % NF], fv[i] ^ 0x80000000u,
                                   fv[j], fv[i], fv[(j + 7) % NF], fv[i],
                                   fv[(i + j) % NF], 0x12345678, fv[j], 0x9abcdef0};
                uint64_t fl = 0, r = f(in, fpcrs[c], &fl);
                h = mix(mix(h, r), fl);
            }
    printf("%-10s %016llx\n", name, (unsigned long long)h);
}

/* Structure loads and stores: x0 = a 256-byte buffer filled from a and b,
 * x1 = a post-index offset from b; the hash covers v2-v5, x0 after
 * writeback (relative to the buffer) and the buffer. */
#define LDST(name, insn)                                                                \
    NOINL static uint64_t name(uint64_t a, uint64_t b, uint64_t *fl)                    \
    {                                                                                   \
        uint64_t buf[48], out[8];                                                       \
        for (int k = 0; k < 48; k++) buf[k] = mix(a + (uint64_t)k, b);                  \
        register uint64_t *x0 __asm__("x0") = buf + 8;                                  \
        register uint64_t x1 __asm__("x1") = (b & 0x3f) - 16, x3 __asm__("x3");         \
        register uint64_t *x5 __asm__("x5") = out;                                      \
        __asm__ volatile("ldp q2, q3, [x0, #-64]\nldp q4, q5, [x0, #-32]\ncmp x1, x3\n" \
                         insn "\nmrs x3, nzcv\nstp q2, q3, [x5]\nstp q4, q5, [x5, #32]"  \
                         : "+r"(x0), "+r"(x1), "=r"(x3) : "r"(x5)                       \
                         : "cc", "memory", "v0", "v2", "v3", "v4", "v5", "v6", "v31", "x9"); \
        *fl = x3;                                                                       \
        uint64_t h = mix((uint64_t)((uint8_t *)x0 - (uint8_t *)buf), x1);               \
        for (int k = 0; k < 8; k++) h = mix(h, out[k]);                                 \
        for (int k = 0; k < 48; k++) h = mix(h, buf[k]);                                \
        return h;                                                                       \
    }
LDST(t_ld1a, "ld1 {v2.16b}, [x0]")
LDST(t_ld1b, "ld1 {v2.8b, v3.8b}, [x0], #16")
LDST(t_ld1c, "ld1 {v2.4s, v3.4s, v4.4s}, [x0], x1")
LDST(t_ld1d, "ld1 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0], #64")
LDST(t_ld1e, "ld1 {v2.1d, v3.1d, v4.1d, v5.1d}, [x0], x1")
LDST(t_ld1f, "ld1 {v31.4h, v0.4h}, [x0]\nmov v2.16b, v31.16b\nmov v3.16b, v0.16b")
LDST(t_st1a, "st1 {v2.16b}, [x0]")
LDST(t_st1b, "st1 {v3.2s, v4.2s}, [x0], x1")
LDST(t_st1c, "st1 {v2.8h, v3.8h, v4.8h}, [x0], #48")
LDST(t_st1d, "st1 {v2.1d, v3.1d, v4.1d, v5.1d}, [x0], #32")
LDST(t_st1e, "st1 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0], x1")
LDST(t_ld1r0, "ld1r {v2.16b}, [x0]")
LDST(t_ld1r1, "ld1r {v3.4h}, [x0], #2")
LDST(t_ld1r2, "ld1r {v4.4s}, [x0], x1")
LDST(t_ld1r3, "ld1r {v5.2d}, [x0], #8")
LDST(t_ld1r4, "ld1r {v2.1d}, [x0]")
LDST(t_ld1r5, "ld1r {v3.8b}, [x0], x1")
LDST(t_ld1l0, "ld1 {v2.b}[13], [x0]")
LDST(t_ld1l1, "ld1 {v3.h}[5], [x0], #2")
LDST(t_ld1l2, "ld1 {v4.s}[3], [x0], x1")
LDST(t_ld1l3, "ld1 {v5.d}[1], [x0], #8")
LDST(t_st1l0, "st1 {v2.b}[7], [x0], #1")
LDST(t_st1l1, "st1 {v3.h}[6], [x0], x1")
LDST(t_st1l2, "st1 {v4.s}[1], [x0]")
LDST(t_st1l3, "st1 {v5.d}[0], [x0], #8")
/* SP as the base */
LDST(t_ld1sp, "mov x9, sp\nsub sp, sp, #128\nst1 {v2.16b, v3.16b, v4.16b, v5.16b}, [sp]\n"
              "movi v2.16b, #0\nld1 {v5.4s, v6.4s}, [sp], #32\nld1r {v4.8h}, [sp]\n"
              "ld1 {v3.s}[2], [sp], #4\nmov sp, x9\nmov v2.16b, v6.16b")

/* Signal contexts. A load faults with NZCV live from the compare before
 * it: the handler must see the load's pc, the compare's NZCV and x0, and
 * what it writes to cpsr and x3 must be what the code after the load sees,
 * whether it skips the load (pc + 4) or repairs the page and returns to it. */
static volatile uint64_t sg_pc, sg_seen_pc, sg_seen_nzcv, sg_seen_x0, sg_flip, sg_count, sg_repair;
static uint64_t *sg_page;
static void sg_fault(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    uintptr_t pc = GET_PC(uc);
    sg_seen_pc = pc - sg_pc;
    sg_seen_nzcv = SS(uc).__cpsr >> 28;
    sg_seen_x0 = SS(uc).__x[0];
    sg_count++;
    SS(uc).__cpsr ^= (uint32_t)sg_flip << 28;
    if (sg_repair) mprotect(sg_page, 16384, PROT_READ | PROT_WRITE);
    else { SS(uc).__x[3] = 0x1234 + sg_count; SET_PC_RAW(uc, pc + 4); }
}
NOINL static uint64_t sg_probe(uint64_t a, uint64_t b, uint64_t *p, uint64_t *fl)
{
    register uint64_t x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register uint64_t *x2 __asm__("x2") = p;
    register uint64_t x3 __asm__("x3") = 0, x4 __asm__("x4") = 0;
    register volatile uint64_t *x5 __asm__("x5") = &sg_pc;
    __asm__ volatile("adr x9, 1f\nstr x9, [x5]\ncmp x0, x1\n1: ldr x3, [x2]\nmrs x4, nzcv"
                     : "+r"(x3), "+r"(x4) : "r"(x0), "r"(x1), "r"(x2), "r"(x5) : "cc", "memory", "x9");
    *fl = x4;
    return x3;
}
/* the same for a two-register LD1 with post-index: no writeback when it faults */
NOINL static uint64_t sg_probe_v(uint64_t a, uint64_t b, uint64_t *p, uint64_t *fl)
{
    register uint64_t x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register uint64_t *x2 __asm__("x2") = p;
    register uint64_t x3 __asm__("x3") = 0, x4 __asm__("x4") = 0;
    register volatile uint64_t *x5 __asm__("x5") = &sg_pc;
    __asm__ volatile("adr x9, 1f\nstr x9, [x5]\ncmp x0, x1\n1: ld1 {v2.2d, v3.2d}, [x2], #32\nmrs x4, nzcv"
                     : "+r"(x3), "+r"(x4), "+r"(x2) : "r"(x0), "r"(x1), "r"(x5) : "cc", "memory", "x9", "v2", "v3");
    *fl = x4;
    return mix(x3, (uint64_t)((uint8_t *)x2 - (uint8_t *)p));
}
static void sigctx(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sg_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sg_page = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (int repair = 0; repair < 2; repair++) {
        uint64_t h = 14695981039346656037ull;
        for (unsigned i = 0; i < NV; i += 3)
            for (unsigned j = 0; j < NV; j += 5)
                for (unsigned flip = 0; flip < 16; flip += 5) {
                    uint64_t fl = 0, r;
                    sg_repair = (uint64_t)repair;
                    sg_flip = flip;
                    if (repair) { sg_page[0] = vals[i] ^ 0x77; mprotect(sg_page, 16384, PROT_NONE); }
                    r = sg_probe(vals[i], vals[j], repair ? sg_page : (uint64_t *)(uintptr_t)0x10, &fl);
                    h = mix(mix(mix(mix(mix(mix(h, r), fl), sg_seen_pc), sg_seen_nzcv), sg_seen_x0), sg_count);
                }
        printf("%-10s %016llx\n", repair ? "sig-repair" : "sig-skip", (unsigned long long)h);
        h = 14695981039346656037ull;
        for (unsigned i = 0; i < NV; i += 3)
            for (unsigned flip = 0; flip < 16; flip += 5) {
                uint64_t fl = 0, r;
                sg_repair = (uint64_t)repair;
                sg_flip = flip;
                if (repair) { sg_page[0] = vals[i] ^ 0x77; mprotect(sg_page, 16384, PROT_NONE); }
                r = sg_probe_v(vals[i], vals[NV - 1 - i], repair ? sg_page : (uint64_t *)(uintptr_t)0x10, &fl);
                h = mix(mix(mix(mix(mix(mix(h, r), fl), sg_seen_pc), sg_seen_nzcv), sg_seen_x0), sg_count);
            }
        printf("%-10s %016llx\n", repair ? "sigv-repair" : "sigv-skip", (unsigned long long)h);
    }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

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

VEC2(t_cov_zip18b, "zip1 v2.8b, v0.8b, v1.8b")
VEC2(t_cov_zip116b, "zip1 v2.16b, v0.16b, v1.16b")
VEC2(t_cov_zip14h, "zip1 v2.4h, v0.4h, v1.4h")
VEC2(t_cov_zip18h, "zip1 v2.8h, v0.8h, v1.8h")
VEC2(t_cov_zip12s, "zip1 v2.2s, v0.2s, v1.2s")
VEC2(t_cov_zip14s, "zip1 v2.4s, v0.4s, v1.4s")
VEC2(t_cov_zip12d, "zip1 v2.2d, v0.2d, v1.2d")
VEC2(t_cov_zip28b, "zip2 v2.8b, v0.8b, v1.8b")
VEC2(t_cov_zip216b, "zip2 v2.16b, v0.16b, v1.16b")
VEC2(t_cov_zip24h, "zip2 v2.4h, v0.4h, v1.4h")
VEC2(t_cov_zip28h, "zip2 v2.8h, v0.8h, v1.8h")
VEC2(t_cov_zip22s, "zip2 v2.2s, v0.2s, v1.2s")
VEC2(t_cov_zip24s, "zip2 v2.4s, v0.4s, v1.4s")
VEC2(t_cov_zip22d, "zip2 v2.2d, v0.2d, v1.2d")
VEC2(t_cov_uzp18b, "uzp1 v2.8b, v0.8b, v1.8b")
VEC2(t_cov_uzp116b, "uzp1 v2.16b, v0.16b, v1.16b")
VEC2(t_cov_uzp14h, "uzp1 v2.4h, v0.4h, v1.4h")
VEC2(t_cov_uzp18h, "uzp1 v2.8h, v0.8h, v1.8h")
VEC2(t_cov_uzp12s, "uzp1 v2.2s, v0.2s, v1.2s")
VEC2(t_cov_uzp14s, "uzp1 v2.4s, v0.4s, v1.4s")
VEC2(t_cov_uzp12d, "uzp1 v2.2d, v0.2d, v1.2d")
VEC2(t_cov_uzp28b, "uzp2 v2.8b, v0.8b, v1.8b")
VEC2(t_cov_uzp216b, "uzp2 v2.16b, v0.16b, v1.16b")
VEC2(t_cov_uzp24h, "uzp2 v2.4h, v0.4h, v1.4h")
VEC2(t_cov_uzp28h, "uzp2 v2.8h, v0.8h, v1.8h")
VEC2(t_cov_uzp22s, "uzp2 v2.2s, v0.2s, v1.2s")
VEC2(t_cov_uzp24s, "uzp2 v2.4s, v0.4s, v1.4s")
VEC2(t_cov_uzp22d, "uzp2 v2.2d, v0.2d, v1.2d")
VEC2(t_cov_trn18b, "trn1 v2.8b, v0.8b, v1.8b")
VEC2(t_cov_trn116b, "trn1 v2.16b, v0.16b, v1.16b")
VEC2(t_cov_trn14h, "trn1 v2.4h, v0.4h, v1.4h")
VEC2(t_cov_trn18h, "trn1 v2.8h, v0.8h, v1.8h")
VEC2(t_cov_trn12s, "trn1 v2.2s, v0.2s, v1.2s")
VEC2(t_cov_trn14s, "trn1 v2.4s, v0.4s, v1.4s")
VEC2(t_cov_trn12d, "trn1 v2.2d, v0.2d, v1.2d")
VEC2(t_cov_trn28b, "trn2 v2.8b, v0.8b, v1.8b")
VEC2(t_cov_trn216b, "trn2 v2.16b, v0.16b, v1.16b")
VEC2(t_cov_trn24h, "trn2 v2.4h, v0.4h, v1.4h")
VEC2(t_cov_trn28h, "trn2 v2.8h, v0.8h, v1.8h")
VEC2(t_cov_trn22s, "trn2 v2.2s, v0.2s, v1.2s")
VEC2(t_cov_trn24s, "trn2 v2.4s, v0.4s, v1.4s")
VEC2(t_cov_trn22d, "trn2 v2.2d, v0.2d, v1.2d")

VEC2(t_cov_addv8b, "addv b2, v0.8b")
VEC2(t_cov_addv16b, "addv b2, v0.16b")
VEC2(t_cov_addv4h, "addv h2, v0.4h")
VEC2(t_cov_addv8h, "addv h2, v0.8h")
VEC2(t_cov_addv4s, "addv s2, v0.4s")
VEC2(t_cov_saddlp4h, "saddlp v2.4h, v0.8b")
VEC2(t_cov_saddlp8h, "saddlp v2.8h, v0.16b")
VEC2(t_cov_saddlp2s, "saddlp v2.2s, v0.4h")
VEC2(t_cov_saddlp4s, "saddlp v2.4s, v0.8h")
VEC2(t_cov_saddlp1d, "saddlp v2.1d, v0.2s")
VEC2(t_cov_saddlp2d, "saddlp v2.2d, v0.4s")
VEC2(t_cov_uaddlp4h, "uaddlp v2.4h, v0.8b")
VEC2(t_cov_uaddlp8h, "uaddlp v2.8h, v0.16b")
VEC2(t_cov_uaddlp2s, "uaddlp v2.2s, v0.4h")
VEC2(t_cov_uaddlp4s, "uaddlp v2.4s, v0.8h")
VEC2(t_cov_uaddlp1d, "uaddlp v2.1d, v0.2s")
VEC2(t_cov_uaddlp2d, "uaddlp v2.2d, v0.4s")
VEC2(t_cov_sadalp4h, "sadalp v2.4h, v0.8b")
VEC2(t_cov_sadalp8h, "sadalp v2.8h, v0.16b")
VEC2(t_cov_sadalp2s, "sadalp v2.2s, v0.4h")
VEC2(t_cov_sadalp4s, "sadalp v2.4s, v0.8h")
VEC2(t_cov_sadalp1d, "sadalp v2.1d, v0.2s")
VEC2(t_cov_sadalp2d, "sadalp v2.2d, v0.4s")
VEC2(t_cov_uadalp4h, "uadalp v2.4h, v0.8b")
VEC2(t_cov_uadalp8h, "uadalp v2.8h, v0.16b")
VEC2(t_cov_uadalp2s, "uadalp v2.2s, v0.4h")
VEC2(t_cov_uadalp4s, "uadalp v2.4s, v0.8h")
VEC2(t_cov_uadalp1d, "uadalp v2.1d, v0.2s")
VEC2(t_cov_uadalp2d, "uadalp v2.2d, v0.4s")

#define R(n) run(#n, t_##n)
#define RA(n) R(n##b); R(n##h); R(n##w); R(n##x)
int main(void)
{
    R(cov_zip18b);
    R(cov_zip116b);
    R(cov_zip14h);
    R(cov_zip18h);
    R(cov_zip12s);
    R(cov_zip14s);
    R(cov_zip12d);
    R(cov_zip28b);
    R(cov_zip216b);
    R(cov_zip24h);
    R(cov_zip28h);
    R(cov_zip22s);
    R(cov_zip24s);
    R(cov_zip22d);
    R(cov_uzp18b);
    R(cov_uzp116b);
    R(cov_uzp14h);
    R(cov_uzp18h);
    R(cov_uzp12s);
    R(cov_uzp14s);
    R(cov_uzp12d);
    R(cov_uzp28b);
    R(cov_uzp216b);
    R(cov_uzp24h);
    R(cov_uzp28h);
    R(cov_uzp22s);
    R(cov_uzp24s);
    R(cov_uzp22d);
    R(cov_trn18b);
    R(cov_trn116b);
    R(cov_trn14h);
    R(cov_trn18h);
    R(cov_trn12s);
    R(cov_trn14s);
    R(cov_trn12d);
    R(cov_trn28b);
    R(cov_trn216b);
    R(cov_trn24h);
    R(cov_trn28h);
    R(cov_trn22s);
    R(cov_trn24s);
    R(cov_trn22d);
    R(cov_addv8b);
    R(cov_addv16b);
    R(cov_addv4h);
    R(cov_addv8h);
    R(cov_addv4s);
    R(cov_saddlp4h);
    R(cov_saddlp8h);
    R(cov_saddlp2s);
    R(cov_saddlp4s);
    R(cov_saddlp1d);
    R(cov_saddlp2d);
    R(cov_uaddlp4h);
    R(cov_uaddlp8h);
    R(cov_uaddlp2s);
    R(cov_uaddlp4s);
    R(cov_uaddlp1d);
    R(cov_uaddlp2d);
    R(cov_sadalp4h);
    R(cov_sadalp8h);
    R(cov_sadalp2s);
    R(cov_sadalp4s);
    R(cov_sadalp1d);
    R(cov_sadalp2d);
    R(cov_uadalp4h);
    R(cov_uadalp8h);
    R(cov_uadalp2s);
    R(cov_uadalp4s);
    R(cov_uadalp1d);
    R(cov_uadalp2d);
    R(udiv); R(udivw); R(sdiv); R(sdivw);
    R(adc); R(adcw); R(adcs); R(adcsw); R(sbc); R(sbcw); R(sbcs); R(sbcsw); R(ngcs); R(add128);
    R(rbit); R(rbitw); R(rev16); R(rev16w); R(rev32); R(ctz);
    R(bfi0); R(bfi1); R(bfi2); R(bfi3); R(bfi4); R(bfi5);
    R(bfxil0); R(bfxil1); R(bfxil2); R(bfxil3); R(bfxil4); R(bfc); R(bfcw);
    R(fl1); R(fl2); R(fl3); R(fl4); R(fl5); R(fl6); R(fl7);
    R(movk); R(movkw); R(movk2); R(movn); R(movk3); R(movk4); R(movk5); R(movk6); R(movk7); R(movz2);
    RA(ldadd); RA(ldclr); RA(ldeor); RA(ldset); RA(ldsmax); RA(ldsmin); RA(ldumax); RA(ldumin);
    RA(swp); RA(ldaddr); RA(swpl); RA(cas);
    R(stadd); R(stclrb); R(casx2); R(ldapr); R(ldaprb); R(ldraa); R(ldrab); R(stlr);
#define RV(n) R(n##16b); R(n##8h); R(n##4s); R(n##8b)
    RV(vadd); RV(vsub); RV(vcmeq); RV(vcmgt); RV(vcmge); RV(vcmtst); RV(vcmhi); RV(vcmhs);
    RV(vumax); RV(vumin); RV(vsmax); RV(vsmin); RV(vmul);
    RV(vcmeqz); RV(vcmgtz); RV(vcmgez); RV(vcmlez); RV(vcmltz); RV(vneg); RV(vabs);
    R(vadd2d); R(vsub2d); R(vcmeq2d); R(vneg2d); R(vaddd);
    RV(vmla); RV(vmls);
    R(vcmgt2d); R(vcmge2d); R(vcmhi2d); R(vcmhs2d); R(vcmhs2d2); R(vcmtst2d);
    R(vcmgtz2d); R(vcmgez2d); R(vcmeqz2d); R(vcmlez2d); R(vcmltz2d); R(vabs2d); R(vabs2d2);
    R(vcmhi4s2); R(vcmhs2s); R(vabs2s); R(vabs8b2);
    R(vmule0); R(vmule1); R(vmule2); R(vmule3); R(vmlae0); R(vmlae1); R(vmlse0); R(vmlse1); R(vmul16b2);
    R(tbl1); R(tbl1m); R(tbl1h); R(tbl2); R(tbl3); R(tbl4); R(tbl4h); R(tbx1); R(tbx2); R(tbx3); R(tbx4); R(tbxr);
    R(ext0); R(ext1); R(ext2); R(ext3); R(ext4); R(ext5); R(ext6);
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
    R(shl7); R(shl8); R(ushr6); R(ushr7); R(sshr5); R(sshr6); R(sshr7); R(sshr8); R(sshr9); R(sshr10); R(sshr11); R(sshr12);
    R(sra0); R(sra1); R(sra2); R(sra3); R(sra4); R(rshr0); R(rshr1); R(rshr2); R(rshr3); R(rshr4); R(rshr5);
    R(rsra0); R(rsra1); R(rsra2);
    R(xtn0); R(xtn1); R(xtn2); R(xtn3); R(xtn4); R(xtn5);
    R(shrn0); R(shrn1); R(shrn2); R(shrn3); R(shrn4); R(shrn5); R(shrn6);
    R(shll0); R(shll1); R(shll2); R(shll3); R(shll4); R(shll5); R(shll6); R(shll7);
    R(fabs0); R(fabs1); R(fabs2); R(fneg0); R(fneg1); R(fneg2);
#define RF(n) runf(#n, t_##n)
#define RF4(n) RF(n##4s); RF(n##2s)
    RF(fmla4h); RF(fmla8h); RF(fmlah0); RF(fmlah1); RF(fmlah2); RF(fmlah3);
    RF(fmlah4); RF(fmlah5); RF(fmlah6); RF(fmlah7); RF(fmls4h); RF(fmls8h);
    RF(fmlsh0); RF(fmlsh1); RF(fmlsh2); RF(fmlsh3); RF(fmlsh4); RF(fmlsh5);
    RF(fmlsh6); RF(fmlsh7);
    RF4(fmla); RF4(fmls);
    RF(fmlae0); RF(fmlae1); RF(fmlae2); RF(fmlae3); RF(fmlse0); RF(fmlse1); RF(fmlse2); RF(fmlse3);
    RF4(fadd); RF4(fsub); RF4(fmul); RF4(fmax); RF4(fmin); RF4(fmaxnm); RF4(fminnm); RF4(fcmeq); RF4(fcmge); RF4(fcmgt);
    RF(fmule0); RF(fmule1); RF(fmule2); RF(fmule3); RF(fchain); RF(fadd2d);
#define RR(n) runr(#n, t_##n)
#define RFR(n) RR(n##4s); RR(n##2s); RR(n##2d); RR(n##s); RR(n##d)
    RFR(frintn); RFR(frintm); RFR(frintp); RFR(frintz); RFR(frinta); RFR(frintx); RFR(frinti); RR(frchain);
    R(ld1a); R(ld1b); R(ld1c); R(ld1d); R(ld1e); R(ld1f); R(st1a); R(st1b); R(st1c); R(st1d); R(st1e);
    R(ld1r0); R(ld1r1); R(ld1r2); R(ld1r3); R(ld1r4); R(ld1r5);
    R(ld1l0); R(ld1l1); R(ld1l2); R(ld1l3); R(st1l0); R(st1l1); R(st1l2); R(st1l3); R(ld1sp);
    sigctx();
    pthread_t th[NT];
    for (uintptr_t k = 0; k < NT; k++) pthread_create(&th[k], NULL, worker, (void *)k);
    for (int k = 0; k < NT; k++) pthread_join(th[k], NULL);
    printf("threads    add=%llu w=%u b=%u cas=%llu max=%llu or=%016llx\n", (unsigned long long)ctr_add, ctr_w, ctr_b,
           (unsigned long long)ctr_cas, (unsigned long long)ctr_max, (unsigned long long)ctr_or);
    return 0;
}
