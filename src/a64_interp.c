/*
 * hao — arm64/arm64e reference interpreter (integer, branch, system,
 * load/store, atomics). FP/SIMD lives in a64_simd.c.
 *
 * Roles:
 *   1. Oracle for differential tests of the JIT.
 *   2. Fallback "helper" the JIT calls for instructions it does not
 *      translate natively (as QEMU's TCG helpers do).
 *
 * Decode follows the Arm ARM; where the architecture leaves a choice
 * (CONSTRAINED UNPREDICTABLE writeback/register overlaps, should-be-one
 * fields) hao does what Apple silicon does, as measured by the oracle
 * (tests/oracle): e.g. LDP with Rt == Rt2 and pre/post-index with Rn == Rt
 * are UNDEFINED, ignored SBO fields are accepted. FEAT_CSSC and FEAT_MTE
 * are not implemented by the M4 the oracle runs on, so their encodings are
 * UNDEFINED here too.
 *
 * arm64e pointer authentication: PAC* and AUT* (the HINT-space forms too)
 * leave values unchanged, and AA and AB branch variants and LDRAA/LDRAB
 * behave as their unauthenticated forms. The loader applies authenticated
 * fixups unsigned and nothing hao produces carries a signature, so every
 * pointer the guest sees is raw and AUT* never fails. That is also what the
 * M4 does in the arm64 (non-e) process the oracle runs in: the A/B keys are
 * off there, so PAC* and AUT* are the identity for every value and modifier.
 * XPACI / XPACD / XPACLRI strip as the hardware does (a64_xpac): bits above
 * the 47-bit VA become copies of bit 55, the top byte kept by XPACD for a
 * bit-55-clear pointer (TBI0 for data only). Canonical pointers, the only
 * ones hao makes, come back unchanged. PACGA computes a real MAC natively;
 * hao's is a deterministic stand-in (the oracle skips PACGA).
 */
#include "hao/cpu.h"
#include "hao/a64_imm.h"
#include "hao/code.h"
#include <string.h>
#include <mach/mach_time.h>

typedef unsigned __int128 u128;
typedef __int128 s128;

#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(x, n) (((x) >> (n)) & 1u)

static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static inline uint64_t rx(hao_cpu *c, unsigned r) { return r == 31 ? 0 : c->x[r]; }
static inline uint64_t rsp(hao_cpu *c, unsigned r) { return c->x[r]; }
static inline void wx(hao_cpu *c, unsigned r, uint64_t v) { if (r != 31) c->x[r] = v; }
static inline void wsp(hao_cpu *c, unsigned r, uint64_t v) { c->x[r] = v; }
static inline uint64_t msk(int sf, uint64_t v) { return sf ? v : (uint32_t)v; }

static inline uint64_t ldm(uint64_t a, int size)
{
    const void *p = (const void *)(uintptr_t)hao_tbi(a);
    switch (size) {
    case 1: return *(const volatile uint8_t *)p;
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    default: { uint64_t v; memcpy(&v, p, 8); return v; }
    }
}
static inline void stm(uint64_t a, int size, uint64_t v)
{
    void *p = (void *)(uintptr_t)hao_tbi(a);
    switch (size) {
    case 1: *(volatile uint8_t *)p = (uint8_t)v; break;
    case 2: { uint16_t t = (uint16_t)v; memcpy(p, &t, 2); break; }
    case 4: { uint32_t t = (uint32_t)v; memcpy(p, &t, 4); break; }
    default: memcpy(p, &v, 8); break;
    }
}

/*
 * 16-byte single-copy-atomic load/store of a 16-byte aligned address: what
 * FEAT_LSE2 (every Apple core) guarantees for LDP/STP of two X registers,
 * and what LDXP/LDAXP need. An aligned SSE access (movdqa) is atomic on every
 * CPU that enumerates AVX (Intel SDM vol. 3A 9.1.1; every Intel Mac that runs
 * a hao-supported macOS has AVX) and, unlike cmpxchg16b, never writes: it
 * works on read-only pages and does not dirty copy-on-write or shared ones.
 */
typedef long long v2di __attribute__((vector_size(16)));
static inline u128 ld128_atomic(uint64_t a)
{
    const volatile v2di *p = (const volatile v2di *)(uintptr_t)hao_tbi(a);
    v2di x;
    __asm__ volatile("movdqa %1, %0" : "=x"(x) : "m"(*p));
    u128 v;
    memcpy(&v, &x, 16);
    return v;
}
static inline void st128_atomic(uint64_t a, u128 v)
{
    volatile v2di *p = (volatile v2di *)(uintptr_t)hao_tbi(a);
    v2di x;
    memcpy(&x, &v, 16);
    __asm__ volatile("movdqa %1, %0" : "=m"(*p) : "x"(x));
}

static uint64_t awc(int sf, uint64_t x, uint64_t y, unsigned carry, unsigned *nzcv)
{
    uint64_t res;
    unsigned n, z, cf, v;
    if (sf) {
        u128 us = (u128)x + y + carry;
        s128 ss = (s128)(int64_t)x + (int64_t)y + carry;
        res = (uint64_t)us;
        n = res >> 63; z = res == 0; cf = (us >> 64) != 0; v = ss != (s128)(int64_t)res;
    } else {
        x = (uint32_t)x; y = (uint32_t)y;
        uint64_t us = x + y + carry;
        int64_t ss = (int64_t)(int32_t)x + (int32_t)y + carry;
        res = (uint32_t)us;
        n = (res >> 31) & 1; z = res == 0; cf = (us >> 32) != 0; v = ss != (int64_t)(int32_t)res;
    }
    if (nzcv) *nzcv = n << 3 | z << 2 | cf << 1 | v;
    return res;
}

static void set_nz(hao_cpu *c, int sf, uint64_t r)
{
    unsigned n = sf ? (unsigned)(r >> 63) : (unsigned)((r >> 31) & 1);
    hao_set_nzcv(c, n << 3 | (msk(sf, r) == 0) << 2);
}

static uint64_t ror(uint64_t v, unsigned r, unsigned w)
{
    uint64_t m = w == 64 ? ~0ull : ((1ull << w) - 1);
    v &= m;
    r %= w;
    if (!r) return v;
    return ((v >> r) | (v << (w - r))) & m;
}

static uint64_t shift_reg(int sf, uint64_t v, unsigned type, unsigned amt)
{
    unsigned w = sf ? 64 : 32;
    v = msk(sf, v);
    amt %= w;
    switch (type) {
    case 0: return msk(sf, v << amt);
    case 1: return v >> amt;
    case 2: return sf ? (uint64_t)((int64_t)v >> amt) : (uint32_t)((int32_t)v >> amt);
    default: return ror(v, amt, w);
    }
}

static uint64_t extend_reg(uint64_t v, unsigned opt, unsigned sh)
{
    switch (opt) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
    default: break;
    }
    return v << sh;
}

/* ------------------------------------------------- data processing (imm) */

static int dp_imm(hao_cpu *c, uint32_t i, uint64_t pc)
{
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int sf = BIT(i, 31);
    switch (BITS(i, 25, 23)) {
    case 0: case 1: {  /* ADR / ADRP */
        int64_t imm = sext((uint64_t)BITS(i, 23, 5) << 2 | BITS(i, 30, 29), 21);
        if (BIT(i, 31)) wx(c, rd, (pc & ~0xfffull) + ((uint64_t)imm << 12));
        else wx(c, rd, pc + (uint64_t)imm);
        return 0;
    }
    case 2: {  /* ADD/SUB immediate */
        uint64_t imm = BITS(i, 21, 10);
        if (BIT(i, 22)) imm <<= 12;
        int op = BIT(i, 30), S = BIT(i, 29);
        uint64_t a = rsp(c, rn), r;
        unsigned nzcv;
        r = op ? awc(sf, a, ~imm, 1, &nzcv) : awc(sf, a, imm, 0, &nzcv);
        if (S) { hao_set_nzcv(c, nzcv); wx(c, rd, r); }
        else wsp(c, rd, r);
        return 0;
    }
    case 4: {  /* logical immediate */
        uint64_t imm;
        unsigned N = BIT(i, 22);
        if (!sf && N) return HAO_EXIT_UNDEF;
        if (!a64_decode_bitmask_imm(sf, N, BITS(i, 21, 16), BITS(i, 15, 10), &imm))
            return HAO_EXIT_UNDEF;
        uint64_t a = rx(c, rn), r;
        switch (BITS(i, 30, 29)) {
        case 0: r = a & imm; wsp(c, rd, msk(sf, r)); break;
        case 1: r = a | imm; wsp(c, rd, msk(sf, r)); break;
        case 2: r = a ^ imm; wsp(c, rd, msk(sf, r)); break;
        default: r = msk(sf, a & imm); set_nz(c, sf, r); wx(c, rd, r); break;
        }
        return 0;
    }
    case 5: {  /* move wide */
        unsigned hw = BITS(i, 22, 21), opc = BITS(i, 30, 29);
        if (!sf && hw >= 2) return HAO_EXIT_UNDEF;
        uint64_t imm = (uint64_t)BITS(i, 20, 5) << (hw * 16);
        switch (opc) {
        case 0: wx(c, rd, msk(sf, ~imm)); break;
        case 2: wx(c, rd, imm); break;
        case 3: wx(c, rd, msk(sf, (rx(c, rd) & ~(0xffffull << (hw * 16))) | imm)); break;
        default: return HAO_EXIT_UNDEF;
        }
        return 0;
    }
    case 6: {  /* bitfield */
        unsigned opc = BITS(i, 30, 29), N = BIT(i, 22), immr = BITS(i, 21, 16), imms = BITS(i, 15, 10);
        uint64_t wm, tm;
        if (opc == 3 || !a64_decode_bitfield((unsigned)sf, N, immr, imms, &wm, &tm)) return HAO_EXIT_UNDEF;
        unsigned w = sf ? 64 : 32;
        uint64_t src = msk(sf, rx(c, rn)), r;
        uint64_t bot = ror(src, immr, w) & wm;
        if (opc == 1) {
            uint64_t dst = msk(sf, rx(c, rd));
            bot = (dst & ~wm) | bot;
            r = (dst & ~tm) | (bot & tm);
        } else if (opc == 0) {
            uint64_t top = ((src >> imms) & 1) ? msk(sf, ~0ull) : 0;
            r = (top & ~tm) | (bot & tm);
        } else {
            r = bot & tm;
        }
        wx(c, rd, msk(sf, r));
        return 0;
    }
    case 7: {  /* EXTR */
        unsigned imms = BITS(i, 15, 10), rm = BITS(i, 20, 16);
        if (BIT(i, 22) != (unsigned)sf || BITS(i, 30, 29) || BIT(i, 21)) return HAO_EXIT_UNDEF;
        if (!sf && imms >= 32) return HAO_EXIT_UNDEF;
        uint64_t hi = msk(sf, rx(c, rn)), lo = msk(sf, rx(c, rm)), r;
        unsigned w = sf ? 64 : 32;
        r = imms ? ((lo >> imms) | (hi << (w - imms))) : lo;
        wx(c, rd, msk(sf, r));
        return 0;
    }
    default:
        return HAO_EXIT_UNDEF;
    }
}

/* ------------------------------------------------------ branches/system */

static int sysinsn(hao_cpu *c, uint32_t i)
{
    unsigned L = BIT(i, 21), op0 = BITS(i, 20, 19), op1 = BITS(i, 18, 16);
    unsigned crn = BITS(i, 15, 12), crm = BITS(i, 11, 8), op2 = BITS(i, 7, 5), rt = BITS(i, 4, 0);

    if (op0 == 0 && !L) {
        if (crn == 2 && rt == 31) {           /* HINT space: PAC*SP, AUT*SP, BTI etc. are the identity */
            if (crm == 0 && op2 == 7) c->x[30] = a64_xpac(c->x[30], false);   /* XPACLRI */
            if (crm == 0 && op2 == 3) return HAO_EXIT_UNDEF;                 /* WFI traps at EL0 (SIGILL) */
            return 0;
        }
        if (crn == 3 && rt == 31) {           /* barriers */
            if (op2 == 2) { c->excl_size = 0; return 0; }          /* CLREX */
            if (op2 == 4 || op2 == 5) {                            /* DSB / DMB */
                if ((crm & 3) == 3) __atomic_thread_fence(__ATOMIC_SEQ_CST);
                else __atomic_thread_fence(__ATOMIC_ACQ_REL);
            }
            return 0;                                              /* ISB, SB, others */
        }
        if (crn == 4 && rt == 31) {           /* MSR (immediate) / flag manipulation */
            if (op1 == 0 && crm == 0 && op2 == 0) {                /* CFINV */
                c->flags ^= HAO_FL_CF;
                return 0;
            }
            if (op1 == 0 && crm == 0 && op2 == 1) {                /* XAFLAG */
                unsigned f = hao_nzcv(c);
                unsigned C = (f >> 1) & 1, Z = (f >> 2) & 1;
                unsigned n = !C && !Z, z = Z && C, cc = C || Z, v = !C && Z;
                hao_set_nzcv(c, n << 3 | z << 2 | cc << 1 | v);
                return 0;
            }
            if (op1 == 0 && crm == 0 && op2 == 2) {                /* AXFLAG */
                unsigned f = hao_nzcv(c);
                unsigned V = f & 1, C = (f >> 1) & 1, Z = (f >> 2) & 1;
                hao_set_nzcv(c, (Z | V) << 2 | (C & !V) << 1);
                return 0;
            }
            return 0;  /* PSTATE.{DIT,SSBS,TCO,PAN,...}: no architectural effect we model */
        }
        return HAO_EXIT_UNDEF;
    }
    if (op0 == 1) {  /* SYS / SYSL: DC, IC, AT, TLBI */
        if (L) return HAO_EXIT_UNDEF;
        if (op1 == 3 && crn == 7 && crm == 4 && op2 == 1) {        /* DC ZVA: 64-byte blocks */
            uint64_t a = rx(c, rt) & ~63ull;
            memset((void *)(uintptr_t)hao_tbi(a), 0, 64);
            return 0;
        }
        return 0;  /* cache maintenance: coherent host, nothing to do */
    }
    /* MRS / MSR (register) */
    unsigned enc = op0 << 14 | op1 << 11 | crn << 7 | crm << 3 | op2;
    switch (enc) {
    case 0xDA10: /* NZCV  3,3,4,2,0 */
        if (L) wx(c, rt, (uint64_t)hao_nzcv(c) << 28);
        else hao_set_nzcv(c, (unsigned)(rx(c, rt) >> 28) & 0xf);
        return 0;
    case 0xDA20: /* FPCR 3,3,4,4,0 */
        if (L) wx(c, rt, a64_fpcr_read(c)); else a64_fpcr_write(c, (uint32_t)rx(c, rt));
        return 0;
    case 0xDA21: /* FPSR 3,3,4,4,1 */
        if (L) wx(c, rt, a64_fpsr_read(c)); else a64_fpsr_write(c, (uint32_t)rx(c, rt));
        return 0;
    case 0xDE82: /* TPIDR_EL0 3,3,13,0,2 */
        if (L) wx(c, rt, c->tpidr_el0); else c->tpidr_el0 = rx(c, rt);
        return 0;
    case 0xDE83: /* TPIDRRO_EL0 3,3,13,0,3 (read-only at EL0) */
        if (L) { wx(c, rt, c->tpidrro_el0); return 0; }
        return HAO_EXIT_UNDEF;
    case 0xD807: /* DCZID_EL0 3,3,0,0,7: 64-byte DC ZVA */
        if (L) { wx(c, rt, 4); return 0; }
        return HAO_EXIT_UNDEF;
    default: {
        bool ok = false;
        if (L) {
            uint64_t v = hao_sysreg_read(c, enc, &ok);
            if (!ok) return HAO_EXIT_UNDEF;
            wx(c, rt, v);
            return 0;
        }
        return hao_sysreg_write(c, enc, rx(c, rt)) ? 0 : HAO_EXIT_UNDEF;
    }
    }
}

static int branch_sys(hao_cpu *c, uint32_t i, uint64_t pc)
{
    if ((i & 0x7C000000) == 0x14000000) {  /* B / BL */
        int64_t off = sext(BITS(i, 25, 0), 26) * 4;
        if (BIT(i, 31)) c->x[30] = pc + 4;
        c->pc = pc + (uint64_t)off;
        return HAO_EXIT_BRANCH;
    }
    if ((i & 0x7E000000) == 0x34000000) {  /* CBZ / CBNZ */
        int sf = BIT(i, 31);
        uint64_t v = msk(sf, rx(c, BITS(i, 4, 0)));
        bool take = BIT(i, 24) ? v != 0 : v == 0;
        if (take) { c->pc = pc + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4); return HAO_EXIT_BRANCH; }
        return 0;
    }
    if ((i & 0x7E000000) == 0x36000000) {  /* TBZ / TBNZ */
        unsigned bit = BIT(i, 31) << 5 | BITS(i, 23, 19);
        bool set = (rx(c, BITS(i, 4, 0)) >> bit) & 1;
        if (set == (bool)BIT(i, 24)) { c->pc = pc + (uint64_t)(sext(BITS(i, 18, 5), 14) * 4); return HAO_EXIT_BRANCH; }
        return 0;
    }
    if ((i & 0xFF000000) == 0x54000000) {  /* B.cond / BC.cond */
        if (hao_cond(hao_nzcv(c), BITS(i, 3, 0))) {
            c->pc = pc + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4);
            return HAO_EXIT_BRANCH;
        }
        return 0;
    }
    if ((i & 0xFF000000) == 0xD4000000) {  /* exception generation */
        unsigned opc = BITS(i, 23, 21), ll = BITS(i, 1, 0), imm = BITS(i, 20, 5);
        c->exit_insn = imm;
        if (opc == 0 && ll == 1) { c->pc = pc + 4; return HAO_EXIT_SVC; }
        if (opc == 1 && ll == 0) { c->pc = pc; return HAO_EXIT_BRK; }
        if (opc == 2 && ll == 0) { c->pc = pc; return HAO_EXIT_HLT; }
        c->exit_insn = i;
        return HAO_EXIT_UNDEF;
    }
    if ((i & 0xFFC00000) == 0xD5000000) return sysinsn(c, i);
    if ((i & 0xFE000000) == 0xD6000000) {  /* branch to register (incl. arm64e AA and AB variants) */
        unsigned opc = BITS(i, 24, 21), op3 = BITS(i, 15, 10), rn = BITS(i, 9, 5);
        if (!a64_branch_reg_valid(i)) { c->exit_insn = i; return HAO_EXIT_UNDEF; }
        uint64_t target = (opc == 2 && op3) ? c->x[30] : rx(c, rn);   /* RETAA/RETAB use x30 */
        if (opc & 1) c->x[30] = pc + 4;
        c->pc = target;
        return HAO_EXIT_BRANCH;
    }
    return HAO_EXIT_UNDEF;
}

/* ----------------------------------------------------------- load/store */

int a64_ldst_simd(hao_cpu *c, uint32_t i, uint64_t pc);  /* a64_simd.c */
void a64_vload(hao_cpu *c, unsigned vt, uint64_t addr, int size);
void a64_vstore(hao_cpu *c, unsigned vt, uint64_t addr, int size);

static uint64_t atomic_rmw(uint64_t addr, int size, unsigned op, uint64_t operand)
{
    void *p = (void *)(uintptr_t)hao_tbi(addr);
#define RMW(T, ST)                                                                    \
    do {                                                                              \
        T *q = (T *)p, old = __atomic_load_n(q, __ATOMIC_RELAXED), nv;                \
        T s = (T)operand;                                                             \
        do {                                                                          \
            switch (op) {                                                             \
            case 0: nv = (T)(old + s); break;                                         \
            case 1: nv = (T)(old & ~s); break;                                        \
            case 2: nv = (T)(old ^ s); break;                                         \
            case 3: nv = (T)(old | s); break;                                         \
            case 4: nv = ((ST)old > (ST)s) ? old : s; break;                          \
            case 5: nv = ((ST)old < (ST)s) ? old : s; break;                          \
            case 6: nv = old > s ? old : s; break;                                    \
            case 7: nv = old < s ? old : s; break;                                    \
            default: nv = s; break; /* SWP */                                         \
            }                                                                         \
        } while (!__atomic_compare_exchange_n(q, &old, nv, true, __ATOMIC_SEQ_CST,    \
                                              __ATOMIC_RELAXED));                     \
        return (uint64_t)old;                                                         \
    } while (0)
    switch (size) {
    case 1: RMW(uint8_t, int8_t);
    case 2: RMW(uint16_t, int16_t);
    case 4: RMW(uint32_t, int32_t);
    default: RMW(uint64_t, int64_t);
    }
#undef RMW
}

static bool cas_sized(uint64_t addr, int size, uint64_t *expected, uint64_t desired)
{
    void *p = (void *)(uintptr_t)hao_tbi(addr);
    switch (size) {
    case 1: { uint8_t e = (uint8_t)*expected; bool ok = __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    case 2: { uint16_t e = (uint16_t)*expected; bool ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    case 4: { uint32_t e = (uint32_t)*expected; bool ok = __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    default: { uint64_t e = *expected; bool ok = __atomic_compare_exchange_n((uint64_t *)p, &e, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    }
}

static bool cas128(uint64_t addr, u128 *expected, u128 desired)
{
    u128 *p = (u128 *)(uintptr_t)hao_tbi(addr);
    u128 old = __sync_val_compare_and_swap(p, *expected, desired);
    bool ok = old == *expected;
    *expected = old;
    return ok;
}

static int ldst_excl(hao_cpu *c, uint32_t i)
{
    unsigned size = BITS(i, 31, 30), o2 = BIT(i, 23), L = BIT(i, 22), o1 = BIT(i, 21);
    unsigned rs = BITS(i, 20, 16), rt2 = BITS(i, 14, 10), rn = BITS(i, 9, 5), rt = BITS(i, 4, 0);
    uint64_t addr = rsp(c, rn);
    int bytes = 1 << size;

    if (o2 && o1) {  /* CAS{A,L,AL}{B,H,,} */
        if (rt2 != 31) return HAO_EXIT_UNDEF;
        uint64_t e = rx(c, rs);
        cas_sized(addr, bytes, &e, rx(c, rt));
        wx(c, rs, e);
        return 0;
    }
    if (!o2 && o1 && !BIT(i, 31)) {  /* CASP: size<0> selects 32/64-bit pair */
        int sf = BIT(i, 30);
        if ((rs & 1) || (rt & 1) || rt2 != 31) return HAO_EXIT_UNDEF;
        if (sf) {
            u128 e = (u128)rx(c, rs + 1) << 64 | rx(c, rs);
            u128 d = (u128)rx(c, rt + 1) << 64 | rx(c, rt);
            cas128(addr, &e, d);
            wx(c, rs, (uint64_t)e);
            wx(c, rs + 1, (uint64_t)(e >> 64));
        } else {
            uint64_t e = (uint32_t)rx(c, rs) | rx(c, rs + 1) << 32;
            uint64_t d = (uint32_t)rx(c, rt) | rx(c, rt + 1) << 32;
            cas_sized(addr, 8, &e, d);
            wx(c, rs, (uint32_t)e);
            wx(c, rs + 1, e >> 32);
        }
        return 0;
    }
    if (o2) {  /* LDAR/STLR/LDLAR/STLLR: plain ordered accesses on x86 TSO */
        if (L) { uint64_t v = ldm(addr, bytes); __atomic_thread_fence(__ATOMIC_ACQUIRE); wx(c, rt, v); }
        else { __atomic_thread_fence(__ATOMIC_SEQ_CST); stm(addr, bytes, rx(c, rt)); __atomic_thread_fence(__ATOMIC_SEQ_CST); }
        return 0;
    }
    /* exclusives: emulate the monitor with compare-and-swap on store */
    if (!o1) {
        if (L) {
            uint64_t v = ldm(addr, bytes);
            c->excl_addr = addr; c->excl_val[0] = v; c->excl_size = (uint32_t)bytes; c->excl_pair = 0;
            wx(c, rt, v);
        } else {
            uint32_t fail = 1;
            if (!c->excl_pair && c->excl_size == (uint32_t)bytes && c->excl_addr == addr) {
                uint64_t e = c->excl_val[0];
                fail = cas_sized(addr, bytes, &e, rx(c, rt)) ? 0 : 1;
            }
            c->excl_size = 0;
            wx(c, rs, fail);
        }
        return 0;
    }
    /* LDXP / STXP. The monitor records the pair that was loaded; STXP
     * succeeds only if memory still holds it (compare-and-swap). */
    {
        int sf = BIT(i, 30);
        if (L) {
            if (rt == rt2) return HAO_EXIT_UNDEF;
            if (sf) {
                /* 16-byte single-copy-atomic read that never writes (LDXP on
                 * read-only memory must work); an unaligned address, an
                 * alignment fault on hardware, is read as two halves. */
                u128 v = (addr & 15) ? (u128)ldm(addr + 8, 8) << 64 | ldm(addr, 8) : ld128_atomic(addr);
                c->excl_addr = addr; c->excl_val[0] = (uint64_t)v; c->excl_val[1] = (uint64_t)(v >> 64);
                c->excl_size = 16; c->excl_pair = 1;
                wx(c, rt, (uint64_t)v); wx(c, rt2, (uint64_t)(v >> 64));
            } else {
                uint64_t v = __atomic_load_n((uint64_t *)(uintptr_t)hao_tbi(addr), __ATOMIC_SEQ_CST);
                c->excl_addr = addr; c->excl_val[0] = v; c->excl_size = 8; c->excl_pair = 1;
                wx(c, rt, (uint32_t)v); wx(c, rt2, v >> 32);
            }
        } else {
            uint32_t fail = 1;
            if (c->excl_pair && c->excl_addr == addr && c->excl_size == (sf ? 16u : 8u)) {
                if (sf) {
                    u128 e = (u128)c->excl_val[1] << 64 | c->excl_val[0];
                    u128 d = (u128)rx(c, rt2) << 64 | rx(c, rt);
                    fail = cas128(addr, &e, d) ? 0 : 1;
                } else {
                    uint64_t e = c->excl_val[0];
                    fail = cas_sized(addr, 8, &e, (uint32_t)rx(c, rt) | rx(c, rt2) << 32) ? 0 : 1;
                }
            }
            c->excl_size = 0; c->excl_pair = 0;
            wx(c, rs, fail);
        }
        return 0;
    }
}

/* Generic integer load/store of `bytes` with extension kind. */
enum { LD_ST = 0, LD_ZX, LD_SX64, LD_SX32, LD_PRFM };

static void do_ldst(hao_cpu *c, int kind, int bytes, unsigned rt, uint64_t addr)
{
    switch (kind) {
    case LD_ST: stm(addr, bytes, rx(c, rt)); break;
    case LD_ZX: wx(c, rt, ldm(addr, bytes)); break;
    case LD_SX64: wx(c, rt, (uint64_t)sext(ldm(addr, bytes), bytes * 8)); break;
    case LD_SX32: wx(c, rt, (uint32_t)sext(ldm(addr, bytes), bytes * 8)); break;
    default: break;
    }
}

static int ldst_kind(unsigned size, unsigned opc, int *kind)
{
    switch (opc) {
    case 0: *kind = LD_ST; return 0;
    case 1: *kind = LD_ZX; return 0;
    case 2: if (size == 3) { *kind = LD_PRFM; return 0; } *kind = LD_SX64; return 0;
    default: if (size >= 2) return -1; *kind = LD_SX32; return 0;
    }
}

static int simd_size(unsigned size, unsigned opc)
{
    if (BIT(opc, 1)) return size == 0 ? 16 : -1;
    return 1 << size;
}

static int ldst(hao_cpu *c, uint32_t i, uint64_t pc)
{
    unsigned rt = BITS(i, 4, 0), rn = BITS(i, 9, 5);

    if ((i & 0x3F000000) == 0x08000000) return ldst_excl(c, i);

    if ((i & 0x3F200C00) == 0x19000000) {  /* LDAPUR/STLUR (RCpc unscaled) */
        unsigned size = BITS(i, 31, 30), opc = BITS(i, 23, 22);
        int kind;
        if (ldst_kind(size, opc, &kind) < 0 || kind == LD_PRFM) return HAO_EXIT_UNDEF;
        uint64_t addr = rsp(c, rn) + (uint64_t)sext(BITS(i, 20, 12), 9);
        do_ldst(c, kind, 1 << size, rt, addr);
        return 0;
    }

    if ((i & 0x3B000000) == 0x18000000) {  /* load literal */
        unsigned opc = BITS(i, 31, 30);
        uint64_t addr = pc + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4);
        if (BIT(i, 26)) {
            if (opc == 3) return HAO_EXIT_UNDEF;
            a64_vload(c, rt, addr, 4 << opc);
            return 0;
        }
        switch (opc) {
        case 0: wx(c, rt, ldm(addr, 4)); break;
        case 1: wx(c, rt, ldm(addr, 8)); break;
        case 2: wx(c, rt, (uint64_t)sext(ldm(addr, 4), 32)); break;
        default: break; /* PRFM literal */
        }
        return 0;
    }

    if ((i & 0x3A000000) == 0x28000000) {  /* load/store pair */
        unsigned opc = BITS(i, 31, 30), type = BITS(i, 24, 23), L = BIT(i, 22), rt2 = BITS(i, 14, 10);
        int V = BIT(i, 26), bytes, sx = 0;
        if (V) { if (opc == 3) return HAO_EXIT_UNDEF; bytes = 4 << opc; }
        else if (opc == 0) bytes = 4;
        else if (opc == 2) bytes = 8;
        else if (opc == 1 && L && type != 0) { bytes = 4; sx = 1; }
        else return HAO_EXIT_UNDEF; /* STGP (MTE), LDNP/STNP opc=01 */
        if (L && rt == rt2) return HAO_EXIT_UNDEF;
        if (!V && (type == 1 || type == 3) && rn != 31 && (rn == rt || rn == rt2)) return HAO_EXIT_UNDEF;
        int64_t off = sext(BITS(i, 21, 15), 7) * bytes;
        uint64_t base = rsp(c, rn), addr = type == 1 ? base : base + (uint64_t)off;
        if (V) {
            if (L) { a64_vload(c, rt, addr, bytes); a64_vload(c, rt2, addr + (uint64_t)bytes, bytes); }
            else { a64_vstore(c, rt, addr, bytes); a64_vstore(c, rt2, addr + (uint64_t)bytes, bytes); }
        } else if (L) {
            uint64_t a, b;
            if (bytes == 8 && !(addr & 15)) {            /* FEAT_LSE2: one 16-byte atomic access */
                u128 v = ld128_atomic(addr);
                a = (uint64_t)v; b = (uint64_t)(v >> 64);
            } else {
                a = ldm(addr, bytes); b = ldm(addr + (uint64_t)bytes, bytes);
            }
            if (sx) { a = (uint64_t)sext(a, 32); b = (uint64_t)sext(b, 32); }
            wx(c, rt, a); wx(c, rt2, b);
        } else {
            uint64_t a = rx(c, rt), b = rx(c, rt2);
            if (bytes == 8 && !(addr & 15)) st128_atomic(addr, (u128)b << 64 | a);
            else { stm(addr, bytes, a); stm(addr + (uint64_t)bytes, bytes, b); }
        }
        if (type == 1 || type == 3) wsp(c, rn, base + (uint64_t)off);
        return 0;
    }

    if ((i & 0x3A000000) == 0x38000000) {  /* load/store register */
        unsigned size = BITS(i, 31, 30), opc = BITS(i, 23, 22);
        int V = BIT(i, 26);
        int kind = 0, bytes;
        if (V) {
            bytes = simd_size(size, opc);
            if (bytes < 0) return HAO_EXIT_UNDEF;
        } else {
            bytes = 1 << size;
        }

        if (BIT(i, 24)) {  /* unsigned scaled offset */
            uint64_t addr = rsp(c, rn) + (uint64_t)BITS(i, 21, 10) * (uint64_t)bytes;
            if (V) { if (opc & 1) a64_vload(c, rt, addr, bytes); else a64_vstore(c, rt, addr, bytes); return 0; }
            if (ldst_kind(size, opc, &kind) < 0) return HAO_EXIT_UNDEF;
            do_ldst(c, kind, bytes, rt, addr);
            return 0;
        }
        if (!BIT(i, 21)) {  /* imm9: unscaled / post / unprivileged / pre */
            unsigned mode = BITS(i, 11, 10);
            int64_t simm = sext(BITS(i, 20, 12), 9);
            uint64_t base = rsp(c, rn), addr = mode == 1 ? base : base + (uint64_t)simm;
            if (V) {
                if (mode == 2) return HAO_EXIT_UNDEF;
                if (opc & 1) a64_vload(c, rt, addr, bytes); else a64_vstore(c, rt, addr, bytes);
            } else {
                if (ldst_kind(size, opc, &kind) < 0) return HAO_EXIT_UNDEF;
                if (kind == LD_PRFM && mode != 0) return HAO_EXIT_UNDEF;       /* PRFUM has no other form */
                if ((mode == 1 || mode == 3) && rn != 31 && rn == rt) return HAO_EXIT_UNDEF;
                do_ldst(c, kind, bytes, rt, addr);
            }
            if (mode == 1 || mode == 3) wsp(c, rn, base + (uint64_t)simm);
            return 0;
        }
        unsigned op4 = BITS(i, 11, 10);
        if (op4 == 2) {  /* register offset */
            unsigned rm = BITS(i, 20, 16), opt = BITS(i, 15, 13), S = BIT(i, 12);
            if (!(opt & 2)) return HAO_EXIT_UNDEF;
            unsigned sh = S ? (unsigned)__builtin_ctz((unsigned)bytes) : 0;
            uint64_t addr = rsp(c, rn) + extend_reg(rx(c, rm), opt, sh);
            if (V) { if (opc & 1) a64_vload(c, rt, addr, bytes); else a64_vstore(c, rt, addr, bytes); return 0; }
            if (ldst_kind(size, opc, &kind) < 0) return HAO_EXIT_UNDEF;
            do_ldst(c, kind, bytes, rt, addr);
            return 0;
        }
        if (op4 == 0 && !V) {  /* LSE atomic memory operations */
            unsigned rs = BITS(i, 20, 16), o3 = BIT(i, 15), aop = BITS(i, 14, 12);
            uint64_t addr = rsp(c, rn);
            if (o3 && aop == 4) {  /* LDAPR: A = 1, R = 0, Rs = 11111 */
                if (!BIT(i, 23) || BIT(i, 22) || rs != 31) return HAO_EXIT_UNDEF;
                wx(c, rt, ldm(addr, bytes));
                return 0;
            }
            if (o3 && aop != 0) return HAO_EXIT_UNDEF;
            uint64_t old = atomic_rmw(addr, bytes, o3 ? 8 : aop, rx(c, rs));
            if (bytes < 8 && (aop == 4 || aop == 5) && !o3) old &= (bytes == 4 ? 0xffffffffull : (1ull << (bytes * 8)) - 1);
            wx(c, rt, old);
            return 0;
        }
        if ((op4 & 1) && !V && size == 3) {  /* LDRAA / LDRAB (arm64e) */
            int64_t off = sext((uint64_t)BIT(i, 22) << 9 | BITS(i, 20, 12), 10) * 8;
            if (BIT(i, 11) && rn != 31 && rn == rt) return HAO_EXIT_UNDEF;
            uint64_t base = rsp(c, rn), addr = base + (uint64_t)off;
            wx(c, rt, ldm(addr, 8));
            if (BIT(i, 11)) wsp(c, rn, addr);
            return 0;
        }
        return HAO_EXIT_UNDEF;
    }

    /* Advanced SIMD structure loads/stores */
    if ((i & 0xBF800000) == 0x0C000000 || (i & 0xBF800000) == 0x0C800000 ||
        (i & 0xBF800000) == 0x0D000000 || (i & 0xBF800000) == 0x0D800000)
        return a64_ldst_simd(c, i, pc);

    return HAO_EXIT_UNDEF;
}

/* ---------------------------------------------- data processing (register) */

static uint32_t crc32_update(uint32_t crc, uint64_t data, int bytes, uint32_t poly)
{
    for (int b = 0; b < bytes; b++) {
        crc ^= (uint8_t)(data >> (8 * b));
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
    }
    return crc;
}

static int dp_reg(hao_cpu *c, uint32_t i)
{
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    int sf = BIT(i, 31);

    if ((i & 0x1F000000) == 0x0A000000) {  /* logical shifted register */
        unsigned opc = BITS(i, 30, 29), N = BIT(i, 21), imm6 = BITS(i, 15, 10);
        if (!sf && imm6 >= 32) return HAO_EXIT_UNDEF;
        uint64_t b = shift_reg(sf, rx(c, rm), BITS(i, 23, 22), imm6), a = rx(c, rn), r;
        if (N) b = ~b;
        switch (opc) {
        case 0: r = a & b; break;
        case 1: r = a | b; break;
        case 2: r = a ^ b; break;
        default: r = a & b; break;
        }
        r = msk(sf, r);
        if (opc == 3) set_nz(c, sf, r);
        wx(c, rd, r);
        return 0;
    }
    if ((i & 0x1F200000) == 0x0B000000) {  /* add/sub shifted register */
        unsigned imm6 = BITS(i, 15, 10), type = BITS(i, 23, 22);
        if (type == 3 || (!sf && imm6 >= 32)) return HAO_EXIT_UNDEF;
        uint64_t b = shift_reg(sf, rx(c, rm), type, imm6), a = rx(c, rn), r;
        unsigned nzcv;
        r = BIT(i, 30) ? awc(sf, a, ~b, 1, &nzcv) : awc(sf, a, b, 0, &nzcv);
        if (BIT(i, 29)) hao_set_nzcv(c, nzcv);
        wx(c, rd, r);
        return 0;
    }
    if ((i & 0x1F200000) == 0x0B200000) {  /* add/sub extended register */
        unsigned opt = BITS(i, 15, 13), imm3 = BITS(i, 12, 10);
        if (imm3 > 4 || BITS(i, 23, 22)) return HAO_EXIT_UNDEF;
        uint64_t b = msk(sf, extend_reg(rx(c, rm), opt, imm3)), a = rsp(c, rn), r;
        unsigned nzcv;
        r = BIT(i, 30) ? awc(sf, a, ~b, 1, &nzcv) : awc(sf, a, b, 0, &nzcv);
        if (BIT(i, 29)) { hao_set_nzcv(c, nzcv); wx(c, rd, r); }
        else wsp(c, rd, r);
        return 0;
    }
    if ((i & 0x1FE00000) == 0x1A000000) {  /* ADC/SBC/ADCS/SBCS, RMIF, SETF */
        unsigned op3 = BITS(i, 15, 10);
        if (op3 == 0) {
            unsigned carry = (hao_nzcv(c) >> 1) & 1, nzcv;
            uint64_t b = rx(c, rm), r;
            if (BIT(i, 30)) b = ~b;
            r = awc(sf, rx(c, rn), b, carry, &nzcv);
            if (BIT(i, 29)) hao_set_nzcv(c, nzcv);
            wx(c, rd, r);
            return 0;
        }
        if ((i & 0xFFE07C10) == 0xBA000400) {  /* RMIF */
            unsigned lsb = BITS(i, 20, 15), mask = BITS(i, 3, 0);
            unsigned tmp = (unsigned)ror(rx(c, rn), lsb, 64) & 0xf;
            hao_set_nzcv(c, (hao_nzcv(c) & ~mask) | (tmp & mask));
            return 0;
        }
        if ((i & 0xFFFFBC1F) == 0x3A00080D) {  /* SETF8 / SETF16 */
            int sz = BIT(i, 14) ? 16 : 8;
            uint64_t v = rx(c, rn);
            unsigned n = (v >> (sz - 1)) & 1, z = ((v & ((1u << sz) - 1)) == 0);
            unsigned vv = ((v >> sz) & 1) ^ n;
            hao_set_nzcv(c, n << 3 | z << 2 | (hao_nzcv(c) & 2) | vv);
            return 0;
        }
        return HAO_EXIT_UNDEF;
    }
    if ((i & 0x1FE00000) == 0x1A400000) {  /* CCMN / CCMP */
        unsigned cond = BITS(i, 15, 12), nzcv_imm = BITS(i, 3, 0);
        if (BIT(i, 10) || BIT(i, 4) || !BIT(i, 29)) return HAO_EXIT_UNDEF;
        if (hao_cond(hao_nzcv(c), cond)) {
            uint64_t b = BIT(i, 11) ? rm : rx(c, rm), a = rx(c, rn);
            unsigned nzcv;
            if (BIT(i, 30)) awc(sf, a, ~b, 1, &nzcv); else awc(sf, a, b, 0, &nzcv);
            hao_set_nzcv(c, nzcv);
        } else {
            hao_set_nzcv(c, nzcv_imm);
        }
        return 0;
    }
    if ((i & 0x1FE00000) == 0x1A800000) {  /* conditional select */
        unsigned cond = BITS(i, 15, 12), op2 = BITS(i, 11, 10);
        if (BIT(i, 29) || op2 > 1) return HAO_EXIT_UNDEF;
        uint64_t r;
        if (hao_cond(hao_nzcv(c), cond)) r = rx(c, rn);
        else {
            r = rx(c, rm);
            if (BIT(i, 30)) r = ~r;
            if (op2 == 1) r += 1;
        }
        wx(c, rd, msk(sf, r));
        return 0;
    }
    if ((i & 0x5FE00000) == 0x1AC00000) {  /* data processing 2-source */
        unsigned opc = BITS(i, 15, 10);
        if (BIT(i, 29)) return HAO_EXIT_UNDEF;    /* S = 1: SUBPS (MTE) */
        uint64_t a = msk(sf, rx(c, rn)), b = msk(sf, rx(c, rm)), r;
        switch (opc) {
        case 2: r = b ? a / b : 0; break;
        case 3:
            if (!b) r = 0;
            else if (sf) r = ((int64_t)a == INT64_MIN && (int64_t)b == -1) ? a : (uint64_t)((int64_t)a / (int64_t)b);
            else r = ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b);
            break;
        case 8: case 9: case 10: case 11:
            r = shift_reg(sf, a, opc - 8, (unsigned)(b % (sf ? 64 : 32)));
            break;
        case 12:  /* PACGA: deterministic stand-in for the generic MAC */
            if (!sf) return HAO_EXIT_UNDEF;
            r = ((a * 0x9E3779B97F4A7C15ull) ^ rsp(c, rm)) & 0xffffffff00000000ull;
            wx(c, rd, r);
            return 0;
        case 16: case 17: case 18: case 19:
        case 20: case 21: case 22: case 23: {
            int bytes = 1 << (opc & 3);
            uint32_t poly = (opc & 4) ? 0x82F63B78u : 0xEDB88320u;
            if ((bytes == 8) != sf) return HAO_EXIT_UNDEF;
            r = crc32_update((uint32_t)rx(c, rn), rx(c, rm), bytes, poly);
            wx(c, rd, r);
            return 0;
        }
        default: return HAO_EXIT_UNDEF;
        }
        wx(c, rd, msk(sf, r));
        return 0;
    }
    if ((i & 0x5FE00000) == 0x5AC00000) {  /* data processing 1-source */
        unsigned op2 = BITS(i, 20, 16), opc = BITS(i, 15, 10);
        if (BIT(i, 29)) return HAO_EXIT_UNDEF;
        if (op2 == 1) {                                     /* arm64e PAC/AUT: identity; XPACI/XPACD strip */
            if (!a64_pac_valid(i)) return HAO_EXIT_UNDEF;
            unsigned rd = BITS(i, 4, 0);
            if (opc >= 16) wx(c, rd, a64_xpac(rx(c, rd), opc == 17));
            return 0;
        }
        if (op2) return HAO_EXIT_UNDEF;
        uint64_t a = msk(sf, rx(c, rn)), r = 0;
        switch (opc) {
        case 0: { /* RBIT */
            unsigned w = sf ? 64 : 32;
            for (unsigned k = 0; k < w; k++) if ((a >> k) & 1) r |= 1ull << (w - 1 - k);
            break;
        }
        case 1: /* REV16 */
            r = ((a & 0x00ff00ff00ff00ffull) << 8) | ((a >> 8) & 0x00ff00ff00ff00ffull);
            break;
        case 2: /* REV32 (64-bit) / REV (32-bit) */
            r = sf ? ((uint64_t)__builtin_bswap32((uint32_t)a) | (uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32)
                   : __builtin_bswap32((uint32_t)a);
            break;
        case 3: if (!sf) return HAO_EXIT_UNDEF; r = __builtin_bswap64(a); break;
        case 4: r = sf ? (a ? (uint64_t)__builtin_clzll(a) : 64) : (a ? (uint64_t)__builtin_clz((uint32_t)a) : 32); break;
        case 5: { /* CLS */
            uint64_t x = sf ? a ^ (uint64_t)((int64_t)a >> 1) : (uint32_t)(a ^ (uint64_t)((int32_t)a >> 1));
            unsigned w = sf ? 64 : 32;
            x &= sf ? 0x7fffffffffffffffull : 0x7fffffffull;
            r = x ? (sf ? (uint64_t)__builtin_clzll(x) : (uint64_t)__builtin_clz((uint32_t)x)) - 1 : w - 1;
            break;
        }
        default: return HAO_EXIT_UNDEF;   /* includes CTZ/CNT/ABS: FEAT_CSSC, not on Apple M4 */
        }
        wx(c, rd, msk(sf, r));
        return 0;
    }
    if ((i & 0x1F000000) == 0x1B000000) {  /* data processing 3-source */
        unsigned op31 = BITS(i, 23, 21), o0 = BIT(i, 15), ra = BITS(i, 14, 10);
        if (BITS(i, 30, 29) || ((op31 == 2 || op31 == 6) && o0)) return HAO_EXIT_UNDEF;
        uint64_t a = rx(c, rn), b = rx(c, rm), acc = rx(c, ra), r;
        switch (op31) {
        case 0:
            r = o0 ? acc - a * b : acc + a * b;
            wx(c, rd, msk(sf, r));
            return 0;
        case 1: { int64_t p = (int64_t)(int32_t)a * (int32_t)b; r = o0 ? acc - (uint64_t)p : acc + (uint64_t)p; break; }
        case 5: { uint64_t p = (uint64_t)(uint32_t)a * (uint32_t)b; r = o0 ? acc - p : acc + p; break; }
        case 2: r = (uint64_t)(((s128)(int64_t)a * (int64_t)b) >> 64); break;
        case 6: r = (uint64_t)(((u128)a * b) >> 64); break;
        default: return HAO_EXIT_UNDEF;
        }
        if (!sf) return HAO_EXIT_UNDEF;
        wx(c, rd, r);
        return 0;
    }
    return HAO_EXIT_UNDEF;
}

/* -------------------------------------------------------------- entry */

static inline int exec_result(hao_cpu *c, uint32_t i, uint64_t pc, int result)
{
    if (result == HAO_EXIT_UNDEF || result < 0) {
        c->pc = pc; c->exit_insn = i;
        return HAO_EXIT_UNDEF;
    }
    return result;
}

int a64_exec(hao_cpu *c, uint32_t i, uint64_t pc)
{
    int r;
    c->pc = pc + 4;
    switch (BITS(i, 28, 25)) {
    case 0x8: case 0x9: r = dp_imm(c, i, pc); break;
    case 0xA: case 0xB: r = branch_sys(c, i, pc); break;
    case 0x4: case 0x6: case 0xC: case 0xE: r = ldst(c, i, pc); break;
    case 0x5: case 0xD: r = dp_reg(c, i); break;
    case 0x7: case 0xF: r = a64_exec_simd(c, i, pc); break;
    default:
        if (i == 0) { c->pc = pc; c->exit_insn = i; return HAO_EXIT_UDF; }
        r = HAO_EXIT_UNDEF;
        break;
    }
    return exec_result(c, i, pc, r);
}

uint64_t a64_helper_descriptor(uint32_t i)
{
    return (uint64_t)i | (a64_vector_fp_eligible(i) ? (uint64_t)HAO_HELPER_TAG_VECTOR_FP << 32 : 0);
}

int a64_exec_helper(hao_cpu *c, uint64_t descriptor, uint64_t pc)
{
    uint32_t i = (uint32_t)descriptor;
    uint32_t tag = (uint32_t)(descriptor >> 32);
    if (!tag) return a64_exec(c, i, pc);
    c->pc = pc + 4;
    int r = tag == HAO_HELPER_TAG_VECTOR_FP ? a64_exec_vector_fp(c, i) : -1;
    return exec_result(c, i, pc, r);
}

/* ------------------------------------- decode rules shared with the JIT */

/* Unconditional branch (register): BR/BLR/RET and their arm64e forms. */
bool a64_branch_reg_valid(uint32_t i)
{
    if ((i & 0xFE000000) != 0xD6000000 || BITS(i, 20, 16) != 31) return false;
    unsigned opc = BITS(i, 24, 21), op3 = BITS(i, 15, 10), rn = BITS(i, 9, 5), op4 = BITS(i, 4, 0);
    bool pac = (op3 >> 1) == 1;
    switch (opc) {
    case 0: case 1: return (op3 == 0 && op4 == 0) || (pac && op4 == 31);              /* BR/BLR, BRAAZ/BLRAAZ.. */
    case 2: return (op3 == 0 && op4 == 0) || (pac && rn == 31 && op4 == 31);          /* RET, RETAA/RETAB */
    case 8: case 9: return pac;                                                      /* BRAA/BLRAA.. */
    default: return false;                                                           /* ERET, DRPS, unallocated */
    }
}

/* Data-processing (1 source) with op2 = 00001: PACIA..AUTDB take any Rn,
 * the zero-modifier forms and XPACI/XPACD need Rn = 11111; 64-bit only. */
bool a64_pac_valid(uint32_t i)
{
    unsigned opc = BITS(i, 15, 10), rn = BITS(i, 9, 5);
    if (!BIT(i, 31) || BIT(i, 29) || BITS(i, 20, 16) != 1 || opc > 0x11) return false;
    return opc < 8 || rn == 31;
}

/* Strip(ptr, data) of the Arm ARM with the EL0 translation regime the M4
 * gives macOS processes, as the oracle's pac records measure it natively:
 * T0SZ = T1SZ = 17 (the PAC field is bits 63:47 or, with TBI, 55:47),
 * TBI0 = 1 with TBID0 = 1 (the top byte is ignored for data addresses only),
 * TBI1 = 0. Bits 46:0 never change. */
uint64_t a64_xpac(uint64_t v, bool data)
{
    const uint64_t va = (1ull << 47) - 1;
    if ((v >> 55) & 1) return v | ~va;                       /* TTBR1 half: no TBI */
    return v & (data ? (va | 0xffull << 56) : va);           /* TTBR0 half: TBI for data */
}

int a64_step(hao_cpu *c)
{
    uint64_t pc = c->pc;
    uint32_t insn = hao_code_read32(pc);   /* hao/code.h */
    c->insn_count++;
    return a64_exec(c, insn, pc);
}
