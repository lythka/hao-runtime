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
static const uint64_t fpcrs[] = {0u << 22, 1u << 22, 2u << 22, 3u << 22, 4u << 22, 5u << 22, 6u << 22, 7u << 22, 8u << 22, 9u << 22, 10u << 22, 11u << 22, 12u << 22, 13u << 22, 14u << 22, 15u << 22};
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
VECF(t_abd2s, "fabd v2.2s,v0.2s,v1.2s")
VECF(t_abd4s, "fabd v2.4s,v0.4s,v1.4s")
VECF(t_abd2d, "fabd v2.2d,v0.2d,v1.2d")
VECF(t_abds, "fabd s2,s0,s1")
VECF(t_abdd, "fabd d2,d0,d1")
VECF(t_sqadd_8b, "sqadd v2.8b,v0.8b,v1.8b")
VECF(t_sqadd_16b, "sqadd v2.16b,v0.16b,v1.16b")
VECF(t_sqadd_4h, "sqadd v2.4h,v0.4h,v1.4h")
VECF(t_sqadd_8h, "sqadd v2.8h,v0.8h,v1.8h")
VECF(t_sqadd_2s, "sqadd v2.2s,v0.2s,v1.2s")
VECF(t_sqadd_4s, "sqadd v2.4s,v0.4s,v1.4s")
VECF(t_sqadd_2d, "sqadd v2.2d,v0.2d,v1.2d")
VECF(t_sqadd_b, "sqadd b2,b0,b1")
VECF(t_sqadd_h, "sqadd h2,h0,h1")
VECF(t_sqadd_s, "sqadd s2,s0,s1")
VECF(t_sqadd_d, "sqadd d2,d0,d1")
VECF(t_uqadd_8b, "uqadd v2.8b,v0.8b,v1.8b")
VECF(t_uqadd_16b, "uqadd v2.16b,v0.16b,v1.16b")
VECF(t_uqadd_4h, "uqadd v2.4h,v0.4h,v1.4h")
VECF(t_uqadd_8h, "uqadd v2.8h,v0.8h,v1.8h")
VECF(t_uqadd_2s, "uqadd v2.2s,v0.2s,v1.2s")
VECF(t_uqadd_4s, "uqadd v2.4s,v0.4s,v1.4s")
VECF(t_uqadd_2d, "uqadd v2.2d,v0.2d,v1.2d")
VECF(t_uqadd_b, "uqadd b2,b0,b1")
VECF(t_uqadd_h, "uqadd h2,h0,h1")
VECF(t_uqadd_s, "uqadd s2,s0,s1")
VECF(t_uqadd_d, "uqadd d2,d0,d1")
VECF(t_sqsub_8b, "sqsub v2.8b,v0.8b,v1.8b")
VECF(t_sqsub_16b, "sqsub v2.16b,v0.16b,v1.16b")
VECF(t_sqsub_4h, "sqsub v2.4h,v0.4h,v1.4h")
VECF(t_sqsub_8h, "sqsub v2.8h,v0.8h,v1.8h")
VECF(t_sqsub_2s, "sqsub v2.2s,v0.2s,v1.2s")
VECF(t_sqsub_4s, "sqsub v2.4s,v0.4s,v1.4s")
VECF(t_sqsub_2d, "sqsub v2.2d,v0.2d,v1.2d")
VECF(t_sqsub_b, "sqsub b2,b0,b1")
VECF(t_sqsub_h, "sqsub h2,h0,h1")
VECF(t_sqsub_s, "sqsub s2,s0,s1")
VECF(t_sqsub_d, "sqsub d2,d0,d1")
VECF(t_uqsub_8b, "uqsub v2.8b,v0.8b,v1.8b")
VECF(t_uqsub_16b, "uqsub v2.16b,v0.16b,v1.16b")
VECF(t_uqsub_4h, "uqsub v2.4h,v0.4h,v1.4h")
VECF(t_uqsub_8h, "uqsub v2.8h,v0.8h,v1.8h")
VECF(t_uqsub_2s, "uqsub v2.2s,v0.2s,v1.2s")
VECF(t_uqsub_4s, "uqsub v2.4s,v0.4s,v1.4s")
VECF(t_uqsub_2d, "uqsub v2.2d,v0.2d,v1.2d")
VECF(t_uqsub_b, "uqsub b2,b0,b1")
VECF(t_uqsub_h, "uqsub h2,h0,h1")
VECF(t_uqsub_s, "uqsub s2,s0,s1")
VECF(t_uqsub_d, "uqsub d2,d0,d1")
VECF(t_sabd_8b, "sabd v2.8b,v0.8b,v1.8b")
VECF(t_sabd_4h, "sabd v2.4h,v0.4h,v1.4h")
VECF(t_sabd_2s, "sabd v2.2s,v0.2s,v1.2s")
VECF(t_sabd_16b, "sabd v2.16b,v0.16b,v1.16b")
VECF(t_sabd_8h, "sabd v2.8h,v0.8h,v1.8h")
VECF(t_sabd_4s, "sabd v2.4s,v0.4s,v1.4s")
VECF(t_uabd_8b, "uabd v2.8b,v0.8b,v1.8b")
VECF(t_uabd_4h, "uabd v2.4h,v0.4h,v1.4h")
VECF(t_uabd_2s, "uabd v2.2s,v0.2s,v1.2s")
VECF(t_uabd_16b, "uabd v2.16b,v0.16b,v1.16b")
VECF(t_uabd_8h, "uabd v2.8h,v0.8h,v1.8h")
VECF(t_uabd_4s, "uabd v2.4s,v0.4s,v1.4s")
VECF(t_saba_8b, "saba v2.8b,v0.8b,v1.8b")
VECF(t_saba_4h, "saba v2.4h,v0.4h,v1.4h")
VECF(t_saba_2s, "saba v2.2s,v0.2s,v1.2s")
VECF(t_saba_16b, "saba v2.16b,v0.16b,v1.16b")
VECF(t_saba_8h, "saba v2.8h,v0.8h,v1.8h")
VECF(t_saba_4s, "saba v2.4s,v0.4s,v1.4s")
VECF(t_uaba_8b, "uaba v2.8b,v0.8b,v1.8b")
VECF(t_uaba_4h, "uaba v2.4h,v0.4h,v1.4h")
VECF(t_uaba_2s, "uaba v2.2s,v0.2s,v1.2s")
VECF(t_uaba_16b, "uaba v2.16b,v0.16b,v1.16b")
VECF(t_uaba_8h, "uaba v2.8h,v0.8h,v1.8h")
VECF(t_uaba_4s, "uaba v2.4s,v0.4s,v1.4s")
VECF(t_sabdl_8h, "sabdl v2.8h,v0.8b,v1.8b")
VECF(t_sabdl_4s, "sabdl v2.4s,v0.4h,v1.4h")
VECF(t_sabdl_2d, "sabdl v2.2d,v0.2s,v1.2s")
VECF(t_sabdl2_8h, "sabdl2 v2.8h,v0.16b,v1.16b")
VECF(t_sabdl2_4s, "sabdl2 v2.4s,v0.8h,v1.8h")
VECF(t_sabdl2_2d, "sabdl2 v2.2d,v0.4s,v1.4s")
VECF(t_uabdl_8h, "uabdl v2.8h,v0.8b,v1.8b")
VECF(t_uabdl_4s, "uabdl v2.4s,v0.4h,v1.4h")
VECF(t_uabdl_2d, "uabdl v2.2d,v0.2s,v1.2s")
VECF(t_uabdl2_8h, "uabdl2 v2.8h,v0.16b,v1.16b")
VECF(t_uabdl2_4s, "uabdl2 v2.4s,v0.8h,v1.8h")
VECF(t_uabdl2_2d, "uabdl2 v2.2d,v0.4s,v1.4s")
VECF(t_sabal_8h, "sabal v2.8h,v0.8b,v1.8b")
VECF(t_sabal_4s, "sabal v2.4s,v0.4h,v1.4h")
VECF(t_sabal_2d, "sabal v2.2d,v0.2s,v1.2s")
VECF(t_sabal2_8h, "sabal2 v2.8h,v0.16b,v1.16b")
VECF(t_sabal2_4s, "sabal2 v2.4s,v0.8h,v1.8h")
VECF(t_sabal2_2d, "sabal2 v2.2d,v0.4s,v1.4s")
VECF(t_uabal_8h, "uabal v2.8h,v0.8b,v1.8b")
VECF(t_uabal_4s, "uabal v2.4s,v0.4h,v1.4h")
VECF(t_uabal_2d, "uabal v2.2d,v0.2s,v1.2s")
VECF(t_uabal2_8h, "uabal2 v2.8h,v0.16b,v1.16b")
VECF(t_uabal2_4s, "uabal2 v2.4s,v0.8h,v1.8h")
VECF(t_uabal2_2d, "uabal2 v2.2d,v0.4s,v1.4s")
VECF(t_addhn_8b, "addhn v2.8b,v0.8h,v1.8h")
VECF(t_addhn_4h, "addhn v2.4h,v0.4s,v1.4s")
VECF(t_addhn_2s, "addhn v2.2s,v0.2d,v1.2d")
VECF(t_addhn2_16b, "addhn2 v2.16b,v0.8h,v1.8h")
VECF(t_addhn2_8h, "addhn2 v2.8h,v0.4s,v1.4s")
VECF(t_addhn2_4s, "addhn2 v2.4s,v0.2d,v1.2d")
VECF(t_raddhn_8b, "raddhn v2.8b,v0.8h,v1.8h")
VECF(t_raddhn_4h, "raddhn v2.4h,v0.4s,v1.4s")
VECF(t_raddhn_2s, "raddhn v2.2s,v0.2d,v1.2d")
VECF(t_raddhn2_16b, "raddhn2 v2.16b,v0.8h,v1.8h")
VECF(t_raddhn2_8h, "raddhn2 v2.8h,v0.4s,v1.4s")
VECF(t_raddhn2_4s, "raddhn2 v2.4s,v0.2d,v1.2d")
VECF(t_subhn_8b, "subhn v2.8b,v0.8h,v1.8h")
VECF(t_subhn_4h, "subhn v2.4h,v0.4s,v1.4s")
VECF(t_subhn_2s, "subhn v2.2s,v0.2d,v1.2d")
VECF(t_subhn2_16b, "subhn2 v2.16b,v0.8h,v1.8h")
VECF(t_subhn2_8h, "subhn2 v2.8h,v0.4s,v1.4s")
VECF(t_subhn2_4s, "subhn2 v2.4s,v0.2d,v1.2d")
VECF(t_rsubhn_8b, "rsubhn v2.8b,v0.8h,v1.8h")
VECF(t_rsubhn_4h, "rsubhn v2.4h,v0.4s,v1.4s")
VECF(t_rsubhn_2s, "rsubhn v2.2s,v0.2d,v1.2d")
VECF(t_rsubhn2_16b, "rsubhn2 v2.16b,v0.8h,v1.8h")
VECF(t_rsubhn2_8h, "rsubhn2 v2.8h,v0.4s,v1.4s")
VECF(t_rsubhn2_4s, "rsubhn2 v2.4s,v0.2d,v1.2d")
VECF(t_addp_8b, "addp v2.8b,v0.8b,v1.8b")
VECF(t_addp_16b, "addp v2.16b,v0.16b,v1.16b")
VECF(t_addp_4h, "addp v2.4h,v0.4h,v1.4h")
VECF(t_addp_8h, "addp v2.8h,v0.8h,v1.8h")
VECF(t_addp_2s, "addp v2.2s,v0.2s,v1.2s")
VECF(t_addp_4s, "addp v2.4s,v0.4s,v1.4s")
VECF(t_addp_2d, "addp v2.2d,v0.2d,v1.2d")
VECF(t_fmul_2d_e0, "fmul v2.2d,v0.2d,v17.d[0]")
VECF(t_fmul_2d_e1, "fmul v2.2d,v0.2d,v17.d[1]")
VECF(t_tail_fcvtn_4h, "fcvtn v2.4h,v0.4s")
VECF(t_tail_fcvtn_2s, "fcvtn v2.2s,v0.2d")
VECF(t_tail_fcvtn2_8h, "fcvtn2 v2.8h,v0.4s")
VECF(t_tail_fcvtn2_4s, "fcvtn2 v2.4s,v0.2d")
VECF(t_tail_fcvtl_4s, "fcvtl v2.4s,v0.4h")
VECF(t_tail_fcvtl_2d, "fcvtl v2.2d,v0.2s")
VECF(t_tail_fcvtl2_4s, "fcvtl2 v2.4s,v0.8h")
VECF(t_tail_fcvtl2_2d, "fcvtl2 v2.2d,v0.4s")
VECF(t_tail_fmul_4h, "fmul v2.4h,v0.4h,v1.4h")
VECF(t_tail_fmul_8h, "fmul v2.8h,v0.8h,v1.8h")
VECF(t_tail_fmul_h, "fmul h2,h0,h1")
VECF(t_tail_fadd_4h, "fadd v2.4h,v0.4h,v1.4h")
VECF(t_tail_fadd_8h, "fadd v2.8h,v0.8h,v1.8h")
VECF(t_tail_fadd_h, "fadd h2,h0,h1")
VECF(t_tail_fsub_4h, "fsub v2.4h,v0.4h,v1.4h")
VECF(t_tail_fsub_8h, "fsub v2.8h,v0.8h,v1.8h")
VECF(t_tail_fsub_h, "fsub h2,h0,h1")
VECF(t_tail_fmax_4h, "fmax v2.4h,v0.4h,v1.4h")
VECF(t_tail_fmax_8h, "fmax v2.8h,v0.8h,v1.8h")
VECF(t_tail_fmax_h, "fmax h2,h0,h1")
VECF(t_tail_fmin_4h, "fmin v2.4h,v0.4h,v1.4h")
VECF(t_tail_fmin_8h, "fmin v2.8h,v0.8h,v1.8h")
VECF(t_tail_fmin_h, "fmin h2,h0,h1")
VECF(t_tail_fmaxnm_4h, "fmaxnm v2.4h,v0.4h,v1.4h")
VECF(t_tail_fmaxnm_8h, "fmaxnm v2.8h,v0.8h,v1.8h")
VECF(t_tail_fmaxnm_h, "fmaxnm h2,h0,h1")
VECF(t_tail_fminnm_4h, "fminnm v2.4h,v0.4h,v1.4h")
VECF(t_tail_fminnm_8h, "fminnm v2.8h,v0.8h,v1.8h")
VECF(t_tail_fminnm_h, "fminnm h2,h0,h1")
VECF(t_tail_fmul_4h_e0, "fmul v2.4h,v0.4h,v1.h[0]")
VECF(t_tail_fmul_8h_e0, "fmul v2.8h,v0.8h,v1.h[0]")
VECF(t_tail_fmul_4h_e1, "fmul v2.4h,v0.4h,v1.h[1]")
VECF(t_tail_fmul_8h_e1, "fmul v2.8h,v0.8h,v1.h[1]")
VECF(t_tail_fmul_4h_e2, "fmul v2.4h,v0.4h,v1.h[2]")
VECF(t_tail_fmul_8h_e2, "fmul v2.8h,v0.8h,v1.h[2]")
VECF(t_tail_fmul_4h_e3, "fmul v2.4h,v0.4h,v1.h[3]")
VECF(t_tail_fmul_8h_e3, "fmul v2.8h,v0.8h,v1.h[3]")
VECF(t_tail_fmul_4h_e4, "fmul v2.4h,v0.4h,v1.h[4]")
VECF(t_tail_fmul_8h_e4, "fmul v2.8h,v0.8h,v1.h[4]")
VECF(t_tail_fmul_4h_e5, "fmul v2.4h,v0.4h,v1.h[5]")
VECF(t_tail_fmul_8h_e5, "fmul v2.8h,v0.8h,v1.h[5]")
VECF(t_tail_fmul_4h_e6, "fmul v2.4h,v0.4h,v1.h[6]")
VECF(t_tail_fmul_8h_e6, "fmul v2.8h,v0.8h,v1.h[6]")
VECF(t_tail_fmul_4h_e7, "fmul v2.4h,v0.4h,v1.h[7]")
VECF(t_tail_fmul_8h_e7, "fmul v2.8h,v0.8h,v1.h[7]")
/* Tail conversions: every allocated arrangement and explicit rounding mode. */
VECF(t_tail_fcvtns_2s, "fcvtns v2.2s,v0.2s")
VECF(t_tail_fcvtns_4s, "fcvtns v2.4s,v0.4s")
VECF(t_tail_fcvtns_2d, "fcvtns v2.2d,v0.2d")
VECF(t_tail_fcvtns_s, "fcvtns s2,s0")
VECF(t_tail_fcvtns_d, "fcvtns d2,d0")
VECF(t_tail_fcvtns_4h, "fcvtns v2.4h,v0.4h")
VECF(t_tail_fcvtns_8h, "fcvtns v2.8h,v0.8h")
VECF(t_tail_fcvtnu_2s, "fcvtnu v2.2s,v0.2s")
VECF(t_tail_fcvtnu_4s, "fcvtnu v2.4s,v0.4s")
VECF(t_tail_fcvtnu_2d, "fcvtnu v2.2d,v0.2d")
VECF(t_tail_fcvtnu_s, "fcvtnu s2,s0")
VECF(t_tail_fcvtnu_d, "fcvtnu d2,d0")
VECF(t_tail_fcvtnu_4h, "fcvtnu v2.4h,v0.4h")
VECF(t_tail_fcvtnu_8h, "fcvtnu v2.8h,v0.8h")
VECF(t_tail_fcvtps_2s, "fcvtps v2.2s,v0.2s")
VECF(t_tail_fcvtps_4s, "fcvtps v2.4s,v0.4s")
VECF(t_tail_fcvtps_2d, "fcvtps v2.2d,v0.2d")
VECF(t_tail_fcvtps_s, "fcvtps s2,s0")
VECF(t_tail_fcvtps_d, "fcvtps d2,d0")
VECF(t_tail_fcvtpu_2s, "fcvtpu v2.2s,v0.2s")
VECF(t_tail_fcvtpu_4s, "fcvtpu v2.4s,v0.4s")
VECF(t_tail_fcvtpu_2d, "fcvtpu v2.2d,v0.2d")
VECF(t_tail_fcvtpu_s, "fcvtpu s2,s0")
VECF(t_tail_fcvtpu_d, "fcvtpu d2,d0")
VECF(t_tail_fcvtms_2s, "fcvtms v2.2s,v0.2s")
VECF(t_tail_fcvtms_4s, "fcvtms v2.4s,v0.4s")
VECF(t_tail_fcvtms_2d, "fcvtms v2.2d,v0.2d")
VECF(t_tail_fcvtms_s, "fcvtms s2,s0")
VECF(t_tail_fcvtms_d, "fcvtms d2,d0")
VECF(t_tail_fcvtmu_2s, "fcvtmu v2.2s,v0.2s")
VECF(t_tail_fcvtmu_4s, "fcvtmu v2.4s,v0.4s")
VECF(t_tail_fcvtmu_2d, "fcvtmu v2.2d,v0.2d")
VECF(t_tail_fcvtmu_s, "fcvtmu s2,s0")
VECF(t_tail_fcvtmu_d, "fcvtmu d2,d0")
VECF(t_tail_fcvtas_2s, "fcvtas v2.2s,v0.2s")
VECF(t_tail_fcvtas_4s, "fcvtas v2.4s,v0.4s")
VECF(t_tail_fcvtas_2d, "fcvtas v2.2d,v0.2d")
VECF(t_tail_fcvtas_s, "fcvtas s2,s0")
VECF(t_tail_fcvtas_d, "fcvtas d2,d0")
VECF(t_tail_fcvtau_2s, "fcvtau v2.2s,v0.2s")
VECF(t_tail_fcvtau_4s, "fcvtau v2.4s,v0.4s")
VECF(t_tail_fcvtau_2d, "fcvtau v2.2d,v0.2d")
VECF(t_tail_fcvtau_s, "fcvtau s2,s0")
VECF(t_tail_fcvtau_d, "fcvtau d2,d0")
VECF(t_tail_fcvtzs_2s, "fcvtzs v2.2s,v0.2s")
VECF(t_tail_fcvtzs_4s, "fcvtzs v2.4s,v0.4s")
VECF(t_tail_fcvtzs_2d, "fcvtzs v2.2d,v0.2d")
VECF(t_tail_fcvtzs_s, "fcvtzs s2,s0")
VECF(t_tail_fcvtzs_d, "fcvtzs d2,d0")
VECF(t_tail_fcvtzs_4h, "fcvtzs v2.4h,v0.4h")
VECF(t_tail_fcvtzs_8h, "fcvtzs v2.8h,v0.8h")
VECF(t_tail_fcvtzu_2s, "fcvtzu v2.2s,v0.2s")
VECF(t_tail_fcvtzu_4s, "fcvtzu v2.4s,v0.4s")
VECF(t_tail_fcvtzu_2d, "fcvtzu v2.2d,v0.2d")
VECF(t_tail_fcvtzu_s, "fcvtzu s2,s0")
VECF(t_tail_fcvtzu_d, "fcvtzu d2,d0")
VECF(t_tail_fcvtzu_4h, "fcvtzu v2.4h,v0.4h")
VECF(t_tail_fcvtzu_8h, "fcvtzu v2.8h,v0.8h")
VECF(t_tail_scvtf_2s, "scvtf v2.2s,v0.2s")
VECF(t_tail_scvtf_4s, "scvtf v2.4s,v0.4s")
VECF(t_tail_scvtf_2d, "scvtf v2.2d,v0.2d")
VECF(t_tail_scvtf_s, "scvtf s2,s0")
VECF(t_tail_scvtf_d, "scvtf d2,d0")
VECF(t_tail_scvtf_4h, "scvtf v2.4h,v0.4h")
VECF(t_tail_scvtf_8h, "scvtf v2.8h,v0.8h")
VECF(t_tail_ucvtf_2s, "ucvtf v2.2s,v0.2s")
VECF(t_tail_ucvtf_4s, "ucvtf v2.4s,v0.4s")
VECF(t_tail_ucvtf_2d, "ucvtf v2.2d,v0.2d")
VECF(t_tail_ucvtf_s, "ucvtf s2,s0")
VECF(t_tail_ucvtf_d, "ucvtf d2,d0")
VECF(t_tail_ucvtf_4h, "ucvtf v2.4h,v0.4h")
VECF(t_tail_ucvtf_8h, "ucvtf v2.8h,v0.8h")
VECF(t_tail_fcvtns_ws, "fcvtns w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtns_xs, "fcvtns x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtns_wd, "fcvtns w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtns_xd, "fcvtns x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtns_wh, "fcvtns w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtns_xh, "fcvtns x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtnu_ws, "fcvtnu w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtnu_xs, "fcvtnu x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtnu_wd, "fcvtnu w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtnu_xd, "fcvtnu x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtnu_wh, "fcvtnu w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtnu_xh, "fcvtnu x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtps_ws, "fcvtps w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtps_xs, "fcvtps x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtps_wd, "fcvtps w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtps_xd, "fcvtps x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtps_wh, "fcvtps w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtps_xh, "fcvtps x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtpu_ws, "fcvtpu w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtpu_xs, "fcvtpu x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtpu_wd, "fcvtpu w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtpu_xd, "fcvtpu x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtpu_wh, "fcvtpu w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtpu_xh, "fcvtpu x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtms_ws, "fcvtms w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtms_xs, "fcvtms x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtms_wd, "fcvtms w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtms_xd, "fcvtms x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtms_wh, "fcvtms w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtms_xh, "fcvtms x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtmu_ws, "fcvtmu w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtmu_xs, "fcvtmu x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtmu_wd, "fcvtmu w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtmu_xd, "fcvtmu x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtmu_wh, "fcvtmu w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtmu_xh, "fcvtmu x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtas_ws, "fcvtas w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtas_xs, "fcvtas x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtas_wd, "fcvtas w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtas_xd, "fcvtas x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtas_wh, "fcvtas w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtas_xh, "fcvtas x7,h0\nfmov d2,x7")
VECF(t_tail_fcvtau_ws, "fcvtau w7,s0\nfmov d2,x7")
VECF(t_tail_fcvtau_xs, "fcvtau x7,s0\nfmov d2,x7")
VECF(t_tail_fcvtau_wd, "fcvtau w7,d0\nfmov d2,x7")
VECF(t_tail_fcvtau_xd, "fcvtau x7,d0\nfmov d2,x7")
VECF(t_tail_fcvtau_wh, "fcvtau w7,h0\nfmov d2,x7")
VECF(t_tail_fcvtau_xh, "fcvtau x7,h0\nfmov d2,x7")
VECF(t_tail_fcvt_sd, "fcvt s2,d0")
VECF(t_tail_fcvt_ds, "fcvt d2,s0")
VECF(t_tail_fcvt_hs, "fcvt h2,s0")
VECF(t_tail_fcvt_hd, "fcvt h2,d0")
VECF(t_tail_fcvt_sh, "fcvt s2,h0")
VECF(t_tail_fcvt_dh, "fcvt d2,h0")
VECF(t_tail_scvtf_hw, "fmov x7,d0\nscvtf h2,w7")
VECF(t_tail_scvtf_hx, "fmov x7,d0\nscvtf h2,x7")
VECF(t_tail_ucvtf_hw, "fmov x7,d0\nucvtf h2,w7")
VECF(t_tail_ucvtf_hx, "fmov x7,d0\nucvtf h2,x7")
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
VECF(t_tail_fmadd, "fmadd s2, s0, s1, s2")
VECF(t_tail_fmsub, "fmsub s2, s0, s1, s2")
VECF(t_tail_fnmadd, "fnmadd s2, s0, s1, s2")
VECF(t_tail_fnmsub, "fnmsub s2, s0, s1, s2")
VECF(t_tail_fnmul, "fnmul s2, s0, s1")
VECF(t_tail_frecpss, "frecps s2, s0, s1")
VECF(t_tail_frecps2s, "frecps v2.2s, v0.2s, v1.2s")
VECF(t_tail_frecps4s, "frecps v2.4s, v0.4s, v1.4s")
VECF(t_tail_frsqrtss, "frsqrts s2, s0, s1")
VECF(t_tail_frsqrts2s, "frsqrts v2.2s, v0.2s, v1.2s")
VECF(t_tail_frsqrts4s, "frsqrts v2.4s, v0.4s, v1.4s")
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
VECF(t_faddp2s, "faddp v2.2s, v0.2s, v1.2s")
VECF(t_faddp4s, "faddp v2.4s, v0.4s, v1.4s")
VECF(t_faddp2d, "faddp v2.2d, v0.2d, v1.2d")
VECF(t_faddps, "faddp s2, v0.2s")
VECF(t_faddpd, "faddp d2, v0.2d")
VECF(t_fmaxv, "fmaxv s2, v0.4s")
VECF(t_fminv, "fminv s2, v0.4s")
VECF(t_fmaxnmv, "fmaxnmv s2, v0.4s")
VECF(t_fminnmv, "fminnmv s2, v0.4s")
VECF(t_fdiv2s, "fdiv v2.2s, v0.2s, v1.2s")
VECF(t_fdiv4s, "fdiv v2.4s, v0.4s, v1.4s")
VECF(t_cov_fmuls0, "fmul s2, s0, v17.s[0]")
VECF(t_cov_fmuls1, "fmul s2, s0, v17.s[1]")
VECF(t_cov_fmuls2, "fmul s2, s0, v17.s[2]")
VECF(t_cov_fmuls3, "fmul s2, s0, v17.s[3]")
VECF(t_cov_fmlas0, "fmla s2, s0, v17.s[0]")
VECF(t_cov_fmlas1, "fmla s2, s0, v17.s[1]")
VECF(t_cov_fmlas2, "fmla s2, s0, v17.s[2]")
VECF(t_cov_fmlas3, "fmla s2, s0, v17.s[3]")
VECF(t_cov_fmlss0, "fmls s2, s0, v17.s[0]")
VECF(t_cov_fmlss1, "fmls s2, s0, v17.s[1]")
VECF(t_cov_fmlss2, "fmls s2, s0, v17.s[2]")
VECF(t_cov_fmlss3, "fmls s2, s0, v17.s[3]")
VECF(t_cov_fmuld0, "fmul d2, d0, v17.d[0]")
VECF(t_tail_aese, "aese v2.16b, v0.16b")
VECF(t_tail_aese_alias, "aese v2.16b, v2.16b")
VECF(t_tail_aesd, "aesd v2.16b, v0.16b")
VECF(t_tail_aesd_alias, "aesd v2.16b, v2.16b")
VECF(t_tail_aesmc, "aesmc v2.16b, v0.16b")
VECF(t_tail_aesmc_alias, "aesmc v2.16b, v2.16b")
VECF(t_tail_aesimc, "aesimc v2.16b, v0.16b")
VECF(t_tail_aesimc_alias, "aesimc v2.16b, v2.16b")
VECF(t_wide_fadd, "fadd v2.2d, v0.2d, v1.2d")
VECF(t_wide_fsub, "fsub v2.2d, v0.2d, v1.2d")
VECF(t_wide_fmul, "fmul v2.2d, v0.2d, v1.2d")
VECF(t_wide_fdiv, "fdiv v2.2d, v0.2d, v1.2d")
VECF(t_wide_sqrtd, "fsqrt v2.2d, v0.2d")
VECF(t_wide_sqrts, "fsqrt v2.4s, v0.4s")
VECF(t_wide_sqrt2s, "fsqrt v2.2s, v0.2s")
VECF(t_cov_fmuld1, "fmul d2, d0, v17.d[1]")

VECF(t_fcmeqz2s, "fcmeq v2.2s, v0.2s, #0.0")
VECF(t_fcmeqz4s, "fcmeq v2.4s, v0.4s, #0.0")
VECF(t_fcmgez2s, "fcmge v2.2s, v0.2s, #0.0")
VECF(t_fcmgez4s, "fcmge v2.4s, v0.4s, #0.0")
VECF(t_fcmgtz2s, "fcmgt v2.2s, v0.2s, #0.0")
VECF(t_fcmgtz4s, "fcmgt v2.4s, v0.4s, #0.0")
VECF(t_fcmlez2s, "fcmle v2.2s, v0.2s, #0.0")
VECF(t_fcmlez4s, "fcmle v2.4s, v0.4s, #0.0")
VECF(t_fcmltz2s, "fcmlt v2.2s, v0.2s, #0.0")
VECF(t_fcmltz4s, "fcmlt v2.4s, v0.4s, #0.0")
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
LDST(t_ld2_8b_n, "ld2 {v2.8b, v3.8b}, [x0]")
LDST(t_ld2_8b_i, "ld2 {v2.8b, v3.8b}, [x0], #16")
LDST(t_ld2_8b_r, "ld2 {v2.8b, v3.8b}, [x0], x1")
LDST(t_ld2_16b_n, "ld2 {v2.16b, v3.16b}, [x0]")
LDST(t_ld2_16b_i, "ld2 {v2.16b, v3.16b}, [x0], #32")
LDST(t_ld2_16b_r, "ld2 {v2.16b, v3.16b}, [x0], x1")
LDST(t_ld2_4h_n, "ld2 {v2.4h, v3.4h}, [x0]")
LDST(t_ld2_4h_i, "ld2 {v2.4h, v3.4h}, [x0], #16")
LDST(t_ld2_4h_r, "ld2 {v2.4h, v3.4h}, [x0], x1")
LDST(t_ld2_8h_n, "ld2 {v2.8h, v3.8h}, [x0]")
LDST(t_ld2_8h_i, "ld2 {v2.8h, v3.8h}, [x0], #32")
LDST(t_ld2_8h_r, "ld2 {v2.8h, v3.8h}, [x0], x1")
LDST(t_ld2_2s_n, "ld2 {v2.2s, v3.2s}, [x0]")
LDST(t_ld2_2s_i, "ld2 {v2.2s, v3.2s}, [x0], #16")
LDST(t_ld2_2s_r, "ld2 {v2.2s, v3.2s}, [x0], x1")
LDST(t_ld2_4s_n, "ld2 {v2.4s, v3.4s}, [x0]")
LDST(t_ld2_4s_i, "ld2 {v2.4s, v3.4s}, [x0], #32")
LDST(t_ld2_4s_r, "ld2 {v2.4s, v3.4s}, [x0], x1")
LDST(t_ld2_2d_n, "ld2 {v2.2d, v3.2d}, [x0]")
LDST(t_ld2_2d_i, "ld2 {v2.2d, v3.2d}, [x0], #32")
LDST(t_ld2_2d_r, "ld2 {v2.2d, v3.2d}, [x0], x1")
LDST(t_ld3_8b_n, "ld3 {v2.8b, v3.8b, v4.8b}, [x0]")
LDST(t_ld3_8b_i, "ld3 {v2.8b, v3.8b, v4.8b}, [x0], #24")
LDST(t_ld3_8b_r, "ld3 {v2.8b, v3.8b, v4.8b}, [x0], x1")
LDST(t_ld3_16b_n, "ld3 {v2.16b, v3.16b, v4.16b}, [x0]")
LDST(t_ld3_16b_i, "ld3 {v2.16b, v3.16b, v4.16b}, [x0], #48")
LDST(t_ld3_16b_r, "ld3 {v2.16b, v3.16b, v4.16b}, [x0], x1")
LDST(t_ld3_4h_n, "ld3 {v2.4h, v3.4h, v4.4h}, [x0]")
LDST(t_ld3_4h_i, "ld3 {v2.4h, v3.4h, v4.4h}, [x0], #24")
LDST(t_ld3_4h_r, "ld3 {v2.4h, v3.4h, v4.4h}, [x0], x1")
LDST(t_ld3_8h_n, "ld3 {v2.8h, v3.8h, v4.8h}, [x0]")
LDST(t_ld3_8h_i, "ld3 {v2.8h, v3.8h, v4.8h}, [x0], #48")
LDST(t_ld3_8h_r, "ld3 {v2.8h, v3.8h, v4.8h}, [x0], x1")
LDST(t_ld3_2s_n, "ld3 {v2.2s, v3.2s, v4.2s}, [x0]")
LDST(t_ld3_2s_i, "ld3 {v2.2s, v3.2s, v4.2s}, [x0], #24")
LDST(t_ld3_2s_r, "ld3 {v2.2s, v3.2s, v4.2s}, [x0], x1")
LDST(t_ld3_4s_n, "ld3 {v2.4s, v3.4s, v4.4s}, [x0]")
LDST(t_ld3_4s_i, "ld3 {v2.4s, v3.4s, v4.4s}, [x0], #48")
LDST(t_ld3_4s_r, "ld3 {v2.4s, v3.4s, v4.4s}, [x0], x1")
LDST(t_ld3_2d_n, "ld3 {v2.2d, v3.2d, v4.2d}, [x0]")
LDST(t_ld3_2d_i, "ld3 {v2.2d, v3.2d, v4.2d}, [x0], #48")
LDST(t_ld3_2d_r, "ld3 {v2.2d, v3.2d, v4.2d}, [x0], x1")
LDST(t_ld4_8b_n, "ld4 {v2.8b, v3.8b, v4.8b, v5.8b}, [x0]")
LDST(t_ld4_8b_i, "ld4 {v2.8b, v3.8b, v4.8b, v5.8b}, [x0], #32")
LDST(t_ld4_8b_r, "ld4 {v2.8b, v3.8b, v4.8b, v5.8b}, [x0], x1")
LDST(t_ld4_16b_n, "ld4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0]")
LDST(t_ld4_16b_i, "ld4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0], #64")
LDST(t_ld4_16b_r, "ld4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0], x1")
LDST(t_ld4_4h_n, "ld4 {v2.4h, v3.4h, v4.4h, v5.4h}, [x0]")
LDST(t_ld4_4h_i, "ld4 {v2.4h, v3.4h, v4.4h, v5.4h}, [x0], #32")
LDST(t_ld4_4h_r, "ld4 {v2.4h, v3.4h, v4.4h, v5.4h}, [x0], x1")
LDST(t_ld4_8h_n, "ld4 {v2.8h, v3.8h, v4.8h, v5.8h}, [x0]")
LDST(t_ld4_8h_i, "ld4 {v2.8h, v3.8h, v4.8h, v5.8h}, [x0], #64")
LDST(t_ld4_8h_r, "ld4 {v2.8h, v3.8h, v4.8h, v5.8h}, [x0], x1")
LDST(t_ld4_2s_n, "ld4 {v2.2s, v3.2s, v4.2s, v5.2s}, [x0]")
LDST(t_ld4_2s_i, "ld4 {v2.2s, v3.2s, v4.2s, v5.2s}, [x0], #32")
LDST(t_ld4_2s_r, "ld4 {v2.2s, v3.2s, v4.2s, v5.2s}, [x0], x1")
LDST(t_ld4_4s_n, "ld4 {v2.4s, v3.4s, v4.4s, v5.4s}, [x0]")
LDST(t_ld4_4s_i, "ld4 {v2.4s, v3.4s, v4.4s, v5.4s}, [x0], #64")
LDST(t_ld4_4s_r, "ld4 {v2.4s, v3.4s, v4.4s, v5.4s}, [x0], x1")
LDST(t_ld4_2d_n, "ld4 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0]")
LDST(t_ld4_2d_i, "ld4 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0], #64")
LDST(t_ld4_2d_r, "ld4 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0], x1")
LDST(t_st2_8b_n, "st2 {v2.8b, v3.8b}, [x0]")
LDST(t_st2_8b_i, "st2 {v2.8b, v3.8b}, [x0], #16")
LDST(t_st2_8b_r, "st2 {v2.8b, v3.8b}, [x0], x1")
LDST(t_st2_16b_n, "st2 {v2.16b, v3.16b}, [x0]")
LDST(t_st2_16b_i, "st2 {v2.16b, v3.16b}, [x0], #32")
LDST(t_st2_16b_r, "st2 {v2.16b, v3.16b}, [x0], x1")
LDST(t_st2_4h_n, "st2 {v2.4h, v3.4h}, [x0]")
LDST(t_st2_4h_i, "st2 {v2.4h, v3.4h}, [x0], #16")
LDST(t_st2_4h_r, "st2 {v2.4h, v3.4h}, [x0], x1")
LDST(t_st2_8h_n, "st2 {v2.8h, v3.8h}, [x0]")
LDST(t_st2_8h_i, "st2 {v2.8h, v3.8h}, [x0], #32")
LDST(t_st2_8h_r, "st2 {v2.8h, v3.8h}, [x0], x1")
LDST(t_st2_2s_n, "st2 {v2.2s, v3.2s}, [x0]")
LDST(t_st2_2s_i, "st2 {v2.2s, v3.2s}, [x0], #16")
LDST(t_st2_2s_r, "st2 {v2.2s, v3.2s}, [x0], x1")
LDST(t_st2_4s_n, "st2 {v2.4s, v3.4s}, [x0]")
LDST(t_st2_4s_i, "st2 {v2.4s, v3.4s}, [x0], #32")
LDST(t_st2_4s_r, "st2 {v2.4s, v3.4s}, [x0], x1")
LDST(t_st2_2d_n, "st2 {v2.2d, v3.2d}, [x0]")
LDST(t_st2_2d_i, "st2 {v2.2d, v3.2d}, [x0], #32")
LDST(t_st2_2d_r, "st2 {v2.2d, v3.2d}, [x0], x1")
LDST(t_st3_8b_n, "st3 {v2.8b, v3.8b, v4.8b}, [x0]")
LDST(t_st3_8b_i, "st3 {v2.8b, v3.8b, v4.8b}, [x0], #24")
LDST(t_st3_8b_r, "st3 {v2.8b, v3.8b, v4.8b}, [x0], x1")
LDST(t_st3_16b_n, "st3 {v2.16b, v3.16b, v4.16b}, [x0]")
LDST(t_st3_16b_i, "st3 {v2.16b, v3.16b, v4.16b}, [x0], #48")
LDST(t_st3_16b_r, "st3 {v2.16b, v3.16b, v4.16b}, [x0], x1")
LDST(t_st3_4h_n, "st3 {v2.4h, v3.4h, v4.4h}, [x0]")
LDST(t_st3_4h_i, "st3 {v2.4h, v3.4h, v4.4h}, [x0], #24")
LDST(t_st3_4h_r, "st3 {v2.4h, v3.4h, v4.4h}, [x0], x1")
LDST(t_st3_8h_n, "st3 {v2.8h, v3.8h, v4.8h}, [x0]")
LDST(t_st3_8h_i, "st3 {v2.8h, v3.8h, v4.8h}, [x0], #48")
LDST(t_st3_8h_r, "st3 {v2.8h, v3.8h, v4.8h}, [x0], x1")
LDST(t_st3_2s_n, "st3 {v2.2s, v3.2s, v4.2s}, [x0]")
LDST(t_st3_2s_i, "st3 {v2.2s, v3.2s, v4.2s}, [x0], #24")
LDST(t_st3_2s_r, "st3 {v2.2s, v3.2s, v4.2s}, [x0], x1")
LDST(t_st3_4s_n, "st3 {v2.4s, v3.4s, v4.4s}, [x0]")
LDST(t_st3_4s_i, "st3 {v2.4s, v3.4s, v4.4s}, [x0], #48")
LDST(t_st3_4s_r, "st3 {v2.4s, v3.4s, v4.4s}, [x0], x1")
LDST(t_st3_2d_n, "st3 {v2.2d, v3.2d, v4.2d}, [x0]")
LDST(t_st3_2d_i, "st3 {v2.2d, v3.2d, v4.2d}, [x0], #48")
LDST(t_st3_2d_r, "st3 {v2.2d, v3.2d, v4.2d}, [x0], x1")
LDST(t_st4_8b_n, "st4 {v2.8b, v3.8b, v4.8b, v5.8b}, [x0]")
LDST(t_st4_8b_i, "st4 {v2.8b, v3.8b, v4.8b, v5.8b}, [x0], #32")
LDST(t_st4_8b_r, "st4 {v2.8b, v3.8b, v4.8b, v5.8b}, [x0], x1")
LDST(t_st4_16b_n, "st4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0]")
LDST(t_st4_16b_i, "st4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0], #64")
LDST(t_st4_16b_r, "st4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x0], x1")
LDST(t_st4_4h_n, "st4 {v2.4h, v3.4h, v4.4h, v5.4h}, [x0]")
LDST(t_st4_4h_i, "st4 {v2.4h, v3.4h, v4.4h, v5.4h}, [x0], #32")
LDST(t_st4_4h_r, "st4 {v2.4h, v3.4h, v4.4h, v5.4h}, [x0], x1")
LDST(t_st4_8h_n, "st4 {v2.8h, v3.8h, v4.8h, v5.8h}, [x0]")
LDST(t_st4_8h_i, "st4 {v2.8h, v3.8h, v4.8h, v5.8h}, [x0], #64")
LDST(t_st4_8h_r, "st4 {v2.8h, v3.8h, v4.8h, v5.8h}, [x0], x1")
LDST(t_st4_2s_n, "st4 {v2.2s, v3.2s, v4.2s, v5.2s}, [x0]")
LDST(t_st4_2s_i, "st4 {v2.2s, v3.2s, v4.2s, v5.2s}, [x0], #32")
LDST(t_st4_2s_r, "st4 {v2.2s, v3.2s, v4.2s, v5.2s}, [x0], x1")
LDST(t_st4_4s_n, "st4 {v2.4s, v3.4s, v4.4s, v5.4s}, [x0]")
LDST(t_st4_4s_i, "st4 {v2.4s, v3.4s, v4.4s, v5.4s}, [x0], #64")
LDST(t_st4_4s_r, "st4 {v2.4s, v3.4s, v4.4s, v5.4s}, [x0], x1")
LDST(t_st4_2d_n, "st4 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0]")
LDST(t_st4_2d_i, "st4 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0], #64")
LDST(t_st4_2d_r, "st4 {v2.2d, v3.2d, v4.2d, v5.2d}, [x0], x1")
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
static volatile uint64_t sg_seen_x2;
static void sg_fault(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    uintptr_t pc = GET_PC(uc);
    sg_seen_pc = pc - sg_pc;
    sg_seen_nzcv = SS(uc).__cpsr >> 28;
    sg_seen_x0 = SS(uc).__x[0];
    sg_seen_x2 = SS(uc).__x[2];
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
/* Interleaved structures fault after earlier elements have been accessed.
 * Native ARM may leave destination vectors UNKNOWN on an abort; skip cases
 * therefore check PC/NZCV/base, and repaired cases also check all results. */
#define SG_STRUCTURE(name, insn) \
NOINL static uint64_t name(uint64_t *p, uint64_t *fl) \
{ \
    uint64_t out[8]; \
    register uint64_t x0 __asm__("x0") = 3, x1 __asm__("x1") = 7; \
    register uint64_t *x2 __asm__("x2") = p; \
    register uint64_t x3 __asm__("x3") = 0, x4 __asm__("x4") = 0; \
    register volatile uint64_t *x5 __asm__("x5") = &sg_pc; \
    register uint64_t *x6 __asm__("x6") = out; \
    __asm__ volatile("movi v2.16b, #2\nmovi v3.16b, #3\nmovi v4.16b, #4\nmovi v5.16b, #5\n" \
                     "adr x9, 1f\nstr x9, [x5]\ncmp x0, x1\n1: " insn "\nmrs x4, nzcv\n" \
                     "stp q2, q3, [x6]\nstp q4, q5, [x6, #32]" \
                     : "+r"(x3), "+r"(x4), "+r"(x2) : "r"(x0), "r"(x1), "r"(x5), "r"(x6) \
                     : "cc", "memory", "x9", "v2", "v3", "v4", "v5"); \
    *fl = x4; \
    uint64_t h = mix(x3, (uint64_t)((uint8_t *)x2 - (uint8_t *)p)); \
    h = mix(h, sg_seen_x2 - (uint64_t)(uintptr_t)p); \
    if (sg_repair) for (int k = 0; k < 8; k++) h = mix(h, out[k]); \
    return h; \
}
SG_STRUCTURE(sg_ld2, "ld2 {v2.2d, v3.2d}, [x2], #32")
SG_STRUCTURE(sg_ld3, "ld3 {v2.8h, v3.8h, v4.8h}, [x2], x1")
SG_STRUCTURE(sg_ld4, "ld4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x2], #64")
SG_STRUCTURE(sg_st2, "st2 {v2.2d, v3.2d}, [x2], #32")
SG_STRUCTURE(sg_st3, "st3 {v2.8h, v3.8h, v4.8h}, [x2], x1")
SG_STRUCTURE(sg_st4, "st4 {v2.16b, v3.16b, v4.16b, v5.16b}, [x2], #64")

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
    munmap(sg_page, 16384);
    uint8_t *pages = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    sg_page = (uint64_t *)(pages + 16384);
    uint64_t (*probes[])(uint64_t *, uint64_t *) = {sg_ld2, sg_ld3, sg_ld4, sg_st2, sg_st3, sg_st4};
    for (unsigned kind = 0; kind < 6; kind++) for (unsigned repair = 0; repair < 2; repair++) {
        uint64_t h = 0;
        for (unsigned offset = 1; offset <= 24; offset++) {
            mprotect(sg_page, 16384, PROT_READ | PROT_WRITE);
            for (unsigned b = 0; b < 32768; b++) pages[b] = (uint8_t)(b * 13 + offset);
            mprotect(sg_page, 16384, PROT_NONE);
            sg_repair = repair; sg_flip = offset & 15;
            uint64_t fl, r = probes[kind]((uint64_t *)(pages + 16384 - offset), &fl);
            h = mix(mix(mix(mix(mix(h, r), fl), sg_seen_pc), sg_seen_nzcv), sg_count);
            if (repair) for (int b = -64; b < 64; b++) h = mix(h, pages[16384 + b]);
        }
        printf("sigstr-%u-%u %016llx\n", kind, repair, (unsigned long long)h);
    }
    munmap(pages, 32768);
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
VEC2(t_cov_ushl2s, "ushl v2.2s, v0.2s, v1.2s")
VEC2(t_cov_ushl4s, "ushl v2.4s, v0.4s, v1.4s")
VEC2(t_cov_ushl2d, "ushl v2.2d, v0.2d, v1.2d")
VEC2(t_cov_sshl2s, "sshl v2.2s, v0.2s, v1.2s")
VEC2(t_cov_sshl4s, "sshl v2.4s, v0.4s, v1.4s")
VEC2(t_cov_sshl2d, "sshl v2.2d, v0.2d, v1.2d")

#define R(n) run(#n, t_##n)
#define RA(n) R(n##b); R(n##h); R(n##w); R(n##x)
/* Integer tail, with FPSR.QC included in each checksum. */
VEC2(t_tail_saddl0_0, "msr fpsr, xzr\nsaddl v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_saddl0_1, "msr fpsr, xzr\nsaddl v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_saddl0_2, "msr fpsr, xzr\nsaddl v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_saddl1_0, "msr fpsr, xzr\nsaddl2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_saddl1_1, "msr fpsr, xzr\nsaddl2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_saddl1_2, "msr fpsr, xzr\nsaddl2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_uaddl0_0, "msr fpsr, xzr\nuaddl v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_uaddl0_1, "msr fpsr, xzr\nuaddl v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_uaddl0_2, "msr fpsr, xzr\nuaddl v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_uaddl1_0, "msr fpsr, xzr\nuaddl2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_uaddl1_1, "msr fpsr, xzr\nuaddl2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_uaddl1_2, "msr fpsr, xzr\nuaddl2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_saddw0_0, "msr fpsr, xzr\nsaddw v2.8h, v0.8h, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_saddw0_1, "msr fpsr, xzr\nsaddw v2.4s, v0.4s, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_saddw0_2, "msr fpsr, xzr\nsaddw v2.2d, v0.2d, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_saddw1_0, "msr fpsr, xzr\nsaddw2 v2.8h, v0.8h, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_saddw1_1, "msr fpsr, xzr\nsaddw2 v2.4s, v0.4s, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_saddw1_2, "msr fpsr, xzr\nsaddw2 v2.2d, v0.2d, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_uaddw0_0, "msr fpsr, xzr\nuaddw v2.8h, v0.8h, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_uaddw0_1, "msr fpsr, xzr\nuaddw v2.4s, v0.4s, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_uaddw0_2, "msr fpsr, xzr\nuaddw v2.2d, v0.2d, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_uaddw1_0, "msr fpsr, xzr\nuaddw2 v2.8h, v0.8h, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_uaddw1_1, "msr fpsr, xzr\nuaddw2 v2.4s, v0.4s, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_uaddw1_2, "msr fpsr, xzr\nuaddw2 v2.2d, v0.2d, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_ssubl0_0, "msr fpsr, xzr\nssubl v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_ssubl0_1, "msr fpsr, xzr\nssubl v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_ssubl0_2, "msr fpsr, xzr\nssubl v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_ssubl1_0, "msr fpsr, xzr\nssubl2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_ssubl1_1, "msr fpsr, xzr\nssubl2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_ssubl1_2, "msr fpsr, xzr\nssubl2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_usubl0_0, "msr fpsr, xzr\nusubl v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_usubl0_1, "msr fpsr, xzr\nusubl v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_usubl0_2, "msr fpsr, xzr\nusubl v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_usubl1_0, "msr fpsr, xzr\nusubl2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_usubl1_1, "msr fpsr, xzr\nusubl2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_usubl1_2, "msr fpsr, xzr\nusubl2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_ssubw0_0, "msr fpsr, xzr\nssubw v2.8h, v0.8h, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_ssubw0_1, "msr fpsr, xzr\nssubw v2.4s, v0.4s, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_ssubw0_2, "msr fpsr, xzr\nssubw v2.2d, v0.2d, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_ssubw1_0, "msr fpsr, xzr\nssubw2 v2.8h, v0.8h, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_ssubw1_1, "msr fpsr, xzr\nssubw2 v2.4s, v0.4s, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_ssubw1_2, "msr fpsr, xzr\nssubw2 v2.2d, v0.2d, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_usubw0_0, "msr fpsr, xzr\nusubw v2.8h, v0.8h, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_usubw0_1, "msr fpsr, xzr\nusubw v2.4s, v0.4s, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_usubw0_2, "msr fpsr, xzr\nusubw v2.2d, v0.2d, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_usubw1_0, "msr fpsr, xzr\nusubw2 v2.8h, v0.8h, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_usubw1_1, "msr fpsr, xzr\nusubw2 v2.4s, v0.4s, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_usubw1_2, "msr fpsr, xzr\nusubw2 v2.2d, v0.2d, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_smull0_0, "msr fpsr, xzr\nsmull v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_smull0_1, "msr fpsr, xzr\nsmull v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_smull0_2, "msr fpsr, xzr\nsmull v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_smull1_0, "msr fpsr, xzr\nsmull2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_smull1_1, "msr fpsr, xzr\nsmull2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_smull1_2, "msr fpsr, xzr\nsmull2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_umull0_0, "msr fpsr, xzr\numull v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_umull0_1, "msr fpsr, xzr\numull v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_umull0_2, "msr fpsr, xzr\numull v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_umull1_0, "msr fpsr, xzr\numull2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_umull1_1, "msr fpsr, xzr\numull2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_umull1_2, "msr fpsr, xzr\numull2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_smlal0_0, "msr fpsr, xzr\nsmlal v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_smlal0_1, "msr fpsr, xzr\nsmlal v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_smlal0_2, "msr fpsr, xzr\nsmlal v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_smlal1_0, "msr fpsr, xzr\nsmlal2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_smlal1_1, "msr fpsr, xzr\nsmlal2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_smlal1_2, "msr fpsr, xzr\nsmlal2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_umlal0_0, "msr fpsr, xzr\numlal v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_umlal0_1, "msr fpsr, xzr\numlal v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_umlal0_2, "msr fpsr, xzr\numlal v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_umlal1_0, "msr fpsr, xzr\numlal2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_umlal1_1, "msr fpsr, xzr\numlal2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_umlal1_2, "msr fpsr, xzr\numlal2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_smlsl0_0, "msr fpsr, xzr\nsmlsl v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_smlsl0_1, "msr fpsr, xzr\nsmlsl v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_smlsl0_2, "msr fpsr, xzr\nsmlsl v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_smlsl1_0, "msr fpsr, xzr\nsmlsl2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_smlsl1_1, "msr fpsr, xzr\nsmlsl2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_smlsl1_2, "msr fpsr, xzr\nsmlsl2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_umlsl0_0, "msr fpsr, xzr\numlsl v2.8h, v0.8b, v1.8b\nmrs x2, fpsr")
VEC2(t_tail_umlsl0_1, "msr fpsr, xzr\numlsl v2.4s, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_umlsl0_2, "msr fpsr, xzr\numlsl v2.2d, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_umlsl1_0, "msr fpsr, xzr\numlsl2 v2.8h, v0.16b, v1.16b\nmrs x2, fpsr")
VEC2(t_tail_umlsl1_1, "msr fpsr, xzr\numlsl2 v2.4s, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_umlsl1_2, "msr fpsr, xzr\numlsl2 v2.2d, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_sqxtn0_0, "msr fpsr, xzr\nsqxtn v2.8b, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_sqxtn0_1, "msr fpsr, xzr\nsqxtn v2.4h, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_sqxtn0_2, "msr fpsr, xzr\nsqxtn v2.2s, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_sqxtn1_0, "msr fpsr, xzr\nsqxtn2 v2.16b, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_sqxtn1_1, "msr fpsr, xzr\nsqxtn2 v2.8h, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_sqxtn1_2, "msr fpsr, xzr\nsqxtn2 v2.4s, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_uqxtn0_0, "msr fpsr, xzr\nuqxtn v2.8b, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_uqxtn0_1, "msr fpsr, xzr\nuqxtn v2.4h, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_uqxtn0_2, "msr fpsr, xzr\nuqxtn v2.2s, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_uqxtn1_0, "msr fpsr, xzr\nuqxtn2 v2.16b, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_uqxtn1_1, "msr fpsr, xzr\nuqxtn2 v2.8h, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_uqxtn1_2, "msr fpsr, xzr\nuqxtn2 v2.4s, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_sqxtun0_0, "msr fpsr, xzr\nsqxtun v2.8b, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_sqxtun0_1, "msr fpsr, xzr\nsqxtun v2.4h, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_sqxtun0_2, "msr fpsr, xzr\nsqxtun v2.2s, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_sqxtun1_0, "msr fpsr, xzr\nsqxtun2 v2.16b, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_sqxtun1_1, "msr fpsr, xzr\nsqxtun2 v2.8h, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_sqxtun1_2, "msr fpsr, xzr\nsqxtun2 v2.4s, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_shll0_0, "msr fpsr, xzr\nshll v2.8h, v0.8b, #8\nmrs x2, fpsr")
VEC2(t_tail_shll0_1, "msr fpsr, xzr\nshll v2.4s, v0.4h, #16\nmrs x2, fpsr")
VEC2(t_tail_shll0_2, "msr fpsr, xzr\nshll v2.2d, v0.2s, #32\nmrs x2, fpsr")
VEC2(t_tail_shll1_0, "msr fpsr, xzr\nshll2 v2.8h, v0.16b, #8\nmrs x2, fpsr")
VEC2(t_tail_shll1_1, "msr fpsr, xzr\nshll2 v2.4s, v0.8h, #16\nmrs x2, fpsr")
VEC2(t_tail_shll1_2, "msr fpsr, xzr\nshll2 v2.2d, v0.4s, #32\nmrs x2, fpsr")
VEC2(t_tail_sri0_0_1, "msr fpsr, xzr\nsri v2.8b, v0.8b, #1\nmrs x2, fpsr")
VEC2(t_tail_sri0_0_4, "msr fpsr, xzr\nsri v2.8b, v0.8b, #4\nmrs x2, fpsr")
VEC2(t_tail_sri0_0_8, "msr fpsr, xzr\nsri v2.8b, v0.8b, #8\nmrs x2, fpsr")
VEC2(t_tail_sli0_0_0, "msr fpsr, xzr\nsli v2.8b, v0.8b, #0\nmrs x2, fpsr")
VEC2(t_tail_sli0_0_4, "msr fpsr, xzr\nsli v2.8b, v0.8b, #4\nmrs x2, fpsr")
VEC2(t_tail_sli0_0_7, "msr fpsr, xzr\nsli v2.8b, v0.8b, #7\nmrs x2, fpsr")
VEC2(t_tail_rev160_0, "msr fpsr, xzr\nrev16 v2.8b, v0.8b\nmrs x2, fpsr")
VEC2(t_tail_rev320_0, "msr fpsr, xzr\nrev32 v2.8b, v0.8b\nmrs x2, fpsr")
VEC2(t_tail_rev640_0, "msr fpsr, xzr\nrev64 v2.8b, v0.8b\nmrs x2, fpsr")
VEC2(t_tail_sri0_1_1, "msr fpsr, xzr\nsri v2.4h, v0.4h, #1\nmrs x2, fpsr")
VEC2(t_tail_sri0_1_8, "msr fpsr, xzr\nsri v2.4h, v0.4h, #8\nmrs x2, fpsr")
VEC2(t_tail_sri0_1_16, "msr fpsr, xzr\nsri v2.4h, v0.4h, #16\nmrs x2, fpsr")
VEC2(t_tail_sli0_1_0, "msr fpsr, xzr\nsli v2.4h, v0.4h, #0\nmrs x2, fpsr")
VEC2(t_tail_sli0_1_8, "msr fpsr, xzr\nsli v2.4h, v0.4h, #8\nmrs x2, fpsr")
VEC2(t_tail_sli0_1_15, "msr fpsr, xzr\nsli v2.4h, v0.4h, #15\nmrs x2, fpsr")
VEC2(t_tail_rev320_1, "msr fpsr, xzr\nrev32 v2.4h, v0.4h\nmrs x2, fpsr")
VEC2(t_tail_rev640_1, "msr fpsr, xzr\nrev64 v2.4h, v0.4h\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_0, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[0]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_1, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[1]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_2, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[2]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_3, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[3]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_4, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[4]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_5, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[5]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_6, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[6]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_1_7, "msr fpsr, xzr\nsqdmulh v2.4h, v0.4h, v1.h[7]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.4h\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_0, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[0]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_1, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[1]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_2, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[2]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_3, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[3]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_4, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[4]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_5, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[5]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_6, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[6]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_1_7, "msr fpsr, xzr\nsqrdmulh v2.4h, v0.4h, v1.h[7]\nmrs x2, fpsr")
VEC2(t_tail_sri0_2_1, "msr fpsr, xzr\nsri v2.2s, v0.2s, #1\nmrs x2, fpsr")
VEC2(t_tail_sri0_2_16, "msr fpsr, xzr\nsri v2.2s, v0.2s, #16\nmrs x2, fpsr")
VEC2(t_tail_sri0_2_32, "msr fpsr, xzr\nsri v2.2s, v0.2s, #32\nmrs x2, fpsr")
VEC2(t_tail_sli0_2_0, "msr fpsr, xzr\nsli v2.2s, v0.2s, #0\nmrs x2, fpsr")
VEC2(t_tail_sli0_2_16, "msr fpsr, xzr\nsli v2.2s, v0.2s, #16\nmrs x2, fpsr")
VEC2(t_tail_sli0_2_31, "msr fpsr, xzr\nsli v2.2s, v0.2s, #31\nmrs x2, fpsr")
VEC2(t_tail_rev640_2, "msr fpsr, xzr\nrev64 v2.2s, v0.2s\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_2, "msr fpsr, xzr\nsqdmulh v2.2s, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_2_0, "msr fpsr, xzr\nsqdmulh v2.2s, v0.2s, v1.s[0]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_2_1, "msr fpsr, xzr\nsqdmulh v2.2s, v0.2s, v1.s[1]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_2_2, "msr fpsr, xzr\nsqdmulh v2.2s, v0.2s, v1.s[2]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh0_2_3, "msr fpsr, xzr\nsqdmulh v2.2s, v0.2s, v1.s[3]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_2, "msr fpsr, xzr\nsqrdmulh v2.2s, v0.2s, v1.2s\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_2_0, "msr fpsr, xzr\nsqrdmulh v2.2s, v0.2s, v1.s[0]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_2_1, "msr fpsr, xzr\nsqrdmulh v2.2s, v0.2s, v1.s[1]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_2_2, "msr fpsr, xzr\nsqrdmulh v2.2s, v0.2s, v1.s[2]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh0_2_3, "msr fpsr, xzr\nsqrdmulh v2.2s, v0.2s, v1.s[3]\nmrs x2, fpsr")
VEC2(t_tail_sri1_0_1, "msr fpsr, xzr\nsri v2.16b, v0.16b, #1\nmrs x2, fpsr")
VEC2(t_tail_sri1_0_4, "msr fpsr, xzr\nsri v2.16b, v0.16b, #4\nmrs x2, fpsr")
VEC2(t_tail_sri1_0_8, "msr fpsr, xzr\nsri v2.16b, v0.16b, #8\nmrs x2, fpsr")
VEC2(t_tail_sli1_0_0, "msr fpsr, xzr\nsli v2.16b, v0.16b, #0\nmrs x2, fpsr")
VEC2(t_tail_sli1_0_4, "msr fpsr, xzr\nsli v2.16b, v0.16b, #4\nmrs x2, fpsr")
VEC2(t_tail_sli1_0_7, "msr fpsr, xzr\nsli v2.16b, v0.16b, #7\nmrs x2, fpsr")
VEC2(t_tail_rev161_0, "msr fpsr, xzr\nrev16 v2.16b, v0.16b\nmrs x2, fpsr")
VEC2(t_tail_rev321_0, "msr fpsr, xzr\nrev32 v2.16b, v0.16b\nmrs x2, fpsr")
VEC2(t_tail_rev641_0, "msr fpsr, xzr\nrev64 v2.16b, v0.16b\nmrs x2, fpsr")
VEC2(t_tail_sri1_1_1, "msr fpsr, xzr\nsri v2.8h, v0.8h, #1\nmrs x2, fpsr")
VEC2(t_tail_sri1_1_8, "msr fpsr, xzr\nsri v2.8h, v0.8h, #8\nmrs x2, fpsr")
VEC2(t_tail_sri1_1_16, "msr fpsr, xzr\nsri v2.8h, v0.8h, #16\nmrs x2, fpsr")
VEC2(t_tail_sli1_1_0, "msr fpsr, xzr\nsli v2.8h, v0.8h, #0\nmrs x2, fpsr")
VEC2(t_tail_sli1_1_8, "msr fpsr, xzr\nsli v2.8h, v0.8h, #8\nmrs x2, fpsr")
VEC2(t_tail_sli1_1_15, "msr fpsr, xzr\nsli v2.8h, v0.8h, #15\nmrs x2, fpsr")
VEC2(t_tail_rev321_1, "msr fpsr, xzr\nrev32 v2.8h, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_rev641_1, "msr fpsr, xzr\nrev64 v2.8h, v0.8h\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_0, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[0]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_1, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[1]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_2, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[2]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_3, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[3]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_4, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[4]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_5, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[5]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_6, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[6]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_1_7, "msr fpsr, xzr\nsqdmulh v2.8h, v0.8h, v1.h[7]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.8h\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_0, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[0]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_1, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[1]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_2, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[2]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_3, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[3]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_4, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[4]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_5, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[5]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_6, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[6]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_1_7, "msr fpsr, xzr\nsqrdmulh v2.8h, v0.8h, v1.h[7]\nmrs x2, fpsr")
VEC2(t_tail_sri1_2_1, "msr fpsr, xzr\nsri v2.4s, v0.4s, #1\nmrs x2, fpsr")
VEC2(t_tail_sri1_2_16, "msr fpsr, xzr\nsri v2.4s, v0.4s, #16\nmrs x2, fpsr")
VEC2(t_tail_sri1_2_32, "msr fpsr, xzr\nsri v2.4s, v0.4s, #32\nmrs x2, fpsr")
VEC2(t_tail_sli1_2_0, "msr fpsr, xzr\nsli v2.4s, v0.4s, #0\nmrs x2, fpsr")
VEC2(t_tail_sli1_2_16, "msr fpsr, xzr\nsli v2.4s, v0.4s, #16\nmrs x2, fpsr")
VEC2(t_tail_sli1_2_31, "msr fpsr, xzr\nsli v2.4s, v0.4s, #31\nmrs x2, fpsr")
VEC2(t_tail_rev641_2, "msr fpsr, xzr\nrev64 v2.4s, v0.4s\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_2, "msr fpsr, xzr\nsqdmulh v2.4s, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_2_0, "msr fpsr, xzr\nsqdmulh v2.4s, v0.4s, v1.s[0]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_2_1, "msr fpsr, xzr\nsqdmulh v2.4s, v0.4s, v1.s[1]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_2_2, "msr fpsr, xzr\nsqdmulh v2.4s, v0.4s, v1.s[2]\nmrs x2, fpsr")
VEC2(t_tail_sqdmulh1_2_3, "msr fpsr, xzr\nsqdmulh v2.4s, v0.4s, v1.s[3]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_2, "msr fpsr, xzr\nsqrdmulh v2.4s, v0.4s, v1.4s\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_2_0, "msr fpsr, xzr\nsqrdmulh v2.4s, v0.4s, v1.s[0]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_2_1, "msr fpsr, xzr\nsqrdmulh v2.4s, v0.4s, v1.s[1]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_2_2, "msr fpsr, xzr\nsqrdmulh v2.4s, v0.4s, v1.s[2]\nmrs x2, fpsr")
VEC2(t_tail_sqrdmulh1_2_3, "msr fpsr, xzr\nsqrdmulh v2.4s, v0.4s, v1.s[3]\nmrs x2, fpsr")
VEC2(t_tail_sri1_3_1, "msr fpsr, xzr\nsri v2.2d, v0.2d, #1\nmrs x2, fpsr")
VEC2(t_tail_sri1_3_32, "msr fpsr, xzr\nsri v2.2d, v0.2d, #32\nmrs x2, fpsr")
VEC2(t_tail_sri1_3_64, "msr fpsr, xzr\nsri v2.2d, v0.2d, #64\nmrs x2, fpsr")
VEC2(t_tail_sli1_3_0, "msr fpsr, xzr\nsli v2.2d, v0.2d, #0\nmrs x2, fpsr")
VEC2(t_tail_sli1_3_32, "msr fpsr, xzr\nsli v2.2d, v0.2d, #32\nmrs x2, fpsr")
VEC2(t_tail_sli1_3_63, "msr fpsr, xzr\nsli v2.2d, v0.2d, #63\nmrs x2, fpsr")
VEC2(t_tail_addpd, "msr fpsr, xzr\naddp d2, v0.2d\nmrs x2, fpsr")
VEC2(t_tail_ldrb_imm, "msr fpsr, xzr\nldr b2, [x4, #2]\nmrs x2, fpsr")
VEC2(t_tail_ldrb_post, "msr fpsr, xzr\nldr b2, [x4], #2\nmrs x2, fpsr")
VEC2(t_tail_ldrb_pre, "msr fpsr, xzr\nldr b2, [x4, #2]!\nmrs x2, fpsr")
VEC2(t_tail_ldrb_reg, "msr fpsr, xzr\nldr b2, [x4, x2]\nmrs x2, fpsr")
VEC2(t_tail_strb, "msr fpsr, xzr\nstr b0, [x4, #32]\nldr q2, [x4, #32]\nmrs x2, fpsr")
VEC2(t_tail_ldrh_imm, "msr fpsr, xzr\nldr h2, [x4, #2]\nmrs x2, fpsr")
VEC2(t_tail_ldrh_post, "msr fpsr, xzr\nldr h2, [x4], #2\nmrs x2, fpsr")
VEC2(t_tail_ldrh_pre, "msr fpsr, xzr\nldr h2, [x4, #2]!\nmrs x2, fpsr")
VEC2(t_tail_ldrh_reg, "msr fpsr, xzr\nldr h2, [x4, x2]\nmrs x2, fpsr")
VEC2(t_tail_strh, "msr fpsr, xzr\nstr h0, [x4, #32]\nldr q2, [x4, #32]\nmrs x2, fpsr")

/* Conditional FP compares, including false conditions with NaN operands. */
VECF(t_tail_fccmp_s_eq, "cmp x6, #0\nfccmp s0, s1, #5, eq")
VECF(t_tail_fccmp_s_ne, "cmp x6, #0\nfccmp s0, s1, #5, ne")
VECF(t_tail_fccmp_s_cs, "cmp x6, #0\nfccmp s0, s1, #5, cs")
VECF(t_tail_fccmp_s_cc, "cmp x6, #0\nfccmp s0, s1, #5, cc")
VECF(t_tail_fccmp_s_mi, "cmp x6, #0\nfccmp s0, s1, #5, mi")
VECF(t_tail_fccmp_s_pl, "cmp x6, #0\nfccmp s0, s1, #5, pl")
VECF(t_tail_fccmp_s_vs, "cmp x6, #0\nfccmp s0, s1, #5, vs")
VECF(t_tail_fccmp_s_vc, "cmp x6, #0\nfccmp s0, s1, #5, vc")
VECF(t_tail_fccmp_s_hi, "cmp x6, #0\nfccmp s0, s1, #5, hi")
VECF(t_tail_fccmp_s_ls, "cmp x6, #0\nfccmp s0, s1, #5, ls")
VECF(t_tail_fccmp_s_ge, "cmp x6, #0\nfccmp s0, s1, #5, ge")
VECF(t_tail_fccmp_s_lt, "cmp x6, #0\nfccmp s0, s1, #5, lt")
VECF(t_tail_fccmp_s_gt, "cmp x6, #0\nfccmp s0, s1, #5, gt")
VECF(t_tail_fccmp_s_le, "cmp x6, #0\nfccmp s0, s1, #5, le")
VECF(t_tail_fccmp_s_al, "cmp x6, #0\nfccmp s0, s1, #5, al")
VECF(t_tail_fccmp_s_nv, "cmp x6, #0\nfccmp s0, s1, #5, nv")
VECF(t_tail_fccmp_d_eq, "cmp x6, #0\nfccmp d0, d1, #5, eq")
VECF(t_tail_fccmp_d_ne, "cmp x6, #0\nfccmp d0, d1, #5, ne")
VECF(t_tail_fccmp_d_cs, "cmp x6, #0\nfccmp d0, d1, #5, cs")
VECF(t_tail_fccmp_d_cc, "cmp x6, #0\nfccmp d0, d1, #5, cc")
VECF(t_tail_fccmp_d_mi, "cmp x6, #0\nfccmp d0, d1, #5, mi")
VECF(t_tail_fccmp_d_pl, "cmp x6, #0\nfccmp d0, d1, #5, pl")
VECF(t_tail_fccmp_d_vs, "cmp x6, #0\nfccmp d0, d1, #5, vs")
VECF(t_tail_fccmp_d_vc, "cmp x6, #0\nfccmp d0, d1, #5, vc")
VECF(t_tail_fccmp_d_hi, "cmp x6, #0\nfccmp d0, d1, #5, hi")
VECF(t_tail_fccmp_d_ls, "cmp x6, #0\nfccmp d0, d1, #5, ls")
VECF(t_tail_fccmp_d_ge, "cmp x6, #0\nfccmp d0, d1, #5, ge")
VECF(t_tail_fccmp_d_lt, "cmp x6, #0\nfccmp d0, d1, #5, lt")
VECF(t_tail_fccmp_d_gt, "cmp x6, #0\nfccmp d0, d1, #5, gt")
VECF(t_tail_fccmp_d_le, "cmp x6, #0\nfccmp d0, d1, #5, le")
VECF(t_tail_fccmp_d_al, "cmp x6, #0\nfccmp d0, d1, #5, al")
VECF(t_tail_fccmp_d_nv, "cmp x6, #0\nfccmp d0, d1, #5, nv")
VECF(t_tail_fccmpe_s_eq, "cmp x6, #0\nfccmpe s0, s1, #5, eq")
VECF(t_tail_fccmpe_s_ne, "cmp x6, #0\nfccmpe s0, s1, #5, ne")
VECF(t_tail_fccmpe_s_cs, "cmp x6, #0\nfccmpe s0, s1, #5, cs")
VECF(t_tail_fccmpe_s_cc, "cmp x6, #0\nfccmpe s0, s1, #5, cc")
VECF(t_tail_fccmpe_s_mi, "cmp x6, #0\nfccmpe s0, s1, #5, mi")
VECF(t_tail_fccmpe_s_pl, "cmp x6, #0\nfccmpe s0, s1, #5, pl")
VECF(t_tail_fccmpe_s_vs, "cmp x6, #0\nfccmpe s0, s1, #5, vs")
VECF(t_tail_fccmpe_s_vc, "cmp x6, #0\nfccmpe s0, s1, #5, vc")
VECF(t_tail_fccmpe_s_hi, "cmp x6, #0\nfccmpe s0, s1, #5, hi")
VECF(t_tail_fccmpe_s_ls, "cmp x6, #0\nfccmpe s0, s1, #5, ls")
VECF(t_tail_fccmpe_s_ge, "cmp x6, #0\nfccmpe s0, s1, #5, ge")
VECF(t_tail_fccmpe_s_lt, "cmp x6, #0\nfccmpe s0, s1, #5, lt")
VECF(t_tail_fccmpe_s_gt, "cmp x6, #0\nfccmpe s0, s1, #5, gt")
VECF(t_tail_fccmpe_s_le, "cmp x6, #0\nfccmpe s0, s1, #5, le")
VECF(t_tail_fccmpe_s_al, "cmp x6, #0\nfccmpe s0, s1, #5, al")
VECF(t_tail_fccmpe_s_nv, "cmp x6, #0\nfccmpe s0, s1, #5, nv")
VECF(t_tail_fccmpe_d_eq, "cmp x6, #0\nfccmpe d0, d1, #5, eq")
VECF(t_tail_fccmpe_d_ne, "cmp x6, #0\nfccmpe d0, d1, #5, ne")
VECF(t_tail_fccmpe_d_cs, "cmp x6, #0\nfccmpe d0, d1, #5, cs")
VECF(t_tail_fccmpe_d_cc, "cmp x6, #0\nfccmpe d0, d1, #5, cc")
VECF(t_tail_fccmpe_d_mi, "cmp x6, #0\nfccmpe d0, d1, #5, mi")
VECF(t_tail_fccmpe_d_pl, "cmp x6, #0\nfccmpe d0, d1, #5, pl")
VECF(t_tail_fccmpe_d_vs, "cmp x6, #0\nfccmpe d0, d1, #5, vs")
VECF(t_tail_fccmpe_d_vc, "cmp x6, #0\nfccmpe d0, d1, #5, vc")
VECF(t_tail_fccmpe_d_hi, "cmp x6, #0\nfccmpe d0, d1, #5, hi")
VECF(t_tail_fccmpe_d_ls, "cmp x6, #0\nfccmpe d0, d1, #5, ls")
VECF(t_tail_fccmpe_d_ge, "cmp x6, #0\nfccmpe d0, d1, #5, ge")
VECF(t_tail_fccmpe_d_lt, "cmp x6, #0\nfccmpe d0, d1, #5, lt")
VECF(t_tail_fccmpe_d_gt, "cmp x6, #0\nfccmpe d0, d1, #5, gt")
VECF(t_tail_fccmpe_d_le, "cmp x6, #0\nfccmpe d0, d1, #5, le")
VECF(t_tail_fccmpe_d_al, "cmp x6, #0\nfccmpe d0, d1, #5, al")
VECF(t_tail_fccmpe_d_nv, "cmp x6, #0\nfccmpe d0, d1, #5, nv")
VEC2(t_tail_fpcr, "and x2, x0, #0x3c00000\nmsr fpcr, x2\nmrs x2, fpcr\nmsr fpcr, xzr")
VEC2(t_tail_fpsr, "mov x2, #0x9f\nmovk x2, #0x800, lsl #16\nand x2, x0, x2\nmsr fpsr, x2\nmrs x2, fpsr\nmsr fpsr, xzr")

VECF(t_tail_frecpe_2s, "frecpe v2.2s, v0.2s")
VECF(t_tail_frecpe_4s, "frecpe v2.4s, v0.4s")
VECF(t_tail_frecpe_2d, "frecpe v2.2d, v0.2d")
VECF(t_tail_frecpe_s, "frecpe s2, s0")
VECF(t_tail_frecpe_d, "frecpe d2, d0")
VECF(t_tail_frsqrte_2s, "frsqrte v2.2s, v0.2s")
VECF(t_tail_frsqrte_4s, "frsqrte v2.4s, v0.4s")
VECF(t_tail_frsqrte_2d, "frsqrte v2.2d, v0.2d")
VECF(t_tail_frsqrte_s, "frsqrte s2, s0")
VECF(t_tail_frsqrte_d, "frsqrte d2, d0")

/* Every estimate table index, exponent parity, sign and exceptional exponent,
 * with both ends of each fraction bucket and all FPCR controls. */
static void run_estimate(const char *name, fnf f, int dbl)
{
    uint64_t hash = 14695981039346656037ull;
    unsigned mbits = dbl ? 52 : 23, maxexp = dbl ? 2047 : 255;
    for (unsigned c = 0; c < 16; c++) for (unsigned idx = 0; idx < 256; idx++)
        for (unsigned k = 0; k < 8; k++) for (unsigned sign = 0; sign < 2; sign++) {
            unsigned exp = (unsigned[]){0,1,maxexp/2-1,maxexp/2,maxexp-3,maxexp-2,maxexp-1,maxexp}[k];
            uint64_t bits = (uint64_t)exp << mbits | (uint64_t)idx << (mbits-8);
            if (idx & 1) bits |= (1ull << (mbits-8))-1;
            if (sign) bits |= 1ull << (dbl ? 63 : 31);
            uint32_t in[12];
            for (unsigned lane=0; lane<(dbl ? 6 : 12); lane++) {
                if (dbl) memcpy(in+2*lane, &bits, 8); else in[lane]=(uint32_t)bits;
            }
            uint64_t fl=0, result=f(in, fpcrs[c], &fl);
            hash=mix(mix(hash,result),fl);
        }
    printf("%-10s %016llx\n",name,(unsigned long long)hash);
}

int main(void)
{
    run_estimate("tail_frecpe_2s", t_tail_frecpe_2s, 0);
    run_estimate("tail_frecpe_4s", t_tail_frecpe_4s, 0);
    run_estimate("tail_frecpe_2d", t_tail_frecpe_2d, 1);
    run_estimate("tail_frecpe_s", t_tail_frecpe_s, 0);
    run_estimate("tail_frecpe_d", t_tail_frecpe_d, 1);
    run_estimate("tail_frsqrte_2s", t_tail_frsqrte_2s, 0);
    run_estimate("tail_frsqrte_4s", t_tail_frsqrte_4s, 0);
    run_estimate("tail_frsqrte_2d", t_tail_frsqrte_2d, 1);
    run_estimate("tail_frsqrte_s", t_tail_frsqrte_s, 0);
    run_estimate("tail_frsqrte_d", t_tail_frsqrte_d, 1);

    R(tail_fpcr);
    R(tail_fpsr);
    runf("tail_fccmp_s_eq", t_tail_fccmp_s_eq);
    runf("tail_fccmp_s_ne", t_tail_fccmp_s_ne);
    runf("tail_fccmp_s_cs", t_tail_fccmp_s_cs);
    runf("tail_fccmp_s_cc", t_tail_fccmp_s_cc);
    runf("tail_fccmp_s_mi", t_tail_fccmp_s_mi);
    runf("tail_fccmp_s_pl", t_tail_fccmp_s_pl);
    runf("tail_fccmp_s_vs", t_tail_fccmp_s_vs);
    runf("tail_fccmp_s_vc", t_tail_fccmp_s_vc);
    runf("tail_fccmp_s_hi", t_tail_fccmp_s_hi);
    runf("tail_fccmp_s_ls", t_tail_fccmp_s_ls);
    runf("tail_fccmp_s_ge", t_tail_fccmp_s_ge);
    runf("tail_fccmp_s_lt", t_tail_fccmp_s_lt);
    runf("tail_fccmp_s_gt", t_tail_fccmp_s_gt);
    runf("tail_fccmp_s_le", t_tail_fccmp_s_le);
    runf("tail_fccmp_s_al", t_tail_fccmp_s_al);
    runf("tail_fccmp_s_nv", t_tail_fccmp_s_nv);
    runf("tail_fccmp_d_eq", t_tail_fccmp_d_eq);
    runf("tail_fccmp_d_ne", t_tail_fccmp_d_ne);
    runf("tail_fccmp_d_cs", t_tail_fccmp_d_cs);
    runf("tail_fccmp_d_cc", t_tail_fccmp_d_cc);
    runf("tail_fccmp_d_mi", t_tail_fccmp_d_mi);
    runf("tail_fccmp_d_pl", t_tail_fccmp_d_pl);
    runf("tail_fccmp_d_vs", t_tail_fccmp_d_vs);
    runf("tail_fccmp_d_vc", t_tail_fccmp_d_vc);
    runf("tail_fccmp_d_hi", t_tail_fccmp_d_hi);
    runf("tail_fccmp_d_ls", t_tail_fccmp_d_ls);
    runf("tail_fccmp_d_ge", t_tail_fccmp_d_ge);
    runf("tail_fccmp_d_lt", t_tail_fccmp_d_lt);
    runf("tail_fccmp_d_gt", t_tail_fccmp_d_gt);
    runf("tail_fccmp_d_le", t_tail_fccmp_d_le);
    runf("tail_fccmp_d_al", t_tail_fccmp_d_al);
    runf("tail_fccmp_d_nv", t_tail_fccmp_d_nv);
    runf("tail_fccmpe_s_eq", t_tail_fccmpe_s_eq);
    runf("tail_fccmpe_s_ne", t_tail_fccmpe_s_ne);
    runf("tail_fccmpe_s_cs", t_tail_fccmpe_s_cs);
    runf("tail_fccmpe_s_cc", t_tail_fccmpe_s_cc);
    runf("tail_fccmpe_s_mi", t_tail_fccmpe_s_mi);
    runf("tail_fccmpe_s_pl", t_tail_fccmpe_s_pl);
    runf("tail_fccmpe_s_vs", t_tail_fccmpe_s_vs);
    runf("tail_fccmpe_s_vc", t_tail_fccmpe_s_vc);
    runf("tail_fccmpe_s_hi", t_tail_fccmpe_s_hi);
    runf("tail_fccmpe_s_ls", t_tail_fccmpe_s_ls);
    runf("tail_fccmpe_s_ge", t_tail_fccmpe_s_ge);
    runf("tail_fccmpe_s_lt", t_tail_fccmpe_s_lt);
    runf("tail_fccmpe_s_gt", t_tail_fccmpe_s_gt);
    runf("tail_fccmpe_s_le", t_tail_fccmpe_s_le);
    runf("tail_fccmpe_s_al", t_tail_fccmpe_s_al);
    runf("tail_fccmpe_s_nv", t_tail_fccmpe_s_nv);
    runf("tail_fccmpe_d_eq", t_tail_fccmpe_d_eq);
    runf("tail_fccmpe_d_ne", t_tail_fccmpe_d_ne);
    runf("tail_fccmpe_d_cs", t_tail_fccmpe_d_cs);
    runf("tail_fccmpe_d_cc", t_tail_fccmpe_d_cc);
    runf("tail_fccmpe_d_mi", t_tail_fccmpe_d_mi);
    runf("tail_fccmpe_d_pl", t_tail_fccmpe_d_pl);
    runf("tail_fccmpe_d_vs", t_tail_fccmpe_d_vs);
    runf("tail_fccmpe_d_vc", t_tail_fccmpe_d_vc);
    runf("tail_fccmpe_d_hi", t_tail_fccmpe_d_hi);
    runf("tail_fccmpe_d_ls", t_tail_fccmpe_d_ls);
    runf("tail_fccmpe_d_ge", t_tail_fccmpe_d_ge);
    runf("tail_fccmpe_d_lt", t_tail_fccmpe_d_lt);
    runf("tail_fccmpe_d_gt", t_tail_fccmpe_d_gt);
    runf("tail_fccmpe_d_le", t_tail_fccmpe_d_le);
    runf("tail_fccmpe_d_al", t_tail_fccmpe_d_al);
    runf("tail_fccmpe_d_nv", t_tail_fccmpe_d_nv);

    R(tail_saddl0_0);
    R(tail_saddl0_1);
    R(tail_saddl0_2);
    R(tail_saddl1_0);
    R(tail_saddl1_1);
    R(tail_saddl1_2);
    R(tail_uaddl0_0);
    R(tail_uaddl0_1);
    R(tail_uaddl0_2);
    R(tail_uaddl1_0);
    R(tail_uaddl1_1);
    R(tail_uaddl1_2);
    R(tail_saddw0_0);
    R(tail_saddw0_1);
    R(tail_saddw0_2);
    R(tail_saddw1_0);
    R(tail_saddw1_1);
    R(tail_saddw1_2);
    R(tail_uaddw0_0);
    R(tail_uaddw0_1);
    R(tail_uaddw0_2);
    R(tail_uaddw1_0);
    R(tail_uaddw1_1);
    R(tail_uaddw1_2);
    R(tail_ssubl0_0);
    R(tail_ssubl0_1);
    R(tail_ssubl0_2);
    R(tail_ssubl1_0);
    R(tail_ssubl1_1);
    R(tail_ssubl1_2);
    R(tail_usubl0_0);
    R(tail_usubl0_1);
    R(tail_usubl0_2);
    R(tail_usubl1_0);
    R(tail_usubl1_1);
    R(tail_usubl1_2);
    R(tail_ssubw0_0);
    R(tail_ssubw0_1);
    R(tail_ssubw0_2);
    R(tail_ssubw1_0);
    R(tail_ssubw1_1);
    R(tail_ssubw1_2);
    R(tail_usubw0_0);
    R(tail_usubw0_1);
    R(tail_usubw0_2);
    R(tail_usubw1_0);
    R(tail_usubw1_1);
    R(tail_usubw1_2);
    R(tail_smull0_0);
    R(tail_smull0_1);
    R(tail_smull0_2);
    R(tail_smull1_0);
    R(tail_smull1_1);
    R(tail_smull1_2);
    R(tail_umull0_0);
    R(tail_umull0_1);
    R(tail_umull0_2);
    R(tail_umull1_0);
    R(tail_umull1_1);
    R(tail_umull1_2);
    R(tail_smlal0_0);
    R(tail_smlal0_1);
    R(tail_smlal0_2);
    R(tail_smlal1_0);
    R(tail_smlal1_1);
    R(tail_smlal1_2);
    R(tail_umlal0_0);
    R(tail_umlal0_1);
    R(tail_umlal0_2);
    R(tail_umlal1_0);
    R(tail_umlal1_1);
    R(tail_umlal1_2);
    R(tail_smlsl0_0);
    R(tail_smlsl0_1);
    R(tail_smlsl0_2);
    R(tail_smlsl1_0);
    R(tail_smlsl1_1);
    R(tail_smlsl1_2);
    R(tail_umlsl0_0);
    R(tail_umlsl0_1);
    R(tail_umlsl0_2);
    R(tail_umlsl1_0);
    R(tail_umlsl1_1);
    R(tail_umlsl1_2);
    R(tail_sqxtn0_0);
    R(tail_sqxtn0_1);
    R(tail_sqxtn0_2);
    R(tail_sqxtn1_0);
    R(tail_sqxtn1_1);
    R(tail_sqxtn1_2);
    R(tail_uqxtn0_0);
    R(tail_uqxtn0_1);
    R(tail_uqxtn0_2);
    R(tail_uqxtn1_0);
    R(tail_uqxtn1_1);
    R(tail_uqxtn1_2);
    R(tail_sqxtun0_0);
    R(tail_sqxtun0_1);
    R(tail_sqxtun0_2);
    R(tail_sqxtun1_0);
    R(tail_sqxtun1_1);
    R(tail_sqxtun1_2);
    R(tail_shll0_0);
    R(tail_shll0_1);
    R(tail_shll0_2);
    R(tail_shll1_0);
    R(tail_shll1_1);
    R(tail_shll1_2);
    R(tail_sri0_0_1);
    R(tail_sri0_0_4);
    R(tail_sri0_0_8);
    R(tail_sli0_0_0);
    R(tail_sli0_0_4);
    R(tail_sli0_0_7);
    R(tail_rev160_0);
    R(tail_rev320_0);
    R(tail_rev640_0);
    R(tail_sri0_1_1);
    R(tail_sri0_1_8);
    R(tail_sri0_1_16);
    R(tail_sli0_1_0);
    R(tail_sli0_1_8);
    R(tail_sli0_1_15);
    R(tail_rev320_1);
    R(tail_rev640_1);
    R(tail_sqdmulh0_1);
    R(tail_sqdmulh0_1_0);
    R(tail_sqdmulh0_1_1);
    R(tail_sqdmulh0_1_2);
    R(tail_sqdmulh0_1_3);
    R(tail_sqdmulh0_1_4);
    R(tail_sqdmulh0_1_5);
    R(tail_sqdmulh0_1_6);
    R(tail_sqdmulh0_1_7);
    R(tail_sqrdmulh0_1);
    R(tail_sqrdmulh0_1_0);
    R(tail_sqrdmulh0_1_1);
    R(tail_sqrdmulh0_1_2);
    R(tail_sqrdmulh0_1_3);
    R(tail_sqrdmulh0_1_4);
    R(tail_sqrdmulh0_1_5);
    R(tail_sqrdmulh0_1_6);
    R(tail_sqrdmulh0_1_7);
    R(tail_sri0_2_1);
    R(tail_sri0_2_16);
    R(tail_sri0_2_32);
    R(tail_sli0_2_0);
    R(tail_sli0_2_16);
    R(tail_sli0_2_31);
    R(tail_rev640_2);
    R(tail_sqdmulh0_2);
    R(tail_sqdmulh0_2_0);
    R(tail_sqdmulh0_2_1);
    R(tail_sqdmulh0_2_2);
    R(tail_sqdmulh0_2_3);
    R(tail_sqrdmulh0_2);
    R(tail_sqrdmulh0_2_0);
    R(tail_sqrdmulh0_2_1);
    R(tail_sqrdmulh0_2_2);
    R(tail_sqrdmulh0_2_3);
    R(tail_sri1_0_1);
    R(tail_sri1_0_4);
    R(tail_sri1_0_8);
    R(tail_sli1_0_0);
    R(tail_sli1_0_4);
    R(tail_sli1_0_7);
    R(tail_rev161_0);
    R(tail_rev321_0);
    R(tail_rev641_0);
    R(tail_sri1_1_1);
    R(tail_sri1_1_8);
    R(tail_sri1_1_16);
    R(tail_sli1_1_0);
    R(tail_sli1_1_8);
    R(tail_sli1_1_15);
    R(tail_rev321_1);
    R(tail_rev641_1);
    R(tail_sqdmulh1_1);
    R(tail_sqdmulh1_1_0);
    R(tail_sqdmulh1_1_1);
    R(tail_sqdmulh1_1_2);
    R(tail_sqdmulh1_1_3);
    R(tail_sqdmulh1_1_4);
    R(tail_sqdmulh1_1_5);
    R(tail_sqdmulh1_1_6);
    R(tail_sqdmulh1_1_7);
    R(tail_sqrdmulh1_1);
    R(tail_sqrdmulh1_1_0);
    R(tail_sqrdmulh1_1_1);
    R(tail_sqrdmulh1_1_2);
    R(tail_sqrdmulh1_1_3);
    R(tail_sqrdmulh1_1_4);
    R(tail_sqrdmulh1_1_5);
    R(tail_sqrdmulh1_1_6);
    R(tail_sqrdmulh1_1_7);
    R(tail_sri1_2_1);
    R(tail_sri1_2_16);
    R(tail_sri1_2_32);
    R(tail_sli1_2_0);
    R(tail_sli1_2_16);
    R(tail_sli1_2_31);
    R(tail_rev641_2);
    R(tail_sqdmulh1_2);
    R(tail_sqdmulh1_2_0);
    R(tail_sqdmulh1_2_1);
    R(tail_sqdmulh1_2_2);
    R(tail_sqdmulh1_2_3);
    R(tail_sqrdmulh1_2);
    R(tail_sqrdmulh1_2_0);
    R(tail_sqrdmulh1_2_1);
    R(tail_sqrdmulh1_2_2);
    R(tail_sqrdmulh1_2_3);
    R(tail_sri1_3_1);
    R(tail_sri1_3_32);
    R(tail_sri1_3_64);
    R(tail_sli1_3_0);
    R(tail_sli1_3_32);
    R(tail_sli1_3_63);
    R(tail_addpd);
    R(tail_ldrb_imm);
    R(tail_ldrb_post);
    R(tail_ldrb_pre);
    R(tail_ldrb_reg);
    R(tail_strb);
    R(tail_ldrh_imm);
    R(tail_ldrh_post);
    R(tail_ldrh_pre);
    R(tail_ldrh_reg);
    R(tail_strh);

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
    R(cov_ushl2s);
    R(cov_ushl4s);
    R(cov_ushl2d);
    R(cov_sshl2s);
    R(cov_sshl4s);
    R(cov_sshl2d);
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
    RF(faddp2s);
    RF(faddp4s);
    RF(faddp2d);
    RF(faddps);
    RF(faddpd);
    RF(fmaxv);
    RF(fminv);
    RF(fmaxnmv);
    RF(fminnmv);
    RF(fdiv2s); RF(fdiv4s);
    RF(fcmeqz2s);
    RF(fcmeqz4s);
    RF(fcmgez2s);
    RF(fcmgez4s);
    RF(fcmgtz2s);
    RF(fcmgtz4s);
    RF(fcmlez2s);
    RF(fcmlez4s);
    RF(fcmltz2s);
    RF(fcmltz4s);
    RF(tail_fmadd); RF(tail_fmsub); RF(tail_fnmadd); RF(tail_fnmsub); RF(tail_fnmul); RF(tail_frecpss); RF(tail_frecps2s); RF(tail_frecps4s); RF(tail_frsqrtss); RF(tail_frsqrts2s); RF(tail_frsqrts4s);
    RF4(fmla); RF4(fmls);
    RF(fmlae0); RF(fmlae1); RF(fmlae2); RF(fmlae3); RF(fmlse0); RF(fmlse1); RF(fmlse2); RF(fmlse3);
    RF4(fadd); RF4(fsub); RF4(fmul); RF4(fmax); RF4(fmin); RF4(fmaxnm); RF4(fminnm); RF4(fcmeq); RF4(fcmge); RF4(fcmgt);
    RF(fmule0); RF(fmule1); RF(fmule2); RF(fmule3); RF(fchain); RF(fadd2d);
#define RR(n) runr(#n, t_##n)
#define RFR(n) RR(n##4s); RR(n##2s); RR(n##2d); RR(n##s); RR(n##d)
    RF(abd2s);
    RF(abd4s);
    RF(abd2d);
    RF(abds);
    RF(abdd);
    RF(sqadd_8b);
    RF(sqadd_16b);
    RF(sqadd_4h);
    RF(sqadd_8h);
    RF(sqadd_2s);
    RF(sqadd_4s);
    RF(sqadd_2d);
    RF(sqadd_b);
    RF(sqadd_h);
    RF(sqadd_s);
    RF(sqadd_d);
    RF(uqadd_8b);
    RF(uqadd_16b);
    RF(uqadd_4h);
    RF(uqadd_8h);
    RF(uqadd_2s);
    RF(uqadd_4s);
    RF(uqadd_2d);
    RF(uqadd_b);
    RF(uqadd_h);
    RF(uqadd_s);
    RF(uqadd_d);
    RF(sqsub_8b);
    RF(sqsub_16b);
    RF(sqsub_4h);
    RF(sqsub_8h);
    RF(sqsub_2s);
    RF(sqsub_4s);
    RF(sqsub_2d);
    RF(sqsub_b);
    RF(sqsub_h);
    RF(sqsub_s);
    RF(sqsub_d);
    RF(uqsub_8b);
    RF(uqsub_16b);
    RF(uqsub_4h);
    RF(uqsub_8h);
    RF(uqsub_2s);
    RF(uqsub_4s);
    RF(uqsub_2d);
    RF(uqsub_b);
    RF(uqsub_h);
    RF(uqsub_s);
    RF(uqsub_d);
    RF(sabd_8b);
    RF(sabd_4h);
    RF(sabd_2s);
    RF(sabd_16b);
    RF(sabd_8h);
    RF(sabd_4s);
    RF(uabd_8b);
    RF(uabd_4h);
    RF(uabd_2s);
    RF(uabd_16b);
    RF(uabd_8h);
    RF(uabd_4s);
    RF(saba_8b);
    RF(saba_4h);
    RF(saba_2s);
    RF(saba_16b);
    RF(saba_8h);
    RF(saba_4s);
    RF(uaba_8b);
    RF(uaba_4h);
    RF(uaba_2s);
    RF(uaba_16b);
    RF(uaba_8h);
    RF(uaba_4s);
    RF(sabdl_8h);
    RF(sabdl_4s);
    RF(sabdl_2d);
    RF(sabdl2_8h);
    RF(sabdl2_4s);
    RF(sabdl2_2d);
    RF(uabdl_8h);
    RF(uabdl_4s);
    RF(uabdl_2d);
    RF(uabdl2_8h);
    RF(uabdl2_4s);
    RF(uabdl2_2d);
    RF(sabal_8h);
    RF(sabal_4s);
    RF(sabal_2d);
    RF(sabal2_8h);
    RF(sabal2_4s);
    RF(sabal2_2d);
    RF(uabal_8h);
    RF(uabal_4s);
    RF(uabal_2d);
    RF(uabal2_8h);
    RF(uabal2_4s);
    RF(uabal2_2d);
    RF(addhn_8b);
    RF(addhn_4h);
    RF(addhn_2s);
    RF(addhn2_16b);
    RF(addhn2_8h);
    RF(addhn2_4s);
    RF(raddhn_8b);
    RF(raddhn_4h);
    RF(raddhn_2s);
    RF(raddhn2_16b);
    RF(raddhn2_8h);
    RF(raddhn2_4s);
    RF(subhn_8b);
    RF(subhn_4h);
    RF(subhn_2s);
    RF(subhn2_16b);
    RF(subhn2_8h);
    RF(subhn2_4s);
    RF(rsubhn_8b);
    RF(rsubhn_4h);
    RF(rsubhn_2s);
    RF(rsubhn2_16b);
    RF(rsubhn2_8h);
    RF(rsubhn2_4s);
    RF(addp_8b);
    RF(addp_16b);
    RF(addp_4h);
    RF(addp_8h);
    RF(addp_2s);
    RF(addp_4s);
    RF(addp_2d);
    RF(fmul_2d_e0);
    RF(fmul_2d_e1);
    RF(cov_fmuls0);
    RF(cov_fmuls1);
    RF(cov_fmuls2);
    RF(cov_fmuls3);
    RF(cov_fmlas0);
    RF(cov_fmlas1);
    RF(cov_fmlas2);
    RF(cov_fmlas3);
    RF(cov_fmlss0);
    RF(cov_fmlss1);
    RF(cov_fmlss2);
    RF(cov_fmlss3);
    RF(tail_aese); RF(tail_aese_alias);
    RF(tail_aesd); RF(tail_aesd_alias);
    RF(tail_aesmc); RF(tail_aesmc_alias);
    RF(tail_aesimc); RF(tail_aesimc_alias);
    RR(wide_fadd);
    RR(wide_fsub);
    RR(wide_fmul);
    RR(wide_fdiv);
    RR(wide_sqrtd);
    RF(wide_sqrts); RF(wide_sqrt2s);
    RR(cov_fmuld0);
    RR(cov_fmuld1);
    RR(tail_fcvtns_2s);
    RR(tail_fcvtns_4s);
    RR(tail_fcvtns_2d);
    RR(tail_fcvtns_s);
    RR(tail_fcvtns_d);
    RR(tail_fcvtns_4h);
    RR(tail_fcvtns_8h);
    RR(tail_fcvtnu_2s);
    RR(tail_fcvtnu_4s);
    RR(tail_fcvtnu_2d);
    RR(tail_fcvtnu_s);
    RR(tail_fcvtnu_d);
    RR(tail_fcvtnu_4h);
    RR(tail_fcvtnu_8h);
    RR(tail_fcvtps_2s);
    RR(tail_fcvtps_4s);
    RR(tail_fcvtps_2d);
    RR(tail_fcvtps_s);
    RR(tail_fcvtps_d);
    RR(tail_fcvtpu_2s);
    RR(tail_fcvtpu_4s);
    RR(tail_fcvtpu_2d);
    RR(tail_fcvtpu_s);
    RR(tail_fcvtpu_d);
    RR(tail_fcvtms_2s);
    RR(tail_fcvtms_4s);
    RR(tail_fcvtms_2d);
    RR(tail_fcvtms_s);
    RR(tail_fcvtms_d);
    RR(tail_fcvtmu_2s);
    RR(tail_fcvtmu_4s);
    RR(tail_fcvtmu_2d);
    RR(tail_fcvtmu_s);
    RR(tail_fcvtmu_d);
    RR(tail_fcvtas_2s);
    RR(tail_fcvtas_4s);
    RR(tail_fcvtas_2d);
    RR(tail_fcvtas_s);
    RR(tail_fcvtas_d);
    RR(tail_fcvtau_2s);
    RR(tail_fcvtau_4s);
    RR(tail_fcvtau_2d);
    RR(tail_fcvtau_s);
    RR(tail_fcvtau_d);
    RR(tail_fcvtzs_2s);
    RR(tail_fcvtzs_4s);
    RR(tail_fcvtzs_2d);
    RR(tail_fcvtzs_s);
    RR(tail_fcvtzs_d);
    RR(tail_fcvtzs_4h);
    RR(tail_fcvtzs_8h);
    RR(tail_fcvtzu_2s);
    RR(tail_fcvtzu_4s);
    RR(tail_fcvtzu_2d);
    RR(tail_fcvtzu_s);
    RR(tail_fcvtzu_d);
    RR(tail_fcvtzu_4h);
    RR(tail_fcvtzu_8h);
    RR(tail_scvtf_2s);
    RR(tail_scvtf_4s);
    RR(tail_scvtf_2d);
    RR(tail_scvtf_s);
    RR(tail_scvtf_d);
    RR(tail_scvtf_4h);
    RR(tail_scvtf_8h);
    RR(tail_ucvtf_2s);
    RR(tail_ucvtf_4s);
    RR(tail_ucvtf_2d);
    RR(tail_ucvtf_s);
    RR(tail_ucvtf_d);
    RR(tail_ucvtf_4h);
    RR(tail_ucvtf_8h);
    RR(tail_fcvtns_ws);
    RR(tail_fcvtns_xs);
    RR(tail_fcvtns_wd);
    RR(tail_fcvtns_xd);
    RR(tail_fcvtns_wh);
    RR(tail_fcvtns_xh);
    RR(tail_fcvtnu_ws);
    RR(tail_fcvtnu_xs);
    RR(tail_fcvtnu_wd);
    RR(tail_fcvtnu_xd);
    RR(tail_fcvtnu_wh);
    RR(tail_fcvtnu_xh);
    RR(tail_fcvtps_ws);
    RR(tail_fcvtps_xs);
    RR(tail_fcvtps_wd);
    RR(tail_fcvtps_xd);
    RR(tail_fcvtps_wh);
    RR(tail_fcvtps_xh);
    RR(tail_fcvtpu_ws);
    RR(tail_fcvtpu_xs);
    RR(tail_fcvtpu_wd);
    RR(tail_fcvtpu_xd);
    RR(tail_fcvtpu_wh);
    RR(tail_fcvtpu_xh);
    RR(tail_fcvtms_ws);
    RR(tail_fcvtms_xs);
    RR(tail_fcvtms_wd);
    RR(tail_fcvtms_xd);
    RR(tail_fcvtms_wh);
    RR(tail_fcvtms_xh);
    RR(tail_fcvtmu_ws);
    RR(tail_fcvtmu_xs);
    RR(tail_fcvtmu_wd);
    RR(tail_fcvtmu_xd);
    RR(tail_fcvtmu_wh);
    RR(tail_fcvtmu_xh);
    RR(tail_fcvtas_ws);
    RR(tail_fcvtas_xs);
    RR(tail_fcvtas_wd);
    RR(tail_fcvtas_xd);
    RR(tail_fcvtas_wh);
    RR(tail_fcvtas_xh);
    RR(tail_fcvtau_ws);
    RR(tail_fcvtau_xs);
    RR(tail_fcvtau_wd);
    RR(tail_fcvtau_xd);
    RR(tail_fcvtau_wh);
    RR(tail_fcvtau_xh);
    RR(tail_fcvtn_4h);
    RR(tail_fcvtn_2s);
    RR(tail_fcvtn2_8h);
    RR(tail_fcvtn2_4s);
    RR(tail_fcvtl_4s);
    RR(tail_fcvtl_2d);
    RR(tail_fcvtl2_4s);
    RR(tail_fcvtl2_2d);
    RR(tail_fmul_4h);
    RR(tail_fmul_8h);
    RR(tail_fmul_h);
    RR(tail_fadd_4h);
    RR(tail_fadd_8h);
    RR(tail_fadd_h);
    RR(tail_fsub_4h);
    RR(tail_fsub_8h);
    RR(tail_fsub_h);
    RR(tail_fmax_4h);
    RR(tail_fmax_8h);
    RR(tail_fmax_h);
    RR(tail_fmin_4h);
    RR(tail_fmin_8h);
    RR(tail_fmin_h);
    RR(tail_fmaxnm_4h);
    RR(tail_fmaxnm_8h);
    RR(tail_fmaxnm_h);
    RR(tail_fminnm_4h);
    RR(tail_fminnm_8h);
    RR(tail_fminnm_h);
    RR(tail_fmul_4h_e0);
    RR(tail_fmul_8h_e0);
    RR(tail_fmul_4h_e1);
    RR(tail_fmul_8h_e1);
    RR(tail_fmul_4h_e2);
    RR(tail_fmul_8h_e2);
    RR(tail_fmul_4h_e3);
    RR(tail_fmul_8h_e3);
    RR(tail_fmul_4h_e4);
    RR(tail_fmul_8h_e4);
    RR(tail_fmul_4h_e5);
    RR(tail_fmul_8h_e5);
    RR(tail_fmul_4h_e6);
    RR(tail_fmul_8h_e6);
    RR(tail_fmul_4h_e7);
    RR(tail_fmul_8h_e7);
    RR(tail_fcvt_sd);
    RR(tail_fcvt_ds);
    RR(tail_fcvt_hs);
    RR(tail_fcvt_hd);
    RR(tail_fcvt_sh);
    RR(tail_fcvt_dh);
    RR(tail_scvtf_hw);
    RR(tail_scvtf_hx);
    RR(tail_ucvtf_hw);
    RR(tail_ucvtf_hx);
    RFR(frintn); RFR(frintm); RFR(frintp); RFR(frintz); RFR(frinta); RFR(frintx); RFR(frinti); RR(frchain);
    R(ld1a); R(ld1b); R(ld1c); R(ld1d); R(ld1e); R(ld1f); R(st1a); R(st1b); R(st1c); R(st1d); R(st1e);
    R(ld1r0); R(ld1r1); R(ld1r2); R(ld1r3); R(ld1r4); R(ld1r5);
    R(ld1l0); R(ld1l1); R(ld1l2); R(ld1l3); R(st1l0); R(st1l1); R(st1l2); R(st1l3); R(ld1sp);
    R(ld2_8b_n);
    R(ld2_8b_i);
    R(ld2_8b_r);
    R(ld2_16b_n);
    R(ld2_16b_i);
    R(ld2_16b_r);
    R(ld2_4h_n);
    R(ld2_4h_i);
    R(ld2_4h_r);
    R(ld2_8h_n);
    R(ld2_8h_i);
    R(ld2_8h_r);
    R(ld2_2s_n);
    R(ld2_2s_i);
    R(ld2_2s_r);
    R(ld2_4s_n);
    R(ld2_4s_i);
    R(ld2_4s_r);
    R(ld2_2d_n);
    R(ld2_2d_i);
    R(ld2_2d_r);
    R(ld3_8b_n);
    R(ld3_8b_i);
    R(ld3_8b_r);
    R(ld3_16b_n);
    R(ld3_16b_i);
    R(ld3_16b_r);
    R(ld3_4h_n);
    R(ld3_4h_i);
    R(ld3_4h_r);
    R(ld3_8h_n);
    R(ld3_8h_i);
    R(ld3_8h_r);
    R(ld3_2s_n);
    R(ld3_2s_i);
    R(ld3_2s_r);
    R(ld3_4s_n);
    R(ld3_4s_i);
    R(ld3_4s_r);
    R(ld3_2d_n);
    R(ld3_2d_i);
    R(ld3_2d_r);
    R(ld4_8b_n);
    R(ld4_8b_i);
    R(ld4_8b_r);
    R(ld4_16b_n);
    R(ld4_16b_i);
    R(ld4_16b_r);
    R(ld4_4h_n);
    R(ld4_4h_i);
    R(ld4_4h_r);
    R(ld4_8h_n);
    R(ld4_8h_i);
    R(ld4_8h_r);
    R(ld4_2s_n);
    R(ld4_2s_i);
    R(ld4_2s_r);
    R(ld4_4s_n);
    R(ld4_4s_i);
    R(ld4_4s_r);
    R(ld4_2d_n);
    R(ld4_2d_i);
    R(ld4_2d_r);
    R(st2_8b_n);
    R(st2_8b_i);
    R(st2_8b_r);
    R(st2_16b_n);
    R(st2_16b_i);
    R(st2_16b_r);
    R(st2_4h_n);
    R(st2_4h_i);
    R(st2_4h_r);
    R(st2_8h_n);
    R(st2_8h_i);
    R(st2_8h_r);
    R(st2_2s_n);
    R(st2_2s_i);
    R(st2_2s_r);
    R(st2_4s_n);
    R(st2_4s_i);
    R(st2_4s_r);
    R(st2_2d_n);
    R(st2_2d_i);
    R(st2_2d_r);
    R(st3_8b_n);
    R(st3_8b_i);
    R(st3_8b_r);
    R(st3_16b_n);
    R(st3_16b_i);
    R(st3_16b_r);
    R(st3_4h_n);
    R(st3_4h_i);
    R(st3_4h_r);
    R(st3_8h_n);
    R(st3_8h_i);
    R(st3_8h_r);
    R(st3_2s_n);
    R(st3_2s_i);
    R(st3_2s_r);
    R(st3_4s_n);
    R(st3_4s_i);
    R(st3_4s_r);
    R(st3_2d_n);
    R(st3_2d_i);
    R(st3_2d_r);
    R(st4_8b_n);
    R(st4_8b_i);
    R(st4_8b_r);
    R(st4_16b_n);
    R(st4_16b_i);
    R(st4_16b_r);
    R(st4_4h_n);
    R(st4_4h_i);
    R(st4_4h_r);
    R(st4_8h_n);
    R(st4_8h_i);
    R(st4_8h_r);
    R(st4_2s_n);
    R(st4_2s_i);
    R(st4_2s_r);
    R(st4_4s_n);
    R(st4_4s_i);
    R(st4_4s_r);
    R(st4_2d_n);
    R(st4_2d_i);
    R(st4_2d_r);
    sigctx();
    pthread_t th[NT];
    for (uintptr_t k = 0; k < NT; k++) pthread_create(&th[k], NULL, worker, (void *)k);
    for (int k = 0; k < NT; k++) pthread_join(th[k], NULL);
    printf("threads    add=%llu w=%u b=%u cas=%llu max=%llu or=%016llx\n", (unsigned long long)ctr_add, ctr_w, ctr_b,
           (unsigned long long)ctr_cas, (unsigned long long)ctr_max, (unsigned long long)ctr_or);
    return 0;
}
