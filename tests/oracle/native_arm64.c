/*
 * Native arm64 oracle: executes random instruction encodings on the host
 * Apple silicon CPU and records input/output state. Build with -arch arm64.
 *   native_arm64 OUT.bin COUNT SEED MODE    (MODE: dp | simd | mem | block | pac | fma)
 *
 * Every record covers all of x0-x30, SP, NZCV, FPCR, FPSR and v0-v31. Register
 * fields take any value 0-31 (so SP and XZR are exercised) except 18, the
 * Darwin platform register. For each record a harness is generated around
 * the code: it loads the whole input state through one scratch register
 * `sreg` that no field of the code names, runs the code, and stores the whole
 * output state. Modes:
 *   dp, simd, mem  one random encoding of that class; simd and block records
 *                  run under a random FPCR (rounding mode, FZ, DN)
 *   mem            base registers point into scratch memory, some into a
 *                  read-only page (loads, LDXP/LDAXP included, must work there)
 *   block          2-8 instructions mixing flag producers, x86-flag-clobbering
 *                  fillers, flag consumers, MSR FPCR + FP arithmetic, and
 *                  forward branches (B.cond, CBZ, TBZ, B, BL) inside the block,
 *                  so the JIT's flag liveness and multi-exit blocks are covered
 *   pac            pointer authentication (see pac_gen): every PAC*, AUT* and XPAC*
 *                  data-processing form and every HINT-space PAC/AUT/XPACLRI
 *                  form on canonical, non-canonical, top-byte-tagged and
 *                  kernel-half values with several modifiers, then random
 *                  values and nearby (reserved) encodings; one dp record in
 *                  eight is a pac record, so `make oracle` covers it
 *   fma            single-precision fused multiply-add (see fma_gen): FMLA and
 *                  FMLS .2S/.4S, both by element, the scalar by-element forms
 *                  and FMADD/FMSUB/FNMADD/FNMSUB, on the operand classes of
 *                  fma_cases.h, under every RMode/FZ/DN with preset FPSR
 *                  flags and aliased registers. The checker compares FPSR's
 *                  cumulative flags exactly for these records
 */
#include "record.h"
#include "fma_cases.h"
#include "known.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Harness block: the state plus host registers the generated code restores. */
typedef struct {
    oracle_state st;
    uint64_t host_sp, host_lr, host_fpcr;
} harness;
#define H_SP 784
#define H_LR 792
#define H_FPCR 800
_Static_assert(offsetof(harness, host_sp) == H_SP && offsetof(harness, host_fpcr) == H_FPCR, "layout");

/* oracle_call(harness *h, void *code): saves callee-saved state, records the
 * host sp/lr/fpcr in h, jumps to code; the generated epilogue returns to 1:. */
void oracle_call(harness *h, void *code);
__asm__(".text\n.globl _oracle_call\n.p2align 2\n_oracle_call:\n"
        "  sub sp, sp, #0xa0\n"
        "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
        "  mov x9, sp\n  str x9, [x0, #784]\n  adr x9, 1f\n  str x9, [x0, #792]\n  mrs x9, fpcr\n  str x9, [x0, #800]\n"
        "  br x1\n"
        "1:\n"
        "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #0xa0\n  ret\n");

static sigjmp_buf jb;
static volatile int sig;
static void on_sig(int s, siginfo_t *si, void *uc) { (void)si; (void)uc; sig = s; siglongjmp(jb, 1); }

static uint64_t rng;
static uint64_t rnd(void) { return oracle_xs(&rng); }
static unsigned rr(unsigned n) { return (unsigned)(rnd() % n); }

static uint64_t interesting(void)
{
    static const uint64_t v[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
                                 0xffffffff, 0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull,
                                 ~0ull, 0x3ff0000000000000ull, 0x4000000000000000ull, 0xbff0000000000000ull,
                                 0x7ff8000000000000ull, 0x3f8000003f800000ull, 0x7fc00000ull,
                                 /* NaN payloads (quiet and signalling), infinities, zeros, denormals, ties */
                                 0x7ff8000012345678ull, 0xfff0000000000001ull, 0x7ff4000000000000ull,
                                 0x7f8000017fc00123ull, 0xff800000ff812345ull, 0x7ff0000000000000ull,
                                 0xfff0000000000000ull, 0x8000000080000000ull, 0x0000000100000001ull,
                                 0x000fffffffffffffull, 0x007fffff807fffffull, 0x4004000000000000ull,
                                 0x3fe0000000000000ull, 0x4002000040200000ull, 0x4330000000000001ull};
    uint64_t r = rnd();
    switch (r & 3) {
    case 0: return v[(r >> 8) % (sizeof v / sizeof *v)];
    case 1: return rnd() & 0xffff;
    default: return rnd();
    }
}

/* ---- encodings used by the generator */
static unsigned reg(void) { unsigned r; do r = rr(32); while (r == 18); return r; }
static unsigned gpr(void) { unsigned r; do r = rr(31); while (r == 18); return r; }   /* not 31 */
static uint32_t i_ldr_w(unsigned rt, unsigned rn, unsigned off) { return 0xB9400000u | (off / 4) << 10 | rn << 5 | rt; }
static uint32_t i_str_w(unsigned rt, unsigned rn, unsigned off) { return 0xB9000000u | (off / 4) << 10 | rn << 5 | rt; }
static uint32_t i_ldr_x(unsigned rt, unsigned rn, unsigned off) { return 0xF9400000u | (off / 8) << 10 | rn << 5 | rt; }
static uint32_t i_str_x(unsigned rt, unsigned rn, unsigned off) { return 0xF9000000u | (off / 8) << 10 | rn << 5 | rt; }
static uint32_t i_ldp_q(unsigned a, unsigned b, unsigned rn, unsigned off) { return 0xAD400000u | (off / 16) << 15 | b << 10 | rn << 5 | a; }
static uint32_t i_stp_q(unsigned a, unsigned b, unsigned rn, unsigned off) { return 0xAD000000u | (off / 16) << 15 | b << 10 | rn << 5 | a; }
#define I_MSR_NZCV_X0 0xD51B4200u
#define I_MRS_X0_NZCV 0xD53B4200u
#define I_MSR_FPCR_X0 0xD51B4400u
#define I_MRS_X0_FPCR 0xD53B4400u
#define I_MSR_FPSR_X0 0xD51B4420u
#define I_MRS_X0_FPSR 0xD53B4420u

/* registers an encoding can name (all 5-bit fields, and the next register for pairs) */
static uint32_t field_regs(uint32_t i)
{
    uint32_t m = 0;
    unsigned f[4] = {i & 31, (i >> 5) & 31, (i >> 10) & 31, (i >> 16) & 31};
    for (int k = 0; k < 4; k++) m |= 1u << f[k] | 1u << ((f[k] + 1) & 31);
    return m;
}
static bool names18(uint32_t i)
{
    return (i & 31) == 18 || ((i >> 5) & 31) == 18 || ((i >> 10) & 31) == 18 || ((i >> 16) & 31) == 18;
}

/* One simd record in eight lands in a cryptographic-extension group (Arm ARM
 * C4.1.95: AES, SHA1/SHA256, SHA512/SHA3, SM3/SM4), fields and reserved
 * values random, so every encoding of the small groups is sampled (random
 * words alone hit SHA512SU0, 1 in 2^20, about once a run). These groups name
 * only vector registers, so a field that reads 18 (AESE's bits 14:10 always
 * do) is no reason to skip the record. */
static const struct { uint32_t mask, value; bool vonly; } crypto[] = {
    {0xFF3E0C00, 0x4E280800, true},  /* AES */
    {0xFF208C00, 0x5E000000, true},  /* SHA three-register */
    {0xFF3E0C00, 0x5E280800, true},  /* SHA two-register */
    {0xFFFFCC00, 0x4E284800, true},  /* AESE, AESD, AESMC, AESIMC */
    {0xFFE08C00, 0x5E000000, true},  /* SHA1C ... SHA256SU1 and the reserved opcode 7 */
    {0xFFFFCC00, 0x5E280800, true},  /* SHA1H, SHA1SU1, SHA256SU0 and opcode 3 */
    {0xFF000000, 0xCE000000, true},  /* the 0xCE crypto space, reserved slots included */
    {0xFF808000, 0xCE000000, true},  /* EOR3, BCAX, SM3SS1, reserved Op0 = 3 */
    {0xFF800000, 0xCE000000, true},  /* the same with bit 15 random */
    {0xFFE00000, 0xCE800000, true},  /* XAR */
    {0xFFE08000, 0xCE408000, true},  /* SM3TT (imm2) */
    {0xFFE08000, 0xCE608000, true},  /* SHA512 three-register, SM3PARTW, SM4EKEY */
    {0xFFE0B000, 0xCE608000, true},  /* the same, bits 13:12 clear */
    {0xFFFFF000, 0xCEC08000, true},  /* SHA512SU0, SM4E */
    {0xFFE00000, 0xCEC00000, true},  /* the rest of Op0 = 110 */
    {0xBF000000, 0x8E000000, false}, /* bit 30 clear */
};
static bool crypto_vonly(uint32_t i)
{
    for (size_t g = 0; g < sizeof crypto / sizeof *crypto; g++)
        if (crypto[g].vonly && (i & crypto[g].mask) == crypto[g].value) return true;
    return false;
}

/* ---- block-mode templates */
static unsigned cond(void) { return rr(16); }
static uint32_t t_producer(void)
{
    unsigned sf = rr(2), rd = reg(), rn = reg(), rm = reg();
    switch (rr(8)) {
    case 0: return sf << 31 | rr(2) << 30 | 1u << 29 | 0x11000000u | rr(2) << 22 | rr(4096) << 10 | rn << 5 | (rr(2) ? 31 : rd); /* ADDS/SUBS/CMP imm */
    case 1: return sf << 31 | rr(2) << 30 | 1u << 29 | 0x0B000000u | rr(3) << 22 | rm << 16 | (sf ? rr(64) : rr(32)) << 10 | rn << 5 | rd;
    case 2: return sf << 31 | 3u << 29 | 0x0A000000u | rr(4) << 22 | rr(2) << 21 | rm << 16 | (sf ? rr(64) : rr(32)) << 10 | rn << 5 | rd; /* ANDS/BICS */
    case 3: return sf << 31 | rr(2) << 30 | 1u << 29 | 0x1A000000u | rm << 16 | rn << 5 | rd;            /* ADCS/SBCS */
    case 4: return sf << 31 | rr(2) << 30 | 1u << 29 | 0x1A400000u | rm << 16 | cond() << 12 | rr(2) << 11 | rn << 5 | rr(16); /* CCMP/CCMN */
    case 5: return 0x1E202000u | rr(2) << 22 | (rm & 31) << 16 | (rn & 31) << 5 | rr(4) << 3;            /* FCMP/FCMPE (+#0) */
    case 6: return 0x1E200400u | rr(2) << 22 | rm << 16 | cond() << 12 | rn << 5 | rr(2) << 4 | rr(16);   /* FCCMP(E) */
    default: return 0xD51B4200u | gpr();                                                                 /* MSR NZCV */
    }
}
static uint32_t t_consumer(void)
{
    unsigned sf = rr(2), rd = reg(), rn = reg(), rm = reg();
    switch (rr(6)) {
    case 0: case 1: return sf << 31 | rr(2) << 30 | 0x1A800000u | rm << 16 | cond() << 12 | rr(2) << 10 | rn << 5 | rd; /* CSEL family */
    case 2: return sf << 31 | rr(2) << 30 | 0x1A000000u | rm << 16 | rn << 5 | rd;                       /* ADC/SBC */
    case 3: return 0x1E200C00u | rr(2) << 22 | rm << 16 | cond() << 12 | rn << 5 | rd;                   /* FCSEL */
    case 4: return 0xD53B4200u | gpr();                                                                  /* MRS NZCV */
    default: return sf << 31 | rr(2) << 30 | 1u << 29 | 0x1A400000u | rm << 16 | cond() << 12 | rr(2) << 11 | rn << 5 | rr(16);
    }
}
static uint32_t t_filler(void)
{
    unsigned sf = rr(2), rd = reg(), rn = reg(), rm = reg(), type = rr(2);
    switch (rr(10)) {
    case 0: case 1: {   /* any data-processing encoding */
        static const uint32_t cls[] = {0x8, 0x9, 0x5, 0xD};
        return ((uint32_t)rnd() & ~(0xFu << 25)) | cls[rr(4)] << 25;
    }
    case 2: return sf << 31 | 0x0A000000u | rr(3) << 29 | rr(4) << 22 | rr(2) << 21 | rm << 16 | (sf ? rr(64) : rr(32)) << 10 | rn << 5 | rd; /* AND/ORR/EOR(+shift) */
    case 3: return sf << 31 | 0x53000000u | sf << 22 | rr(sf ? 64 : 32) << 16 | rr(sf ? 64 : 32) << 10 | rn << 5 | rd; /* UBFM */
    case 4: return sf << 31 | 0x1B000000u | rm << 16 | rr(2) << 15 | reg() << 10 | rn << 5 | rd;        /* MADD/MSUB */
    case 5: return 0x1E200800u | type << 22 | rm << 16 | rr(4) << 12 | rn << 5 | rd;                   /* FMUL/FDIV/FADD/FSUB */
    case 6: return 0x1E21C000u | type << 22 | rn << 5 | rd;                                            /* FSQRT */
    case 7: return sf << 31 | 0x1E220000u | type << 22 | rr(2) << 16 | rn << 5 | rd;                   /* SCVTF/UCVTF */
    case 8: return 0x1E204000u | type << 22 | (8 + rr(8)) << 15 | rn << 5 | rd;                        /* FRINT* / FCVT */
    default:
        if (!rr(4)) return 0xCE000000u | rr(2) << 21 | rm << 16 | reg() << 10 | rn << 5 | rd;            /* EOR3/BCAX (native in the JIT) */
        return sf << 31 | 0x1AC00800u | rm << 16 | rr(4) << 10 | rn << 5 | rd;                     /* UDIV/SDIV/shifts by reg */
    }
}
/* forward branch at k to a target in k+1..n (n = past the block) that is not
 * the second half of a MOVZ/MSR FPCR pair */
static uint32_t t_branch(int k, int n, const bool *notarget)
{
    int tgt;
    do tgt = k + 1 + (int)rr((unsigned)(n - k)); while (tgt < n && notarget[tgt]);
    int d = tgt - k;
    unsigned rt = reg();
    switch (rr(6)) {
    case 0: case 1: return 0x54000000u | ((uint32_t)d & 0x7ffff) << 5 | cond();                    /* B.cond */
    case 2: return rr(2) << 31 | 0x34000000u | rr(2) << 24 | ((uint32_t)d & 0x7ffff) << 5 | rt;     /* CBZ/CBNZ */
    case 3: return rr(2) << 31 | 0x36000000u | rr(2) << 24 | rr(32) << 19 | ((uint32_t)d & 0x3fff) << 5 | rt; /* TBZ/TBNZ */
    case 4: return 0x14000000u | ((uint32_t)d & 0x3ffffff);                                         /* B */
    default: return 0x94000000u | ((uint32_t)d & 0x3ffffff);                                        /* BL */
    }
}

static uint32_t random_fpcr(void)
{
    uint32_t f = rr(4) << 22;                 /* RMode */
    if (!rr(4)) f |= 1u << 24;                /* FZ */
    if (!rr(4)) f |= 1u << 25;                /* DN */
    return f;
}

/* FPCR is only ever written through MOVZ xR, #(fpcr >> 16), LSL #16 ; MSR
 * FPCR, xR, so it only takes the values random_fpcr() makes (FEAT_AFP bits
 * and trap enables, which hao does not model, stay clear). */
static int gen_block(uint32_t *code)
{
    int n = 2 + (int)rr(ORACLE_MAXI - 1);
    bool branch[ORACLE_MAXI] = {false}, notarget[ORACLE_MAXI] = {false};
    for (int k = 0; k < n; k++) {
        unsigned c = rr(12);
        if (c < 3) code[k] = t_producer();
        else if (c < 6) code[k] = t_consumer();
        else if (c < 9) code[k] = t_filler();
        else if (c < 11 || k + 1 == n) branch[k] = true;
        else {
            unsigned r = gpr();
            code[k] = 0xD2A00000u | (random_fpcr() >> 16) << 5 | r;
            if (k + 1 < n) { code[++k] = 0xD51B4400u | r; notarget[k] = true; }
        }
    }
    for (int k = 0; k < n; k++) if (branch[k]) code[k] = t_branch(k, n, notarget);
    return n;
}

/* ---- pac mode. Natively (arm64, non-e process, M4): PAC* and AUT* are the
 * identity, XPACI/XPACD/XPACLRI strip bits 63:47 (55:47 for XPACD with bit
 * 55 clear) to copies of bit 55. The first records enumerate every form x
 * every value x every modifier; the rest draw them at random. */
static const uint64_t pac_vals[] = {
    0, 1, 0x0000000100004000ull, 0x00007ffffffff000ull, 0x0000800000000000ull, 0x0000ffffffffffffull,
    0x0000fffffffff000ull, 0x0001000000000000ull, 0x00ff000100004000ull, 0x0080000000000000ull,
    0x007fffffffffffffull, 0xff00000100004000ull, 0x8000000100004000ull, 0x3ebce676ed7cc604ull,
    0x1234567890abcdefull, 0xfffffe0007001234ull, 0xffff800000000000ull, 0xff7f800000000000ull,
    0x00807fffffffffffull, 0xffffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull,
    0xa5a5a5a5a5a5a5a5ull, 0x5a5a5a5a5a5a5a5aull, 0x0040000000000000ull, 0xfeedfacecafebeefull,
    0x0123c00100004000ull, 0x2000000104a8c3f0ull, 0x0000400000000000ull, 0xffff7fffffffffffull};
static const uint64_t pac_mods[] = {0, 0x1234, ~0ull, 0x0000000100004000ull};
/* HINT #7 (XPACLRI) and #8..#31 except the BTI/CSDB/ESB/PSB/TSB slots */
static const uint8_t pac_hints[] = {7, 8, 10, 12, 14, 24, 25, 26, 27, 28, 29, 30, 31};
#define PAC_NDP 18
#define PAC_NFORMS (PAC_NDP + (int)sizeof pac_hints)
#define PAC_NVALS (int)(sizeof pac_vals / sizeof *pac_vals)
#define PAC_NMODS (int)(sizeof pac_mods / sizeof *pac_mods)

static uint64_t pac_value(void)
{
    uint64_t v = rnd();
    switch (rr(6)) {
    case 0: return pac_vals[rr(PAC_NVALS)];
    case 1: return interesting();
    case 2: return v & 0x00007fffffffffffull;                     /* canonical user */
    case 3: return v | 0xffff800000000000ull;                     /* canonical kernel half */
    case 4: return (v & 0x00007fffffffffffull) | (rnd() & 0xff80000000000000ull);  /* tagged / signed-looking */
    default: return v;
    }
}

/* One pac record: code[0], and x0-x30 and SP with the inputs it reads. */
static uint32_t pac_gen(long rec, oracle_rec *r)
{
    for (int k = 0; k < 31; k++) r->in.x[k] = interesting();
    r->in.sp = interesting();
    int form, vi = -1, mi = -1;
    if (rec < (long)PAC_NFORMS * PAC_NVALS * PAC_NMODS) {
        form = (int)(rec % PAC_NFORMS);
        vi = (int)(rec / PAC_NFORMS % PAC_NVALS);
        mi = (int)(rec / PAC_NFORMS / PAC_NVALS);
    } else form = (int)rr(PAC_NFORMS + 2);
    uint64_t v = vi >= 0 ? pac_vals[vi] : pac_value(), m = mi >= 0 ? pac_mods[mi] : (rr(2) ? pac_value() : interesting());
    uint32_t insn;
    if (form < PAC_NDP) {           /* PACIA..AUTDB Xd, Xn|SP; PACIZA..AUTDZB Xd; XPACI/XPACD Xd */
        unsigned rd = rr(16) ? gpr() : 31, rn = form < 8 ? reg() : 31;
        insn = 0xDAC10000u | (uint32_t)form << 10 | rn << 5 | rd;
        if (rd != 31) r->in.x[rd] = v;
        if (rn == 31) r->in.sp = m;
        else if (rn != rd) r->in.x[rn] = m;
    } else if (form < PAC_NFORMS) { /* x30 (LR forms) or x17 with modifier x16 or SP */
        insn = 0xD503201Fu | (uint32_t)pac_hints[form - PAC_NDP] << 5;
        r->in.x[30] = v;
        r->in.x[17] = rr(2) ? v : pac_value();
        r->in.x[16] = m;
        r->in.sp = rr(4) ? m : pac_value();
    } else if (form == PAC_NFORMS) { /* the 1-source op2 = 00001 space: sf = 0, Rn != 31 for the Z forms, opc > 17 */
        insn = 0x5AC10000u | rr(2) << 31 | rr(64) << 10 | reg() << 5 | reg();
        if ((insn & 31) != 31) r->in.x[insn & 31] = v;
    } else {                        /* the rest of the HINT space */
        insn = 0xD503201Fu | rr(128) << 5;
        r->in.x[30] = v;
    }
    r->in.x[18] = 0;
    return insn;
}

/* ---- fma mode. The encoding, then (after the generic register fill) its
 * operands: one operand class per record, with one lane in six taken from
 * another class so that a vector mixes lanes the fast path admits with lanes
 * it must refuse. Lanes the instruction does not read keep the generic fill
 * (NaNs included). A third of the records alias registers. */
static void set_lane(unsigned __int128 *v, unsigned lane, uint32_t bits)
{
    uint32_t w[4];
    memcpy(w, v, 16);
    w[lane] = bits;
    memcpy(v, w, 16);
}
static uint32_t fma_gen(oracle_rec *r, bool operands)
{
    static unsigned form, q, sub, neg, idx, rd, rn, rm, ra;
    if (!operands) {
        form = rr(8); q = rr(2); sub = rr(2); neg = rr(2); idx = rr(4);
        rd = reg(); rn = reg(); rm = reg(); ra = reg();
        switch (rr(12)) {
        case 0: rn = rd; break;
        case 1: rm = rd; break;
        case 2: rm = rn; break;
        case 3: rn = rm = rd; break;
        default: break;
        }
        if (!rr(3)) ra = rr(3) == 0 ? rd : rr(2) ? rn : rm;
        if (form < 3) return 0x0E20CC00u | q << 30 | sub << 23 | rm << 16 | rn << 5 | rd;      /* FMLA/FMLS vector */
        if (form < 5) return 0x0F801000u | q << 30 | sub << 14 | (idx & 1) << 21 | rm << 16 | (idx >> 1) << 11 | rn << 5 | rd;
        if (form < 6) return 0x5F801000u | sub << 14 | (idx & 1) << 21 | rm << 16 | (idx >> 1) << 11 | rn << 5 | rd;
        return 0x1F000000u | neg << 21 | rm << 16 | sub << 15 | ra << 10 | rn << 5 | rd;      /* FMADD family */
    }
    unsigned lanes = form < 5 ? (q ? 4 : 2) : 1, cat = rr(FC_N), acc = form < 6 ? rd : ra;
    fma_ops o[4];
    for (unsigned l = 0; l < lanes; l++) {
        o[l] = fma_case(&rng, !l || rr(6) ? cat : rr(FC_N));
        if (form >= 3 && l && rr(2)) o[l] = o[0];                 /* by element: lanes built for the shared b */
    }
    /* later writes win when registers alias */
    for (unsigned l = 0; l < lanes; l++) set_lane(&r->in.v[acc], l, o[l].c);
    for (unsigned l = 0; l < lanes; l++) set_lane(&r->in.v[rn], l, o[l].a);
    for (unsigned l = 0; l < lanes && form < 3; l++) set_lane(&r->in.v[rm], l, o[l].b);
    if (form >= 3) set_lane(&r->in.v[rm], form < 6 ? idx : 0, o[0].b);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s OUT COUNT SEED dp|simd|mem|block|pac|fma|known|reserved\n", argv[0]); return 2; }
    long count = atol(argv[2]);
    rng = strtoull(argv[3], 0, 0) | 1;
    int mode = -1;
    for (int m = 0; m < OM_NMODES; m++) if (!strcmp(argv[4], oracle_mode_name[m])) mode = m;
    if (mode < 0) { fprintf(stderr, "unknown mode %s\n", argv[4]); return 2; }
    /* A fixed sweep makes exception coverage independent of the random-mode
     * sample size. Operand state still comes from the requested seed. */
    size_t nu = sizeof known_unimpl / sizeof *known_unimpl;
    size_t nl = sizeof known_lenient / sizeof *known_lenient;
    size_t ns = sizeof encoding_sweeps / sizeof *encoding_sweeps;
    if (mode == OM_KNOWN) count = (long)(ns + nu + nl) * 1024;
    /* Exhaust every non-register selector, including ignored immediate bits.
     * Rm is fixed only in permute and indexed groups; indexed M stays swept. */
    uint32_t sweep_mask[sizeof encoding_sweeps / sizeof *encoding_sweeps];
    long sweep_count = 0;
    for (size_t k = 0; k < ns; k++) {
        sweep_mask[k] = ~encoding_sweeps[k].mask & ~0x3ffu;
        if (k == 7) sweep_mask[k] &= ~0x1f0000u;
        if (k == 8) sweep_mask[k] &= ~0x0f0000u;
        sweep_count += 1L << __builtin_popcount(sweep_mask[k]);
    }
    if (mode == OM_RESERVED) count = sweep_count;
    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror("open"); return 1; }

    void *scr = mmap((void *)ORACLE_SCRATCH, ORACLE_SCRATCH_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (scr == MAP_FAILED) { perror("scratch"); return 1; }
    oracle_fill_ro();
    if (mprotect((void *)(ORACLE_SCRATCH + ORACLE_RO), ORACLE_RO_SIZE, PROT_READ)) { perror("mprotect"); return 1; }
    uint32_t *code = mmap(0, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (code == MAP_FAILED) { perror("jit"); return 1; }
    static harness h;

    /* The code runs with a guest SP; signals are taken on an alternate stack. */
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0};
    sigaltstack(&ss, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    uint64_t host_fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(host_fpcr));

    long written = 0;
    for (long rec = 0; rec < count; rec++) {
        oracle_rec r;
        memset(&r, 0, sizeof r);
        /* pac records: the whole pac mode, and one dp record in eight
         * (make oracle runs dp, not pac) */
        bool pacrec = mode == OM_PAC || (mode == OM_DP && rec % 8 == 7);
        r.mode = (uint32_t)mode;
        if (mode == OM_BLOCK) r.n = (uint32_t)gen_block(r.code);
        else if (pacrec) r.n = 1;
        else if (mode == OM_KNOWN) {
            size_t k = (size_t)rec / 1024;
            const known_enc *e = k < ns ? &encoding_sweeps[k] : k < ns + nu ? &known_unimpl[k - ns] : &known_lenient[k - ns - nu];
            r.code[0] = ((uint32_t)rnd() & ~e->mask) | e->value;
            r.n = 1;
        }
        else if (mode == OM_RESERVED) {
            long index = rec;
            size_t k = 0;
            while (index >= (1L << __builtin_popcount(sweep_mask[k])))
                index -= 1L << __builtin_popcount(sweep_mask[k++]);
            uint32_t insn = encoding_sweeps[k].value | (1u << 5);
            if (k >= 7) insn |= 2u << 16;
            for (unsigned b = 10; b < 32; b++)
                if (sweep_mask[k] & (1u << b)) { insn |= (index & 1) << b; index >>= 1; }
            r.code[0] = insn;
            r.n = 1;
        }
        else if (mode == OM_FMA) { r.code[0] = fma_gen(&r, false); r.n = 1; }
        else {
            uint32_t insn = (uint32_t)rnd();
            static const uint32_t dp[] = {0x8, 0x9, 0x5, 0xD}, simd[] = {0x7, 0xF}, mem[] = {0x4, 0x6, 0xC, 0xE};
            if (mode == OM_DP) insn = (insn & ~(0xFu << 25)) | dp[rr(4)] << 25;
            else if (mode == OM_SIMD) {
                insn = (insn & ~(0xFu << 25)) | simd[rr(2)] << 25;
                if (!rr(8)) {   /* see crypto[] */
                    unsigned g = rr(sizeof crypto / sizeof *crypto);
                    insn = ((uint32_t)rnd() & ~crypto[g].mask) | crypto[g].value;
                }
            }
            else {
                insn = (insn & ~(0xFu << 25)) | mem[rr(4)] << 25;
                if ((insn & 0x3B000000) == 0x18000000) { rec--; continue; }  /* literal loads read code memory */
            }
            if ((insn & 0x7FE0FC00) == 0x1AC03000) { rec--; continue; }       /* PACGA uses real keys */
            r.code[0] = insn;
            r.n = 1;
        }
        /* pac: the encoding and its x/SP inputs, copied over the generic fill */
        oracle_rec pin;
        if (pacrec) {
            memset(&pin, 0, sizeof pin);
            r.code[0] = pac_gen(mode == OM_PAC ? rec : rec / 8, &pin);
        }
        bool skip = false;
        /* x18 is reserved, x0 is the harness temp, BL writes x30; the HINT
         * PAC forms read x16/x17 */
        uint32_t used = 1u << 18 | 1u | 1u << 30 | (pacrec ? 3u << 16 : 0);
        /* The reserved sweep fixes GPR operands at x0/x1. Its variable
         * fields name only SIMD registers or selectors, including value 18. */
        for (uint32_t k = 0; k < r.n; k++) {
            if ((mode != OM_RESERVED && names18(r.code[k]) && !crypto_vonly(r.code[k])) || (r.code[k] & 0x7FE0FC00) == 0x1AC03000) skip = true;   /* PACGA uses real keys */
            used |= field_regs(r.code[k]);
        }
        if (skip || (used & 0x7fffffffu) == 0x7fffffffu) { rec--; continue; }
        unsigned s;
        do s = rr(31); while (used >> s & 1);
        r.sreg = s;
        r.seed = rnd();

        for (int k = 0; k < 31; k++) {
            if (mode == OM_MEM) {
                unsigned c = rr(8);
                if (c < 5) r.in.x[k] = ORACLE_SCRATCH + 0x8000 + ((rnd() & 0x1ff) << ((rnd() & 1) ? 0 : 3)) - 0x400;
                else if (c < 6) r.in.x[k] = ORACLE_SCRATCH + ORACLE_RO + ((rnd() & 0xff) << ((rnd() & 1) ? 0 : 4));
                else r.in.x[k] = rnd() & 0xff;   /* small index */
            } else r.in.x[k] = interesting();
        }
        r.in.x[18] = 0;
        if (pacrec) { memcpy(r.in.x, pin.in.x, sizeof r.in.x); r.in.sp = pin.in.sp; }
        else if (mode == OM_MEM) r.in.sp = rr(4) ? ORACLE_SCRATCH + 0x8000 + (rnd() & 0x3f0) - 0x200 : ORACLE_SCRATCH + ORACLE_RO + (rnd() & 0x3f0);
        else r.in.sp = rr(2) ? (interesting() & ~15ull) : interesting();
        for (int k = 0; k < 32; k++) r.in.v[k] = (unsigned __int128)interesting() << 64 | interesting();
        r.in.nzcv = (uint32_t)(rnd() & 0xf) << 28;
        r.in.fpcr = (mode == OM_SIMD || mode == OM_KNOWN || mode == OM_BLOCK) ? random_fpcr() : 0;
        r.in.fpsr = rr(4) ? 0 : (uint32_t)(rnd() & 0x0800009f);
        if (mode == OM_FMA) {
            fma_gen(&r, true);
            r.in.fpcr = rr(16) << 22;                        /* RMode, FZ, DN: all sixteen */
            r.in.fpsr = rr(2) ? 0 : (uint32_t)(rnd() & 0x0800009f);
        }

        /* harness: prologue, code, epilogue */
        uint32_t *p = code;
        pthread_jit_write_protect_np(0);
        *p++ = 0xAA0003E0u | s;                              /* mov xs, x0 */
        *p++ = i_ldr_w(0, s, OS_NZCV); *p++ = I_MSR_NZCV_X0;
        *p++ = i_ldr_w(0, s, OS_FPCR); *p++ = I_MSR_FPCR_X0;
        *p++ = i_ldr_w(0, s, OS_FPSR); *p++ = I_MSR_FPSR_X0;
        *p++ = 0x91000000u | OS_V << 10 | s << 5 | 0;        /* add x0, xs, #OS_V */
        for (unsigned v = 0; v < 32; v += 2) *p++ = i_ldp_q(v, v + 1, 0, 16 * v);
        *p++ = i_ldr_x(0, s, OS_SP); *p++ = 0x9100001Fu;     /* mov sp, x0 */
        for (unsigned k = 0; k < 31; k++) if (k != s && k != 18) *p++ = i_ldr_x(k, s, 8 * k);
        uint32_t *c0 = p;
        for (uint32_t k = 0; k < r.n; k++) *p++ = r.code[k];
        for (unsigned k = 0; k < 31; k++) if (k != s && k != 18) *p++ = i_str_x(k, s, 8 * k);
        *p++ = 0x910003E0u; *p++ = i_str_x(0, s, OS_SP);     /* mov x0, sp */
        *p++ = I_MRS_X0_NZCV; *p++ = i_str_w(0, s, OS_NZCV);
        *p++ = I_MRS_X0_FPCR; *p++ = i_str_w(0, s, OS_FPCR);
        *p++ = I_MRS_X0_FPSR; *p++ = i_str_w(0, s, OS_FPSR);
        *p++ = 0x91000000u | OS_V << 10 | s << 5 | 0;
        for (unsigned v = 0; v < 32; v += 2) *p++ = i_stp_q(v, v + 1, 0, 16 * v);
        *p++ = i_ldr_x(0, s, H_SP); *p++ = 0x9100001Fu;
        *p++ = i_ldr_x(0, s, H_FPCR); *p++ = I_MSR_FPCR_X0;
        *p++ = i_ldr_x(30, s, H_LR);
        *p++ = 0xD65F03C0u;                                  /* ret */
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(code, (size_t)((uint8_t *)p - (uint8_t *)code));
        r.pc = (uint64_t)(uintptr_t)c0;

        h.st = r.in;
        oracle_fill(r.seed);
        sig = 0;
        if (sigsetjmp(jb, 1) == 0) {
            oracle_call(&h, code);
            r.status = 0;
        } else {
            r.status = sig == SIGILL ? 1 : 2;
            __asm__ volatile("msr fpcr, %0" : : "r"(host_fpcr));
        }
        r.out = h.st;
        r.out.pad = (uint32_t)sig;          /* signal number when status != 0 */
        r.out.x[s] = r.in.x[s];
        r.out.x[18] = 0;
        r.memhash = oracle_memhash();
        memcpy(r.mem_after, (void *)(ORACLE_SCRATCH + ORACLE_MEMWIN), sizeof r.mem_after);
        fwrite(&r, sizeof r, 1, out);
        written++;
    }
    fclose(out);
    fprintf(stderr, "oracle: %ld records (%s)\n", written, argv[4]);
    return 0;
}
