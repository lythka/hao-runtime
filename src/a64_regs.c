/*
 * hao — A64 register use read from a function's own instructions
 * (hao/a64_regs.h). Field positions and encodings follow the Arm ARM's
 * A64 instruction-set encoding tables; anything not classified here stops
 * the walk if it names register 8 and is neutral otherwise (an A64
 * instruction reaches a general register only through its Rd/Rt, Rn, Rm/Rs
 * and Ra/Rt2 fields, apart from fixed uses of x16, x17 and x30).
 */
#include "hao/a64_regs.h"

enum { R = 1, W = 2, STOP = 4, JUMP = 8, BOTH = 16, CALL = 32 };

static inline unsigned f(uint32_t i, unsigned lo) { return (i >> lo) & 31; }
static inline int64_t sext(uint32_t v, unsigned bits) { return (int64_t)((uint64_t)v << (64 - bits)) >> (64 - bits); }

/* any of the four register fields is register 8 */
static bool names8(uint32_t i) { return f(i, 0) == 8 || f(i, 5) == 8 || f(i, 10) == 8 || f(i, 16) == 8; }

/* reads of x8 among the fields given (a negative position: none) and a write when Rd is 8 */
static int rw(uint32_t i, int r1, int r2, int r3, bool writes_rd)
{
    if ((r1 >= 0 && f(i, (unsigned)r1) == 8) || (r2 >= 0 && f(i, (unsigned)r2) == 8) || (r3 >= 0 && f(i, (unsigned)r3) == 8)) return R;
    return writes_rd && f(i, 0) == 8 ? W : 0;
}

static int dp_imm(uint32_t i)
{
    switch ((i >> 23) & 7) {
    case 0: case 1: return rw(i, -1, -1, -1, true);                       /* ADR, ADRP */
    case 2: case 3: case 4: return rw(i, 5, -1, -1, true);                /* add/sub (tags), logical */
    case 5:                                                               /* MOVN, MOVZ, MOVK */
        switch ((i >> 29) & 3) {
        case 0: case 2: return rw(i, -1, -1, -1, true);
        case 3: return rw(i, 0, -1, -1, true);
        default: return names8(i) ? STOP : 0;
        }
    case 6:                                                               /* SBFM, BFM, UBFM */
        switch ((i >> 29) & 3) {
        case 0: case 2: return rw(i, 5, -1, -1, true);
        case 1: return rw(i, 5, 0, -1, true);
        default: return names8(i) ? STOP : 0;
        }
    default: return rw(i, 5, 16, -1, true);                               /* EXTR */
    }
}

static int dp_reg(uint32_t i)
{
    unsigned op = (i >> 24) & 0x1F;
    if (op == 0x0A || op == 0x0B) return rw(i, 5, 16, -1, true);         /* logical, add/sub shifted or extended */
    if (op == 0x1B) return rw(i, 5, 16, 10, true);                        /* 3-source */
    if (op == 0x1A) {
        switch ((i >> 21) & 7) {
        case 0: if (!((i >> 10) & 0x3F)) return rw(i, 5, 16, -1, true); break;   /* ADC, SBC */
        case 2: return rw(i, 5, (i >> 11) & 1 ? -1 : 16, -1, false);      /* CCMN, CCMP */
        case 4: return rw(i, 5, 16, -1, true);                            /* CSEL family */
        case 6:
            if (!((i >> 30) & 1)) return rw(i, 5, 16, -1, true);          /* 2-source */
            if (f(i, 16) == 0) return rw(i, 5, -1, -1, true);             /* 1-source: RBIT, REV, CLZ, ... */
            if (f(i, 16) == 1) return rw(i, 5, 0, -1, true);              /* PAC*, AUT*, XPAC*: Rd in and out */
            break;
        }
    }
    return names8(i) ? STOP : 0;
}

static int ldst(uint32_t i)
{
    bool v = (i >> 26) & 1;
    unsigned rt = f(i, 0);
    if ((i & 0x3B000000) == 0x18000000)                                   /* literal (no base register) */
        return !v && (i >> 30) != 3 && rt == 8 ? W : 0;
    if (f(i, 5) == 8) return R;                                           /* the base register, read first */
    if ((i & 0x3A000000) == 0x28000000) {                                 /* pair; a writeback writes Rn, not 8 here */
        if (!v && (rt == 8 || f(i, 10) == 8)) return (i >> 22) & 1 ? W : R;
        return 0;
    }
    if ((i & 0x3A000000) == 0x38000000) {                                 /* register: imm, unscaled, pre/post, offset */
        unsigned size = i >> 30, opc = (i >> 22) & 3;
        bool uimm = (i >> 24) & 1, b21 = (i >> 21) & 1;
        unsigned k = (i >> 10) & 3;
        if (!uimm && b21 && k == 2 && f(i, 16) == 8) return R;           /* register offset: Rm */
        if (!uimm && b21 && k == 0) {                                     /* atomic memory operations */
            if (v) return names8(i) ? STOP : 0;
            if (f(i, 16) == 8) return R;
            return rt == 8 ? W : 0;
        }
        if (!uimm && b21 && (k & 1)) return !v && size == 3 && rt == 8 ? W : (names8(i) ? STOP : 0);   /* LDRAA, LDRAB */
        if (v || rt != 8) return 0;
        if (size == 3 && opc == 2) return 0;                              /* PRFM, PRFUM */
        return opc == 0 ? R : W;
    }
    return names8(i) ? STOP : 0;
}

static int effect(uint32_t i, int64_t *off)
{
    if ((i & 0xFFFFF01F) == 0xD503201F || (i & 0xFFFFF01F) == 0xD503301F) return 0;   /* hints (PACIBSP, BTI, ...), barriers */
    if ((i & 0x7C000000) == 0x14000000) {                                 /* B, BL */
        *off = sext(i & 0x3FFFFFF, 26) * 4;
        return i >> 31 ? CALL : JUMP;
    }
    if ((i & 0x7E000000) == 0x34000000) { *off = sext((i >> 5) & 0x7FFFF, 19) * 4; return f(i, 0) == 8 ? R : BOTH; }   /* CBZ, CBNZ */
    if ((i & 0x7E000000) == 0x36000000) { *off = sext((i >> 5) & 0x3FFF, 14) * 4; return f(i, 0) == 8 ? R : BOTH; }    /* TBZ, TBNZ */
    if ((i & 0xFF000000) == 0x54000000) { *off = sext((i >> 5) & 0x7FFFF, 19) * 4; return BOTH; }                      /* B.cond, BC.cond */
    switch ((i >> 25) & 0xF) {
    case 0x8: case 0x9: return dp_imm(i);
    case 0x5: case 0xD: return dp_reg(i);
    case 0x4: case 0x6: case 0xC: case 0xE: return ldst(i);
    case 0x7: case 0xF: return f(i, 0) == 8 || f(i, 5) == 8 ? STOP : 0;   /* SIMD and FP: GPRs only as Rd or Rn */
    default: return STOP;                                                 /* branch, exception, system; reserved, SVE, SME */
    }
}

static bool walk(a64_fetch_fn fetch, void *ctx, uint64_t pc, int depth, int *budget)
{
    for (;;) {
        uint32_t i;
        int64_t off = 0;
        if (--*budget < 0 || !fetch(ctx, pc, &i)) return false;
        int e = effect(i, &off);
        if (e & R) return true;
        if (e & (W | STOP)) return false;
        if (e & JUMP) { pc += (uint64_t)off; continue; }
        /* BL with x8 not yet written: x8 reaches the callee, which takes it as an input when its own code
         * reads it first. After the call x8 is the callee's (caller-saved), so the walk ends here. */
        if (e & CALL) return depth < 4 && walk(fetch, ctx, pc + (uint64_t)off, depth + 1, budget);
        if ((e & BOTH) && depth < 4 && walk(fetch, ctx, pc + (uint64_t)off, depth + 1, budget)) return true;
        pc += 4;
    }
}

bool a64_x8_live_in(a64_fetch_fn fetch, void *ctx, uint64_t entry, int budget)
{
    if (entry & 3) return false;
    return walk(fetch, ctx, entry, 0, &budget);
}
