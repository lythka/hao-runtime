/*
 * hao — the x86_64 swifttailcc argument area of a guest async function.
 *
 * x86_64 swifttailcc callees pop their own stack-argument area on return:
 * 8 bytes per stack slot, rounded so that the area plus the return address
 * is a multiple of 16 (8 for 0 or 1 slots, 24 for 2 or 3, 40 for 4 or 5).
 * When native code tail-calls a translated async function, the native side
 * laid out that function's arguments for its x86_64 signature and moved the
 * return address below them, so hao_callback_entry must pop exactly the
 * guest function's own area (see callee_pop in runtime.c).
 *
 * The signature comes from the guest function itself. Swift symbols are
 * stripped from OS binaries, so the source is the arm64 code: which
 * argument registers and incoming stack slots the function reads before
 * writing them, and the arm64 area it pops at its tail-call exits. hao's
 * bridge maps the x86_64 stack arguments to [x6, x7, guest stack slot 0,
 * slot 1, ...] (hao_callback), so the function needs as many x86_64 slots
 * as the highest position of that sequence it uses:
 *   x6 read -> 1, x7 read -> 2, guest slot k read -> 3 + k,
 *   arm64 area A > 0 popped at an exit -> at least 2 + A/8 - 1.
 *
 * The scan is a forward must-define dataflow over the function's control
 * flow graph: conditional branches inside it and direct tail calls into
 * other guest code are followed (forwarding thunks read their arguments in
 * the function they jump to), calls into the machine outliner's code run in
 * place (it shares the caller's frame and may move sp: an outlined
 * `sub sp, sp, #N` prologue), other calls clobber x0-x18, indirect branches
 * end a path, and so does running on into the next function (after a call
 * that does not return). It tracks register values that are sp-relative
 * (frame pointer, copies of sp) to recognise incoming-argument loads, and
 * stores to the incoming area (a tail call that reuses it) so that the
 * callee's reads of those slots are not counted. tools/swiftslots27.sh
 * checks it offline against the compiler's role records: every resume
 * partial (__swift_as_ret, __swift_as_cont) of the 27.0 arm64-only
 * programs must come out at 0 slots.
 *
 * The result is a lower bound: an argument the code never reads (a generic
 * witness's trailing Self metadata and witness table, an unused parameter
 * at -O) is not visible, and the arm64 area only bounds the slot count to
 * one of two values. callee_pop in runtime.c uses it only when the
 * caller's frame geometry gives no exact value.
 */
#include "hao/runtime.h"
#include "hao/gmap.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UNK INT32_MIN
#define MAXADDR 4096      /* distinct instructions visited */
#define MAXSTEPS 32768    /* instructions processed (revisits included) */

typedef struct {
    uint32_t def;         /* x0-x30 defined on every path from entry */
    uint64_t sdef;        /* incoming stack slots 0-63 written on every path */
    int32_t off[32];      /* value - entry sp, for sp-derived registers; [31] = sp */
} st;

typedef struct {
    uint64_t pc;
    st s;
    bool seen;
} node;

enum { F_NEXT, F_JUMP, F_COND, F_CALL, F_EXIT, F_END, F_UNKNOWN };

typedef struct {
    uint32_t rd, wr;      /* GPR x0-x30 read / written */
    int flow;
    uint64_t target;
    /* memory access: base register (31 = sp), address = base + imm (pre),
     * bytes, load or store, writeback amount applied to the base */
    int mem, base, load, bytes, wb_reg;
    int64_t imm, wb;
    /* sp-relative value propagation: rd = (src register) + add, or unknown */
    int dst, src;
    int64_t add;
} ins;

static inline uint32_t bit(unsigned r) { return r < 31 ? 1u << r : 0; }
static inline int64_t sx(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

/* Registers read and written by one A64 instruction (GPRs only), its
 * control flow, and what the scan needs about memory and sp arithmetic. */
static void decode(uint32_t i, uint64_t pc, ins *o)
{
    memset(o, 0, sizeof *o);
    o->flow = F_NEXT;
    o->dst = -1;
    o->wb_reg = -1;
    unsigned rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31, ra = (i >> 10) & 31;
    unsigned op0 = (i >> 25) & 15;

    if ((op0 & 0xE) == 0x8) {                         /* data processing, immediate */
        unsigned op = (i >> 23) & 7;
        bool sf = i >> 31;
        switch (op) {
        case 0: case 1: o->wr = bit(rd); return;      /* ADR, ADRP */
        case 2: {                                     /* ADD/SUB immediate */
            bool sub = (i >> 30) & 1, s = (i >> 29) & 1;
            int64_t imm = ((i >> 10) & 0xfff) << (((i >> 22) & 1) ? 12 : 0);
            o->rd = bit(rn);
            int d = (rd == 31 && !s) ? 31 : (rd == 31 ? -1 : (int)rd);
            if (d >= 0) {
                o->wr = bit(d);
                o->dst = d;
                if (sf) { o->src = rn; o->add = sub ? -imm : imm; }   /* rn 31 = sp */
                else o->src = -1;
            }
            return;
        }
        case 3: o->rd = bit(rn); o->wr = bit(rd); o->dst = rd; o->src = -1; return;   /* ADDG/SUBG */
        case 4: {                                     /* logical immediate */
            bool ands = ((i >> 29) & 3) == 3;
            o->rd = bit(rn);
            int d = (rd == 31 && !ands) ? 31 : (rd == 31 ? -1 : (int)rd);
            if (d >= 0) { o->wr = bit(d); o->dst = d; o->src = -1; }
            return;
        }
        case 5:                                       /* MOVN/MOVZ/MOVK */
            if (((i >> 29) & 3) == 3) o->rd = bit(rd);
            o->wr = bit(rd);
            return;
        case 6:                                       /* SBFM/BFM/UBFM */
            o->rd = bit(rn) | (((i >> 29) & 3) == 1 ? bit(rd) : 0);
            o->wr = bit(rd);
            return;
        default:                                      /* EXTR */
            o->rd = bit(rn) | bit(rm);
            o->wr = bit(rd);
            return;
        }
    }

    if ((op0 & 0xE) == 0xA) {                         /* branches, exceptions, system */
        if ((i & 0x7C000000) == 0x14000000) {         /* B, BL */
            o->target = pc + (uint64_t)(sx(i & 0x3ffffff, 26) << 2);
            if (i >> 31) { o->flow = F_CALL; o->wr = bit(30); }
            else o->flow = F_JUMP;
            return;
        }
        if ((i & 0x7E000000) == 0x34000000) {         /* CBZ/CBNZ */
            o->rd = bit(rd);
            o->flow = F_COND;
            o->target = pc + (uint64_t)(sx((i >> 5) & 0x7ffff, 19) << 2);
            return;
        }
        if ((i & 0x7E000000) == 0x36000000) {         /* TBZ/TBNZ */
            o->rd = bit(rd);
            o->flow = F_COND;
            o->target = pc + (uint64_t)(sx((i >> 5) & 0x3fff, 14) << 2);
            return;
        }
        if ((i & 0xFF000000) == 0x54000000) {         /* B.cond, BC.cond */
            o->flow = F_COND;
            o->target = pc + (uint64_t)(sx((i >> 5) & 0x7ffff, 19) << 2);
            return;
        }
        if ((i & 0xFF000000) == 0xD4000000) {         /* SVC/HVC/SMC; BRK, HLT end the path */
            if (((i >> 21) & 7) == 0) { o->rd = bit(16); o->wr = bit(0) | bit(1); return; }
            o->flow = F_END;
            return;
        }
        if ((i & 0xFFC00000) == 0xD5000000) {         /* system */
            unsigned l = (i >> 21) & 1, sop0 = (i >> 19) & 3;
            if (l) o->wr = bit(rd);                   /* MRS, SYSL */
            else if (sop0) o->rd = bit(rd);           /* SYS, MSR (register) */
            return;                                   /* hints (PAC*SP, BTI), barriers, MSR imm */
        }
        if ((i & 0xFE000000) == 0xD6000000) {         /* branch to register */
            unsigned opc = (i >> 21) & 15;
            o->rd = bit(rn) | ((opc & 8) ? bit(rd) : 0);   /* BRAA/BLRAA modifier */
            if (opc == 1 || opc == 9) { o->flow = F_CALL; o->wr = bit(30); o->target = 0; }
            else o->flow = F_EXIT;                    /* BR, RET, BRAA, RETAA, ERET */
            return;
        }
        o->flow = F_UNKNOWN;
        return;
    }

    if ((op0 & 5) == 4) {                             /* loads and stores */
        bool v = (i >> 26) & 1;
        unsigned rt = rd, rt2 = ra;
        if ((i & 0xBF800000) == 0x0C000000 || (i & 0xBF000000) == 0x0D000000 ||
            (i & 0xBF800000) == 0x0C800000) {         /* SIMD load/store multiple, single */
            o->rd = bit(rn);
            if ((i >> 23) & 1) { if (rm != 31) o->rd |= bit(rm); o->dst = rn == 31 ? 31 : (int)rn; o->src = -1; o->wr = bit(rn); }
            return;
        }
        if ((i & 0x3F000000) == 0x08000000) {         /* exclusive, ordered, compare-and-swap */
            bool l = (i >> 22) & 1, pair = (i >> 21) & 1 && !((i >> 23) & 1);
            o->rd = bit(rn) | bit(rm);
            if (l) o->wr = bit(rt) | (pair ? bit(rt2) : 0);
            else o->rd |= bit(rt) | (pair ? bit(rt2) : 0);
            if ((i >> 23) & 1 && (i >> 21) & 1) { o->rd |= bit(rt); o->wr |= bit(rm); }   /* CAS */
            else if (!l && !((i >> 23) & 1)) o->wr |= bit(rm);                           /* STXR status */
            return;
        }
        if ((i & 0x3B000000) == 0x18000000) {         /* LDR (literal), PRFM */
            if (!v && (i >> 30) != 3) o->wr = bit(rt);
            return;
        }
        if ((i & 0x3F000000) == 0x19000000) {         /* LDAPUR/STLUR, memory tags */
            o->rd = bit(rn);
            if (((i >> 22) & 3) && !v) o->wr = bit(rt); else o->rd |= bit(rt);
            return;
        }
        if ((i & 0x38000000) == 0x28000000) {         /* load/store pair */
            unsigned opc = i >> 30, mode = (i >> 23) & 3;
            bool l = (i >> 22) & 1;
            int scale = v ? 4 << opc : (opc == 2 ? 8 : 4);
            int64_t imm = sx((i >> 15) & 0x7f, 7) * scale;
            o->mem = 1; o->base = rn; o->load = l; o->bytes = 2 * scale;
            o->imm = mode == 1 ? 0 : imm;
            if (mode == 1 || mode == 3) { o->wb_reg = rn; o->wb = imm; }
            o->rd = bit(rn);
            if (!v) {
                if (l) o->wr = bit(rt) | bit(rt2);
                else o->rd |= bit(rt) | bit(rt2);
            }
            if (o->wb_reg >= 0) o->wr |= bit(rn);
            return;
        }
        if ((i & 0x38000000) == 0x38000000) {         /* load/store register */
            unsigned size = i >> 30, opc = (i >> 22) & 3;
            bool l = opc != 0, prfm = !v && size == 3 && opc == 2;
            if (v && opc >= 2) { l = opc == 3; size = 4; }
            if (!v && opc >= 2) l = true;
            o->rd = bit(rn);
            o->mem = !prfm; o->base = rn; o->load = l; o->bytes = 1 << size;
            if ((i >> 24) & 1) {                      /* unsigned offset */
                o->imm = (int64_t)((i >> 10) & 0xfff) << size;
            } else if (!((i >> 21) & 1)) {            /* unscaled, post, unprivileged, pre */
                int64_t imm = sx((i >> 12) & 0x1ff, 9);
                unsigned m = (i >> 10) & 3;
                o->imm = m == 1 ? 0 : imm;
                if (m == 1 || m == 3) { o->wb_reg = rn; o->wb = imm; o->wr |= bit(rn); }
            } else if (((i >> 10) & 3) == 2) {        /* register offset */
                o->rd |= bit(rm);
                o->mem = 0;
            } else if (((i >> 10) & 3) == 0) {        /* atomic memory operations */
                o->rd |= bit(rm);
                o->mem = 0;
                if (!v) o->wr = bit(rt);
                return;
            } else {                                  /* LDRAA/LDRAB */
                int64_t imm = sx(((i >> 12) & 0x1ff) | ((i >> 13) & 0x200), 10) << 3;
                o->imm = imm; o->bytes = 8; o->load = 1;
                if ((i >> 11) & 1) { o->wb_reg = rn; o->wb = imm; o->imm = imm; o->wr |= bit(rn); }
                o->wr |= bit(rt);
                return;
            }
            if (!v && !prfm) {
                if (l) o->wr |= bit(rt);
                else o->rd |= bit(rt);
            }
            return;
        }
        o->flow = F_UNKNOWN;
        return;
    }

    if ((op0 & 7) == 5) {                             /* data processing, register */
        if (!((i >> 28) & 1)) {                       /* logical / add-sub shifted or extended */
            o->rd = bit(rn) | bit(rm);
            bool ext = ((i >> 24) & 1) && ((i >> 21) & 1);
            bool s = (i >> 29) & 1;
            if (ext) {                                /* extended: rn and (non-S) rd may be sp */
                int d = (rd == 31 && !s) ? 31 : (rd == 31 ? -1 : (int)rd);
                if (d >= 0) { o->wr = bit(d); o->dst = d; o->src = -1; }
                return;
            }
            o->wr = bit(rd);
            /* MOV Xd, Xm = ORR Xd, XZR, Xm (64-bit, no shift): copies an sp-relative value */
            if ((i & 0xFFE0FFE0) == 0xAA0003E0) { o->dst = rd; o->src = rm == 31 ? -1 : (int)rm; o->add = 0; }
            return;
        }
        unsigned op2 = (i >> 21) & 15;
        if (op2 == 0) { o->rd = bit(rn) | bit(rm); if (((i >> 10) & 0x3f) == 0) o->wr = bit(rd); return; }   /* ADC/SBC; RMIF/SETF */
        if (op2 == 2) { o->rd = bit(rn) | (((i >> 11) & 1) ? 0 : bit(rm)); return; }                        /* CCMP/CCMN */
        if (op2 == 4) { o->rd = bit(rn) | bit(rm); o->wr = bit(rd); return; }                               /* CSEL... */
        if (op2 == 6) {
            if ((i >> 30) & 1) {                      /* 1-source; PAC/AUT/XPAC also read rd */
                o->rd = bit(rn) | (rm == 1 ? bit(rd) : 0);
                o->wr = bit(rd);
            } else { o->rd = bit(rn) | bit(rm); o->wr = bit(rd); }
            return;
        }
        if (op2 & 8) { o->rd = bit(rn) | bit(rm) | bit(ra); o->wr = bit(rd); return; }                       /* 3-source */
        o->flow = F_UNKNOWN;
        return;
    }

    if ((op0 & 7) == 7) {                             /* SIMD and FP data processing */
        if ((i & 0x5F20FC00) == 0x1E200000) {         /* FP <-> integer */
            unsigned opc = (i >> 16) & 7;
            if (opc == 2 || opc == 3 || opc == 7) o->rd = bit(rn);
            else o->wr = bit(rd);
            return;
        }
        if ((i & 0x5F200000) == 0x1E000000) {         /* FP <-> fixed point */
            unsigned opc = (i >> 16) & 7;
            if (opc == 2 || opc == 3) o->rd = bit(rn);
            else o->wr = bit(rd);
            return;
        }
        if ((i & 0x9FE08400) == 0x0E000400) {         /* SIMD copy: DUP/INS (general), SMOV/UMOV */
            unsigned imm4 = (i >> 11) & 15;
            if (imm4 == 1 || imm4 == 3) o->rd = bit(rn);
            else if (imm4 == 5 || imm4 == 7) o->wr = bit(rd);
            return;
        }
        return;                                       /* no general registers */
    }
    o->flow = F_UNKNOWN;                              /* SVE, SME, unallocated */
}

/* Join a predecessor's out-state into a node's in-state; true if it changed. */
static bool merge(node *n, const st *s)
{
    if (!n->seen) { n->s = *s; n->seen = true; return true; }
    bool ch = false;
    uint32_t d = n->s.def & s->def;
    uint64_t sd = n->s.sdef & s->sdef;
    if (d != n->s.def || sd != n->s.sdef) { n->s.def = d; n->s.sdef = sd; ch = true; }
    for (int k = 0; k < 32; k++)
        if (n->s.off[k] != s->off[k] && n->s.off[k] != UNK) { n->s.off[k] = UNK; ch = true; }
    return ch;
}

static node *lookup(node *tab, uint64_t pc, bool add)
{
    for (uint32_t k = 0, h = (uint32_t)((pc >> 2) * 0x9E3779B1u) & (MAXADDR - 1); k < MAXADDR; k++) {
        node *n = &tab[(h + k) & (MAXADDR - 1)];
        if (n->pc == pc) return n;
        if (!n->pc) { if (!add) return NULL; n->pc = pc; return n; }
    }
    return NULL;
}

/* What one scan accumulates: the slot count from reads and from the arm64
 * pops at exits, and the instruction that set each. */
typedef struct {
    int res, popslots;
    uint64_t res_pc, pop_pc;
} acc;

/* One instruction's effect on the state: argument registers and incoming
 * stack slots it reads before they are written (counted into a), stores
 * to the incoming area, sp-relative values, registers defined. */
static void step(st *s, const ins *o, uint64_t pc, acc *a)
{
    uint32_t und = o->rd & ~s->def;
    if ((und & bit(7)) && a->res < 2) { a->res = 2; a->res_pc = pc; }
    else if ((und & bit(6)) && a->res < 1) { a->res = 1; a->res_pc = pc; }
    /* incoming stack slots read (or written) through an sp-relative base */
    if (o->mem && s->off[o->base] != UNK) {
        int64_t lo = (int64_t)s->off[o->base] + o->imm, e = lo + o->bytes;
        if (o->load) {
            for (int64_t b = lo < 0 ? 0 : lo; b < e && b < 64 * 8; b = (b & ~7) + 8)
                if (!(s->sdef >> (b >> 3) & 1) && 3 + (int)(b >> 3) > a->res) { a->res = 3 + (int)(b >> 3); a->res_pc = pc; }
        } else {                                      /* whole slots written: no longer incoming */
            for (int64_t c = (lo + 7) & ~7; c >= 0 && c + 8 <= e && c < 64 * 8; c += 8) s->sdef |= 1ull << (c >> 3);
        }
    }
    /* sp-relative value tracking */
    int32_t nv = UNK;
    bool setv = false;
    if (o->dst >= 0) {
        setv = true;
        if (o->src >= 0 && s->off[o->src] != UNK && o->add > -(1 << 30) && o->add < (1 << 30)) nv = s->off[o->src] + (int32_t)o->add;
    }
    if (o->wb_reg >= 0) {
        int r = o->wb_reg;
        s->off[r] = s->off[r] == UNK ? UNK : s->off[r] + (int32_t)o->wb;
    }
    s->def |= o->wr;
    for (int k = 0; k < 31; k++) if ((o->wr >> k & 1) && k != o->wb_reg) s->off[k] = UNK;
    if (setv) s->off[o->dst] = nv;
}

/* A call's effect when the callee is a function that keeps the AAPCS64
 * contract: x0-x18 and x30 are the callee's, sp and the rest preserved. */
static void clobber(st *s)
{
    s->def |= 0x4007ffff;
    for (int k = 0; k <= 18; k++) s->off[k] = UNK;
    s->off[30] = UNK;
}

/* Outlined code (the machine outliner's functions): a straight run of at
 * most MAXOUT instructions ending in RET, or in B to another function (its
 * thunk variant). Unlike a function it keeps no contract: it runs on the
 * caller's sp, so it may move sp (an outlined `sub sp, sp, #N` prologue or
 * epilogue), derive registers from it (`add x16, sp, #N`) and read the
 * caller's frame, and it writes any register. Returns its length (the RET
 * or B not counted) and sets *tail for the thunk variant, or -1 when the
 * code at t is not such a run. */
#define MAXOUT 32
static int outlined(uint64_t t, bool *tail)
{
    hao_image *im = hao_image_for_code(t);
    if (!im) return -1;
    uint64_t end = hao_image_extent(im, t);
    for (int k = 0; k < MAXOUT && t + 4 * (uint64_t)k + 4 <= end; k++) {
        uint64_t pc = t + 4 * (uint64_t)k;
        uint32_t i = hao_code_read32(pc);
        if (i == 0xd65f03c0) { *tail = false; return k; }   /* RET */
        if (k && hao_is_guest_function(pc)) return -1;       /* ran into the next function */
        ins o;
        decode(i, pc, &o);
        if (o.flow == F_JUMP) { *tail = true; return k; }
        if (o.flow != F_NEXT || (o.wr & bit(30)) || (o.rd & bit(30))) return -1;
    }
    return -1;
}

/* The x86_64 stack-argument slot count of the guest function at fn, or -1
 * when its code cannot be followed (unknown encoding, too large).
 *
 * The control flow followed is the function's own: a path ends at an exit
 * (RET, an indirect branch, a tail call out of guest code), at BRK/HLT, and
 * where it would run on into the next function in LC_FUNCTION_STARTS
 * (after a call that does not return: ___stack_chk_fail, a Swift runtime
 * failure). A direct branch to another guest function is a tail call and is
 * followed (a forwarding thunk reads its arguments there). A call into
 * outlined code is followed in place, since that code is part of the
 * caller's frame; any other call keeps sp and clobbers x0-x18. */
static int arg_slots(uint64_t fn, int *from_pop, uint64_t *at)
{
    node *tab = calloc(MAXADDR, sizeof *tab);
    uint64_t *work = malloc(MAXSTEPS * sizeof *work);
    if (!tab || !work) { free(tab); free(work); return -1; }
    int nwork = 0, steps = 0;
    acc a = {0};
    bool bad = false;
    st s0 = {0};
    for (int k = 0; k < 32; k++) s0.off[k] = UNK;
    s0.off[31] = 0;
    node *n0 = lookup(tab, fn, true);
    merge(n0, &s0);
    work[nwork++] = fn;
    while (nwork && !bad) {
        uint64_t pc = work[--nwork];
        if (++steps > MAXSTEPS) { bad = true; break; }
        node *n = lookup(tab, pc, false);
        st s = n->s;
        uint32_t insn = hao_code_read32(pc);
        ins o;
        decode(insn, pc, &o);
        if (o.flow == F_UNKNOWN) { bad = true; break; }
        step(&s, &o, pc, &a);
        if (o.flow == F_CALL) {
            bool tail = false;
            int len = o.target ? outlined(o.target, &tail) : -1;
            if (len >= 0) {                           /* run the outlined code in this frame */
                for (int k = 0; k < len; k++) {
                    uint64_t q = o.target + 4 * (uint64_t)k;
                    ins oi;
                    decode(hao_code_read32(q), q, &oi);
                    step(&s, &oi, q, &a);
                }
                if (tail) clobber(&s);                /* ends by calling a function */
            } else clobber(&s);
        }
        /* the arm64 area this exit pops: sp above the entry sp */
        if (o.flow == F_EXIT || (o.flow == F_JUMP && !hao_image_for_code(o.target))) {
            if (s.off[31] != UNK && s.off[31] > 0 && s.off[31] < 64 * 8) {
                int ps = 2 + s.off[31] / 8 - 1;
                if (ps > a.popslots) { a.popslots = ps; a.pop_pc = pc; }
            }
            continue;
        }
        if (o.flow == F_END) continue;
        uint64_t succ[2];
        int ns = 0;
        /* the next instruction, unless it starts another function */
        if ((o.flow == F_NEXT || o.flow == F_CALL || o.flow == F_COND) && !hao_is_guest_function(pc + 4)) succ[ns++] = pc + 4;
        if (o.flow == F_JUMP || o.flow == F_COND) succ[ns++] = o.target;
        for (int k = 0; k < ns; k++) {
            if (!hao_image_for_code(succ[k])) continue;
            node *m = lookup(tab, succ[k], true);
            if (!m) { bad = true; break; }
            if (merge(m, &s)) {
                if (nwork >= MAXSTEPS) { bad = true; break; }
                work[nwork++] = succ[k];
            }
        }
    }
    free(tab);
    free(work);
    if (bad) return -1;
    *from_pop = a.popslots > a.res;
    if (at) *at = a.popslots > a.res ? a.pop_pc : a.res_pc;
    return a.res > a.popslots ? a.res : a.popslots;
}

int hao_swift_async_slots(uint64_t fn, int *from_pop, uint64_t *at)
{
    *from_pop = 0;
    if (at) *at = 0;
    return hao_image_for_code(fn) ? arg_slots(fn, from_pop, at) : -1;
}

/* x86_64 swifttailcc argument area (bytes the callee pops) for a function
 * with `slots` stack-argument slots. */
static int64_t area(int slots) { return ((int64_t)slots * 8 + 8 + 15) / 16 * 16 - 8; }

int64_t hao_swift_async_arg_area(uint64_t fn, const char **why)
{
    enum { N = 1024 };
    static _Atomic uint64_t cache[N];   /* fn << 16 | (area + 1) << 1 | from_pop */
    uint64_t h = ((fn >> 2) * 0x9E3779B97F4A7C15ull) >> 54;
    uint64_t e = cache[h];
    if (e && (e >> 16) == fn) {
        *why = (e & 1) ? "signature (arm64 pop)" : "signature";
        return (int64_t)((e & 0xffff) >> 1) - 1;
    }
    int from_pop = 0;
    int slots = hao_image_for_code(fn) ? arg_slots(fn, &from_pop, NULL) : -1;
    int64_t r = slots < 0 ? -1 : area(slots);
    *why = slots < 0 ? "signature unknown" : from_pop ? "signature (arm64 pop)" : "signature";
    if (fn < (1ull << 47) && r < 0x7ff0) cache[h] = fn << 16 | (uint64_t)(r + 1) << 1 | (uint64_t)from_pop;
    return r;
}

/* ------------------------------------------------ narrow async results
 *
 * A synchronous Swift function returning Bool/Int8/Int16 leaves al/ax
 * (x86_64) or the low bits of w0 (arm64) with the rest undefined, and Swift
 * callers in both ABIs mask or extend the result themselves, so the bridge
 * passes it through. An async function's result is different: it is an
 * argument of the caller's continuation, which the callee tail-calls, and
 * arm64 Swift code relies on narrow arguments being extended (it stores a
 * Bool argument with strb as 0 or 1), while x86_64 Swift passes them with
 * the other bits undefined (at -Onone a Bool is the low bit of a 32-bit
 * load). So when the guest calls a native async function whose result is
 * narrow (its demangled name says so, checked once when the import is
 * bound), the guest continuation it stored in the callee's context
 * (ResumeParent, at offset 8) is noted, and hao_callback extends x0 when
 * native code resumes it. */

/* char *swift_demangle(mangled, length, buffer, &size, flags) (runtime/Demangle.cpp): NULL on failure */
typedef char *(*demangle_fn)(const char *, size_t, char *, size_t *, uint32_t);

static void demangle_selfcheck_verbose(demangle_fn fn);

static demangle_fn demangler(void)
{
    static demangle_fn fn;
    static int tried;
    if (!tried) {
        /* only once libswiftCore is loaded (by the native library being bound) */
        void *h = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
        if (!h) return NULL;
        fn = (demangle_fn)dlsym(h, "swift_demangle");
        if (!__atomic_exchange_n(&tried, 1, __ATOMIC_ACQ_REL) && hao_verbose) demangle_selfcheck_verbose(fn);
    }
    return fn;
}

/* Kind for a demangled entity "Name(params) async [throws[(E)]] -> T":
 * T a narrow integer type, the arrow the entity's own (its parameter list's
 * '(' follows the name, not a space as in a function-type result). */
char hao_swift_async_kind_for_demangled(const char *d)
{
    static const struct { const char *t; char k; } narrow[] = {
        {"Swift.Bool", 'b'}, {"Swift.Int8", 'h'}, {"Swift.UInt8", 'H'}, {"Swift.Int16", 't'}, {"Swift.UInt16", 'T'},
        {"Swift.Bool?", 'H'}, {"Swift.Optional<Swift.Bool>", 'H'},
    };
    const char *arrow = NULL;
    for (const char *p = d; (p = strstr(p, " -> ")); p++) arrow = p;
    if (!arrow) return 0;
    char k = 0;
    for (unsigned n = 0; n < sizeof narrow / sizeof *narrow; n++)
        if (!strcmp(arrow + 4, narrow[n].t)) k = narrow[n].k;
    if (!k) return 0;
    /* back over " throws(E)", " throws" to " async" */
    const char *e = arrow;
    if (e > d && e[-1] == ')') {                      /* typed throws */
        int depth = 0;
        const char *q = e - 1;
        for (; q > d; q--) { if (*q == ')') depth++; else if (*q == '(' && !--depth) break; }
        if (q - d >= 7 && !strncmp(q - 7, " throws", 7)) e = q - 7; else e = NULL;
    } else if (e - d >= 7 && !strncmp(e - 7, " throws", 7)) e -= 7;
    if (!e || e - d < 7 || strncmp(e - 6, " async", 6)) return 0;
    e -= 6;
    if (e <= d || e[-1] != ')') return 0;             /* the parameter list */
    int depth = 0;
    const char *q = e - 1;
    for (; q >= d; q--) { if (*q == ')') depth++; else if (*q == '(' && !--depth) break; }
    if (q <= d || q[-1] == ' ') return 0;
    return k;
}

char hao_swift_async_result_kind(const char *sym)
{
    if (strncmp(sym, "_$s", 3) || !strstr(sym, "Ya")) return 0;
    size_t n = strlen(sym);
    if (n < 6 || (sym[n - 1] != 'F' && !(sym[n - 1] == 'Z' && sym[n - 2] == 'F'))) return 0;
    demangle_fn dm = demangler();
    if (!dm) return 0;
    char buf[1024];
    size_t len = sizeof buf;
    if (!dm(sym + 1, n - 1, buf, &len, 0)) return 0;
    buf[len < sizeof buf ? len : sizeof buf - 1] = 0;
    return hao_swift_async_kind_for_demangled(buf);
}

/* Does the demangled entity "Name(params) ..." have a parameter whose type
 * contains an async function type ("@Sendable () async throws -> ()")? The
 * parameter list is the first parenthesised group; an " async" token
 * inside it belongs to a parameter's type (the entity's own effect follows
 * the group, a label "async:" is followed by a colon). */
bool hao_swift_async_param_demangled(const char *d)
{
    const char *p = strchr(d, '(');
    if (!p) return false;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && !--depth) return false;
        else if (*p == ' ' && !strncmp(p + 1, "async", 5) && (p[6] == ' ' || p[6] == ')' || p[6] == ',')) return true;
    }
    return false;
}

bool hao_swift_async_param(const char *sym)
{
    /* the mangling of an async function type has "Ya" (Mangling.rst: async-signature) */
    if (strncmp(sym, "_$s", 3) || !strstr(sym, "Ya")) return false;
    demangle_fn dm = demangler();
    if (!dm) return false;
    char buf[4096];
    size_t len = sizeof buf;
    if (!dm(sym + 1, strlen(sym + 1), buf, &len, 0)) return false;
    buf[sizeof buf - 1] = 0;
    return hao_swift_async_param_demangled(buf);
}

/* The narrow-result and async-parameter rules above read the demangler's
 * spelling ("Name() async throws -> Swift.Bool", "@Sendable () async ->
 * ()"): the facts sit in the mangled function type, whose result and
 * parameters only a full Swift demangler finds. These fixed manglings
 * (swiftc 6, -module-name hao) and what the rules must conclude from their
 * spelling pin it: hao_swift_demangle_selfcheck runs them (tests/host/
 * bridge.c), and hao -v reports a mismatch the first time it demangles. */
static const struct { const char *sym; char kind; bool async_param; } spellings[] = {
    {"_$s3hao5readySbyYaF", 'b', false},                   /* ready() async -> Bool */
    {"_$s3hao5levels4Int8VyYaKF", 'h', false},             /* level() async throws -> Int8 */
    {"_$s3hao5typeds6UInt16VyYaAA1EOYKF", 'T', false},     /* typed() async throws(E) -> UInt16 */
    {"_$s3hao5plainSbyF", 0, false},                       /* plain() -> Bool */
    {"_$s3hao3runyyyyYaYbKXEF", 0, true},                  /* run(_: @Sendable () async throws -> ()) */
};

static int demangle_selfcheck(demangle_fn dm, char *why, size_t n)
{
    int bad = 0;
    why[0] = 0;
    for (size_t k = 0; k < sizeof spellings / sizeof *spellings; k++) {
        char buf[1024];
        size_t len = sizeof buf;
        const char *s = spellings[k].sym;
        bool ok = dm(s + 1, strlen(s + 1), buf, &len, 0) != NULL;
        buf[ok ? (len < sizeof buf ? len : sizeof buf - 1) : 0] = 0;
        if (!ok || hao_swift_async_kind_for_demangled(buf) != spellings[k].kind ||
            hao_swift_async_param_demangled(buf) != spellings[k].async_param)
            if (!bad++) snprintf(why, n, "swift_demangle(%s) = \"%s\": the async result or parameter rule reads it otherwise", s + 1,
                                 ok ? buf : "(fails)");
    }
    return bad;
}

int hao_swift_demangle_selfcheck(char *why, size_t n)
{
    demangle_fn dm = demangler();
    return dm ? demangle_selfcheck(dm, why, n) : -1;
}

static void demangle_selfcheck_verbose(demangle_fn fn)
{
    char why[512];
    if (fn && demangle_selfcheck(fn, why, sizeof why) > 0) {
        HAO_LOG(0, "demangler spelling changed: %s; narrow async results may not be extended\n", why);
    }
}

/* A Swift entry point whose signature the metadata ABI fixes, named by its
 * mangling's final operator (docs/ABI/Mangling.rst); the runtime's demangler
 * only confirms that the whole name is a valid mangling (its spelling of the
 * entity is not read):
 *  - a type metadata accessor (`Ma`): swiftcc MetadataResponse
 *    (MetadataRequest, then at most 3 generic arguments or a pointer to an
 *    array of them; IRGen NumDirectGenericTypeMetadataAccessFunctionArgs):
 *    at most 4 GP words in, x0 x1 / rax rdx out;
 *  - a value witness of today's table (`w` + kind; include/swift/ABI/
 *    ValueWitness.def): value and buffer addresses, unsigned and the type's
 *    metadata in, at most 4 GP words; a value address or unsigned out.
 * hao_native_call carries both exactly, and none of their arguments is guest
 * code (integers, metadata, witness tables, addresses of values). The
 * historical witnesses (allocateBuffer, storeExtraInhabitant, ...) are no
 * longer in the table and are not matched. */
bool hao_swift_fixed_abi(const char *sym)
{
    /* value-witness-kind (ValueWitness.def): initializeBufferWithCopyOfBuffer, destroy, initializeWithCopy,
     * assignWithCopy, initializeWithTake, assignWithTake, getEnumTagSinglePayload, storeEnumTagSinglePayload,
     * getEnumTag, destructiveProjectEnumData, destructiveInjectEnumTag */
    static const char vw[][3] = {"CP", "xx", "cp", "ca", "tk", "ta", "et", "st", "ug", "up", "ui"};
    if (!sym || strncmp(sym, "_$s", 3)) return false;
    size_t n = strlen(sym);
    if (n < 7) return false;
    bool fixed = !strcmp(sym + n - 2, "Ma");   /* global ::= type 'Ma' */
    if (!fixed && sym[n - 3] == 'w')           /* global ::= type 'w' value-witness-kind */
        for (unsigned k = 0; k < sizeof vw / sizeof *vw; k++)
            if (!strcmp(sym + n - 2, vw[k])) fixed = true;
    if (!fixed) return false;
    demangle_fn dm = demangler();
    if (!dm) return false;
    char buf[1024];
    size_t len = sizeof buf;
    return dm(sym + 1, n - 1, buf, &len, 0) != NULL;
}

/* guest continuations that receive a narrow result: address -> kind. Lock-free
 * reads; writers hold cont_lock (hao/gmap.h). */
static hao_gmap conts = HAO_GMAP_INIT(2, 1, 8, 24);
static pthread_mutex_t cont_lock = PTHREAD_MUTEX_INITIALIZER;

void hao_swiftcc_fork_child(void) { cont_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

void hao_swift_async_note_call(uint64_t ctx, char kind)
{
    if (!ctx || (ctx & 7)) return;
    uint64_t r = *(const uint64_t *)(uintptr_t)(ctx + 8) & 0x00007fffffffffffull;   /* ResumeParent (PAC stripped) */
    if (!r || hao_gmap_get1(&conts, r) || !hao_image_for_code(r)) return;
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&cont_lock);
    uint64_t *s = hao_gmap_claim1(&conts, r, &fresh);
    if (s && fresh) { s[1] = (uint8_t)kind; hao_gmap_publish1(&conts, s, r); }
    pthread_mutex_unlock(&cont_lock);
    hao_crit_leave();
    /* a continuation left out would get an unextended narrow result */
    if (!s) hao_oom("Swift async narrow-result continuations");
}

uint64_t hao_swift_async_extend_result(uint64_t target, uint64_t x0)
{
    const uint64_t *e = hao_gmap_get1(&conts, target);
    if (e) {
        switch ((char)e[1]) {   /* extended to 32 bits, as an Apple arm64 caller passes it */
        case 'b': return x0 & 1;
        case 'h': return (uint32_t)(int32_t)(int8_t)x0;
        case 'H': return (uint8_t)x0;
        case 't': return (uint32_t)(int32_t)(int16_t)x0;
        case 'T': return (uint16_t)x0;
        default: return x0;
        }
    }
    return x0;
}
