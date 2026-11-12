/*
 * hao — arm64 FP / Advanced SIMD reference interpreter.
 *
 * Element-wise C implementations. Coverage is driven by what Darwin's
 * libsystem_platform/libsystem_c/libc++ actually execute; anything not
 * recognised returns -1 and the caller reports UNDEF with the encoding.
 */
#include "hao/cpu.h"
#include "a64_fp.h"
#include <string.h>
#include <stdlib.h>
#include <emmintrin.h>

#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(x, n) (((x) >> (n)) & 1u)

typedef unsigned __int128 u128;

static inline uint64_t rx(hao_cpu *c, unsigned r) { return r == 31 ? 0 : c->x[r]; }
static inline void wx(hao_cpu *c, unsigned r, uint64_t v) { if (r != 31) c->x[r] = v; }
static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

/* ---- element access (esize in bytes) */
static inline uint64_t eget(const hao_vreg *v, int es, int idx)
{
    switch (es) {
    case 1: return v->b[idx];
    case 2: return v->h[idx];
    case 4: return v->s[idx];
    default: return v->d[idx];
    }
}
static inline void eset(hao_vreg *v, int es, int idx, uint64_t x)
{
    switch (es) {
    case 1: v->b[idx] = (uint8_t)x; break;
    case 2: v->h[idx] = (uint16_t)x; break;
    case 4: v->s[idx] = (uint32_t)x; break;
    default: v->d[idx] = x; break;
    }
}
static inline int64_t esget(const hao_vreg *v, int es, int idx) { return sext(eget(v, es, idx), es * 8); }
static inline uint64_t emask(int es) { return es == 8 ? ~0ull : ((1ull << (es * 8)) - 1); }

static void vfinish(hao_cpu *c, unsigned rd, const hao_vreg *r, int Q)
{
    c->v[rd] = *r;
    if (!Q) c->v[rd].d[1] = 0;
}

/* ---- scalar FP results: the rest of the vector register is zeroed */
static inline void wfs(hao_cpu *c, unsigned r, uint64_t v) { memset(&c->v[r], 0, 16); c->v[r].s[0] = (uint32_t)v; }
static inline void wfd(hao_cpu *c, unsigned r, uint64_t v) { memset(&c->v[r], 0, 16); c->v[r].d[0] = v; }
static inline void wfh(hao_cpu *c, unsigned r, uint64_t v) { memset(&c->v[r], 0, 16); c->v[r].h[0] = (uint16_t)v; }
static inline void wfe(hao_cpu *c, unsigned r, int es, uint64_t v) { if (es == 8) wfd(c, r, v); else if (es == 4) wfs(c, r, v); else wfh(c, r, v); }
static inline uint64_t rfe(hao_cpu *c, unsigned r, int es) { return es == 8 ? c->v[r].d[0] : es == 4 ? c->v[r].s[0] : c->v[r].h[0]; }

/* ------------------------------------------------ FP environment (cpu.h) */

/* FPCR bits that read back as written on Apple silicon (measured on an M4:
 * FIZ/AH/NEP, the trap enables, Len, FZ16, Stride, RMode, FZ, DN, AHP). */
#define FPCR_WRITABLE 0x07FF9F07u
#define FPCR_RMODE_SHIFT 22
#define FPCR_FZ  (1u << 24)
#define FPCR_DN  (1u << 25)
#define FPSR_QC  (1u << 27)
#define FPSR_IDC (1u << 7)
#define MXCSR_DAZ (1u << 6)
#define MXCSR_FTZ (1u << 15)

static inline uint32_t mxcsr_get(void) { uint32_t m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static inline void mxcsr_set(uint32_t m) { __asm__ volatile("ldmxcsr %0" : : "m"(m)); }
/* Native long double code (x87: native libraries that compute in long double) keeps
 * its own rounding control and exception flags. The guest's rounding mode is
 * mirrored into the x87 control word, and x87 exception flags count as
 * raised, as x86_64 libm's fetestexcept counts them. RC and the IE/ZE/OE/UE/PE
 * bits have the MXCSR's encoding (RC at bit 10 instead of 13). */
static inline void x87_rc_set(unsigned rc)
{
    uint16_t cw, n;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    n = (uint16_t)((cw & ~0x0c00u) | rc << 10);
    if (n != cw) __asm__ volatile("fldcw %0" : : "m"(n));
}
static inline uint32_t x87_flags(void) { uint16_t sw; __asm__ volatile("fnstsw %0" : "=m"(sw)); return sw & 0x3du; }
/* ARM RMode (0 RN, 1 RP, 2 RM, 3 RZ) <-> MXCSR.RC (0 RN, 1 RM, 2 RP, 3 RZ): swap 1 and 2. */
static inline unsigned rm_swap(unsigned r) { return (r == 1 || r == 2) ? 3 - r : r; }

uint32_t a64_fpcr_read(hao_cpu *c)
{
    uint32_t m = mxcsr_get();
    uint32_t v = c->fpcr & ~(3u << FPCR_RMODE_SHIFT | FPCR_FZ);
    v |= rm_swap((m >> 13) & 3) << FPCR_RMODE_SHIFT;
    if (m & MXCSR_FTZ) v |= FPCR_FZ;
    return v;
}

void a64_fpcr_write(hao_cpu *c, uint32_t v)
{
    v &= FPCR_WRITABLE;
    c->fpcr = v;
    uint32_t m = mxcsr_get() & ~(3u << 13 | MXCSR_FTZ | MXCSR_DAZ);
    m |= rm_swap((v >> FPCR_RMODE_SHIFT) & 3) << 13;
    if (v & FPCR_FZ) m |= MXCSR_FTZ | MXCSR_DAZ;
    mxcsr_set(m);
    x87_rc_set(rm_swap((v >> FPCR_RMODE_SHIFT) & 3));
}

/* FPSR IOC/DZC/OFC/UFC/IXC (bits 0-4) <-> MXCSR IE/ZE/OE/UE/PE (bits 0, 2-5). */
uint32_t a64_fpsr_read(hao_cpu *c)
{
    uint32_t m = mxcsr_get() | x87_flags();
    uint32_t f = (m & 1) | ((m >> 1) & 0x1e);
    return (c->fpsr & (FPSR_QC | FPSR_IDC)) | f;
}

void a64_fpsr_write(hao_cpu *c, uint32_t v)
{
    c->fpsr = v & (FPSR_QC | FPSR_IDC);
    uint32_t m = mxcsr_get() & ~0x3fu;
    m |= (v & 1) | ((v & 0x1e) << 1);
    mxcsr_set(m);
    if (x87_flags()) __asm__ volatile("fnclex");   /* the flags still raised are in the MXCSR */
}

/* ------------------------------------------------- vector loads/stores */

/* Structure element widths are decoded as 1/2/4/8. Constant-size copies
 * become unaligned scalar accesses without violating C alignment or aliasing. */
static inline uint64_t vread_element(uint64_t addr, int es)
{
    const void *p = (const void *)(uintptr_t)hao_tbi(addr);
    switch (es) {
    case 1: { uint8_t x; memcpy(&x, p, 1); return x; }
    case 2: { uint16_t x; memcpy(&x, p, 2); return x; }
    case 4: { uint32_t x; memcpy(&x, p, 4); return x; }
    default: { uint64_t x; memcpy(&x, p, 8); return x; }
    }
}

static inline void vwrite_element(uint64_t addr, int es, uint64_t x)
{
    void *p = (void *)(uintptr_t)hao_tbi(addr);
    /* Keep memcpy's existing partial-store behavior at a page boundary.
     * All supported host VM pages are multiples of 4K; a contained scalar
     * access cannot introduce a new protection-boundary crossing. */
    if (__builtin_expect(((uintptr_t)p & 4095u) + (unsigned)es > 4096u, 0)) {
        memcpy(p, &x, (size_t)es);
        return;
    }
    switch (es) {
    case 1: { uint8_t v = (uint8_t)x; memcpy(p, &v, 1); break; }
    case 2: { uint16_t v = (uint16_t)x; memcpy(p, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)x; memcpy(p, &v, 4); break; }
    default: memcpy(p, &x, 8); break;
    }
}

void a64_vload(hao_cpu *c, unsigned vt, uint64_t addr, int size)
{
    hao_vreg v;
    memset(&v, 0, sizeof v);
    memcpy(&v, (const void *)(uintptr_t)hao_tbi(addr), (size_t)size);
    c->v[vt] = v;
}

void a64_vstore(hao_cpu *c, unsigned vt, uint64_t addr, int size)
{
    memcpy((void *)(uintptr_t)hao_tbi(addr), &c->v[vt], (size_t)size);
}

int a64_ldst_simd(hao_cpu *c, uint32_t i, uint64_t pc)
{
    (void)pc;
    unsigned Q = BIT(i, 30), L = BIT(i, 22), rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rt = BITS(i, 4, 0);
    int post = BIT(i, 23);
    uint64_t base = c->x[rn], addr = base, total = 0;
    if (!post && rm != 0) return -1;   /* the no-offset forms have Rm = 00000 */

    if (!BIT(i, 24)) {  /* multiple structures */
        unsigned opc = BITS(i, 15, 12), size = BITS(i, 11, 10);
        int es = 1 << size, nelem = (Q ? 16 : 8) / es, regs, selem;
        if (BIT(i, 21)) return -1;
        switch (opc) {
        case 0x0: regs = 4; selem = 4; break;
        case 0x2: regs = 4; selem = 1; break;
        case 0x4: regs = 3; selem = 3; break;
        case 0x6: regs = 3; selem = 1; break;
        case 0x7: regs = 1; selem = 1; break;
        case 0x8: regs = 2; selem = 2; break;
        case 0xA: regs = 2; selem = 1; break;
        default: return -1;
        }
        if (selem > 1 && size == 3 && !Q) return -1;   /* LD2-4/ST2-4 .1D: reserved */
        if (selem == 1) {
            for (int r = 0; r < regs; r++) {
                unsigned v = (rt + r) & 31;
                if (L) { a64_vload(c, v, addr, Q ? 16 : 8); }
                else a64_vstore(c, v, addr, Q ? 16 : 8);
                addr += Q ? 16 : 8;
            }
        } else {
            hao_vreg tmp[4];
            for (int s = 0; s < selem; s++) tmp[s] = c->v[(rt + s) & 31];
            for (int e = 0; e < nelem; e++)
                for (int s = 0; s < selem; s++) {
                    if (L) { uint64_t x = vread_element(addr, es); eset(&tmp[s], es, e, x); }
                    else vwrite_element(addr, es, eget(&tmp[s], es, e));
                    addr += (uint64_t)es;
                }
            if (L) for (int s = 0; s < selem; s++) { c->v[(rt + s) & 31] = tmp[s]; if (!Q) c->v[(rt + s) & 31].d[1] = 0; }
        }
        total = addr - base;
    } else {  /* single structure */
        unsigned opc = BITS(i, 15, 13), S = BIT(i, 12), size = BITS(i, 11, 10), R = BIT(i, 21);
        int selem = (int)((BIT(opc, 0) << 1 | R) + 1), es, idx;
        if (opc == 6 || opc == 7) {  /* LDnR replicate */
            if (!L || S) return -1;
            es = 1 << size;
            for (int s = 0; s < selem; s++) {
                uint64_t x = vread_element(addr, es);
                hao_vreg v;
                memset(&v, 0, sizeof v);
                for (int e = 0; e < (Q ? 16 : 8) / es; e++) eset(&v, es, e, x);
                c->v[(rt + s) & 31] = v;
                addr += (uint64_t)es;
            }
        } else {
            switch (opc >> 1) {
            case 0: es = 1; idx = (int)(Q << 3 | S << 2 | size); break;
            case 1: if (size & 1) return -1; es = 2; idx = (int)(Q << 2 | S << 1 | (size >> 1)); break;
            case 2:
                if (size == 0) { es = 4; idx = (int)(Q << 1 | S); }
                else if (size == 1 && !S) { es = 8; idx = (int)Q; }
                else return -1;
                break;
            default: return -1;
            }
            for (int s = 0; s < selem; s++) {
                unsigned v = (rt + s) & 31;
                if (L) { uint64_t x = vread_element(addr, es); eset(&c->v[v], es, idx, x); }
                else vwrite_element(addr, es, eget(&c->v[v], es, idx));
                addr += (uint64_t)es;
            }
        }
        total = addr - base;
    }
    if (post) c->x[rn] = base + (rm == 31 ? total : rx(c, rm));
    return 0;
}

/* ------------------------------------------------------ scalar FP */

/* FP data-processing (2 source) opcodes 0-8 */
static uint64_t fp_op2(fpctx *x, unsigned op, int es, uint64_t a, uint64_t b)
{
    switch (op) {
    case 0: return fp_mul(x, es, a, b);
    case 1: return fp_div(x, es, a, b);
    case 2: return fp_add(x, es, a, b);
    case 3: return fp_sub(x, es, a, b);
    case 4: return fp_max(x, es, a, b);
    case 5: return fp_min(x, es, a, b);
    case 6: return fp_maxnm(x, es, a, b);
    case 7: return fp_minnm(x, es, a, b);
    default: return fp_neg(fp_mul(x, es, a, b), es);   /* FNMUL: FPNeg(FPMul()) */
    }
}

static int fp_scalar(hao_cpu *c, uint32_t i)
{
    unsigned type = BITS(i, 23, 22), rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    fpctx x = fp_ctx(c);

    /* FMOV Xd, Vn.D[1] / FMOV Vn.D[1], Xd: type 10, rmode 01, opc 11x, sf 1 */
    if ((i & 0xFFFEFC00) == 0x9EAE0000) {
        if (BIT(i, 16)) c->v[rd].d[1] = rx(c, rn); else wx(c, rd, c->v[rn].d[1]);
        return 0;
    }
    if (type == 2 || BIT(i, 29)) return -1;
    int es = type == 1 ? 8 : type == 0 ? 4 : 2;

    /* conversions between FP and integer */
    if ((i & 0x5F20FC00) == 0x1E200000) {
        int sf = BIT(i, 31), bits = sf ? 64 : 32;
        unsigned rmode = BITS(i, 20, 19), opc = BITS(i, 18, 16);
        uint64_t a = rfe(c, rn, es);
        switch (rmode << 3 | opc) {
        case 0x00: case 0x01: wx(c, rd, fp_to_int(&x, es, a, FPR_N, opc & 1, bits, 0) & (sf ? ~0ull : 0xffffffffull)); return 0;
        case 0x04: case 0x05: wx(c, rd, fp_to_int(&x, es, a, FPR_A, opc & 1, bits, 0) & (sf ? ~0ull : 0xffffffffull)); return 0;
        case 0x08: case 0x09: wx(c, rd, fp_to_int(&x, es, a, FPR_P, opc & 1, bits, 0) & (sf ? ~0ull : 0xffffffffull)); return 0;
        case 0x10: case 0x11: wx(c, rd, fp_to_int(&x, es, a, FPR_M, opc & 1, bits, 0) & (sf ? ~0ull : 0xffffffffull)); return 0;
        case 0x18: case 0x19: wx(c, rd, fp_to_int(&x, es, a, FPR_Z, opc & 1, bits, 0) & (sf ? ~0ull : 0xffffffffull)); return 0;
        case 0x02: case 0x03:  /* SCVTF / UCVTF */
            wfe(c, rd, es, fp_from_int(&x, es, rx(c, rn), opc == 2, bits, 0));
            return 0;
        case 0x06:  /* FMOV general <- fp: Wd/Xd <- Hn, Wd <- Sn, Xd <- Dn */
            if ((es == 4 && sf) || (es == 8 && !sf)) return -1;
            wx(c, rd, a);
            return 0;
        case 0x07:  /* FMOV fp <- general */
            if ((es == 4 && sf) || (es == 8 && !sf)) return -1;
            wfe(c, rd, es, rx(c, rn) & (es == 8 ? ~0ull : (1ull << (es * 8)) - 1));
            return 0;
        case 0x1E: {  /* FJCVTZS */
            if (es != 8 || sf) return -1;
            bool exact;
            uint32_t r = fp_jcvt(&x, a, &exact);
            wx(c, rd, r);
            hao_set_nzcv(c, exact ? 4 : 0);
            return 0;
        }
        default: return -1;
        }
    }
    /* conversions between FP and fixed point */
    if ((i & 0x5F200000) == 0x1E000000) {
        int sf = BIT(i, 31), bits = sf ? 64 : 32;
        unsigned rmode = BITS(i, 20, 19), opc = BITS(i, 18, 16), scale = BITS(i, 15, 10);
        int fbits = 64 - (int)scale;
        if (!sf && scale < 32) return -1;
        if (rmode == 0 && (opc == 2 || opc == 3)) {
            wfe(c, rd, es, fp_from_int(&x, es, rx(c, rn), opc == 2, bits, fbits));
            return 0;
        }
        if (rmode == 3 && (opc == 0 || opc == 1)) {
            wx(c, rd, fp_to_int(&x, es, rfe(c, rn, es), FPR_Z, opc & 1, bits, fbits) & (sf ? ~0ull : 0xffffffffull));
            return 0;
        }
        return -1;
    }
    if (BIT(i, 31)) return -1;
    if ((i & 0x5F203C00) == 0x1E202000) {  /* FCMP / FCMPE */
        if (BITS(i, 15, 14) || BITS(i, 2, 0)) return -1;
        uint64_t b = BIT(i, 3) ? 0 : rfe(c, rm, es);
        hao_set_nzcv(c, fp_cmp(&x, es, rfe(c, rn, es), b, BIT(i, 4)));
        return 0;
    }
    if ((i & 0x5F201C00) == 0x1E201000) {  /* FMOV (scalar, immediate) */
        if (BITS(i, 9, 5)) return -1;
        unsigned imm8 = BITS(i, 20, 13);
        unsigned sgn = imm8 >> 7, b6 = (imm8 >> 6) & 1, frac = imm8 & 0x3f;
        if (es == 8)
            wfd(c, rd, (uint64_t)sgn << 63 | ((uint64_t)(!b6) << 62) | ((uint64_t)(b6 ? 0xff : 0) << 54) | (uint64_t)frac << 48);
        else if (es == 2)
            wfh(c, rd, (uint16_t)(sgn << 15 | (!b6) << 14 | (b6 ? 3 : 0) << 12 | frac << 6));
        else
            wfs(c, rd, sgn << 31 | (!b6) << 30 | (b6 ? 0x1f : 0) << 25 | frac << 19);
        return 0;
    }
    if ((i & 0x5F200C00) == 0x1E200400) {  /* FCCMP / FCCMPE */
        if (hao_cond(hao_nzcv(c), BITS(i, 15, 12)))
            hao_set_nzcv(c, fp_cmp(&x, es, rfe(c, rn, es), rfe(c, rm, es), BIT(i, 4)));
        else hao_set_nzcv(c, BITS(i, 3, 0));
        return 0;
    }
    if ((i & 0x5F200C00) == 0x1E200C00) {  /* FCSEL */
        unsigned src = hao_cond(hao_nzcv(c), BITS(i, 15, 12)) ? rn : rm;
        wfe(c, rd, es, rfe(c, src, es));
        return 0;
    }
    if ((i & 0x5F200C00) == 0x1E200800) {  /* FP data-processing 2-source */
        unsigned op = BITS(i, 15, 12);
        if (op > 8) return -1;
        wfe(c, rd, es, fp_op2(&x, op, es, rfe(c, rn, es), rfe(c, rm, es)));
        return 0;
    }
    if ((i & 0x5F207C00) == 0x1E204000) {  /* FP data-processing 1-source */
        unsigned op = BITS(i, 20, 15);
        uint64_t a = rfe(c, rn, es), r;
        switch (op) {
        case 0: r = a; break;
        case 1: r = fp_abs(a, es); break;
        case 2: r = fp_neg(a, es); break;
        case 3: r = fp_sqrt(&x, es, a); break;
        case 4: case 5: case 7: {   /* FCVT to single/double/half */
            int to = op == 4 ? 4 : op == 5 ? 8 : 2;
            if (to == es) return -1;
            wfe(c, rd, to, fp_cvt(&x, a, es, to));
            return 0;
        }
        case 8: r = fp_rint(&x, es, a, FPR_N, false); break;
        case 9: r = fp_rint(&x, es, a, FPR_P, false); break;
        case 10: r = fp_rint(&x, es, a, FPR_M, false); break;
        case 11: r = fp_rint(&x, es, a, FPR_Z, false); break;
        case 12: r = fp_rint(&x, es, a, FPR_A, false); break;
        case 14: r = fp_rint(&x, es, a, fp_cur(&x), true); break;   /* FRINTX */
        case 15: r = fp_rint(&x, es, a, fp_cur(&x), false); break;  /* FRINTI */
        default: return -1;
        }
        wfe(c, rd, es, r);
        return 0;
    }
    if ((i & 0x5F000000) == 0x1F000000) {  /* FMADD, FMSUB, FNMADD, FNMSUB */
        unsigned ra = BITS(i, 14, 10), o1 = BIT(i, 21), o0 = BIT(i, 15);
        uint64_t acc = rfe(c, ra, es), n = rfe(c, rn, es), m = rfe(c, rm, es);
        if (o1) acc = fp_neg(acc, es);              /* FNMADD, FNMSUB */
        if (o0 != o1) n = fp_neg(n, es);            /* FMSUB, FNMADD */
        wfe(c, rd, es, fp_muladd(&x, es, acc, n, m));
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------- Advanced SIMD */

static uint64_t sat_s(int64_t v, int es, bool *sat)
{
    int64_t hi = (int64_t)(emask(es) >> 1), lo = -hi - 1;
    if (es == 8) return (uint64_t)v;
    if (v > hi) { *sat = true; return (uint64_t)hi & emask(es); }
    if (v < lo) { *sat = true; return (uint64_t)lo & emask(es); }
    return (uint64_t)v & emask(es);
}
/* Signed saturating doubling multiply-high, with optional rounding.
 * The doubled 32-bit minimum product is +2^63, so retain it in 128 bits.
 * Adding half before the arithmetic shift rounds ties toward +infinity. */
static uint64_t signed_mulhigh(int64_t x, int64_t y, int es, bool round, bool *sat)
{
    __int128 p = (__int128)x * y * 2;
    if (round) p += (__int128)1 << (es * 8 - 1);
    return sat_s((int64_t)(p >> (es * 8)), es, sat);
}

/* Widened saturating products can reach +2^63 and accumulation can
 * exceed either signed 64-bit bound. Keep both saturation stages exact. */
static uint64_t sat_s128(__int128 v, int es, bool *sat)
{
    __int128 hi = ((__int128)1 << (es * 8 - 1)) - 1, lo = -hi - 1;
    if (v > hi) { *sat = true; v = hi; }
    if (v < lo) { *sat = true; v = lo; }
    return (uint64_t)v & emask(es);
}

static uint64_t signed_mull(int64_t a, int64_t b, int64_t acc, int es, int op, bool *sat)
{
    uint64_t product = sat_s128((__int128)a * b * 2, es * 2, sat);
    if (op == 2) return product;
    __int128 wide = sext(product, es * 16);
    return sat_s128((__int128)acc + (op ? -wide : wide), es * 2, sat);
}

/* SQRDMLAH/SH round and saturate the combined product and accumulator,
 * rather than first saturating SQRDMULH's product. */
static uint64_t signed_rdmla(int64_t a, int64_t b, int64_t acc, int es, bool sub, bool *sat)
{
    __int128 product = (__int128)a * b * 2;
    __int128 sum = (__int128)acc * ((__int128)1 << (es * 8));
    sum += (sub ? -product : product) + ((__int128)1 << (es * 8 - 1));
    return sat_s128(sum >> (es * 8), es, sat);
}

static int simd_rdmla(hao_cpu *c, uint32_t i)
{
    unsigned size = BITS(i, 23, 22), Q = BIT(i, 30), scalar = BIT(i, 28);
    if ((size != 1 && size != 2) || (scalar && !Q)) return -1;
    int es = 1 << size, n = scalar ? 1 : (Q ? 16 : 8) / es;
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    hao_vreg a = c->v[rn], b = c->v[rm], d = c->v[rd], r = {0};
    bool sat = false;
    for (int e = 0; e < n; e++)
        eset(&r, es, e, signed_rdmla(esget(&a, es, e), esget(&b, es, e),
                                  esget(&d, es, e), es, BIT(i, 11), &sat));
    if (sat) c->fpsr |= FPSR_QC;
    vfinish(c, rd, &r, scalar ? 0 : Q);
    return 0;
}

static uint64_t sat_u(int64_t v, int es, bool *sat)
{
    if (es == 8) return (uint64_t)v;
    if (v < 0) { *sat = true; return 0; }
    if ((uint64_t)v > emask(es)) { *sat = true; return emask(es); }
    return (uint64_t)v;
}

static uint64_t shl_reg(uint64_t a, int64_t sh, int es, bool u, bool rnd)
{
    int bits = es * 8;
    sh = (int8_t)sh;
    if (sh >= 0) return sh >= bits ? 0 : (a << sh) & emask(es);
    sh = -sh;
    if (u) {
        if (sh > bits) return 0;
        u128 v = (u128)(a & emask(es));
        if (rnd) v += (u128)1 << (sh - 1);
        return (uint64_t)(v >> sh) & emask(es);
    }
    int64_t s = sext(a, bits);
    if (sh >= bits) return rnd ? 0 : (uint64_t)(s < 0 ? -1 : 0) & emask(es);
    __int128 v = s;
    if (rnd) v += (__int128)1 << (sh - 1);
    return (uint64_t)(v >> sh) & emask(es);
}

/* Saturation is only possible for a left shift. Compare before shifting,
 * including counts at least as wide as the lane; never shift a signed value
 * or invoke C's undefined large-count shifts. Right shifts share rounding
 * and sign-extension with SSHL/USHL. */
static uint64_t sat_shl_reg(uint64_t a, int sh, int es, bool u, bool rnd, bool *sat)
{
    int bits = es * 8;
    sh = (int8_t)sh;
    a &= emask(es);
    if (sh <= 0) return shl_reg(a, sh, es, u, rnd);
    if (!a) return 0;
    if (u) {
        if (sh >= bits || a > (emask(es) >> sh)) {
            *sat = true;
            return emask(es);
        }
    } else {
        int64_t value = sext(a, bits);
        int64_t hi = (int64_t)(emask(es) >> 1), lo = -hi - 1;
        if (sh >= bits || value > (hi >> sh) || value < (lo >> sh)) {
            *sat = true;
            return (uint64_t)(value < 0 ? lo : hi) & emask(es);
        }
    }
    return (a << sh) & emask(es);
}

/* Integer helper paths use only baseline SSE2. Read every input before the
 * caller writes Rd: upper/lower source selection and Rd aliases are distinct
 * from the destination's Q-dependent clearing/preservation rules. */
static inline __m128i vi(const hao_vreg *v) { return _mm_loadu_si128((const __m128i *)v); }
static inline hao_vreg vr(__m128i x) { hao_vreg v; _mm_storeu_si128((__m128i *)&v, x); return v; }

static inline __m128i iadd(__m128i a, __m128i b, int es)
{
    switch (es) {
    case 1: return _mm_add_epi8(a, b);
    case 2: return _mm_add_epi16(a, b);
    case 4: return _mm_add_epi32(a, b);
    default: return _mm_add_epi64(a, b);
    }
}

static inline __m128i isub(__m128i a, __m128i b, int es)
{
    switch (es) {
    case 1: return _mm_sub_epi8(a, b);
    case 2: return _mm_sub_epi16(a, b);
    case 4: return _mm_sub_epi32(a, b);
    default: return _mm_sub_epi64(a, b);
    }
}

static inline __m128i widen_low(__m128i a, int es, bool unsign)
{
    __m128i z = _mm_setzero_si128();
    if (es == 1) return _mm_unpacklo_epi8(a, unsign ? z : _mm_cmpgt_epi8(z, a));
    return _mm_unpacklo_epi16(a, unsign ? z : _mm_cmpgt_epi16(z, a));
}

/* Eight byte products or four halfword products, in twice-wide lanes. */
static inline __m128i mul_long(__m128i a, __m128i b, int es, bool unsign)
{
    if (es == 1) return _mm_mullo_epi16(widen_low(a, es, unsign), widen_low(b, es, unsign));
    __m128i lo = _mm_mullo_epi16(a, b);
    __m128i hi = unsign ? _mm_mulhi_epu16(a, b) : _mm_mulhi_epi16(a, b);
    return _mm_unpacklo_epi16(lo, hi);
}

static inline __m128i mulhigh_half(__m128i a, __m128i b, bool round, int bytes, bool *sat)
{
    __m128i lo = _mm_mullo_epi16(a, b), hi = _mm_mulhi_epi16(a, b);
    __m128i r = _mm_add_epi16(_mm_slli_epi16(hi, 1), _mm_srli_epi16(lo, 15));
    if (round) r = _mm_add_epi16(r, _mm_and_si128(_mm_srli_epi16(lo, 14), _mm_set1_epi16(1)));
    /* The only overflow is MIN * MIN. Ignore inactive scalar/Q0 lanes when
     * setting cumulative QC, even if their temporary SSE results saturate. */
    __m128i min = _mm_set1_epi16((short)0x8000);
    __m128i overflow = _mm_and_si128(_mm_cmpeq_epi16(a, min), _mm_cmpeq_epi16(b, min));
    if ((unsigned)_mm_movemask_epi8(overflow) & ((1u << bytes) - 1)) *sat = true;
    /* Penryn-target Clang fuses the intrinsic mask select into SSE4 PBLENDVB.
     * Spell out its SSE2 equivalent to keep this path on the baseline ISA. */
    __m128i limit = _mm_set1_epi16(0x7fff);
    __asm__("pand %0, %1\n\tpandn %2, %0\n\tpor %1, %0"
            : "+&x"(overflow), "+&x"(limit) : "x"(r));
    return overflow;
}

/* SHRN/RSHRN and signed SQSHRN/SQRSHRN, 16->8 or 32->16. Shift first and add
 * the original round bit afterwards, so a maximum source cannot wrap before
 * shifting. Saturation compares the packed value expanded back to its source
 * width, preserving sticky QC without modifying FP control/exception state. */
static inline __m128i narrow_shift(__m128i a, int es, int shift, bool round, bool saturate, bool *sat)
{
    __m128i count = _mm_cvtsi32_si128(shift), one = _mm_set1_epi32(1);
    __m128i r;
    if (es == 1) {
        r = saturate ? _mm_sra_epi16(a, count) : _mm_srl_epi16(a, count);
        if (round) r = _mm_add_epi16(r, _mm_and_si128(_mm_srl_epi16(a, _mm_cvtsi32_si128(shift - 1)), _mm_set1_epi16(1)));
        if (!saturate) return _mm_packus_epi16(_mm_and_si128(r, _mm_set1_epi16(0xff)), _mm_setzero_si128());
        __m128i packed = _mm_packs_epi16(r, _mm_setzero_si128());
        __m128i expanded = _mm_unpacklo_epi8(packed, _mm_cmpgt_epi8(_mm_setzero_si128(), packed));
        if (_mm_movemask_epi8(_mm_cmpeq_epi16(r, expanded)) != 0xffff) *sat = true;
        return packed;
    }
    r = saturate ? _mm_sra_epi32(a, count) : _mm_srl_epi32(a, count);
    if (round) r = _mm_add_epi32(r, _mm_and_si128(_mm_srl_epi32(a, _mm_cvtsi32_si128(shift - 1)), one));
    if (!saturate) {
        /* SSE2 has no unsigned 32->16 pack. Translate 0..65535 into the
         * signed-halfword range, pack, then restore each halfword's sign bit. */
        /* Keep this lane mask as SSE2 PAND; Penryn-target Clang can otherwise
         * replace the constant mask with SSE4 PBLENDW against zero. */
        __m128i mask = _mm_set1_epi32(0xffff);
        __asm__("pand %1, %0" : "+x"(r) : "x"(mask));
        r = _mm_sub_epi32(r, _mm_set1_epi32(0x8000));
        return _mm_xor_si128(_mm_packs_epi32(r, r), _mm_set1_epi16((short)0x8000));
    }
    __m128i packed = _mm_packs_epi32(r, _mm_setzero_si128());
    __m128i expanded = _mm_unpacklo_epi16(packed, _mm_cmpgt_epi16(_mm_setzero_si128(), packed));
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(r, expanded)) != 0xffff) *sat = true;
    return packed;
}

/* Allocated (U, size, opcode) combinations of Advanced SIMD three same
 * (integer) and scalar three same; everything else is UNDEFINED. */
static bool three_same_int_valid(unsigned op, unsigned U, unsigned size, unsigned Q, int scalar)
{
    if (scalar) {
        switch (op) {
        case 0x01: case 0x05: case 0x09: case 0x0B: return true;                 /* SQADD SQSUB SQSHL SQRSHL */
        case 0x06: case 0x07: case 0x08: case 0x0A: case 0x10: case 0x11: return size == 3;
        case 0x16: return size == 1 || size == 2;                                /* SQDMULH/SQRDMULH */
        default: return false;
        }
    }
    if (op == 0x03) return true;                                                 /* logical: size is opcode */
    if (size == 3 && !Q) return false;
    switch (op) {
    case 0x00: case 0x02: case 0x04: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
    case 0x12: case 0x14: case 0x15: return size != 3;
    case 0x13: return U ? size == 0 : size != 3;                                 /* PMUL / MUL */
    case 0x16: return size == 1 || size == 2;
    case 0x17: return !U;                                                        /* ADDP */
    default: return true;
    }
}

/* Complex arithmetic operates on adjacent real/imaginary lanes. Preserve
 * all sources before writing Rd, including an indexed pair in Rd itself. */
static int simd_complex(hao_cpu *c, uint32_t i, bool indexed, bool add)
{
    unsigned Q = BIT(i, 30), size = BITS(i, 23, 22);
    if (!size || (size == 3 && (!Q || indexed))) return -1;
    if (indexed && ((size == 1 && !Q && BIT(i, 11)) ||
                    (size == 2 && (!Q || BIT(i, 21))))) return -1;
    int es = 1 << size, n = (Q ? 16 : 8) / es;
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    unsigned rot = add ? (BIT(i, 12) ? 3 : 1) : indexed ? BITS(i, 14, 13) : BITS(i, 12, 11);
    unsigned idx = size == 1 ? (BIT(i, 11) << 1 | BIT(i, 21)) : BIT(i, 11);
    hao_vreg a = c->v[rn], b = c->v[rm], d = c->v[rd], r = {0};
    fpctx x = fp_ctx(c);
    for (int e = 0; e < n; e += 2) {
        int j = indexed ? 2 * (int)idx : e;
        uint64_t br = eget(&b, es, j + (rot & 1)), bi = eget(&b, es, j + !(rot & 1));
        if (rot == 1 || rot == 2) br = fp_neg(br, es);
        if (rot == 2 || rot == 3) bi = fp_neg(bi, es);
        uint64_t ar = eget(&a, es, e + (add ? 0 : (rot & 1)));
        uint64_t ai = add ? eget(&a, es, e + 1) : ar;
        eset(&r, es, e, add ? fp_add(&x, es, ar, br) : fp_muladd(&x, es, eget(&d, es, e), ar, br));
        eset(&r, es, e + 1, add ? fp_add(&x, es, ai, bi) : fp_muladd(&x, es, eget(&d, es, e + 1), ai, bi));
    }
    vfinish(c, rd, &r, Q);
    return 0;
}

/* FP16 uses distinct Advanced SIMD encodings from the single/double
 * three-same and miscellaneous groups. Keep these explicit so reserved
 * encodings cannot fall into integer operations sharing nearby bit fields. */
static int simd_half_three_same(hao_cpu *c, uint32_t i)
{
    unsigned key = i & 0xAFE0FC00u;
    bool scalar = BIT(i, 28), pair = false;
    if (scalar && !BIT(i, 30)) return -1;
    if (scalar) {
        switch (key) {
        case 0x0E402400:
        case 0x2E402400:
        case 0x2EC02400:
        case 0x0E401C00:
        case 0x0E403C00:
        case 0x0EC03C00:
        case 0x2EC01400:
        case 0x2E402C00:
        case 0x2EC02C00:
            break;
        default: return -1;
        }
    }
    switch (key) {
    case 0x2E400400: key = 0x0E400400; pair = true; break;
    case 0x2EC00400: key = 0x0EC00400; pair = true; break;
    case 0x2E401400: key = 0x0E401400; pair = true; break;
    case 0x2E403400: key = 0x0E403400; pair = true; break;
    case 0x2EC03400: key = 0x0EC03400; pair = true; break;
    default: break;
    }
    switch (key) {
    case 0x0E400400:
    case 0x0EC00400:
    case 0x0E400C00:
    case 0x0EC00C00:
    case 0x0E401400:
    case 0x0EC01400:
    case 0x2E401C00:
    case 0x2E403C00:
    case 0x0E403400:
    case 0x0EC03400:
    case 0x0E402400:
    case 0x2E402400:
    case 0x2EC02400:
    case 0x0E401C00:
    case 0x0E403C00:
    case 0x0EC03C00:
    case 0x2EC01400:
    case 0x2E402C00:
    case 0x2EC02C00:
        break;
    default: return -1;
    }
    unsigned Q = BIT(i, 30), rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    unsigned n = scalar ? 1 : Q ? 8 : 4;
    hao_vreg a = c->v[rn], b = c->v[rm], d = c->v[rd], r = {0};
    fpctx x = fp_ctx(c);
    if (!scalar && (key == 0x0E400C00 || key == 0x0EC00C00) &&
        fp_vec16_muladd(&x, &d, &a, &b, n, key == 0x0EC00C00, &r)) {
        vfinish(c, rd, &r, Q);
        return 0;
    }
    for (unsigned e = 0; e < n; e++) {
        uint64_t av = a.h[e], bv = b.h[e], v;
        if (pair) {
            const hao_vreg *src = e < n / 2 ? &a : &b;
            av = src->h[2 * (e % (n / 2))];
            bv = src->h[2 * (e % (n / 2)) + 1];
        }
        switch (key) {
        case 0x0E400400: v = fp_maxnm(&x, 2, av, bv); break;
        case 0x0EC00400: v = fp_minnm(&x, 2, av, bv); break;
        case 0x0E400C00: v = fp_muladd(&x, 2, d.h[e], av, bv); break;
        case 0x0EC00C00: v = fp_muladd(&x, 2, d.h[e], fp_neg(av, 2), bv); break;
        case 0x0E401400: v = fp_add(&x, 2, av, bv); break;
        case 0x0EC01400: v = fp_sub(&x, 2, av, bv); break;
        case 0x2E401C00: v = fp_mul(&x, 2, av, bv); break;
        case 0x2E403C00: v = fp_div(&x, 2, av, bv); break;
        case 0x0E403400: v = fp_max(&x, 2, av, bv); break;
        case 0x0EC03400: v = fp_min(&x, 2, av, bv); break;
        case 0x0E402400: v = fp_cmp_eq(&x, 2, av, bv) ? 0xffff : 0; break;
        case 0x2E402400: v = fp_cmp_ge(&x, 2, av, bv) ? 0xffff : 0; break;
        case 0x2EC02400: v = fp_cmp_gt(&x, 2, av, bv) ? 0xffff : 0; break;
        case 0x0E401C00: v = fp_mulx(&x, 2, av, bv); break;
        case 0x0E403C00: v = fp_recps(&x, 2, av, bv); break;
        case 0x0EC03C00: v = fp_rsqrts(&x, 2, av, bv); break;
        case 0x2EC01400: v = fp_abs(fp_sub(&x, 2, av, bv), 2); break;
        case 0x2E402C00: v = fp_cmp_ge(&x, 2, fp_abs(av, 2), fp_abs(bv, 2)) ? 0xffff : 0; break;
        case 0x2EC02C00: v = fp_cmp_gt(&x, 2, fp_abs(av, 2), fp_abs(bv, 2)) ? 0xffff : 0; break;
        default: return -1;
        }
        r.h[e] = (uint16_t)v;
    }
    vfinish(c, rd, &r, scalar ? 0 : Q);
    return 0;
}

static int simd_half_two_misc(hao_cpu *c, uint32_t i)
{
    unsigned key = i & 0xAFFFFC00u;
    bool scalar = BIT(i, 28);
    if (scalar && !BIT(i, 30)) return -1;
    switch (key) {
    case 0x0EF8F800:
    case 0x2EF8F800:
    case 0x2EF9F800:
    case 0x0E798800:
    case 0x0E799800:
    case 0x0EF98800:
    case 0x0EF99800:
    case 0x2E798800:
    case 0x2EF99800:
    case 0x2E799800:
        if (scalar) return -1;
        break;
    case 0x0E79D800:
    case 0x2E79D800:
    case 0x0EF8D800:
    case 0x0EF8C800:
    case 0x2EF8C800:
    case 0x2EF8D800:
    case 0x0EF8E800:
    case 0x0EF9D800:
    case 0x2EF9D800:
    case 0x0E79A800:
    case 0x2E79A800:
    case 0x0E79B800:
    case 0x2E79B800:
    case 0x0E79C800:
    case 0x2E79C800:
    case 0x0EF9A800:
    case 0x2EF9A800:
    case 0x0EF9B800:
    case 0x2EF9B800:
        break;
    case 0x0EF9F800: if (!scalar) return -1; break; /* FRECPX */
    default: return -1;
    }
    unsigned Q = BIT(i, 30), rd = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    hao_vreg a = c->v[rn], r = {0};
    fpctx x = fp_ctx(c);
    for (unsigned e = 0; e < (scalar ? 1u : Q ? 8u : 4u); e++) {
        uint64_t av = a.h[e], v;
        switch (key) {
        case 0x0E79D800: v = fp_from_int(&x, 2, av, true, 16, 0); break;
        case 0x2E79D800: v = fp_from_int(&x, 2, av, false, 16, 0); break;
        case 0x0EF8D800: v = fp_cmp_eq(&x, 2, av, 0) ? 0xffff : 0; break;
        case 0x0EF8C800: v = fp_cmp_gt(&x, 2, av, 0) ? 0xffff : 0; break;
        case 0x2EF8C800: v = fp_cmp_ge(&x, 2, av, 0) ? 0xffff : 0; break;
        case 0x2EF8D800: v = fp_cmp_ge(&x, 2, 0, av) ? 0xffff : 0; break;
        case 0x0EF8E800: v = fp_cmp_gt(&x, 2, 0, av) ? 0xffff : 0; break;
        case 0x0EF9D800: v = fp_recpe(&x, 2, av); break;
        case 0x2EF9D800: v = fp_rsqrte(&x, 2, av); break;
        case 0x0E79A800: v = fp_to_int(&x, 2, av, FPR_N, false, 16, 0); break;
        case 0x2E79A800: v = fp_to_int(&x, 2, av, FPR_N, true, 16, 0); break;
        case 0x0E79B800: v = fp_to_int(&x, 2, av, FPR_M, false, 16, 0); break;
        case 0x2E79B800: v = fp_to_int(&x, 2, av, FPR_M, true, 16, 0); break;
        case 0x0E79C800: v = fp_to_int(&x, 2, av, FPR_A, false, 16, 0); break;
        case 0x2E79C800: v = fp_to_int(&x, 2, av, FPR_A, true, 16, 0); break;
        case 0x0EF9A800: v = fp_to_int(&x, 2, av, FPR_P, false, 16, 0); break;
        case 0x2EF9A800: v = fp_to_int(&x, 2, av, FPR_P, true, 16, 0); break;
        case 0x0EF9B800: v = fp_to_int(&x, 2, av, FPR_Z, false, 16, 0); break;
        case 0x2EF9B800: v = fp_to_int(&x, 2, av, FPR_Z, true, 16, 0); break;
        case 0x0EF8F800: v = fp_abs(av, 2); break;
        case 0x2EF8F800: v = fp_neg(av, 2); break;
        case 0x2EF9F800: v = fp_sqrt(&x, 2, av); break;
        case 0x0E798800: v = fp_rint(&x, 2, av, FPR_N, false); break;
        case 0x0E799800: v = fp_rint(&x, 2, av, FPR_M, false); break;
        case 0x0EF98800: v = fp_rint(&x, 2, av, FPR_P, false); break;
        case 0x0EF99800: v = fp_rint(&x, 2, av, FPR_Z, false); break;
        case 0x2E798800: v = fp_rint(&x, 2, av, FPR_A, false); break;
        case 0x2EF99800: v = fp_rint(&x, 2, av, fp_cur(&x), false); break;
        case 0x2E799800: v = fp_rint(&x, 2, av, fp_cur(&x), true); break;
        default: v = fp_recpx(&x, 2, av); break;
        }
        r.h[e] = (uint16_t)v;
    }
    vfinish(c, rd, &r, scalar ? 0 : Q);
    return 0;
}

/* Only non-pairwise single/double vector FP three-same forms are eligible.
 * This pure predicate is shared by emitted descriptors and checked dispatch. */
bool a64_vector_fp_eligible(uint32_t i)
{
    if ((i & 0x9F200400u) != 0x0E200400u) return false;
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 11);
    if (op < 0x18 || ((size & 1) && !Q)) return false;
    switch ((U << 6) | ((size >> 1) << 5) | (op & 7)) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
    case 0x20: case 0x21: case 0x22: case 0x26: case 0x27:
    case 0x43: case 0x44: case 0x45: case 0x47: case 0x62: case 0x64: case 0x65:
        return true;
    default: return false;
    }
}

static int simd_three_same(hao_cpu *c, uint32_t i, int scalar)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 11);
    unsigned rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], b = c->v[rm], d = c->v[rd], r;
    memset(&r, 0, sizeof r);
    int bytes = scalar ? (op >= 0x18 ? (BIT(size, 0) ? 8 : 4) : (1 << size)) : (Q ? 16 : 8);
    if (op < 0x18 && !three_same_int_valid(op, U, size, Q, scalar)) return -1;
    if (op >= 0x18 && scalar) {   /* scalar FP three same: FMULX FCMEQ FRECPS FRSQRTS FCMGE FACGE FABD FCMGT FACGT */
        unsigned k = U << 6 | BIT(size, 1) << 5 | (op & 7);
        if (k != 0x03 && k != 0x04 && k != 0x07 && k != 0x27 && k != 0x44 && k != 0x45 && k != 0x62 && k != 0x64 && k != 0x65) return -1;
    }
    if (scalar) Q = 0;

    if (op == 3) {  /* logical */
        for (int k = 0; k < 2; k++) {
            uint64_t x = a.d[k], y = b.d[k], z = d.d[k], v;
            if (!U) {
                switch (size) { case 0: v = x & y; break; case 1: v = x & ~y; break; case 2: v = x | y; break; default: v = x | ~y; break; }
            } else {
                switch (size) {
                case 0: v = x ^ y; break;
                case 1: v = (z & x) | (~z & y); break;          /* BSL */
                case 2: v = (z & ~y) | (x & y); break;          /* BIT */
                default: v = (z & y) | (x & ~y); break;         /* BIF */
                }
            }
            r.d[k] = v;
        }
        vfinish(c, rd, &r, Q);
        return 0;
    }

    if (op >= 0x18) {  /* floating point three-same (single/double) */
        int dbl = BIT(size, 0), es = dbl ? 8 : 4, n = bytes / es;
        unsigned hi = BIT(size, 1), key = U << 6 | hi << 5 | (op & 7);
        bool pairwise = U && !hi && (op == 0x18 || op == 0x1A || op == 0x1E);
        pairwise |= U && hi && (op == 0x18 || op == 0x1E);
        if (dbl && !Q && !scalar) return -1;                  /* .1D */
        if (scalar && pairwise) return -1;
        if (!scalar && !dbl && (key == 0x02 || key == 0x22 || key == 0x43) &&
            fp_vec32(key == 0x02 ? FPV_ADD : key == 0x22 ? FPV_SUB : FPV_MUL, &a, &b, n, &r)) {
            vfinish(c, rd, &r, Q);
            return 0;
        }
        if (!scalar && !dbl && !pairwise) {
            unsigned select;
            switch (key) {
            case 0x00: case 0x06: select = FPVS_MAX; break;
            case 0x20: case 0x26: select = FPVS_MIN; break;
            case 0x04: select = FPVS_EQ; break;
            case 0x44: select = FPVS_GE; break;
            case 0x64: select = FPVS_GT; break;
            default: select = FPVS_GT + 1; break;
            }
            if (select <= FPVS_GT && fp_vec32_select(select, &a, &b, n, &r)) {
                vfinish(c, rd, &r, Q);
                return 0;
            }
        }
        fpctx x = fp_ctx(c);
        if (!scalar && !dbl && (key == 0x01 || key == 0x21) &&
            fp_vec32_muladd(&x, &d, &a, &b, n, key == 0x21, &r)) {
            vfinish(c, rd, &r, Q);
            return 0;
        }
        for (int e = 0; e < n; e++) {
            uint64_t xa, ya, acc = eget(&d, es, e), v = 0;
            if (pairwise) {
                const hao_vreg *src = e < n / 2 ? &a : &b;
                int j = (e % (n / 2)) * 2;
                xa = eget(src, es, j); ya = eget(src, es, j + 1);
            } else {
                xa = eget(&a, es, e); ya = eget(&b, es, e);
            }
            switch (key) {
            case 0x00: v = fp_maxnm(&x, es, xa, ya); break;                                 /* FMAXNM */
            case 0x01: v = fp_muladd(&x, es, acc, xa, ya); break;                           /* FMLA */
            case 0x02: v = fp_add(&x, es, xa, ya); break;                                   /* FADD */
            case 0x03: v = fp_mulx(&x, es, xa, ya); break;                                  /* FMULX */
            case 0x04: v = fp_cmp_eq(&x, es, xa, ya) ? emask(es) : 0; break;                /* FCMEQ */
            case 0x06: v = fp_max(&x, es, xa, ya); break;                                   /* FMAX */
            case 0x07: v = fp_recps(&x, es, xa, ya); break;                                 /* FRECPS */
            case 0x20: v = fp_minnm(&x, es, xa, ya); break;                                 /* FMINNM */
            case 0x21: v = fp_muladd(&x, es, acc, fp_neg(xa, es), ya); break;               /* FMLS */
            case 0x22: v = fp_sub(&x, es, xa, ya); break;                                   /* FSUB */
            case 0x26: v = fp_min(&x, es, xa, ya); break;                                   /* FMIN */
            case 0x27: v = fp_rsqrts(&x, es, xa, ya); break;                                /* FRSQRTS */
            case 0x40: v = fp_maxnm(&x, es, xa, ya); break;                                 /* FMAXNMP */
            case 0x42: v = fp_add(&x, es, xa, ya); break;                                   /* FADDP */
            case 0x43: v = fp_mul(&x, es, xa, ya); break;                                   /* FMUL */
            case 0x44: v = fp_cmp_ge(&x, es, xa, ya) ? emask(es) : 0; break;                /* FCMGE */
            case 0x45: v = fp_cmp_ge(&x, es, fp_abs(xa, es), fp_abs(ya, es)) ? emask(es) : 0; break; /* FACGE */
            case 0x46: v = fp_max(&x, es, xa, ya); break;                                   /* FMAXP */
            case 0x47: v = fp_div(&x, es, xa, ya); break;                                   /* FDIV */
            case 0x60: v = fp_minnm(&x, es, xa, ya); break;                                 /* FMINNMP */
            case 0x62: v = fp_abs(fp_sub(&x, es, xa, ya), es); break;                       /* FABD */
            case 0x64: v = fp_cmp_gt(&x, es, xa, ya) ? emask(es) : 0; break;                /* FCMGT */
            case 0x65: v = fp_cmp_gt(&x, es, fp_abs(xa, es), fp_abs(ya, es)) ? emask(es) : 0; break; /* FACGT */
            case 0x66: v = fp_min(&x, es, xa, ya); break;                                   /* FMINP */
            default: return -1;
            }
            eset(&r, es, e, v);
        }
        vfinish(c, rd, &r, Q);
        return 0;
    }

    int es = 1 << size, n = bytes / es;
    bool sat = false;
    if (op == 0x10) {  /* ADD/SUB, including the allocated scalar .D form */
        r = vr(U ? isub(vi(&a), vi(&b), es) : iadd(vi(&a), vi(&b), es));
        vfinish(c, rd, &r, Q);
        return 0;
    }
    if (es == 2 && (op == 0x12 || (op == 0x13 && !U) || op == 0x16)) {
        __m128i v;
        if (op == 0x16) v = mulhigh_half(vi(&a), vi(&b), U, bytes, &sat);
        else {
            v = _mm_mullo_epi16(vi(&a), vi(&b));
            if (op == 0x12) v = U ? _mm_sub_epi16(vi(&d), v) : _mm_add_epi16(vi(&d), v);
        }
        if (sat) c->fpsr |= FPSR_QC;
        r = vr(v);
        if (scalar) r.d[0] &= 0xffff;
        vfinish(c, rd, &r, Q);
        return 0;
    }
    bool pair = op == 0x14 || op == 0x15 || op == 0x17;
    /* pairwise ops have no scalar three-same form and no .1D form (n would
     * be 1 and the pair index n / 2 zero) */
    if (pair && (scalar || n < 2)) return -1;
    for (int e = 0; e < n; e++) {
        uint64_t x, y, v = 0;
        int64_t sx, sy;
        if (pair) {
            const hao_vreg *src = e < n / 2 ? &a : &b;
            int j = (e % (n / 2)) * 2;
            x = eget(src, es, j); y = eget(src, es, j + 1);
        } else {
            x = eget(&a, es, e); y = eget(&b, es, e);
        }
        sx = sext(x, es * 8); sy = sext(y, es * 8);
        switch (op) {
        case 0x00: v = U ? (uint64_t)(((u128)x + y) >> 1) : (uint64_t)(((__int128)sx + sy) >> 1); break;
        case 0x01:
            if (U) { u128 s = (u128)x + y; v = s > emask(es) ? (sat = true, emask(es)) : (uint64_t)s; }
            else v = es == 8 ? (uint64_t)(__builtin_add_overflow(sx, sy, &sx) ? (sat = true, (sy < 0 ? INT64_MIN : INT64_MAX)) : sx)
                             : sat_s(sx + sy, es, &sat);
            break;
        case 0x02: v = U ? (uint64_t)(((u128)x + y + 1) >> 1) : (uint64_t)(((__int128)sx + sy + 1) >> 1); break;
        case 0x04: v = U ? (uint64_t)(((__int128)x - (__int128)y) >> 1) : (uint64_t)(((__int128)sx - sy) >> 1); break;
        case 0x05:
            if (U) v = x < y ? (sat = true, 0) : x - y;
            else v = es == 8 ? (uint64_t)(__builtin_sub_overflow(sx, sy, &sx) ? (sat = true, (sy > 0 ? INT64_MIN : INT64_MAX)) : sx)
                             : sat_s(sx - sy, es, &sat);
            break;
        case 0x06: v = (U ? x > y : sx > sy) ? emask(es) : 0; break;
        case 0x07: v = (U ? x >= y : sx >= sy) ? emask(es) : 0; break;
        case 0x08: v = shl_reg(x, (int64_t)(int8_t)y, es, U, false); break;
        case 0x0A: v = shl_reg(x, (int64_t)(int8_t)y, es, U, true); break;
        case 0x09: case 0x0B: v = sat_shl_reg(x, (int8_t)y, es, U, op == 0x0B, &sat); break;
        case 0x0C: v = U ? (x > y ? x : y) : (uint64_t)(sx > sy ? sx : sy); break;
        case 0x0D: v = U ? (x < y ? x : y) : (uint64_t)(sx < sy ? sx : sy); break;
        case 0x0E: v = U ? (x > y ? x - y : y - x) : (uint64_t)(sx > sy ? sx - sy : sy - sx); break;
        case 0x0F: v = eget(&d, es, e) + (U ? (x > y ? x - y : y - x) : (uint64_t)(sx > sy ? sx - sy : sy - sx)); break;
        case 0x10: v = U ? x - y : x + y; break;
        case 0x11: v = U ? (x == y ? emask(es) : 0) : ((x & y) ? emask(es) : 0); break;
        case 0x12: v = U ? eget(&d, es, e) - x * y : eget(&d, es, e) + x * y; break;
        case 0x13:
            if (U) { /* PMUL (bytes) */
                uint64_t p = 0;
                for (int k = 0; k < 8; k++) if ((y >> k) & 1) p ^= x << k;
                v = p;
            } else v = x * y;
            break;
        case 0x14: v = U ? (x > y ? x : y) : (uint64_t)(sx > sy ? sx : sy); break;
        case 0x15: v = U ? (x < y ? x : y) : (uint64_t)(sx < sy ? sx : sy); break;
        case 0x16: v = signed_mulhigh(sx, sy, es, U, &sat); break;
        case 0x17: if (U) return -1; v = x + y; break;
        default: return -1;
        }
        eset(&r, es, e, v & emask(es));
    }
    if (sat) c->fpsr |= 1u << 27;
    vfinish(c, rd, &r, Q);
    return 0;
}

int a64_exec_vector_fp(hao_cpu *c, uint32_t i)
{
    if (!a64_vector_fp_eligible(i)) return -1;
    return simd_three_same(c, i, 0);
}

static int simd_two_misc(hao_cpu *c, uint32_t i, int scalar)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 16, 12);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], r;
    memset(&r, 0, sizeof r);
    int es = 1 << size, bytes = scalar ? es : (Q ? 16 : 8), n = bytes / es;
    if (scalar) Q = 0;

    switch (op) {
    case 0x00: case 0x01: {  /* REV64 / REV32 / REV16 */
        int cont = op == 1 ? 2 : (U ? 4 : 8);
        if (es >= cont || scalar) return -1;                 /* reserved element sizes */
        for (int e = 0; e < n; e++) {
            int per = cont / es, base = (e / per) * per, j = base + (per - 1 - (e - base));
            eset(&r, es, e, eget(&a, es, j));
        }
        break;
    }
    case 0x02: case 0x06: {  /* SADDLP/UADDLP, SADALP/UADALP */
        if (size == 3) return -1;           /* unallocated: the sums would be 128-bit elements */
        int oes = es * 2, on = bytes / oes;
        for (int e = 0; e < on; e++) {
            uint64_t s = U ? eget(&a, es, 2 * e) + eget(&a, es, 2 * e + 1)
                           : (uint64_t)(esget(&a, es, 2 * e) + esget(&a, es, 2 * e + 1));
            if (op == 6) s += eget(&c->v[rd], oes, e);
            eset(&r, oes, e, s & emask(oes));
        }
        break;
    }
    case 0x04:  /* CLS / CLZ */
        for (int e = 0; e < n; e++) {
            uint64_t x = eget(&a, es, e);
            int bits = es * 8, cnt = 0;
            if (U) { for (int k = bits - 1; k >= 0 && !((x >> k) & 1); k--) cnt++; }
            else { int sgn = (x >> (bits - 1)) & 1; for (int k = bits - 2; k >= 0 && (int)((x >> k) & 1) == sgn; k--) cnt++; }
            eset(&r, es, e, (uint64_t)cnt);
        }
        break;
    case 0x05:
        if (!U) { for (int e = 0; e < bytes; e++) r.b[e] = (uint8_t)__builtin_popcount(a.b[e]); }   /* CNT */
        else if (size == 0) { for (int k = 0; k < 2; k++) r.d[k] = ~a.d[k]; }                       /* NOT */
        else if (size == 1) {                                                                        /* RBIT */
            for (int e = 0; e < bytes; e++) { uint8_t x = a.b[e], y = 0; for (int k = 0; k < 8; k++) if ((x >> k) & 1) y |= (uint8_t)(1 << (7 - k)); r.b[e] = y; }
        } else return -1;
        break;
    case 0x08: case 0x09: case 0x0A: {  /* CMGT/CMGE/CMEQ/CMLE/CMLT zero */
        for (int e = 0; e < n; e++) {
            int64_t x = esget(&a, es, e);
            bool t;
            if (op == 8) t = U ? x >= 0 : x > 0;
            else if (op == 9) t = U ? x <= 0 : x == 0;
            else { if (U) return -1; t = x < 0; }
            eset(&r, es, e, t ? emask(es) : 0);
        }
        break;
    }
    case 0x0B:  /* ABS / NEG */
        for (int e = 0; e < n; e++) {
            int64_t x = esget(&a, es, e);
            uint64_t ux = (uint64_t)x;                       /* wraps at the minimum, as the hardware does */
            eset(&r, es, e, (U ? 0 - ux : (x < 0 ? 0 - ux : ux)) & emask(es));
        }
        break;
    case 0x12: case 0x14: {  /* XTN/SQXTUN, SQXTN/UQXTN */
        if (size == 3) return -1;           /* unallocated: the source would be 128-bit elements */
        int oes = es, ies = es * 2, on = 8 / oes;
        bool sat = false;
        hao_vreg d = c->v[rd];
        if (scalar) { on = 1; }
        r = Q ? d : r;
        for (int e = 0; e < on; e++) {
            uint64_t x = eget(&a, ies, e), v;
            if (op == 0x12 && !U) v = x & emask(oes);
            else if (op == 0x12 && U) v = sat_u(sext(x, ies * 8), oes, &sat);
            else if (!U) v = sat_s(sext(x, ies * 8), oes, &sat);
            else v = x > emask(oes) ? (sat = true, emask(oes)) : x;
            eset(&r, oes, (Q ? on : 0) + e, v);
        }
        if (sat) c->fpsr |= 1u << 27;
        c->v[rd] = r;
        if (!Q) c->v[rd].d[1] = 0;
        return 0;
    }
    case 0x13: {  /* SHLL */
        if (!U || size == 3) return -1;     /* size 3 is unallocated: no 128-bit elements */
        int oes = es * 2, on = 8 / es;
        for (int e = 0; e < on; e++) eset(&r, oes, e, eget(&a, es, (Q ? on : 0) + e) << (es * 8));
        c->v[rd] = r;
        return 0;
    }
    case 0x16: {  /* FCVTN / FCVTXN (U, double source only) */
        fpctx x = fp_ctx(c);
        if ((size & 2) || (U && !(size & 1))) return -1;      /* size 1x: BFCVTN (not implemented) */
        if (scalar && !U) return -1;
        hao_vreg d = Q ? c->v[rd] : r;
        if (scalar) { memset(&d, 0, sizeof d); d.s[0] = (uint32_t)fp_cvt_odd(&x, a.d[0]); c->v[rd] = d; return 0; }
        if (size & 1) { for (int e = 0; e < 2; e++) d.s[(Q ? 2 : 0) + e] = (uint32_t)(U ? fp_cvt_odd(&x, a.d[e]) : fp_cvt(&x, a.d[e], 8, 4)); }
        else { for (int e = 0; e < 4; e++) d.h[(Q ? 4 : 0) + e] = (uint16_t)fp_cvt(&x, a.s[e], 4, 2); }
        c->v[rd] = d;
        if (!Q) c->v[rd].d[1] = 0;
        return 0;
    }
    case 0x17: {  /* FCVTL */
        fpctx x = fp_ctx(c);
        if (U || scalar || (size & 2)) return -1;
        if (size & 1) { for (int e = 0; e < 2; e++) r.d[e] = fp_cvt(&x, a.s[(Q ? 2 : 0) + e], 4, 8); }
        else { for (int e = 0; e < 4; e++) r.s[e] = (uint32_t)fp_cvt(&x, a.h[(Q ? 4 : 0) + e], 2, 4); }
        c->v[rd] = r;
        return 0;
    }
    default: {
        /* floating-point two-reg misc: size<0> = sz, size<1> selects group */
        int dbl = BIT(size, 0), fes = dbl ? 8 : 4, fn = scalar ? 1 : (Q ? 16 : 8) / fes;
        unsigned hi = BIT(size, 1), key = U << 6 | hi << 5 | op;
        if (dbl && !Q && !scalar) return -1;                  /* .1D */
        fpctx x = fp_ctx(c);
        int bits = fes * 8;
        for (int e = 0; e < fn; e++) {
            uint64_t xa = eget(&a, fes, e), v;
            switch (key) {
            case 0x18: v = fp_rint(&x, fes, xa, FPR_N, false); break;                    /* FRINTN */
            case 0x19: v = fp_rint(&x, fes, xa, FPR_M, false); break;                    /* FRINTM */
            case 0x1A: v = fp_to_int(&x, fes, xa, FPR_N, false, bits, 0); break;         /* FCVTNS */
            case 0x1B: v = fp_to_int(&x, fes, xa, FPR_M, false, bits, 0); break;         /* FCVTMS */
            case 0x1C: v = fp_to_int(&x, fes, xa, FPR_A, false, bits, 0); break;         /* FCVTAS */
            case 0x1D: v = fp_from_int(&x, fes, xa, true, bits, 0); break;               /* SCVTF */
            case 0x2C: v = fp_cmp_gt(&x, fes, xa, 0) ? emask(fes) : 0; break;           /* FCMGT #0 */
            case 0x2D: v = fp_cmp_eq(&x, fes, xa, 0) ? emask(fes) : 0; break;           /* FCMEQ #0 */
            case 0x2E: v = fp_cmp_gt(&x, fes, 0, xa) ? emask(fes) : 0; break;           /* FCMLT #0 */
            case 0x2F: if (scalar) return -1; v = fp_abs(xa, fes); break;               /* FABS */
            case 0x38: v = fp_rint(&x, fes, xa, FPR_P, false); break;                    /* FRINTP */
            case 0x39: v = fp_rint(&x, fes, xa, FPR_Z, false); break;                    /* FRINTZ */
            case 0x3A: v = fp_to_int(&x, fes, xa, FPR_P, false, bits, 0); break;         /* FCVTPS */
            case 0x3B: v = fp_to_int(&x, fes, xa, FPR_Z, false, bits, 0); break;         /* FCVTZS */
            case 0x3D: v = fp_recpe(&x, fes, xa); break;                                 /* FRECPE */
            case 0x3F: if (!scalar) return -1; v = fp_recpx(&x, fes, xa); break;        /* FRECPX */
            case 0x58: v = fp_rint(&x, fes, xa, FPR_A, false); break;                    /* FRINTA */
            case 0x59: v = fp_rint(&x, fes, xa, fp_cur(&x), true); break;                /* FRINTX */
            case 0x5A: v = fp_to_int(&x, fes, xa, FPR_N, true, bits, 0); break;          /* FCVTNU */
            case 0x5B: v = fp_to_int(&x, fes, xa, FPR_M, true, bits, 0); break;          /* FCVTMU */
            case 0x5C: v = fp_to_int(&x, fes, xa, FPR_A, true, bits, 0); break;          /* FCVTAU */
            case 0x5D: v = fp_from_int(&x, fes, xa, false, bits, 0); break;              /* UCVTF */
            case 0x6C: v = fp_cmp_ge(&x, fes, xa, 0) ? emask(fes) : 0; break;           /* FCMGE #0 */
            case 0x6D: v = fp_cmp_ge(&x, fes, 0, xa) ? emask(fes) : 0; break;           /* FCMLE #0 */
            case 0x6F: if (scalar) return -1; v = fp_neg(xa, fes); break;               /* FNEG */
            case 0x79: if (scalar) return -1; v = fp_rint(&x, fes, xa, fp_cur(&x), false); break; /* FRINTI */
            case 0x7A: v = fp_to_int(&x, fes, xa, FPR_P, true, bits, 0); break;          /* FCVTPU */
            case 0x7B: v = fp_to_int(&x, fes, xa, FPR_Z, true, bits, 0); break;          /* FCVTZU */
            case 0x7D: v = fp_rsqrte(&x, fes, xa); break;                                /* FRSQRTE */
            case 0x7F: if (scalar) return -1; v = fp_sqrt(&x, fes, xa); break;          /* FSQRT */
            default: return -1;
            }
            if (scalar && (key == 0x18 || key == 0x19 || key == 0x38 || key == 0x39 || key == 0x58 || key == 0x59)) return -1;
            eset(&r, fes, e, v & emask(fes));
        }
        if (scalar) Q = 0;
        vfinish(c, rd, &r, Q);
        return 0;
    }
    }
    vfinish(c, rd, &r, Q);
    return 0;
}

static int simd_across(hao_cpu *c, uint32_t i)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 16, 12);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], r;
    memset(&r, 0, sizeof r);
    int es = 1 << size, n = (Q ? 16 : 8) / es;
    if (op == 0x0C || op == 0x0F) {  /* FMAXNMV/FMAXV/FMINNMV/FMINV: .4S only */
        if (!U || !Q || BIT(size, 0)) return -1;
        fpctx x = fp_ctx(c);
        bool mn = BIT(size, 1), nm = op == 0x0C;
        /* Reduce(): pairwise tree, ((e0 op e1) op (e2 op e3)) */
        uint64_t (*f)(fpctx *, int, uint64_t, uint64_t) = mn ? (nm ? fp_minnm : fp_min) : (nm ? fp_maxnm : fp_max);
        uint64_t lo = f(&x, 4, a.s[0], a.s[1]), hi = f(&x, 4, a.s[2], a.s[3]);
        r.s[0] = (uint32_t)f(&x, 4, lo, hi);
        c->v[rd] = r;
        return 0;
    }
    if (size == 3 || (size == 2 && !Q)) return -1;      /* unallocated: 64-bit lanes, or two 32-bit ones */
    uint64_t acc = eget(&a, es, 0);
    int64_t sacc = esget(&a, es, 0);
    switch (op) {
    case 0x03: {  /* SADDLV / UADDLV */
        uint64_t s = 0;
        for (int e = 0; e < n; e++) s += U ? eget(&a, es, e) : (uint64_t)esget(&a, es, e);
        eset(&r, es * 2, 0, s & emask(es * 2));
        break;
    }
    case 0x0A:  /* SMAXV / UMAXV */
        for (int e = 1; e < n; e++) { uint64_t x = eget(&a, es, e); int64_t sx = esget(&a, es, e); if (x > acc) acc = x; if (sx > sacc) sacc = sx; }
        eset(&r, es, 0, (U ? acc : (uint64_t)sacc) & emask(es));
        break;
    case 0x1A:  /* SMINV / UMINV */
        for (int e = 1; e < n; e++) { uint64_t x = eget(&a, es, e); int64_t sx = esget(&a, es, e); if (x < acc) acc = x; if (sx < sacc) sacc = sx; }
        eset(&r, es, 0, (U ? acc : (uint64_t)sacc) & emask(es));
        break;
    case 0x1B: {  /* ADDV */
        uint64_t s = 0;
        for (int e = 0; e < n; e++) s += eget(&a, es, e);
        eset(&r, es, 0, s & emask(es));
        break;
    }
    default: return -1;
    }
    c->v[rd] = r;
    return 0;
}

uint64_t a64_simd_expand_imm(unsigned op, unsigned cmode, unsigned imm8)
{
    uint64_t imm = 0;
    switch (cmode >> 1) {
    case 0: imm = (uint64_t)imm8; imm |= imm << 32; break;
    case 1: imm = (uint64_t)imm8 << 8; imm |= imm << 32; break;
    case 2: imm = (uint64_t)imm8 << 16; imm |= imm << 32; break;
    case 3: imm = (uint64_t)imm8 << 24; imm |= imm << 32; break;
    case 4: imm = (uint64_t)imm8; imm |= imm << 16; imm |= imm << 32; break;
    case 5: imm = (uint64_t)imm8 << 8; imm |= imm << 16; imm |= imm << 32; break;
    case 6:
        imm = (cmode & 1) ? ((uint64_t)imm8 << 16 | 0xffff) : ((uint64_t)imm8 << 8 | 0xff);
        imm |= imm << 32;
        break;
    default:
        if (!(cmode & 1) && !op) { for (int k = 0; k < 8; k++) imm |= (uint64_t)imm8 << (8 * k); }
        else if (!(cmode & 1) && op) { for (int k = 0; k < 8; k++) if ((imm8 >> k) & 1) imm |= 0xffull << (8 * k); }
        else if ((cmode & 1) && !op) {  /* FMOV single */
            uint32_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
            uint32_t f = a << 31 | (!b) << 30 | (b ? 0x1f : 0) << 25 | (imm8 & 0x3f) << 19;
            imm = (uint64_t)f << 32 | f;
        } else {  /* FMOV double */
            uint64_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
            imm = a << 63 | (uint64_t)(!b) << 62 | (b ? 0xffull : 0) << 54 | (uint64_t)(imm8 & 0x3f) << 48;
        }
        break;
    }
    return imm;
}

static int simd_modimm(hao_cpu *c, uint32_t i)
{
    unsigned Q = BIT(i, 30), op = BIT(i, 29), cmode = BITS(i, 15, 12), rd = BITS(i, 4, 0);
    unsigned imm8 = BITS(i, 18, 16) << 5 | BITS(i, 9, 5);
    if (BIT(i, 11)) {   /* o2: only FMOV (vector, immediate, half precision) */
        if (op || cmode != 15) return -1;
        unsigned a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
        uint16_t h = (uint16_t)(a << 15 | (!b) << 14 | (b ? 3 : 0) << 12 | (imm8 & 0x3f) << 6);
        hao_vreg r;
        for (int e = 0; e < 8; e++) r.h[e] = h;
        vfinish(c, rd, &r, Q);
        return 0;
    }
    if (op && cmode == 15 && !Q) return -1;   /* FMOV .1D immediate */
    uint64_t imm = a64_simd_expand_imm(op, cmode, imm8);
    hao_vreg r = c->v[rd];
    bool is_orr_bic = (cmode < 12 && (cmode & 1));
    if (is_orr_bic) {
        for (int k = 0; k < 2; k++) r.d[k] = op ? (r.d[k] & ~imm) : (r.d[k] | imm);
    } else {
        bool mvn = op && cmode != 14 && cmode != 15;
        for (int k = 0; k < 2; k++) r.d[k] = mvn ? ~imm : imm;
    }
    vfinish(c, rd, &r, Q);
    return 0;
}

static int simd_copy(hao_cpu *c, uint32_t i, int scalar)
{
    unsigned Q = BIT(i, 30), op = BIT(i, 29), imm5 = BITS(i, 20, 16), imm4 = BITS(i, 14, 11);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!(imm5 & 0xF)) return -1;           /* imm5 = x0000 is unallocated: elements are 1 to 8 bytes */
    int sz = __builtin_ctz(imm5), es = 1 << sz;
    int idx = (int)(imm5 >> (sz + 1));
    hao_vreg r;
    memset(&r, 0, sizeof r);
    if (scalar) {  /* DUP (element) scalar */
        eset(&r, es, 0, eget(&c->v[rn], es, idx));
        c->v[rd] = r;
        return 0;
    }
    if (op) {  /* INS (element) */
        r = c->v[rd];
        eset(&r, es, idx, eget(&c->v[rn], es, (int)(imm4 >> sz)));
        c->v[rd] = r;
        return 0;
    }
    switch (imm4) {
    case 0: {  /* DUP element */
        uint64_t x = eget(&c->v[rn], es, idx);
        for (int e = 0; e < (Q ? 16 : 8) / es; e++) eset(&r, es, e, x);
        vfinish(c, rd, &r, Q);
        return 0;
    }
    case 1: {  /* DUP general */
        uint64_t x = rx(c, rn);
        for (int e = 0; e < (Q ? 16 : 8) / es; e++) eset(&r, es, e, x);
        vfinish(c, rd, &r, Q);
        return 0;
    }
    case 3:  /* INS general */
        eset(&c->v[rd], es, idx, rx(c, rn));
        return 0;
    case 5:  /* SMOV */
        wx(c, rd, Q ? (uint64_t)esget(&c->v[rn], es, idx) : (uint32_t)esget(&c->v[rn], es, idx));
        return 0;
    case 7:  /* UMOV */
        wx(c, rd, eget(&c->v[rn], es, idx));
        return 0;
    default: return -1;
    }
}

static int simd_shift_imm(hao_cpu *c, uint32_t i, int scalar)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), immh = BITS(i, 22, 19), immb = BITS(i, 18, 16), op = BITS(i, 15, 11);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!immh) return -1;
    int hb = 31 - __builtin_clz(immh), es = 1 << hb, bits = es * 8;
    unsigned immhb = immh << 3 | immb;
    int rsh = 2 * bits - (int)immhb, lsh = (int)immhb - bits;
    hao_vreg a = c->v[rn], d = c->v[rd], r;
    memset(&r, 0, sizeof r);
    int bytes = scalar ? es : (Q ? 16 : 8), n = bytes / es;
    if (scalar) Q = 0;
    bool sat = false;

    switch (op) {
    case 0x00: case 0x02: case 0x04: case 0x06:  /* SSHR/USHR/SSRA/USRA/SRSHR/URSHR/SRSRA/URSRA */
        for (int e = 0; e < n; e++) {
            uint64_t v = shl_reg(eget(&a, es, e), -rsh, es, U, op >= 4);
            if (rsh >= bits && op < 4) v = U ? 0 : ((esget(&a, es, e) < 0) ? emask(es) : 0);
            if (op == 2 || op == 6) v += eget(&d, es, e);
            eset(&r, es, e, v & emask(es));
        }
        break;
    case 0x08:  /* SRI */
        if (!U) return -1;
        for (int e = 0; e < n; e++) {
            uint64_t m = rsh >= bits ? 0 : (emask(es) >> rsh);
            uint64_t v = rsh >= bits ? 0 : eget(&a, es, e) >> rsh;
            eset(&r, es, e, (eget(&d, es, e) & ~m) | v);
        }
        break;
    case 0x0A:  /* SHL / SLI */
        if (!scalar && !U && (es == 2 || es == 4)) {
            r = vr(es == 2 ? _mm_sll_epi16(vi(&a), _mm_cvtsi32_si128(lsh)) : _mm_sll_epi32(vi(&a), _mm_cvtsi32_si128(lsh)));
            break;
        }
        for (int e = 0; e < n; e++) {
            uint64_t v = (eget(&a, es, e) << lsh) & emask(es);
            if (U) { uint64_t m = (emask(es) << lsh) & emask(es); v = (eget(&d, es, e) & ~m) | v; }
            eset(&r, es, e, v);
        }
        break;
    case 0x0C: case 0x0E:  /* SQSHLU, SQSHL/UQSHL imm */
        for (int e = 0; e < n; e++) {
            __int128 v = U && op == 0x0E ? (__int128)eget(&a, es, e) : (__int128)esget(&a, es, e);
            v = (__int128)((u128)v << lsh);     /* |v| < 2^64 and lsh < 64: no bit is lost; a negative v stays negative */
            uint64_t o;
            if (op == 0x0C || U) { if (v < 0) { sat = true; o = 0; } else if (v > (__int128)emask(es)) { sat = true; o = emask(es); } else o = (uint64_t)v; }
            else if (es == 8) { if (v > INT64_MAX) { sat = true; o = (uint64_t)INT64_MAX; } else if (v < INT64_MIN) { sat = true; o = (uint64_t)INT64_MIN; } else o = (uint64_t)v; }
            else o = sat_s((int64_t)v, es, &sat);
            eset(&r, es, e, o);
        }
        break;
    case 0x10: case 0x11: case 0x12: case 0x13: {  /* SHRN/RSHRN/SQSHRUN/SQRSHRUN/SQSHRN/UQSHRN... */
        /* narrowing: immh encodes the destination element size */
        int oes = es, ies = es * 2;
        if (oes > 4) return -1;
        rsh = 2 * (oes * 8) - (int)immhb;
        r = Q ? d : r;
        int on = scalar ? 1 : 8 / oes;
        if (!scalar && !U && oes <= 2 && rsh > 0 && rsh <= bits) {
            __m128i packed = narrow_shift(vi(&a), oes, rsh, op & 1, op >= 0x12, &sat);
            r = vr(Q ? _mm_unpacklo_epi64(vi(&d), packed) : _mm_unpacklo_epi64(packed, _mm_setzero_si128()));
        } else for (int e = 0; e < on; e++) {
            bool rnd = op & 1;
            uint64_t x = eget(&a, ies, e), o;
            if (op <= 0x11 && !U) o = shl_reg(x, -rsh, ies, true, rnd) & emask(oes);
            else {
                bool src_u = (op >= 0x12 && U);
                int64_t v = src_u ? (int64_t)shl_reg(x, -rsh, ies, true, rnd) : sext(shl_reg(x, -rsh, ies, false, rnd), ies * 8);
                if (op <= 0x11) o = sat_u(v, oes, &sat);              /* SQSHRUN / SQRSHRUN */
                else if (U) o = (uint64_t)v > emask(oes) ? (sat = true, emask(oes)) : (uint64_t)v;
                else o = sat_s(v, oes, &sat);
            }
            eset(&r, oes, (Q ? on : 0) + e, o);
        }
        if (sat) c->fpsr |= 1u << 27;
        c->v[rd] = r;
        if (!Q) c->v[rd].d[1] = 0;
        return 0;
    }
    case 0x14: {  /* SSHLL / USHLL (UXTL/SXTL) */
        if (es == 8) return -1;             /* immh = 1xxx is unallocated: the result would be 128-bit elements */
        int oes = es * 2, on = 8 / es;
        if (!scalar && es <= 2) {
            __m128i src = vi(&a);
            if (Q) src = _mm_srli_si128(src, 8);
            __m128i wide = widen_low(src, es, U), count = _mm_cvtsi32_si128(lsh);
            r = vr(es == 1 ? _mm_sll_epi16(wide, count) : _mm_sll_epi32(wide, count));
            c->v[rd] = r;
            return 0;
        }
        for (int e = 0; e < on; e++) {
            uint64_t x = U ? eget(&a, es, (Q ? on : 0) + e) : (uint64_t)esget(&a, es, (Q ? on : 0) + e);
            eset(&r, oes, e, (x << lsh) & emask(oes));
        }
        c->v[rd] = r;
        return 0;
    }
    case 0x1C: case 0x1F: {  /* SCVTF/UCVTF fixed, FCVTZS/FCVTZU fixed */
        if (es < 2 || (es == 8 && !Q && !scalar)) return -1;
        int fbits = rsh;
        fpctx x = fp_ctx(c);
        for (int e = 0; e < n; e++) {
            if (op == 0x1C) eset(&r, es, e, fp_from_int(&x, es, eget(&a, es, e), !U, es * 8, fbits));
            else eset(&r, es, e, fp_to_int(&x, es, eget(&a, es, e), FPR_Z, U, es * 8, fbits) & emask(es));
        }
        break;
    }
    default: return -1;
    }
    if (sat) c->fpsr |= 1u << 27;
    vfinish(c, rd, &r, Q);
    return 0;
}

static int simd_three_diff(hao_cpu *c, uint32_t i, bool scalar)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 12);
    unsigned rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], b = c->v[rm], d = c->v[rd], r;
    /* The wide elements are twice the narrow ones, so size 3 is unallocated for every opcode but
     * PMULL (64 x 64 -> 128, U = 0); PMULL has no unsigned form and no 16- or 32-bit one. */
    if (op == 0xE ? U || (size != 0 && size != 3) : size == 3) return -1;
    memset(&r, 0, sizeof r);
    int es = 1 << size, oes = es * 2, n = 8 / es, part = Q ? n : 0;
    if (scalar && (U || (op != 9 && op != 11 && op != 13))) return -1;
    if (op == 9 || op == 11 || op == 13) { /* SQDMLAL/SQDMLSL/SQDMULL */
        if (U || (size != 1 && size != 2)) return -1;
        bool sat = false;
        if (scalar) { n = 1; part = 0; }
        for (int e = 0; e < n; e++)
            eset(&r, oes, e, signed_mull(esget(&a, es, part + e), esget(&b, es, part + e),
                                       esget(&d, oes, e), es, op == 13 ? 2 : op == 11, &sat));
        if (sat) c->fpsr |= FPSR_QC;
        c->v[rd] = r;
        return 0;
    }
    if (es <= 2 && (op <= 3 || op == 8 || op == 0xA || op == 0xC)) {
        __m128i x = vi(&a), y = vi(&b), v;
        if (Q) { if (op != 1 && op != 3) x = _mm_srli_si128(x, 8); y = _mm_srli_si128(y, 8); }
        if (op <= 3) {
            if (op != 1 && op != 3) x = widen_low(x, es, U);
            y = widen_low(y, es, U);
            v = op < 2 ? iadd(x, y, oes) : isub(x, y, oes);
        } else {
            v = mul_long(x, y, es, U);
            if (op == 8) v = iadd(vi(&d), v, oes);
            else if (op == 0xA) v = isub(vi(&d), v, oes);
        }
        c->v[rd] = vr(v);
        return 0;
    }
    if (op == 0x4 || op == 0x6) {  /* ADDHN/RADDHN/SUBHN/RSUBHN: wide inputs, narrow output */
        r = Q ? d : r;
        for (int e = 0; e < n; e++) {
            uint64_t x = eget(&a, oes, e), y = eget(&b, oes, e), v = op == 4 ? x + y : x - y;
            if (U) v += 1ull << (es * 8 - 1);
            eset(&r, es, part + e, (v >> (es * 8)) & emask(es));
        }
        c->v[rd] = r;
        if (!Q) c->v[rd].d[1] = 0;
        return 0;
    }
    if (op == 0xE) {  /* PMULL / PMULL2 */
        if (size == 0) {
            for (int e = 0; e < 8; e++) {
                uint64_t x = a.b[part + e], y = b.b[part + e], p = 0;
                for (int k = 0; k < 8; k++) if ((y >> k) & 1) p ^= x << k;
                r.h[e] = (uint16_t)p;
            }
        } else {
            uint64_t x = a.d[Q], y = b.d[Q];
            u128 p = 0;
            for (int k = 0; k < 64; k++) if ((y >> k) & 1) p ^= (u128)x << k;
            r.q = p;
        }
        c->v[rd] = r;
        return 0;
    }
    for (int e = 0; e < n; e++) {
        /* Sign- or zero-extended operands in uint64_t: the wide result is the low oes bytes of the
         * exact one, which unsigned (modulo 2^64) arithmetic gives for sums, differences and products. */
        uint64_t x, y;
        bool wide = op == 1 || op == 3;
        if (wide) x = U ? eget(&a, oes, e) : (uint64_t)esget(&a, oes, e);
        else x = U ? eget(&a, es, part + e) : (uint64_t)esget(&a, es, part + e);
        y = U ? eget(&b, es, part + e) : (uint64_t)esget(&b, es, part + e);
        uint64_t acc = eget(&d, oes, e), v;
        uint64_t ad = (int64_t)x > (int64_t)y ? x - y : y - x;   /* both operands fit in 33 signed bits here */
        switch (op) {
        case 0x0: case 0x1: v = x + y; break;
        case 0x2: case 0x3: v = x - y; break;
        case 0x5: v = acc + ad; break;
        case 0x7: v = ad; break;
        case 0x8: v = acc + x * y; break;
        case 0xA: v = acc - x * y; break;
        case 0xC: v = x * y; break;
        default: return -1;
        }
        eset(&r, oes, e, v & emask(oes));
    }
    c->v[rd] = r;
    return 0;
}

static int simd_permute(hao_cpu *c, uint32_t i)
{
    unsigned Q = BIT(i, 30), size = BITS(i, 23, 22), op = BITS(i, 14, 12);
    unsigned rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], b = c->v[rm], r;
    memset(&r, 0, sizeof r);
    int es = 1 << size, n = (Q ? 16 : 8) / es, half = n / 2;
    for (int e = 0; e < n; e++) {
        uint64_t v;
        switch (op) {
        case 1: case 5: { int j = 2 * e + (op == 5); v = j < n ? eget(&a, es, j) : eget(&b, es, j - n); break; }   /* UZP */
        case 2: case 6: { int p = op == 6; v = (e & 1) ? eget(&b, es, (e & ~1) + p) : eget(&a, es, (e & ~1) + p); break; } /* TRN */
        case 3: case 7: { int base = op == 7 ? half : 0; v = (e & 1) ? eget(&b, es, base + e / 2) : eget(&a, es, base + e / 2); break; } /* ZIP */
        default: return -1;
        }
        eset(&r, es, e, v);
    }
    vfinish(c, rd, &r, Q);
    return 0;
}

static int simd_indexed(hao_cpu *c, uint32_t i, int scalar)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 12);
    unsigned L = BIT(i, 21), M = BIT(i, 20), H = BIT(i, 11), rm = BITS(i, 19, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], d = c->v[rd], r;
    memset(&r, 0, sizeof r);
    int es, idx;
    if (((op == 1 || op == 5) && !U) || op == 9) {  /* FMLA/FMLS/FMUL(X) by element (single/double) */
        if (size == 0) { es = 2; idx = (int)(H << 2 | L << 1 | M); }
        else if (size == 2) { es = 4; idx = (int)(H << 1 | L); rm |= M << 4; }
        else if (size == 3) { es = 8; idx = (int)H; rm |= M << 4; }
        else return -1;
        if ((op != 9 && U) || (es == 8 && (L || (!Q && !scalar)))) return -1;
        int n = scalar ? 1 : (Q ? 16 : 8) / es;
        uint64_t y = eget(&c->v[rm], es, idx);
        if (!scalar && es == 4 && op == 9 && !U) {
            hao_vreg broadcast = {.s = {(uint32_t)y, (uint32_t)y, (uint32_t)y, (uint32_t)y}};
            if (fp_vec32(FPV_MUL, &a, &broadcast, n, &r)) {
                vfinish(c, rd, &r, Q);
                return 0;
            }
        }
        fpctx x = fp_ctx(c);
        if (!scalar && es == 2 && (op == 1 || op == 5)) {
            hao_vreg broadcast;
            for (int e = 0; e < 8; e++) broadcast.h[e] = (uint16_t)y;
            if (fp_vec16_muladd(&x, &d, &a, &broadcast, n, op == 5, &r)) {
                vfinish(c, rd, &r, Q);
                return 0;
            }
        }
        if (!scalar && es == 4 && (op == 1 || op == 5)) {
            hao_vreg broadcast = {.s = {(uint32_t)y, (uint32_t)y, (uint32_t)y, (uint32_t)y}};
            if (fp_vec32_muladd(&x, &d, &a, &broadcast, n, op == 5, &r)) {
                vfinish(c, rd, &r, Q);
                return 0;
            }
        }
        for (int e = 0; e < n; e++) {
            uint64_t xa = eget(&a, es, e), acc = eget(&d, es, e), v;
            if (op == 9) v = U ? fp_mulx(&x, es, xa, y) : fp_mul(&x, es, xa, y);   /* FMULX / FMUL */
            else if (op == 1) v = fp_muladd(&x, es, acc, xa, y);                     /* FMLA */
            else v = fp_muladd(&x, es, acc, fp_neg(xa, es), y);                      /* FMLS */
            eset(&r, es, e, v);
        }
        if (scalar) Q = 0;
        vfinish(c, rd, &r, Q);
        return 0;
    }
    if (size == 1) { es = 2; idx = (int)(H << 2 | L << 1 | M); }
    else if (size == 2) { es = 4; idx = (int)(H << 1 | L); rm |= M << 4; }
    else return -1;
    uint64_t y = eget(&c->v[rm], es, idx);
    int64_t sy = esget(&c->v[rm], es, idx);
    if ((!U && (op == 3 || op == 7 || op == 11)) || (U && (op == 13 || op == 15))) {
        bool sat = false, wide = !U;
        int oes = wide ? es * 2 : es;
        int n = scalar ? 1 : (wide ? 8 : (Q ? 16 : 8)) / es;
        int part = wide && Q && !scalar ? n : 0;
        for (int e = 0; e < n; e++) {
            int64_t av = esget(&a, es, part + e), acc = esget(&d, oes, e);
            uint64_t v = wide ? signed_mull(av, sy, acc, es, op == 11 ? 2 : op == 7, &sat)
                              : signed_rdmla(av, sy, acc, es, op == 15, &sat);
            eset(&r, oes, e, v);
        }
        if (sat) c->fpsr |= FPSR_QC;
        if (wide) c->v[rd] = r;
        else vfinish(c, rd, &r, scalar ? 0 : Q);
        return 0;
    }
    if (!U && (op == 0xC || op == 0xD)) {  /* SQDMULH/SQRDMULH by element */
        bool sat = false;
        int n = scalar ? 1 : (Q ? 16 : 8) / es;
        if (es == 2) {
            r = vr(mulhigh_half(vi(&a), _mm_set1_epi16((short)y), op == 0xD, n * es, &sat));
            if (scalar) r.d[0] &= 0xffff;
        }
        else for (int e = 0; e < n; e++)
            eset(&r, es, e, signed_mulhigh(esget(&a, es, e), sy, es, op == 0xD, &sat));
        if (sat) c->fpsr |= FPSR_QC;
        vfinish(c, rd, &r, scalar ? 0 : Q);
        return 0;
    }
    if ((op == 0x8 && !U) || ((op == 0x0 || op == 0x4) && U)) {  /* MUL / MLA / MLS by element */
        int n = (Q ? 16 : 8) / es;
        if (!scalar && es == 2) {
            __m128i v = _mm_mullo_epi16(vi(&a), _mm_set1_epi16((short)y));
            if (op != 8) v = op == 0 ? _mm_add_epi16(vi(&d), v) : _mm_sub_epi16(vi(&d), v);
            r = vr(v);
        } else for (int e = 0; e < n; e++) {
            uint64_t p = eget(&a, es, e) * y, acc = eget(&d, es, e), v;
            v = op == 8 ? p : (op == 0 ? acc + p : acc - p);
            eset(&r, es, e, v & emask(es));
        }
        vfinish(c, rd, &r, Q);
        return 0;
    }
    if (op == 0xA || op == 0x2 || op == 0x6) {  /* SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL by element */
        int oes = es * 2, n = 8 / es, part = Q ? n : 0;
        if (!scalar && es == 2) {
            __m128i src = vi(&a);
            if (Q) src = _mm_srli_si128(src, 8);
            __m128i v = mul_long(src, _mm_set1_epi16((short)y), es, U);
            if (op != 0xA) v = op == 2 ? _mm_add_epi32(vi(&d), v) : _mm_sub_epi32(vi(&d), v);
            r = vr(v);
        } else for (int e = 0; e < n; e++) {
            uint64_t x = U ? eget(&a, es, part + e) : (uint64_t)esget(&a, es, part + e);
            uint64_t p = x * (U ? y : (uint64_t)sy), acc = eget(&d, oes, e), v;    /* modulo 2^64, as the wide element wraps */
            v = op == 0xA ? p : (op == 2 ? acc + p : acc - p);
            eset(&r, oes, e, v & emask(oes));
        }
        c->v[rd] = r;
        return 0;
    }
    return -1;
}

static int simd_ext_tbl(hao_cpu *c, uint32_t i)
{
    unsigned Q = BIT(i, 30), rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg r;
    memset(&r, 0, sizeof r);
    if ((i & 0xBFE08400) == 0x2E000000) {  /* EXT */
        unsigned pos = BITS(i, 14, 11);
        int n = Q ? 16 : 8;
        if (!Q && (pos & 8)) return -1;    /* reserved: the index is beyond the 64-bit pair */
        uint8_t cat[32];
        memcpy(cat, c->v[rn].b, (size_t)n);
        memcpy(cat + n, c->v[rm].b, (size_t)n);
        for (int e = 0; e < n; e++) r.b[e] = cat[pos + (unsigned)e];
        vfinish(c, rd, &r, Q);
        return 0;
    }
    /* TBL / TBX */
    unsigned len = BITS(i, 14, 13) + 1, tbx = BIT(i, 12);
    uint8_t table[64];
    for (unsigned k = 0; k < len; k++) memcpy(table + 16 * k, c->v[(rn + k) & 31].b, 16);
    hao_vreg idx = c->v[rm];
    if (tbx) r = c->v[rd];
    for (int e = 0; e < (Q ? 16 : 8); e++) {
        unsigned ix = idx.b[e];
        if (ix < 16 * len) r.b[e] = table[ix];
        else if (!tbx) r.b[e] = 0;
    }
    vfinish(c, rd, &r, Q);
    return 0;
}

static int simd_scalar_pairwise(hao_cpu *c, uint32_t i)
{
    unsigned U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 16, 12), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg a = c->v[rn], r;
    memset(&r, 0, sizeof r);
    if (!U && op == 0x1B && size == 3) { r.d[0] = a.d[0] + a.d[1]; c->v[rd] = r; return 0; }  /* ADDP Dd */
    if (U) {
        int es = BIT(size, 0) ? 8 : 4;
        fpctx x = fp_ctx(c);
        uint64_t xa = eget(&a, es, 0), ya = eget(&a, es, 1), v;
        unsigned key = BIT(size, 1) << 5 | op;
        switch (key) {
        case 0x0D: v = fp_add(&x, es, xa, ya); break;     /* FADDP */
        case 0x0C: v = fp_maxnm(&x, es, xa, ya); break;   /* FMAXNMP */
        case 0x0F: v = fp_max(&x, es, xa, ya); break;     /* FMAXP */
        case 0x2C: v = fp_minnm(&x, es, xa, ya); break;   /* FMINNMP */
        case 0x2F: v = fp_min(&x, es, xa, ya); break;     /* FMINP */
        default: return -1;
        }
        eset(&r, es, 0, v);
        c->v[rd] = r;
        return 0;
    }
    return -1;
}

/* SDOT/UDOT (vector): 0 Q U 01110 10 0 Rm 1001 01 Rn Rd */
static int simd_dot(hao_cpu *c, uint32_t i, int indexed)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0), rm = BITS(i, 20, 16);
    hao_vreg a = c->v[rn], b, r = c->v[rd];
    if (indexed) {
        unsigned idx = BIT(i, 11) << 1 | BIT(i, 21);
        rm = BITS(i, 20, 16);
        hao_vreg m = c->v[rm];
        for (int e = 0; e < 4; e++) b.s[e] = m.s[idx];
    } else b = c->v[rm];
    for (int e = 0; e < (Q ? 4 : 2); e++) {
        uint32_t acc = r.s[e];      /* the accumulator wraps */
        for (int k = 0; k < 4; k++)
            acc += (uint32_t)(U ? (int32_t)a.b[4 * e + k] * (int32_t)b.b[4 * e + k] : (int32_t)a.sb[4 * e + k] * (int32_t)b.sb[4 * e + k]);
        r.s[e] = acc;
    }
    vfinish(c, rd, &r, Q);
    return 0;
}

/* ------------------------------------------------ cryptographic extension
 * Decode from the Arm ARM's A64 encoding index (the AES, SHA three-register,
 * SHA two-register, three-register SHA512, two-register SHA512,
 * four-register and XAR groups), semantics from each instruction's
 * pseudocode: FEAT_AES, FEAT_SHA1, FEAT_SHA256, FEAT_SHA512 and FEAT_SHA3,
 * the set Apple silicon implements (hw.optional.arm.FEAT_*; FEAT_PMULL is
 * PMULL in simd_three_diff). Apple silicon has no FEAT_SM3 or FEAT_SM4, so
 * SM3SS1, SM3TT*, SM3PARTW*, SM4E and SM4EKEY are UNDEFINED there, as every
 * reserved size/opcode of these groups is; hao rejects the same set (the
 * oracle's crypto records measure it). */

/* The AES S-box and its inverse (the tables of the Arm ARM's AESSubBytes and
 * AESInvSubBytes; FIPS-197 5.1.1: multiplicative inverse in GF(2^8) modulo
 * x^8+x^4+x^3+x+1, then the affine transform with 0x63). */
static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static inline uint32_t ror32(uint32_t x, unsigned n) { n &= 31; return n ? x >> n | x << (32 - n) : x; }
static inline uint32_t rol32(uint32_t x, unsigned n) { return ror32(x, 32 - n); }
static inline uint64_t ror64(uint64_t x, unsigned n) { n &= 63; return n ? x >> n | x << (64 - n) : x; }

/* Multiply four independent GF(2^8) bytes by x modulo x^8+x^4+x^3+x+1.
 * Clear each high bit before shifting. The reduction lanes are only 0 or 1,
 * so multiplication by 0x1b cannot carry across their byte boundaries. */
static inline uint32_t aes_xtime_word(uint32_t x)
{
    return ((x & 0x7f7f7f7fu) << 1) ^ (((x >> 7) & 0x01010101u) * 0x1bu);
}

/* AESSubBytes(AESShiftRows(a)) or AESInvSubBytes(AESInvShiftRows(a)).
 * State byte k is row k % 4 of column k / 4; row r of column c comes from
 * column c + r (inverse: c - r) of the same row. */
static hao_vreg aes_rows_bytes(hao_vreg a, bool inv)
{
    hao_vreg r;
    for (int k = 0; k < 16; k++) {
        int row = k & 3, col = k >> 2, src = (inv ? col - row : col + row) & 3;
        r.b[k] = inv ? aes_inv_sbox[a.b[4 * src + row]] : aes_sbox[a.b[4 * src + row]];
    }
    return r;
}

/* AESMixColumns / AESInvMixColumns: each little-endian word is one column.
 * Forward row k is a[k] ^ parity(column) ^ xtime(a[k] ^ a[k+1]).
 * For inverse, first XOR 04*(a[k] ^ a[k+2]) into every row, then apply the
 * forward matrix: its product with this preprocessing is (0E 0B 0D 09).
 * All four rows are processed together; no optional host ISA is required. */
static hao_vreg aes_mix(hao_vreg a, bool inv)
{
    hao_vreg r;
    for (int col = 0; col < 4; col++) {
        uint32_t x = a.s[col];
        if (inv) x ^= aes_xtime_word(aes_xtime_word(x ^ ror32(x, 16)));
        uint32_t next = ror32(x, 8);
        uint32_t parity = x ^ next ^ ror32(x, 16) ^ ror32(x, 24);
        r.s[col] = x ^ parity ^ aes_xtime_word(x ^ next);
    }
    return r;
}

/* AESE/AESD/AESMC/AESIMC: 0100 1110 size 10100 opcode 10 Rn Rd */
static int crypto_aes(hao_cpu *c, uint32_t i)
{
    unsigned size = BITS(i, 23, 22), op = BITS(i, 16, 12), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (size || op < 4 || op > 7) return -1;
    hao_vreg a = c->v[rn], d = c->v[rd];
    switch (op) {
    case 4: d.q ^= a.q; c->v[rd] = aes_rows_bytes(d, false); break;   /* AESE */
    case 5: d.q ^= a.q; c->v[rd] = aes_rows_bytes(d, true); break;    /* AESD */
    case 6: c->v[rd] = aes_mix(a, false); break;                       /* AESMC */
    default: c->v[rd] = aes_mix(a, true); break;                       /* AESIMC */
    }
    return 0;
}

static inline uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static inline uint32_t sha_majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }
static inline uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }

/* SHA256hash(X, Y, W, part1) */
static hao_vreg sha256_hash(hao_vreg X, hao_vreg Y, const hao_vreg *W, bool part1)
{
    uint32_t *x = X.s, *y = Y.s;
    for (int e = 0; e < 4; e++) {
        uint32_t chs = sha_choose(y[0], y[1], y[2]), maj = sha_majority(x[0], x[1], x[2]);
        uint32_t t = y[3] + (ror32(y[0], 6) ^ ror32(y[0], 11) ^ ror32(y[0], 25)) + chs + W->s[e];
        x[3] = t + x[3];
        y[3] = t + (ror32(x[0], 2) ^ ror32(x[0], 13) ^ ror32(x[0], 22)) + maj;
        /* <Y, X> = ROL(Y : X, 32) */
        uint32_t xt = x[3], yt = y[3];
        x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = yt;
        y[3] = y[2]; y[2] = y[1]; y[1] = y[0]; y[0] = xt;
    }
    return part1 ? X : Y;
}

/* SHA1C/P/M, SHA1SU0, SHA256H/H2, SHA256SU1: 0101 1110 size 0 Rm 0 opcode 00 Rn Rd */
static int crypto_sha3reg(hao_cpu *c, uint32_t i)
{
    unsigned size = BITS(i, 23, 22), op = BITS(i, 14, 12), rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (size || op == 7) return -1;
    hao_vreg d = c->v[rd], n = c->v[rn], m = c->v[rm], r;
    switch (op) {
    case 0: case 1: case 2: {   /* SHA1C (choose), SHA1P (parity), SHA1M (majority) */
        uint32_t *x = d.s, y = n.s[0];
        for (int e = 0; e < 4; e++) {
            uint32_t t = op == 0 ? sha_choose(x[1], x[2], x[3]) : op == 1 ? sha_parity(x[1], x[2], x[3])
                                                                           : sha_majority(x[1], x[2], x[3]);
            y = y + rol32(x[0], 5) + t + m.s[e];
            x[1] = rol32(x[1], 30);
            uint32_t top = x[3];   /* <Y, X> = ROL(Y : X, 32) */
            x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = y; y = top;
        }
        r = d;
        break;
    }
    case 3:                     /* SHA1SU0: (Vn<63:0> : Vd<127:64>) EOR Vd EOR Vm */
        r.d[0] = d.d[1];
        r.d[1] = n.d[0];
        r.q ^= d.q ^ m.q;
        break;
    case 4: r = sha256_hash(d, n, &m, true); break;    /* SHA256H */
    case 5: r = sha256_hash(n, d, &m, false); break;   /* SHA256H2 */
    default: {                  /* SHA256SU1 */
        uint32_t t0[4] = {n.s[1], n.s[2], n.s[3], m.s[0]};   /* Vm<31:0> : Vn<127:32> */
        for (int e = 0; e < 4; e++) {
            uint32_t elt = e < 2 ? m.s[e + 2] : r.s[e - 2];
            elt = ror32(elt, 17) ^ ror32(elt, 19) ^ elt >> 10;
            r.s[e] = elt + d.s[e] + t0[e];
        }
        break;
    }
    }
    c->v[rd] = r;
    return 0;
}

/* SHA1H, SHA1SU1, SHA256SU0: 0101 1110 size 10100 opcode 10 Rn Rd */
static int crypto_sha2reg(hao_cpu *c, uint32_t i)
{
    unsigned size = BITS(i, 23, 22), op = BITS(i, 16, 12), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (size || op > 2) return -1;
    hao_vreg d = c->v[rd], n = c->v[rn], r;
    memset(&r, 0, sizeof r);
    if (op == 0) r.s[0] = rol32(n.s[0], 30);           /* SHA1H Sd, Sn */
    else if (op == 1) {                                /* SHA1SU1 */
        hao_vreg t;
        t.q = d.q ^ n.q >> 32;
        r.s[0] = rol32(t.s[0], 1);
        r.s[1] = rol32(t.s[1], 1);
        r.s[2] = rol32(t.s[2], 1);
        r.s[3] = rol32(t.s[3], 1) ^ rol32(t.s[0], 2);
    } else {                                           /* SHA256SU0 */
        uint32_t t[4] = {d.s[1], d.s[2], d.s[3], n.s[0]};   /* Vn<31:0> : Vd<127:32> */
        for (int e = 0; e < 4; e++) r.s[e] = (ror32(t[e], 7) ^ ror32(t[e], 18) ^ t[e] >> 3) + d.s[e];
    }
    c->v[rd] = r;
    return 0;
}

/* The 0xCE space: four-register (EOR3, BCAX; SM3SS1), XAR, three-register
 * SHA512 (SHA512H, SHA512H2, SHA512SU1, RAX1; SM3PARTW1/2, SM4EKEY with
 * O = 1), two-register SHA512 (SHA512SU0; SM4E). SM3TT* and every other
 * slot are unallocated or unimplemented: UNDEFINED. */
static int crypto_ce(hao_cpu *c, uint32_t i)
{
    unsigned rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    hao_vreg n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    if ((i & 0xFF808000) == 0xCE000000) {              /* 1100 1110 0 Op0 Rm 0 Ra Rn Rd */
        hao_vreg a = c->v[BITS(i, 14, 10)];
        switch (BITS(i, 22, 21)) {
        case 0: r.q = n.q ^ m.q ^ a.q; break;          /* EOR3 */
        case 1: r.q = n.q ^ (m.q & ~a.q); break;       /* BCAX */
        default: return -1;                            /* SM3SS1, unallocated */
        }
    } else if ((i & 0xFFE00000) == 0xCE800000) {       /* XAR: 1100 1110 100 Rm imm6 Rn Rd */
        unsigned imm6 = BITS(i, 15, 10);
        for (int e = 0; e < 2; e++) r.d[e] = ror64(n.d[e] ^ m.d[e], imm6);
    } else if ((i & 0xFFE0F000) == 0xCE608000) {       /* 1100 1110 011 Rm 1 O=0 00 opcode Rn Rd */
        uint64_t x0 = n.d[0], x1 = n.d[1], y0 = m.d[0], y1 = m.d[1], w0 = d.d[0], w1 = d.d[1];
        switch (BITS(i, 11, 10)) {
        case 0: {                                      /* SHA512H */
            uint64_t s1 = ror64(y1, 14) ^ ror64(y1, 18) ^ ror64(y1, 41);
            uint64_t hi = ((y1 & x0) ^ (~y1 & x1)) + s1 + w1, tmp = hi + y0;
            s1 = ror64(tmp, 14) ^ ror64(tmp, 18) ^ ror64(tmp, 41);
            r.d[1] = hi;
            r.d[0] = ((tmp & y1) ^ (~tmp & x0)) + s1 + w0;
            break;
        }
        case 1: {                                      /* SHA512H2 */
            uint64_t s0 = ror64(y0, 28) ^ ror64(y0, 34) ^ ror64(y0, 39);
            uint64_t hi = ((x0 & y1) ^ (x0 & y0) ^ (y1 & y0)) + s0 + w1;
            s0 = ror64(hi, 28) ^ ror64(hi, 34) ^ ror64(hi, 39);
            r.d[1] = hi;
            r.d[0] = ((hi & y0) ^ (hi & y1) ^ (y1 & y0)) + s0 + w0;
            break;
        }
        case 2:                                        /* SHA512SU1 */
            r.d[1] = w1 + (ror64(x1, 19) ^ ror64(x1, 61) ^ x1 >> 6) + y1;
            r.d[0] = w0 + (ror64(x0, 19) ^ ror64(x0, 61) ^ x0 >> 6) + y0;
            break;
        default:                                       /* RAX1: Vn EOR ROL(Vm, 1) */
            for (int e = 0; e < 2; e++) r.d[e] = n.d[e] ^ ror64(m.d[e], 63);
            break;
        }
    } else if ((i & 0xFFFFFC00) == 0xCEC08000) {       /* SHA512SU0: 1100 1110 1100 0000 1000 00 Rn Rd */
        uint64_t w0 = d.d[0], w1 = d.d[1], x0 = n.d[0];
        r.d[0] = w0 + (ror64(w1, 1) ^ ror64(w1, 8) ^ w1 >> 7);
        r.d[1] = w1 + (ror64(x0, 1) ^ ror64(x0, 8) ^ x0 >> 7);
    } else return -1;
    c->v[rd] = r;
    return 0;
}

int a64_exec_simd(hao_cpu *c, uint32_t i, uint64_t pc)
{
    if ((i & 0xFF3E0C00) == 0x4E280800) return crypto_aes(c, i);
    if ((i & 0xFF208C00) == 0x5E000000) return crypto_sha3reg(c, i);
    if ((i & 0xFF3E0C00) == 0x5E280800) return crypto_sha2reg(c, i);
    if ((i & 0xFF000000) == 0xCE000000) return crypto_ce(c, i);
    if ((i & 0xBF20E400) == 0x2E00C400) return simd_complex(c, i, false, false);
    if ((i & 0xBF20EC00) == 0x2E00E400) return simd_complex(c, i, false, true);
    if ((i & 0xBF009400) == 0x2F001000) return simd_complex(c, i, true, false);
    if ((i & 0x9FE0FC00) == 0x0E809400) return simd_dot(c, i, 0);
    if ((i & 0x9FC0F400) == 0x0F80E000) return simd_dot(c, i, 1);
    if ((i & 0xAF20F400) == 0x2E008400) return simd_rdmla(c, i);
    if (((i & 0x9F200400) == 0x0E000400 || (i & 0xDF200400) == 0x5E000400) && BIT(i, 22)) return simd_half_three_same(c, i);
    if ((i & 0x9F3E0C00) == 0x0E380800 || (i & 0xDF3E0C00) == 0x5E380800) return simd_half_two_misc(c, i);
    int r = -1;
    (void)pc;
    if ((i & 0x5F000000) == 0x1E000000 || (i & 0x5F000000) == 0x1F000000) {
        if (!BIT(i, 30) || (i & 0xFFFEFC00) == 0x9EAE0000) r = fp_scalar(c, i);
        return r;
    }
    if ((i & 0x9F200400) == 0x0E200400) return simd_three_same(c, i, 0);
    if ((i & 0xDF200400) == 0x5E200400) return simd_three_same(c, i, 1);
    if ((i & 0x9F3E0C00) == 0x0E200800) return simd_two_misc(c, i, 0);
    if ((i & 0xDF3E0C00) == 0x5E200800) return simd_two_misc(c, i, 1);
    if ((i & 0x9F3E0C00) == 0x0E300800) return simd_across(c, i);
    if ((i & 0xDF3E0C00) == 0x5E300800) return simd_scalar_pairwise(c, i);
    if ((i & 0x9FF80400) == 0x0F000400) return simd_modimm(c, i);
    if ((i & 0x9FE08400) == 0x0E000400) return simd_copy(c, i, 0);
    if ((i & 0xDFE08400) == 0x5E000400) return simd_copy(c, i, 1);
    if ((i & 0x9F800400) == 0x0F000400) return simd_shift_imm(c, i, 0);
    if ((i & 0xDF800400) == 0x5F000400) return simd_shift_imm(c, i, 1);
    if ((i & 0x9F200C00) == 0x0E200000) return simd_three_diff(c, i, false);
    if ((i & 0xDF200C00) == 0x5E200000) return simd_three_diff(c, i, true);
    if ((i & 0xBF208C00) == 0x0E000800) return simd_permute(c, i);
    if ((i & 0xBFE08400) == 0x2E000000 || (i & 0xBFE08C00) == 0x0E000000) return simd_ext_tbl(c, i);
    if ((i & 0x9F000400) == 0x0F000000) return simd_indexed(c, i, 0);
    if ((i & 0xDF000400) == 0x5F000000) return simd_indexed(c, i, 1);
    return -1;
}
