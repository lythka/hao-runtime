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
 * callee's reads of those slots are not counted. Every resume partial
 * (__swift_as_ret, __swift_as_cont) must come out at 0 slots; checked
 * against the compiler's role records outside this tree.
 *
 * The result is a lower bound: an argument the code never reads (a generic
 * witness's trailing Self metadata and witness table, an unused parameter
 * at -O) is not visible, and the arm64 area only bounds the slot count to
 * one of two values. callee_pop in runtime.c uses it only when the
 * caller's frame geometry gives no exact value.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include "hao/gmap.h"
#include <stddef.h>
#include "hao/x86args.h"
#include <dlfcn.h>
#include <objc/runtime.h>
#include <mach-o/loader.h>
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
            o->target = pc + ((uint64_t)sx(i & 0x3ffffff, 26) << 2);
            if (i >> 31) { o->flow = F_CALL; o->wr = bit(30); }
            else o->flow = F_JUMP;
            return;
        }
        if ((i & 0x7E000000) == 0x34000000) {         /* CBZ/CBNZ */
            o->rd = bit(rd);
            o->flow = F_COND;
            o->target = pc + ((uint64_t)sx((i >> 5) & 0x7ffff, 19) << 2);
            return;
        }
        if ((i & 0x7E000000) == 0x36000000) {         /* TBZ/TBNZ */
            o->rd = bit(rd);
            o->flow = F_COND;
            o->target = pc + ((uint64_t)sx((i >> 5) & 0x3fff, 14) << 2);
            return;
        }
        if ((i & 0xFF000000) == 0x54000000) {         /* B.cond, BC.cond */
            o->flow = F_COND;
            o->target = pc + ((uint64_t)sx((i >> 5) & 0x7ffff, 19) << 2);
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
                int64_t imm = (int64_t)((uint64_t)sx(((i >> 12) & 0x1ff) | ((i >> 13) & 0x200), 10) << 3);
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

/* The demangled text of SYM (its leading '_' dropped, N bytes) in BUF of
 * SIZE bytes, or, when it does not fit there (swift_demangle copies what
 * fits and sets the size it needs), demangled again on the heap: the
 * rules below read the result type at the end of the text. BUF, a string
 * the caller frees with demangled_free, or NULL. Bind time only: it may
 * allocate. */
static char *demangle_full(demangle_fn dm, const char *sym, size_t n, char *buf, size_t size)
{
    size_t len = size;
    if (!dm(sym, n, buf, &len, 0)) return NULL;
    if (len <= size) { buf[size - 1] = 0; return buf; }
    len = 0;
    return dm(sym, n, NULL, &len, 0);
}
static void demangled_free(char *d, char *buf) { if (d != buf) free(d); }

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
    char buf[1024], *d = demangle_full(dm, sym + 1, n - 1, buf, sizeof buf);
    if (!d) return 0;
    char kind = hao_swift_async_kind_for_demangled(d);
    demangled_free(d, buf);
    return kind;
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
    char buf[4096], *d = demangle_full(dm, sym + 1, strlen(sym + 1), buf, sizeof buf);
    if (!d) return false;
    bool yes = hao_swift_async_param_demangled(d);
    demangled_free(d, buf);
    return yes;
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
 * longer in the table and are not matched.
 * Also the two runtime functions of a `dynamic` (dynamically replaceable)
 * function's prologue (stdlib/public/runtime/Exclusivity.cpp, IRGen
 * createReplaceableProlog): swift_getFunctionReplacement(char **link, char
 * *current) and swift_getOrigOfReplaceable(char **orig), C calling
 * convention, pointers in and out. `current` is the calling function's own
 * address, which the runtime only compares with *link: a native entry point
 * in its place makes the replacement look installed, and the function then
 * tail-calls *link, itself, forever. */
bool hao_swift_fixed_abi(const char *sym)
{
    if (sym && (!strcmp(sym, "_swift_getFunctionReplacement") || !strcmp(sym, "_swift_getOrigOfReplaceable"))) return true;
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

/* NSTextAlignment in native Swift signatures. NSText.h gives Center and
 * Right the values 1 and 2 where TARGET_ABI_USES_IOS_VALUES (arm64), 2 and 1
 * on x86_64; objc.c converts the SDK's Objective-C methods typed with it, and
 * this does the same for Swift functions whose demangled name types a
 * parameter or the result `__C.NSTextAlignment` (an Int: one GP register):
 *  - "X.name.setter : __C.NSTextAlignment": the new value is x0 (self, if
 *    any, is x20 or follows the formal arguments);
 *  - "X.name.getter : __C.NSTextAlignment" or a synchronous function whose
 *    result is exactly `__C.NSTextAlignment`: the result is x0;
 *  - a parameter typed `__C.NSTextAlignment`, when every parameter before it
 *    has a type whose GP register count is known (Swift.String and String?
 *    two, the integer types and Bool one, Objective-C classes and their
 *    Optionals one, Double, Float and CGFloat none) and it lands in x0-x7.
 *    Any other type before it ends the scan: later parameters are left alone.
 * Bits 0-7 of the result name argument registers x0-x7, HAO_SWIFT_ENUM_RESULT
 * the result. A class name not yet registered at bind time (the guest's
 * imports bind while their native libraries load) gives
 * HAO_SWIFT_ENUM_PENDING: the first call decides again with final set, when
 * an unknown name is no class. A Swift async function, an Optional NSTextAlignment and a
 * mangling the demangler does not spell this way are not converted. */
static int textalign_gp(const char *t, size_t n, bool final)
{
    static const struct { const char *t; int gp; } known[] = {
        {"Swift.String", 2}, {"Swift.String?", 2}, {"Swift.Int", 1}, {"Swift.UInt", 1}, {"Swift.Bool", 1},
        {"Swift.Int8", 1}, {"Swift.UInt8", 1}, {"Swift.Int16", 1}, {"Swift.UInt16", 1}, {"Swift.Int32", 1},
        {"Swift.UInt32", 1}, {"Swift.Int64", 1}, {"Swift.UInt64", 1}, {"Swift.AnyObject", 1}, {"Swift.AnyObject?", 1},
        {"Swift.Double", 0}, {"Swift.Float", 0}, {"CoreGraphics.CGFloat", 0}, {"__C.NSTextAlignment", 1},
    };
    for (unsigned k = 0; k < sizeof known / sizeof *known; k++)
        if (strlen(known[k].t) == n && !memcmp(known[k].t, t, n)) return known[k].gp;
    /* the runtime's demangler spells an Optional "Swift.Optional<T>": a String's two words, a
     * class reference's one (its nil is the null pointer) */
    static const char opt[] = "Swift.Optional<";
    if (n > sizeof opt && !memcmp(t, opt, sizeof opt - 1) && t[n - 1] == '>') {
        const char *in = t + sizeof opt - 1;
        size_t m = n - (sizeof opt - 1) - 1;
        if ((m == 12 && !memcmp(in, "Swift.String", 12)) || (m == 15 && !memcmp(in, "Swift.AnyObject", 15)))
            return m == 12 ? 2 : 1;
        if (m > 4 && !memcmp(in, "__C.", 4) && in[m - 1] != '?' && !(m == 19 && !memcmp(in, "__C.NSTextAlignment", 19)))
            return textalign_gp(in, m, final);   /* a class (an enum's Optional has a tag word too) */
        return -1;
    }
    /* an imported Objective-C class (or its Optional): one pointer */
    if (n > 4 && !memcmp(t, "__C.", 4)) {
        char cls[128];
        size_t m = n - 4 - (t[n - 1] == '?');
        if (m == 0 || m >= sizeof cls) return -1;
        memcpy(cls, t + 4, m);
        cls[m] = 0;
        for (size_t i = 0; i < m; i++)
            if (!((cls[i] >= 'A' && cls[i] <= 'Z') || (cls[i] >= 'a' && cls[i] <= 'z') || (cls[i] >= '0' && cls[i] <= '9') || cls[i] == '_'))
                return -1;
        /* before the class is registered (its library still loading) the answer waits for the first call */
        return objc_getClass(cls) ? 1 : final ? -1 : -2;
    }
    return -1;
}

uint16_t hao_swift_textalign_demangled(const char *d, bool final)
{
    static const char T[] = "__C.NSTextAlignment";
    size_t tl = sizeof T - 1, dl = strlen(d);
    if (!strstr(d, T)) return 0;
    if (dl > tl + 10 && !strncmp(d + dl - tl - 10, ".setter : ", 10) && !strcmp(d + dl - tl, T)) return 1;
    if (dl > tl + 10 && !strncmp(d + dl - tl - 10, ".getter : ", 10) && !strcmp(d + dl - tl, T)) return HAO_SWIFT_ENUM_RESULT;
    if (strstr(d, " : ") && !strstr(d, " -> ")) return 0;   /* another accessor */
    /* the entity's parameter list: the first '(' that is not "(extension in M):" */
    const char *p = d;
    if (!strncmp(p, "static ", 7)) p += 7;
    if (!strncmp(p, "(extension in ", 14)) {
        p = strstr(p, "):");
        if (!p) return 0;
        p += 2;
    }
    p = strchr(p, '(');
    if (!p) return 0;
    uint16_t bits = 0;
    int gp = 0, depth = 0;
    bool scanning = true;
    const char *start = p + 1, *q = p + 1, *unresolved = NULL;
    for (; *q; q++) {
        char ch = *q;
        if (ch == '(' || ch == '<' || ch == '[') depth++;
        else if ((ch == ')' || (ch == '>' && q[-1] != '-') || ch == ']') && depth) depth--;   /* not an arrow's '>' */
        else if (!depth && (ch == ',' || ch == ')')) {
            if (scanning && q > start) {
                /* "label: Type" or "Type" */
                const char *t = start, *e = q;
                int dd = 0;
                for (const char *s = start; s + 1 < q; s++) {
                    if (*s == '(' || *s == '<' || *s == '[') dd++;
                    else if (*s == ')' || (*s == '>' && s[-1] != '-') || *s == ']') dd--;
                    else if (!dd && s[0] == ':' && s[1] == ' ') { t = s + 2; break; }
                }
                int g = textalign_gp(t, (size_t)(e - t), final);
                if (g == -2) unresolved = q;
                if (g < 0) scanning = false;
                else {
                    if ((size_t)(e - t) == tl && !memcmp(t, T, tl) && gp < 8) bits |= (uint16_t)(1u << gp);
                    gp += g;
                }
            }
            if (ch == ')') break;
            start = q + 2;   /* past ", " */
        }
    }
    if (*q != ')') return 0;
    /* a parameter after an unresolved class name may be one: decided at the first call */
    if (unresolved && memmem(unresolved, (size_t)(q - unresolved), T, tl)) bits |= HAO_SWIFT_ENUM_PENDING;
    /* the result: " -> __C.NSTextAlignment" right after the list and its effects, not async */
    const char *r = q + 1;
    if (!strncmp(r, " throws", 7)) {
        r += 7;
        if (*r == '(') { r = strchr(r, ')'); if (!r) return bits; r++; }
    }
    if (!strcmp(r, " -> __C.NSTextAlignment")) bits |= HAO_SWIFT_ENUM_RESULT;
    if (!strncmp(q + 1, " async", 6)) return 0;
    return bits;
}

uint16_t hao_swift_textalign(const char *sym, bool final)
{
    /* the words of "NSTextAlignment" appear in the mangling at least once (a word substitution
     * refers back to an earlier literal word) */
    if (!sym || strncmp(sym, "_$s", 3) || !strcasestr(sym, "alignment")) return 0;
    demangle_fn dm = demangler();
    if (!dm) return 0;
    char buf[4096], *d = demangle_full(dm, sym + 1, strlen(sym + 1), buf, sizeof buf);
    if (!d) return 0;
    uint16_t at = hao_swift_textalign_demangled(d, final);
    demangled_free(d, buf);
    return at;
}

uint64_t hao_swift_textalign_value(uint64_t v)
{
    return v == 1 || v == 2 ? 3 - v : v;
}

/* guest continuations that receive a narrow result: address -> kind. Lock-free
 * reads; writers hold cont_lock (hao/gmap.h). */
static hao_gmap conts = HAO_GMAP_INIT(2, 1, 8, 24);
static pthread_mutex_t cont_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t genres_lock = PTHREAD_MUTEX_INITIALIZER;   /* native generic results, below */
static pthread_mutex_t sitepack_lock = PTHREAD_MUTEX_INITIALIZER; /* packed stack arguments from call sites, below */
void hao_swiftcc_fork_child(void)
{
    cont_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    genres_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    sitepack_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

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

/*
 * Native generic results (hao_swift_genres_*).
 *
 * A native generic function the guest calls through its unspecialized entry
 * point makes its result in the guest's storage with the native witnesses of
 * the result type, so an outer nil there is in the x86_64 encoding. When the
 * result is `Optional<G>`, G a generic parameter of Self's type, and G's own
 * inhabitants come from a pointer word (`C?`, `(Int, C?)`), the Optional's nil
 * is G's first extra inhabitant: 2 on x86_64, 1 on arm64 (the low bit is the
 * ObjC tagged-pointer bit there). Guest code specialized for G reads it
 * inline, takes 2 for a live object and retains it: the end of a
 * `for try await` over ThrowingTaskGroup<(offset: Int, node: FINode?), any
 * Error> in Finder on 26B131 faulted in objc_retain(0x2).
 *
 * A binding qualifies by its demangled name: a method or getter of a type
 * ("Module.Type[.Inner].name(params) [async] [throws[(E)]] -> G?" or
 * "Module.Type.name.getter : G?") whose result is spelled as an Optional of a
 * depth-0 generic parameter (A..L). Self's metadata is found among the call's
 * argument registers and x20 (Swift passes it after the formal arguments, or
 * as self of a static method): struct or enum metadata of a generic native type
 * whose name without generic arguments is the entity's context. Its position
 * is fixed by the function's signature, so the first call finds it for all.
 * The result type is G? instantiated in that context with Self's generic
 * arguments (swift_getTypeByMangledNameInContext2), the result address is x8
 * (synchronous) or x0 (async: the first indirect result is the first
 * argument). The result is converted when the call returns, or, for an async
 * function, when the native callee resumes the guest's continuation without an
 * error: its ResumeParent is replaced for that invocation by
 * genres_resume[_throwing], which restores it, converts, and tail-calls it.
 *
 * A module-level generic function ("Module.name<A, B>(B, A.Type) -> (A?, B)",
 * synchronous, no where clause) qualifies when each formal parameter takes
 * one register (a generic parameter, its metatype, Optional or inout: by
 * address or a metadata pointer) and its result is a generic parameter or
 * its Optional, in x8, or a tuple of them, which Swift returns as one
 * indirect result per element passed ahead of the formal arguments (x0..).
 * The generic parameters' metadata follows the formal arguments, in order
 * (measured: a metatype argument does not stand in for it). The result
 * type is the first type of the function type in the symbol, after the
 * name and its argument labels; its end is found once, as the longest
 * prefix the native runtime resolves in a generic environment of those
 * parameters with the call's metadata (the parameters' type follows, and
 * a type with another after it does not resolve). The values native code
 * made in the guest's storage are then walked like a nil a witness makes
 * (swiftvalue.c hao_swift_native_result_to_guest), a tuple's element by
 * element at its own address, each by its own type.
 */
#include "hao/swiftvalue.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>

typedef struct genres {
    uint64_t native;
    char *context;          /* the entity's context, "Swift.ThrowingTaskGroup.Iterator" */
    char result[8];         /* G? mangled: "xSg", "q_Sg", "q0_Sg", ... */
    uint8_t is_async, throwing;
    int8_t self_reg;        /* -2 not found yet, -1 none, 0-7 x0-x7, 20 x20 */
    /* a module-level generic function: its formal parameters' registers and generic parameters (ngen 0: a method) */
    uint8_t nformal, ngen;
    uint8_t nres;           /* indirect results ahead of the formal arguments (a tuple's elements), 0: one in x8 */
    char *sym;              /* its symbol (a module-level function's) */
    const char *type_start; /* the function type in it */
    int16_t result_len;     /* the result's mangling there: 0 not found yet, -1 none */
    struct genres *next;
} genres;
typedef struct genres_type { const struct genres *fn; uint64_t self; const void *result; struct genres_type *next; } genres_type;
static hao_gmap genres_fns = HAO_GMAP_INIT(2, 1, 4, 16);     /* native -> genres * */
static genres_type *genres_types;                            /* (function, Self metadata) -> G? metadata, NULL: not Self */

static bool genres_read(uint64_t a, void *out, size_t n)
{
    mach_vm_size_t got = 0;
    return a >= 4096 && a <= UINT64_MAX - n &&
        mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}

/* "Name(params) [async] [throws[(E)]] -> T": T, the arrow the entity's own; NULL otherwise */
static const char *genres_function_result(const char *open, bool *throwing)
{
    int depth = 0;
    const char *q = open;
    for (; *q; q++) { if (*q == '(') depth++; else if (*q == ')' && !--depth) break; }
    if (!*q) return NULL;
    q++;
    if (!strncmp(q, " async", 6)) q += 6;
    if (!strncmp(q, " throws", 7)) {
        *throwing = true;
        q += 7;
        if (*q == '(') {
            for (depth = 0; *q; q++) { if (*q == '(') depth++; else if (*q == ')' && !--depth) break; }
            if (!*q) return NULL;
            q++;
        }
    }
    return !strncmp(q, " -> ", 4) ? q + 4 : NULL;
}

/* "A?" or "Swift.Optional<A>", A..L: the parameter's index, else -1 */
static int genres_param(const char *t)
{
    if (t[0] >= 'A' && t[0] <= 'L' && t[1] == '?' && !t[2]) return t[0] - 'A';
    if (!strncmp(t, "Swift.Optional<", 15) && t[15] >= 'A' && t[15] <= 'L' && t[16] == '>' && !t[17]) return t[15] - 'A';
    return -1;
}

/* Parse a demangled entity (tests/host/bridge.c checks the spellings) */
bool hao_swift_genres_demangled(const char *d, char *context, size_t n, int *param, bool *throwing)
{
    *throwing = false;
    if (!strncmp(d, "static ", 7)) d += 7;
    if (*d == '(' || strstr(d, "(extension in ") || strstr(d, " in ")) return false;   /* extensions, local entities */
    const char *getter = strstr(d, ".getter : ");
    const char *open = strchr(d, '('), *end, *t;
    if (getter && (!open || getter < open)) {
        t = getter + 10;
        end = getter;
    } else {
        if (!open || !(t = genres_function_result(open, throwing))) return false;
        end = open;
    }
    const char *dot = NULL;   /* the '.' before the entity's name */
    for (const char *p = d; p < end; p++) if (*p == '.') dot = p;
    if (!dot || dot == d || (size_t)(dot - d) >= n) return false;
    for (const char *p = d; p < dot; p++) if (*p == '<' || *p == ' ' || *p == '(') return false;
    if ((*param = genres_param(t)) < 0) return false;
    memcpy(context, d, (size_t)(dot - d));
    context[dot - d] = 0;
    return true;
}

/* "Module.name<A, B>(p, ...) -> R": the counts of generic parameters, of
 * formal parameters (each one register: a generic parameter, its metatype,
 * Optional or inout) and of indirect results passed ahead of them (R a
 * tuple of generic parameters or their Optionals: one per element; R one
 * such type: 0, its buffer in x8). False for any other signature. */
static bool genres_generic_spelled(const char *p, const char *end, unsigned n)
{
    if (end - p >= 17 && !strncmp(p, "Swift.Optional<", 15) && p[16] == '>' && end == p + 17) p += 15, end--;
    else if (end - p == 2 && p[1] == '?') end--;
    return end == p + 1 && *p >= 'A' && *p < 'A' + (int)n;
}
bool hao_swift_genres_free_demangled(const char *d, unsigned *ngen, unsigned *nformal, unsigned *nres)
{
    const char *lt = strchr(d, '<'), *open = strchr(d, '(');
    if (!lt || !open || open < lt || strstr(d, " in ") || *d == '(' || strstr(d, "(extension in ")) return false;
    const char *dot = strchr(d, '.');
    const char *next = dot ? strchr(dot + 1, '.') : NULL;
    if (!dot || dot > lt || (next && next < lt)) return false;   /* Module.name: one dot before '<' */
    const char *gt = strchr(lt, '>');
    if (!gt || gt + 1 != open) return false;   /* no where clause: its witness tables would follow the metadata */
    unsigned n = 0;
    for (const char *p = lt + 1; p < gt; p++) {
        if (*p < 'A' || *p > 'L' || (p[1] != ',' && p + 1 != gt)) return false;
        n++;
        p++;
        if (*p == ',') p++;   /* ", " */
    }
    bool throwing = false;
    const char *r = n && n <= 8 ? genres_function_result(open, &throwing) : NULL;
    if (!r || throwing || strstr(open, ") async")) return false;
    unsigned k = 0;
    const char *p = open + 1;
    while (*p && *p != ')') {
        if (!strncmp(p, "inout ", 6)) p += 6;
        if (*p < 'A' || *p >= 'A' + (int)n) return false;
        p++;
        if (!strncmp(p, ".Type", 5)) p += 5;
        else if (*p == '?') p++;
        k++;
        if (*p == ',') { p++; if (*p == ' ') p++; }
        else if (*p != ')') return false;
    }
    if (*p != ')') return false;
    unsigned m = 0;
    size_t rl = strlen(r);
    if (*r == '(' && rl > 2 && r[rl - 1] == ')') {
        for (const char *e = r + 1; e < r + rl - 1; m++) {
            int depth = 0;
            const char *q = e;
            for (; q < r + rl - 1; q++) { if (*q == '<') depth++; else if (*q == '>') depth--; else if (*q == ',' && !depth) break; }
            if (!genres_generic_spelled(e, q, n)) return false;
            e = q < r + rl - 1 ? q + 2 : q;
        }
        if (m < 2) return false;
    } else if (!genres_generic_spelled(r, r + rl, n)) return false;
    if (m + k + n > 8) return false;
    *ngen = n; *nformal = k; *nres = m;
    return true;
}

/* The function type in SYM ("$s" module name labels type...): after the
 * module's and the name's identifiers and the labels (y, or one '_' or
 * identifier per parameter); NULL for any other spelling. */
static const char *genres_free_type(const char *sym, unsigned nformal)
{
    const char *p = sym + 3;   /* "_$s" */
    for (int id = 0; id < 2; id++) {
        if (*p < '1' || *p > '9') return NULL;   /* no word substitutions, no standard module */
        char *end;
        unsigned long n = strtoul(p, &end, 10);
        if (n > 200 || strnlen(end, n) < n) return NULL;
        p = end + n;
    }
    if (*p == 'y') return p + 1;
    for (unsigned k = 0; k < nformal; k++) {
        if (*p == '_') { p++; continue; }
        if (*p < '1' || *p > '9') return NULL;
        char *end;
        unsigned long n = strtoul(p, &end, 10);
        if (n > 200 || strnlen(end, n) < n) return NULL;
        p = end + n;
    }
    return p;
}

static void genres_mangle(char out[8], int index)
{
    /* Mangling.rst: 'x' is depth 0 index 0, 'q' INDEX for index N+1 (INDEX: '_' for 0, NATURAL '_' for N+1) */
    if (index == 0) snprintf(out, 8, "xSg");
    else if (index == 1) snprintf(out, 8, "q_Sg");
    else snprintf(out, 8, "q%d_Sg", index - 2);
}

bool hao_swift_genres_bind(uint64_t native, const char *sym, bool is_async)
{
    if (!sym || strncmp(sym, "_$s", 3) || hao_opt.no_swift_values) return false;
    /* the result, mangled first in the function type, is an Optional of a generic parameter */
    if (!strstr(sym, "xSg") && !strstr(sym, "_Sg")) return false;
    size_t len = strlen(sym);   /* a function (F), static (FZ), a getter (vg, vgZ) */
    if (len < 6 || (strcmp(sym + len - 1, "F") && strcmp(sym + len - 2, "FZ") && strcmp(sym + len - 2, "vg") &&
                    strcmp(sym + len - 3, "vgZ"))) return false;
    if (hao_gmap_get1(&genres_fns, native)) return true;
    demangle_fn dm = demangler();
    if (!dm) return false;
    char buf[2048], context[512], *d = demangle_full(dm, sym + 1, len - 1, buf, sizeof buf);
    if (!d) return false;
    int param = -1;
    bool throwing;
    unsigned ngen = 0, nformal = 0;
    const char *ftype = NULL;
    unsigned nres = 0;
    bool parsed = false;
    /* a module-level generic function first: its "Module.name<A>" would read as a method of a type Module */
    if (!is_async && strcmp(sym + len - 1, "F") == 0 && hao_swift_genres_free_demangled(d, &ngen, &nformal, &nres) &&
        (ftype = genres_free_type(sym, nformal))) {
        parsed = true;
        throwing = false;
        context[0] = 0;
    } else {
        ngen = 0;
        parsed = hao_swift_genres_demangled(d, context, sizeof context, &param, &throwing);
    }
    demangled_free(d, buf);
    if (!parsed) return false;
    genres *g = calloc(1, sizeof *g);
    if (!g || !(g->context = strdup(context))) { free(g); return false; }
    g->native = native;
    if (ngen) {
        char *own = strdup(sym);
        if (!own) { free(g->context); free(g); return false; }
        g->ngen = (uint8_t)ngen; g->nformal = (uint8_t)nformal;
        g->nres = (uint8_t)nres;
        g->sym = own;
        g->type_start = own + (ftype - sym);
    } else genres_mangle(g->result, param);
    g->is_async = is_async;
    g->throwing = throwing;
    g->self_reg = -2;
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    uint64_t *s = hao_gmap_claim1(&genres_fns, native, &fresh);
    if (s && fresh) { s[1] = (uint64_t)(uintptr_t)g; hao_gmap_publish1(&genres_fns, s, native); g = NULL; }
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    if (g) { free(g->context); free(g->sym); free(g); return s != NULL; }
    if (hao_opt.bridge_check)
        fprintf(HAO_DIAG, "hao: native generic result %s: %s (%s%s, Self %s)\n", sym, buf, is_async ? "async" : "sync",
                throwing ? ", throws" : "", context);
    return true;
}

typedef struct { const char *text; uintptr_t length; } genres_name;
typedef genres_name __attribute__((swiftcall)) genres_name_fn(const void *, bool);
typedef const void *__attribute__((swiftcall)) genres_resolve_fn(const char *, size_t, const void *, const void *const *);

/* Is M the metadata of a generic native struct or enum named CONTEXT? */
static bool genres_self(uint64_t m, const char *context)
{
    uint64_t kind, desc, vwt;
    uint32_t flags;
    if ((m & 7) || !genres_read(m, &kind, 8) || (kind != 0x200 && kind != 0x201) || !genres_read(m - 8, &vwt, 8) ||
        !genres_read(vwt, &flags, 4) || !genres_read(m + 8, &desc, 8) || (desc & 3) || !genres_read(desc, &flags, 4) ||
        ((flags & 0x1f) != 17 && (flags & 0x1f) != 18) || !(flags & 0x80) || hao_image_for_addr(desc))
        return false;
    static genres_name_fn *name;
    if (!name) name = (genres_name_fn *)dlsym(RTLD_DEFAULT, "swift_getTypeName");
    if (!name) return false;
    genres_name t = name((const void *)(uintptr_t)m, true);
    size_t w = 0, n = strlen(context);
    int depth = 0;
    for (uintptr_t i = 0; t.text && i < t.length; i++) {
        char ch = t.text[i];
        if (ch == '<') depth++;
        else if (ch == '>') depth--;
        else if (!depth) { if (w >= n || context[w] != ch) return false; w++; }
    }
    return t.text && !depth && w == n;
}

/* G? in Self's context, or NULL when SELF is not Self's metadata; cached per (function, value) */
static const void *genres_result_type(const genres *g, uint64_t self)
{
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    const void *r = NULL;
    bool known = false;
    unsigned n = 0;
    for (genres_type *e = genres_types; e; e = e->next, n++)
        if (e->fn == g && e->self == self) { r = e->result; known = true; break; }
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    if (known) return r;
    if (genres_self(self, g->context)) {
        static genres_resolve_fn *resolve;
        if (!resolve) resolve = (genres_resolve_fn *)dlsym(RTLD_DEFAULT, "swift_getTypeByMangledNameInContext2");
        uint64_t desc = *(const uint64_t *)(uintptr_t)(self + 8);
        r = resolve ? resolve(g->result, strlen(g->result), (const void *)(uintptr_t)desc, (const void *const *)(uintptr_t)(self + 16))
                    : NULL;
    }
    if (n >= 4096) return r;   /* bounded: further values are checked each call */
    genres_type *e = malloc(sizeof *e);
    if (!e) return r;
    e->fn = g; e->self = self; e->result = r;
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    e->next = genres_types; genres_types = e;
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    return r;
}

static void genres_convert(const void *type, uint64_t result, const char *how)
{
    if (hao_swift_native_result_to_guest(type, (void *)(uintptr_t)result)) {
        if (hao_opt.bridge_check) fprintf(HAO_DIAG, "hao: native generic result nil to guest (%s) at %#llx\n", how, (unsigned long long)result);
    }
}

/* async: invocations waiting for their native callee to resume the guest */
typedef struct context_hdr { void *parent; void *resume; } context_hdr;
typedef void __attribute__((swiftasynccall)) genres_resume_fn(context_hdr *__attribute__((swift_async_context)));
typedef void __attribute__((swiftasynccall)) genres_throwing_fn(context_hdr *__attribute__((swift_async_context)),
                                                                 void *__attribute__((swift_context)));
typedef struct genres_wait { context_hdr *ctx; void *resume; const void *type; uint64_t result; struct genres_wait *next; } genres_wait;
static genres_wait *genres_waiting;
static genres_resume_fn genres_resume;
static genres_throwing_fn genres_resume_throwing;

static void *genres_take(context_hdr *ctx, bool error)
{
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    genres_wait **at = &genres_waiting;
    while (*at && (*at)->ctx != ctx) at = &(*at)->next;
    genres_wait *w = *at;
    if (w) { *at = w->next; ctx->resume = w->resume; }
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    if (!w) { fprintf(HAO_DIAG, "hao: native generic result: resume of %p has no invocation\n", (void *)ctx); abort(); }
    void *resume = w->resume;
    if (!error) genres_convert(w->type, w->result, "async");
    free(w);
    return resume;
}

static void __attribute__((swiftasynccall)) genres_resume(context_hdr *__attribute__((swift_async_context)) ctx)
{
    genres_resume_fn *r = (genres_resume_fn *)genres_take(ctx, false);
    __attribute__((musttail)) return r(ctx);
}

static void __attribute__((swiftasynccall)) genres_resume_throwing(context_hdr *__attribute__((swift_async_context)) ctx,
                                                                   void *__attribute__((swift_context)) error)
{
    genres_throwing_fn *r = (genres_throwing_fn *)genres_take(ctx, error != NULL);
    __attribute__((musttail)) return r(ctx, error);
}

/* A generic environment of N depth-0 parameters without requirements
 * (TargetGenericEnvironment, swift/ABI/Metadata.h): flags (levels, and
 * requirements << 12), the parameter count of each level, a descriptor per
 * parameter (0x80: it has a key argument). */
typedef struct { uint32_t flags; uint16_t counts[1]; uint8_t params[8]; } genres_env;
typedef const void *__attribute__((swiftcall)) genres_env_fn(const char *, size_t, const void *, const void *const *);

/* Is M readable metadata (a kind or a class's isa) of a native type? */
static bool genres_metadata(uint64_t m)
{
    uint64_t kind, vwt;
    return m && !(m & 7) && genres_read(m, &kind, 8) && genres_read(m - 8, &vwt, 8) && vwt && !(vwt & 7);
}

/* A module-level function's result type for the call's generic arguments
 * (after its indirect results and formal arguments); its mangling's end
 * found at the first call (above). */
static const void *genres_free_result(genres *g, const hao_cpu *c)
{
    const void *args[8];
    for (unsigned k = 0; k < g->ngen; k++) {
        args[k] = (const void *)(uintptr_t)c->x[g->nres + g->nformal + k];
        if (!genres_metadata((uint64_t)(uintptr_t)args[k])) return NULL;
    }
    static genres_env_fn *by_name;
    if (!by_name) by_name = (genres_env_fn *)dlsym(RTLD_DEFAULT, "swift_getTypeByMangledNameInEnvironment");
    if (!by_name) return NULL;
    genres_env env = {1, {g->ngen}, {0}};
    for (unsigned k = 0; k < g->ngen; k++) env.params[k] = 0x80;
    int len = __atomic_load_n(&g->result_len, __ATOMIC_ACQUIRE);
    if (len < 0) return NULL;
    if (!len) {
        size_t avail = strlen(g->type_start);
        for (size_t n = avail > 256 ? 256 : avail; n > 0 && !len; n--)
            if (!memchr(g->type_start, 'Q', n) && by_name(g->type_start, n, &env, args)) len = (int)n;
        if (!len) len = -1;
        __atomic_store_n(&g->result_len, (int16_t)len, __ATOMIC_RELEASE);
        if (hao_opt.bridge_check)
            fprintf(HAO_DIAG, "hao: native generic result %s: result mangling %.*s\n", g->sym, len > 0 ? len : 4,
                    len > 0 ? g->type_start : "none");
        if (len < 0) return NULL;
    }
    return by_name(g->type_start, (size_t)len, &env, args);
}

/* A guest call of NATIVE with the guest's registers: 0, or the result address
 * to convert with *TYPE when the call returns (synchronous). An async call's
 * conversion is set up here and happens at its resume. */
uint64_t hao_swift_genres_call(const hao_cpu *c, uint64_t native, const void **type)
{
    const uint64_t *e = hao_gmap_get1(&genres_fns, native);
    if (!e) return 0;
    genres *g = (genres *)(uintptr_t)e[1];
    if (g->ngen) {
        const void *t = genres_free_result(g, c);
        if (!t) return 0;
        *type = t;
        if (!g->nres) return c->x[8];
        /* a tuple's elements' buffers, x0.. before the call: tagged (bit 0) for hao_swift_genres_result */
        uint64_t *at = malloc(sizeof *at * (1 + g->nres));
        if (!at) return 0;
        at[0] = g->nres;
        for (unsigned k = 0; k < g->nres; k++) at[1 + k] = c->x[k];
        return (uint64_t)(uintptr_t)at | 1;
    }
    int reg = __atomic_load_n(&g->self_reg, __ATOMIC_ACQUIRE);
    if (reg == -1) return 0;
    if (reg == -2) {
        static const int8_t regs[] = {0, 1, 2, 3, 4, 5, 6, 7, 20};
        int found = -1;
        for (unsigned k = 0; k < sizeof regs && found < 0; k++)
            if (!(g->is_async && regs[k] == 0) && genres_self(c->x[regs[k]], g->context)) found = regs[k];
        __atomic_store_n(&g->self_reg, (int8_t)found, __ATOMIC_RELEASE);
        if (hao_opt.bridge_check)
            fprintf(HAO_DIAG, "hao: native generic result %s: Self metadata %s%d\n", g->context, found < 0 ? "not found, x" : "x", found);
        if (found < 0) return 0;
        reg = found;
    }
    const void *t = genres_result_type(g, c->x[reg]);
    if (!t) return 0;
    if (!g->is_async) { *type = t; return c->x[8]; }
    context_hdr *ctx = (context_hdr *)(uintptr_t)c->x[22];
    if (!ctx || ((uintptr_t)ctx & 7) || !c->x[0]) return 0;
    genres_wait *w = malloc(sizeof *w);
    if (!w) return 0;
    w->ctx = ctx; w->type = t; w->result = c->x[0];
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    w->resume = ctx->resume;
    for (genres_wait **at = &genres_waiting; *at; at = &(*at)->next)
        if ((*at)->ctx == ctx) {   /* a context reused before its resume: keep the original continuation */
            genres_wait *old = *at;
            *at = old->next;
            w->resume = old->resume;
            free(old);
            break;
        }
    ctx->resume = g->throwing ? (void *)genres_resume_throwing : (void *)genres_resume;
    w->next = genres_waiting; genres_waiting = w;
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    return 0;
}

void hao_swift_genres_result(const void *type, uint64_t result)
{
    if (!(result & 1)) { genres_convert(type, result, "sync"); return; }
    /* a tuple returned as one indirect result per element: each element by its own type
     * (TupleTypeMetadata: kind, NumElements, Labels, then {Type, Offset} per element) */
    uint64_t *at = (uint64_t *)(uintptr_t)(result & ~1ull);
    const uint64_t *m = type;
    if (m[0] == 0x301 && m[1] == at[0])
        for (uint64_t k = 0; k < at[0]; k++) genres_convert((const void *)(uintptr_t)m[3 + 2 * k], at[1 + k], "sync, tuple element");
    free(at);
}

/*
 * Native View modifiers (hao_swift_viewself_*).
 *
 * A native protocol extension method of SwiftUI.View ("(extension in M):
 * SwiftUI.View.sheet(isPresented:onDismiss:content:)", and every other
 * modifier) takes self indirectly in x20, and Self's metadata and its
 * Self: View witness table among the generic arguments after the formal
 * ones. It copies self with Self's native value witnesses. Guest code
 * specialized for Self wrote self inline, so a nil whose inhabitants come
 * from a pointer word is in arm64's encoding: the outer nil of `AnyView??`
 * (an `if` around an `AnyView?` in a ViewBuilder) is inhabitant 1, the
 * value 1, where x86_64's witnesses expect 2 (the low bit is the ObjC
 * tagged-pointer bit there). The copy takes 1 for a live AnyView and
 * swift_retain faults at 0x8 (the Bluetooth Settings pane on 26B131:
 * sheet(isPresented:) on an HStack<TupleView<(AnyView??, Spacer)>>).
 *
 * A binding qualifies by its mangled prefix and demangled name. Self is
 * found at the first call: the argument word (x0-x7, then the first eight
 * stack words) holding struct, enum or Optional metadata, followed by a
 * witness table whose conformance descriptor is SwiftUI.View's conformance
 * of that type. The positions are the signature's, so they hold for every
 * call. The closures self holds get Swift entries (hao_swift_value_closures:
 * native code calls them later), and self of a native type is then lent as a
 * converted copy (swiftvalue.c hao_swift_borrow_to_native).
 */
typedef struct viewself { int8_t self_at, wt_at; } viewself;   /* argument word index; -2 not found yet, -1 none */
static hao_gmap viewself_fns = HAO_GMAP_INIT(2, 1, 4, 16);     /* native -> viewself * */
static uint64_t view_protocol;                                 /* SwiftUI.View's protocol descriptor */

bool hao_swift_viewself_bind(uint64_t native, const char *sym)
{
    if (!sym || strncmp(sym, "_$s7SwiftUI4ViewP", 17) || hao_opt.no_swift_values) return false;
    if (hao_gmap_get1(&viewself_fns, native)) return true;
    demangle_fn dm = demangler();
    if (!dm) return false;
    char buf[2048], *d = demangle_full(dm, sym + 1, strlen(sym + 1), buf, sizeof buf);
    if (!d) return false;
    const char *close = strncmp(d, "(extension in ", 14) ? NULL : strchr(d, ')');
    bool member = close && !strncmp(close, "):SwiftUI.View.", 15);   /* an instance member: self is a value */
    demangled_free(d, buf);
    if (!member) return false;
    if (!view_protocol) {
        uint64_t p = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, "$s7SwiftUI4ViewMp");
        if (!p) return false;
        __atomic_store_n(&view_protocol, p, __ATOMIC_RELEASE);
    }
    viewself *v = malloc(sizeof *v);
    if (!v) return false;
    v->self_at = v->wt_at = -2;
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    uint64_t *s = hao_gmap_claim1(&viewself_fns, native, &fresh);
    if (s && fresh) { s[1] = (uint64_t)(uintptr_t)v; hao_gmap_publish1(&viewself_fns, s, native); v = NULL; }
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    free(v);
    if (hao_opt.bridge_check) fprintf(HAO_DIAG, "hao: native View modifier %s: %s\n", sym, buf);
    return s != NULL;
}

/* M: metadata of a struct, enum or Optional; *desc its nominal type descriptor */
static bool viewself_meta(uint64_t m, uint64_t *desc)
{
    uint64_t kind;
    if (!m || (m & 7) || !genres_read(m, &kind, 8) || (kind != 0x200 && kind != 0x201 && kind != 0x202)) return false;
    return genres_read(m + 8, desc, 8) && *desc && !(*desc & 3);
}

/* WT: a witness table of the conformance to SwiftUI.View of the type DESC describes */
static bool viewself_conforms(uint64_t wt, uint64_t desc)
{
    uint64_t cd, proto, tref;
    int32_t f[4];
    if (!wt || (wt & 7) || !genres_read(wt, &cd, 8) || !cd || (cd & 3) || !genres_read(cd, f, sizeof f)) return false;
    proto = cd + (uint64_t)(int64_t)(f[0] & ~1);                 /* RelativeContextPointer: bit 0 indirect */
    if ((f[0] & 1) && !genres_read(proto, &proto, 8)) return false;
    if (proto != __atomic_load_n(&view_protocol, __ATOMIC_ACQUIRE)) return false;
    tref = cd + 4 + (uint64_t)(int64_t)f[1];
    switch (((uint32_t)f[3] >> 3) & 7) {                         /* TypeReferenceKind */
    case 0: break;                                               /* DirectTypeDescriptor */
    case 1: if (!genres_read(tref, &tref, 8)) return false; break;   /* IndirectTypeDescriptor */
    default: return false;
    }
    return tref == desc;
}

/*
 * The Body a guest body returns (hao_swift_body_result).
 *
 * SwiftUI's protocols with a body (View, ViewModifier, Scene, Commands, ...)
 * have native code call the conforming type's body witness, which returns a
 * Body in native memory (the indirect result). Closures in it are called
 * later by native code: a GeometryReader's content made in the body, a guest
 * View's @ViewBuilder property. They get Swift entries
 * (hao_swift_value_closures), Body's metadata asked of the witness table as
 * native code asks it (swift_getAssociatedTypeWitness). A witness's last two
 * arguments are Self's metadata and the witness table; the table is a
 * conformance to a protocol whose body requirement's slot holds the callee's
 * entry. The requirement is the protocol's exported method descriptor of
 * `body` returning its associated type Body: the getter's symbol, made from
 * the protocol's module and name, or ViewModifier's body(content:). Body's
 * own requirement is the associated type access function whose name in the
 * protocol's list is "Body". Which argument is the table is found at a
 * callee's first call and remembered per address.
 */
typedef struct { uint64_t m; uint32_t state; } assoc_response;
typedef assoc_response __attribute__((swiftcall)) assoc_witness_fn(uint64_t, const void *, const void *, const void *, const void *);
typedef struct { uint64_t proto, reqbase, body, type; } body_proto;   /* body 0: none */
/* protocol descriptor -> its body_proto (the entry's words are one), once per
 * protocol: any witness table argument of a callee is looked at, so a process
 * meets many protocols; a full table would compute (dlsym) on every call. */
static hao_gmap body_protos = HAO_GMAP_INIT(4, 1, 6, 16);
static assoc_witness_fn *body_assoc;

/* N bytes of the NUL-terminated string at A into out (false if longer or unreadable) */
static bool body_string(uint64_t a, char *out, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (!genres_read(a + k, &out[k], 1)) return false;
        if (!out[k]) return true;
    }
    return false;
}

/* a relative pointer at A (bit 0: indirect, when INDIRECT) */
static bool body_rel(uint64_t a, bool indirect, uint64_t *out)
{
    int32_t r;
    if (!genres_read(a, &r, 4) || !r) return false;
    *out = a + (uint64_t)(int64_t)(indirect ? r & ~1 : r);
    return !(indirect && (r & 1)) || genres_read(*out, out, 8);
}

/* false: PROTO is no protocol descriptor in a module (any word of a callee's
 * arguments is looked at); otherwise BP's body is 0 where the protocol has none */
static bool body_proto_compute(uint64_t proto, body_proto *bp)
{
    uint32_t h[6];   /* flags, parent, name, requirements in signature, requirements, associated type names */
    uint64_t parent, pname, name, names;
    char mod[64], nm[64], assoc[256], sym[256];
    bp->proto = proto;
    if (!genres_read(proto, h, sizeof h) || (h[0] & 0x1f) != 3 || h[4] > 256) return false;   /* a protocol */
    if (!body_rel(proto + 4, true, &parent) || !genres_read(parent, h, 4) || (h[0] & 0x1f) != 0) return false;   /* a module */
    if (!body_rel(parent + 8, false, &pname) || !body_string(pname, mod, sizeof mod)) return true;
    if (!body_rel(proto + 8, false, &name) || !body_string(name, nm, sizeof nm)) return true;
    if (!body_rel(proto + 20, false, &names) || !body_string(names, assoc, sizeof assoc)) return true;
    uint32_t nsig, nreq;
    genres_read(proto + 12, &nsig, 4);
    genres_read(proto + 16, &nreq, 4);
    uint64_t req = proto + 24 + 12 * (uint64_t)nsig;
    snprintf(sym, sizeof sym, "$s%zu%s%zu%sP4body4BodyQzvgTq", strlen(mod), mod, strlen(nm), nm);
    uint64_t body = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, sym);
    if (!body) {
        uint64_t vm = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, "$s7SwiftUI12ViewModifierP4body7content4BodyQzAA01_cD8_ContentVyxG_tFTq");
        if (vm >= req && vm < req + 8 * (uint64_t)nreq) body = vm;
    }
    if (body < req || body >= req + 8 * (uint64_t)nreq || (body - req) % 8) return true;
    /* Body's position among the associated type names, then that associated type access function */
    int at = -1, k = 0;
    for (char *save = NULL, *w = strtok_r(assoc, " ", &save); w; w = strtok_r(NULL, " ", &save), k++) if (!strcmp(w, "Body")) at = k;
    if (at < 0) return true;
    for (uint32_t i = 0, j = 0; i < nreq; i++) {
        uint32_t f;
        if (!genres_read(req + 8 * (uint64_t)i, &f, 4)) return true;
        if ((f & 0xf) == 7 && (int)j++ == at) { bp->type = req + 8 * (uint64_t)i; break; }
    }
    if (!bp->type) return true;
    bp->reqbase = req - 8;   /* the witness table's slot of requirement k: 1 + k (0: the conformance descriptor) */
    bp->body = body;
    return true;
}

/* the body requirement of the protocol at PROTO, computed once per protocol (NULL: no protocol, or the table is full) */
static const body_proto *body_proto_of(uint64_t proto)
{
    _Static_assert(sizeof(body_proto) == 4 * sizeof(uint64_t) && offsetof(body_proto, proto) == 0, "gmap entry");
    const uint64_t *e = hao_gmap_get1(&body_protos, proto);
    if (e) return (const body_proto *)(const void *)e;
    body_proto bp = {0};
    if (!body_proto_compute(proto, &bp)) return NULL;   /* not remembered: rejected by its first reads */
    hao_crit_enter();
    pthread_mutex_lock(&genres_lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&body_protos, proto, &fresh);
    if (slot && fresh) {
        slot[1] = bp.reqbase; slot[2] = bp.body; slot[3] = bp.type;
        hao_gmap_publish1(&body_protos, slot, proto);
    }
    pthread_mutex_unlock(&genres_lock);
    hao_crit_leave();
    return slot ? (const body_proto *)(const void *)slot : NULL;
}

/* WT: a witness table whose protocol's body requirement slot holds an entry of TARGET */
static const body_proto *body_witness(uint64_t target, uint64_t wt)
{
    uint64_t cd, proto, slot;
    if (!wt || (wt & 7) || !genres_read(wt, &cd, 8) || !cd || (cd & 3) || !body_rel(cd, true, &proto) || !proto) return NULL;
    const body_proto *bp = body_proto_of(proto);
    if (!bp || !bp->body || !genres_read(wt + (bp->body - bp->reqbase), &slot, 8) || !slot) return NULL;
    uint64_t g = hao_guest_of_entry(slot);
    return (g ? g : slot) == target ? bp : NULL;
}

void hao_swift_body_result(uint64_t target, const uint64_t *a, uint64_t value)
{
    enum { N = 2048 };
    static uint64_t memo[N];   /* target << 8 | witness table argument (0: none) */
    if (!value || hao_opt.no_swift_thunks || hao_current()->sig_nest || target >> 56) return;
    if (!body_assoc) {
        void *f = dlsym(RTLD_DEFAULT, "swift_getAssociatedTypeWitness");
        if (!f) return;
        __atomic_store_n(&body_assoc, (assoc_witness_fn *)f, __ATOMIC_RELAXED);
    }
    uint64_t *e = &memo[(target >> 2) & (N - 1)], v = __atomic_load_n(e, __ATOMIC_RELAXED);
    const body_proto *bp = NULL;
    int k = v >> 8 == target ? (int)(v & 0xff) : -1;
    if (k == 0) return;
    if (k > 0) bp = body_witness(target, a[k]);
    else {
        for (k = 1; k < 6 && !(bp = body_witness(target, a[k])); k++) { }
        __atomic_store_n(e, target << 8 | (bp ? (unsigned)k : 0), __ATOMIC_RELAXED);
    }
    if (!bp || !a[k - 1] || (a[k - 1] & 7)) return;
    assoc_response r = body_assoc(0, (const void *)(uintptr_t)a[k], (const void *)(uintptr_t)a[k - 1],
                                  (const void *)(uintptr_t)bp->reqbase, (const void *)(uintptr_t)bp->type);
    if (r.m) hao_swift_value_closures(r.m, value);
}

static uint64_t viewself_word(const hao_cpu *c, int k)
{
    uint64_t w = 0;
    if (k < 8) return c->x[k];
    return genres_read(c->x[31] + 8 * (uint64_t)(k - 8), &w, 8) ? w : 0;
}

/* A guest call of NATIVE with the guest's registers: NULL, or a converted copy
 * of self to pass in x20 instead (free it after the call) */
void *hao_swift_viewself_call(const hao_cpu *c, uint64_t native)
{
    const uint64_t *e = hao_gmap_get1(&viewself_fns, native);
    if (!e || !c->x[20] || (c->x[20] & 7)) return NULL;
    viewself *v = (viewself *)(uintptr_t)e[1];
    int at = __atomic_load_n(&v->self_at, __ATOMIC_ACQUIRE);
    if (at == -1) return NULL;
    uint64_t m = 0, desc;
    if (at == -2) {
        int found = -1, wt = -1;
        for (int i = 0; i < 16 && found < 0; i++) {
            m = viewself_word(c, i);
            if (!viewself_meta(m, &desc)) continue;
            for (int j = i + 1; j < 16 && found < 0; j++)
                if (viewself_conforms(viewself_word(c, j), desc)) { found = i; wt = j; }
        }
        v->wt_at = (int8_t)wt;
        __atomic_store_n(&v->self_at, (int8_t)found, __ATOMIC_RELEASE);
        if (hao_opt.bridge_check)
            fprintf(HAO_DIAG, "hao: native View modifier %#llx: Self metadata %s%d, Self: View at %d\n",
                    (unsigned long long)native, found < 0 ? "not found " : "word ", found, wt);
        if (found < 0) return NULL;
        at = found;
    }
    m = viewself_word(c, at);
    if (!viewself_meta(m, &desc)) return NULL;
    hao_swift_value_closures(m, c->x[20]);   /* closures in self native code calls later get Swift entries */
    if (hao_image_for_addr(desc)) return NULL;   /* the guest's own type: its witnesses are the guest's */
    return hao_swift_borrow_to_native((const void *)(uintptr_t)m, (const void *)(uintptr_t)c->x[20]);
}

/* ------------------------------------------- packed stack arguments, from the call site
 *
 * Apple arm64 passes the arguments past x7 at their natural size and alignment
 * (two Bools take [sp] and [sp+1]); x86_64 SysV gives each its own 8-byte
 * slot. hao_native_call copies the guest's stack words to SysV slots 2
 * onward, which is right only while every stack argument starts at a multiple
 * of 8. An interface-unit record of kind K gives a native Swift function's
 * layout; a function without one (no unit, or one the generator left UNKNOWN)
 * got the word copy, so its later stack arguments came from the wrong words:
 * findmylocateagent on 26B131 calls FindMyLocate's Friend.init with two Bools
 * at [sp] and [sp+1] and the addresses of an indirect Handle? and
 * LocationSharingState? at [sp+8] and [sp+16]; x86_64 took the state's
 * address for the owner Handle's and copied 176 bytes of guest stack as a
 * Handle, which the friend list's copy then retained.
 *
 * The guest's call site holds the layout. LLVM lowers a call's outgoing
 * arguments as stores to [sp, #off] inside the call sequence, after every
 * argument value is computed and after the sequence's own `sub sp`, so they
 * lie between the call and the nearest earlier branch, call, function start
 * or write of sp, each with its argument's width. A store is not always one
 * argument: LLVM merges adjacent constant arguments into one wider store (two
 * constant Bools become `strh` of 0x101), and adjacent values loaded from
 * adjacent memory the same way; such a layout has one argument too few and
 * shifts every later one. So the layout is checked against the native callee
 * (hao/x86args.h): its frame-pointer reads say how many SysV stack slots it
 * takes, K + 2 (slots 0 and 1 carry x6 and x7). A layout is taken only when
 * the K lowest stores prove one: general-register stores at naturally aligned
 * offsets with widths 1, 2, 4 or 8, none overlapping, contiguous from 0 up to
 * alignment padding, at least one at an offset that is not 8 times its
 * position (otherwise the word copy is already right), the callee reading
 * every one of the K slots and none narrower than the store (but see the
 * split below). Stores above the K arguments are the caller's locals (a
 * `str q0` spill, the frame record) and are not looked at. Fewer than K
 * arguments from the stores (an argument stored outside the window, a merged
 * store the split below does not take), a SIMD/FP store among them (FP stack
 * arguments are placed differently), a store through a register offset or a
 * hole leaves the word copy. When the callee's code does not show its slots
 * (no frame-pointer prologue, an instruction the decoder does not know, its
 * image's function starts missing), every store must be an argument and every
 * store wider than a byte must hold an address computed in the window (sp-
 * or x29-relative, ADR/ADRP), which no merge of narrower arguments is.
 *
 * A store wider than the callee's read of its first slot is taken as several
 * arguments only when the callee's read widths, each placed at its natural
 * alignment, fill it exactly: four constant Int32 zeros stored by one `stp
 * xzr, xzr` that the callee reads as 4-byte slots (AppSubscriptions
 * UserSubscriptionContextData.init: the word copy gave its OfferType? the
 * address of a stale stack word, and native JSON encoding of the value
 * jumped through the enum's rawValue table with a garbage tag). Such stores
 * can all lie at multiples of 8, so the callee is read whenever it takes
 * more stack arguments than there are stores. Anything else about such a
 * store (a read width not known, a remainder, fewer than two arguments)
 * leaves the word copy; a callee that reads only part of a wider argument
 * (a truncating conversion) beside a narrow one could be split wrongly,
 * which no case seen does. The store that allocates the outgoing area
 * (`stp xzr, xzr, [sp, #-32]!`, when the caller's frame has no reserved call
 * area) is the window's last instruction, its offsets relative to the sp it
 * writes.
 *
 * The stores describe the native function only when the instruction before
 * lr called it: a BL to a stub whose slot holds the native address, or a
 * BLR/BLRAA whose register still holds it at the call (a guest function that
 * tail-calls the native one reaches it with its caller's lr, whose stores are
 * the guest function's arguments). The answer is kept per (call site, native
 * function), in one word: nearly every call site has none, and a daemon's
 * memory limit counts the table; the layouts are few and kept apart. An
 * indirect call's register is checked on every call.
 *
 * A call with many arguments has a long call sequence and a wide area: a
 * native initializer of 42 stack arguments (Strings, eleven Bools, indirect
 * Date? addresses, the last at [sp+256]) is stored over 104 instructions, and
 * its x86_64 callee reads 44 slots. A 64-instruction window, 32 arguments,
 * 40 callee slots and 8-bit offsets each left the word copy, so the callee
 * took a stale word for an indirect Date?'s address and copied from 0. */
#define SITEPACK_WINDOW 128
#define SITEPACK_DIRECT 0xff
#define SITEPACK_LAYOUTS 256
static hao_gmap sitepacks = HAO_GMAP_INIT(3, 2, 6, 20);   /* (lr, native) -> n | register << 8 | (layout + 1) << 16 */
static struct { uint16_t off[HAO_SITEPACK_MAX]; uint8_t w[HAO_SITEPACK_MAX]; } sitepack_layouts[SITEPACK_LAYOUTS];   /* under sitepack_lock */
static unsigned sitepack_nlayouts;
static inline uint64_t untag(uint64_t a) { return a & 0x00007fffffffffffull; }

/* The address a stub at T branches to: ADRP, ADD, LDR (64-bit, unsigned offset) of x16/x17, then BR or
 * BRAA/BRAB; 0 for anything else. */
static uint64_t stub_target(uint64_t t)
{
    hao_image *im = hao_image_for_code(t);
    uint64_t r[32] = {0};
    uint32_t known = 0;
    for (int k = 0; im && k < 6; k++) {
        uint64_t a = t + 4 * (uint64_t)k;
        if (hao_image_for_code(a) != im) return 0;
        uint32_t i = *(const uint32_t *)hao_code_ptr_im(im, a);
        unsigned rd = i & 31, rn = (i >> 5) & 31;
        if ((i & 0x9F000000) == 0x90000000) {                     /* ADRP */
            uint64_t imm = ((uint64_t)((i >> 29) & 3) | (uint64_t)((i >> 5) & 0x7ffff) << 2) << 12;
            r[rd] = (a & ~0xfffull) + (uint64_t)sx(imm, 33);
            known |= bit(rd);
        } else if ((i & 0xFF800000) == 0x91000000 && rd != 31 && rn != 31 && (known & bit(rn))) {   /* ADD Xd, Xn, #imm */
            r[rd] = r[rn] + ((uint64_t)((i >> 10) & 0xfff) << (((i >> 22) & 1) ? 12 : 0));
            known |= bit(rd);
        } else if ((i & 0xFFC00000) == 0xF9400000 && rn != 31 && (known & bit(rn))) {   /* LDR Xt, [Xn, #imm] */
            if (!genres_read(r[rn] + (((i >> 10) & 0xfff) << 3), &r[rd], 8)) return 0;
            known |= bit(rd);
        } else if ((i & 0xFFFFFC1F) == 0xD61F0000 || (i & 0xFEFFF800) == 0xD61F0800)   /* BR, BRAA/BRAB(Z) */
            return (known & bit(rn)) ? untag(r[rn]) : 0;
        else return 0;
    }
    return 0;
}

/* How the instruction at CALL reaches NATIVE: SITEPACK_DIRECT (a BL to a stub that branches to it), the
 * register of an indirect call (checked per call), or -1 (not a call of it). */
static int sitepack_call(uint64_t call, uint64_t native)
{
    hao_image *im = hao_image_for_code(call);
    if (!im) return -1;
    uint32_t i = *(const uint32_t *)hao_code_ptr_im(im, call);
    if ((i & 0xFC000000) == 0x94000000)                           /* BL */
        return stub_target(call + ((uint64_t)sx(i & 0x3ffffff, 26) << 2)) == untag(native) ? SITEPACK_DIRECT : -1;
    if ((i & 0xFFFFFC1F) == 0xD63F0000 || (i & 0xFEFFF800) == 0xD63F0800)   /* BLR, BLRAA/BLRAB(Z) */
        return (int)((i >> 5) & 31);
    return -1;
}

/* ---- the native callee's stack-argument slots, kept per function */
#define SITEPACK_SKIPS 32
#define SITEPACK_CODE_MAX (256u << 10)
/* native -> 1 | shown << 1 | nslots << 8, then the slots' widths (HAO_X86_MAX_SLOTS bytes) */
static hao_gmap callee_slots = HAO_GMAP_INIT(2 + (HAO_X86_MAX_SLOTS + 7) / 8, 1, 4, 14);

/* The length of the x86_64 function at FN, up to the next entry of its image's LC_FUNCTION_STARTS
 * (FN must be one) inside its executable __TEXT, and its LC_DATA_IN_CODE ranges as offsets from FN;
 * 0 when they are not known. */
static size_t native_code(uint64_t fn, uint32_t skip[2 * SITEPACK_SKIPS], int *nskip)
{
    Dl_info d;
    *nskip = 0;
    if (!fn || !dladdr((const void *)(uintptr_t)fn, &d) || !d.dli_fbase) return 0;
    const struct mach_header_64 *mh = d.dli_fbase;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_X86_64) return 0;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    const struct segment_command_64 *text = NULL, *le = NULL;
    const struct linkedit_data_command *fs = NULL, *dic = NULL;
    for (uint32_t k = 0; k < mh->ncmds; k++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (!strncmp(sg->segname, "__TEXT", 16)) text = sg;
            else if (!strncmp(sg->segname, "__LINKEDIT", 16)) le = sg;
        } else if (l->cmd == LC_FUNCTION_STARTS) fs = (const void *)lc;
        else if (l->cmd == LC_DATA_IN_CODE) dic = (const void *)lc;
    }
    if (!text || !le || !fs || !(text->initprot & VM_PROT_EXECUTE)) return 0;
    uint64_t lo = (uint64_t)(uintptr_t)mh, hi = lo + text->vmsize;
    if (fn < lo || fn >= hi) return 0;
    /* linkedit data by file offset, in the shared cache as in a file */
    const uint8_t *base = (const uint8_t *)mh + (le->vmaddr - text->vmaddr) - le->fileoff;
    const uint8_t *p = base + fs->dataoff, *e = p + fs->datasize;
    uint64_t a = lo, end = 0;
    bool found = false;
    while (p < e && !end) {
        uint64_t v = 0;
        unsigned sh = 0;
        uint8_t b;
        do {
            if (p >= e || sh > 56) return 0;
            b = *p++;
            v |= (uint64_t)(b & 0x7f) << sh;
            sh += 7;
        } while (b & 0x80);
        if (!v) break;
        a += v;
        if (a == fn) found = true;
        else if (a > fn) end = a;
    }
    if (!found || !end || end > hi || end - fn > SITEPACK_CODE_MAX) return 0;
    if (dic) {
        const struct data_in_code_entry *x = (const void *)(base + dic->dataoff);
        for (uint32_t k = 0; k < dic->datasize / sizeof *x; k++) {
            uint64_t at = lo + x[k].offset;
            if (at + x[k].length <= fn || at >= end) continue;
            if (*nskip == SITEPACK_SKIPS) return 0;
            skip[2 * *nskip] = (uint32_t)(at > fn ? at - fn : 0);
            skip[2 * *nskip + 1] = (uint32_t)(at + x[k].length - fn);
            ++*nskip;
        }
    }
    return (size_t)(end - fn);
}

/* A Swift class method's dispatch thunk (`...Tj`) only jumps through the vtable, so its code shows no
 * stack argument: the implementation its exported method descriptor (`...Tq`: Flags, then a relative
 * pointer to the implementation) names takes the same arguments, as does every override. 0 if FN is
 * not such a thunk, or the method is async or has no implementation here. */
static uint64_t descriptor_impl(const char *thunk)
{
    size_t n = strlen(thunk);
    char q[1024];
    if (n < 3 || n >= sizeof q || strncmp(thunk, "$s", 2) || strcmp(thunk + n - 2, "Tj")) return 0;
    memcpy(q, thunk, n + 1);
    q[n - 1] = 'q';
    const uint8_t *d = dlsym(RTLD_DEFAULT, q);
    if (!d) return 0;
    uint32_t flags;
    int32_t rel;
    memcpy(&flags, d, 4);
    memcpy(&rel, d + 4, 4);
    if ((flags & 0x40) || !rel) return 0;
    return (uint64_t)(uintptr_t)(d + 4 + rel);
}

/* thunk -> implementation, noted when a guest import binds one by name: in the shared cache dladdr can
 * name a thunk's address after a nearby local symbol instead */
static hao_gmap dispatch_thunks = HAO_GMAP_INIT(2, 1, 4, 14);

void hao_swift_note_dispatch_thunk(const char *gname, uint64_t native)
{
    if (!gname || gname[0] != '_' || !native) return;
    uint64_t impl = descriptor_impl(gname + 1), key = untag(native);
    if (!impl) return;
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&sitepack_lock);
    uint64_t *s = hao_gmap_claim(&dispatch_thunks, &key, &fresh);
    if (s && fresh) {
        s[1] = impl;
        hao_gmap_publish(&dispatch_thunks, s, &key);
    }
    pthread_mutex_unlock(&sitepack_lock);
    hao_crit_leave();
}

static uint64_t dispatch_impl(uint64_t fn)
{
    const uint64_t *e = hao_gmap_get1(&dispatch_thunks, fn);
    if (e) return e[1];
    Dl_info di;
    if (!dladdr((const void *)(uintptr_t)fn, &di) || !di.dli_sname || (uint64_t)(uintptr_t)di.dli_saddr != fn) return 0;
    return descriptor_impl(di.dli_sname);
}

/* The stack-argument slots of the native function NATIVE (following up to three trampolines, and a
 * dispatch thunk to its implementation): false when its code does not show them (hao_x86_stackargs). */
static bool callee_stackuse(uint64_t native, hao_x86_stackuse *u)
{
    uint64_t fn = untag(native);
    const uint64_t *e = hao_gmap_get1(&callee_slots, fn);
    memset(u, 0, sizeof *u);
    if (e) {
        u->nslots = (int)(e[1] >> 8);
        memcpy(u->width, &e[2], sizeof u->width);
        return (e[1] >> 1) & 1;
    }
    bool shown = false;
    uint64_t impl = dispatch_impl(fn);
    if (impl) fn = impl;
    for (int hop = 0, t = 0; hop < 4 && !t; hop++) {
        uint32_t skip[2 * SITEPACK_SKIPS];
        int nskip;
        size_t len = native_code(fn, skip, &nskip);
        if (!len || !hao_x86_stackargs((const uint8_t *)(uintptr_t)fn, len, skip, nskip, u)) break;
        if (u->jump) fn = u->jump;
        else shown = t = 1;
    }
    if (!shown) memset(u, 0, sizeof *u);
    bool fresh;
    uint64_t key = untag(native);
    hao_crit_enter();
    pthread_mutex_lock(&sitepack_lock);
    uint64_t *s = hao_gmap_claim(&callee_slots, &key, &fresh);
    if (s && fresh) {
        s[1] = 1 | (uint64_t)shown << 1 | (uint64_t)u->nslots << 8;
        memcpy(&s[2], u->width, sizeof u->width);
        hao_gmap_publish(&callee_slots, s, &key);
    }
    pthread_mutex_unlock(&sitepack_lock);
    hao_crit_leave();
    return shown;
}

/* Whether the register a store at window position K (the instruction at call - 4K) writes holds an
 * address computed inside the window: ADR/ADRP, or a 64-bit ADD/SUB immediate of sp, x29 or an
 * ADR/ADRP result. No merge of narrower arguments is such a value. */
static bool sitepack_address(const uint32_t *win, int nwin, int k, unsigned r)
{
    for (int depth = 0; depth < 2; depth++) {
        if (r == 31) return false;                                     /* wzr/xzr: a constant */
        int d = k + 1;
        for (; d <= nwin; d++) {
            ins o;
            decode(win[d], 0, &o);
            if (o.wr & bit(r)) break;
        }
        if (d > nwin) return false;                                    /* set before the window */
        uint32_t i = win[d];
        if ((i & 0x1F000000) == 0x10000000) return true;               /* ADR, ADRP */
        if ((i & 0xBF800000) != 0x91000000) return false;              /* not ADD/SUB Xd, Xn, #imm */
        unsigned rn = (i >> 5) & 31;
        if (rn == 31 || rn == 29) return true;
        if (depth) return false;
        /* ADD Xd, Xn, #imm of an ADRP result */
        for (d++; d <= nwin; d++) {
            ins o;
            decode(win[d], 0, &o);
            if (o.wr & bit(rn)) return (win[d] & 0x9F000000) == 0x90000000;
        }
        return false;
    }
    return false;
}

/* The layout the stores of the call sequence ending at CALL give its stack arguments, as described
 * above, or 0 (word copy); *why says why a packed-looking site was refused. */
static int sitepack_scan(uint64_t call, uint64_t native, uint16_t off[HAO_SITEPACK_MAX], uint8_t width[HAO_SITEPACK_MAX],
                         const char **why)
{
    *why = NULL;
    hao_image *im = hao_image_for_code(call);
    if (!im) return 0;
    ins o;
    struct { int16_t off; uint8_t w, fp, src, k; } st[2 * SITEPACK_WINDOW];
    uint32_t win[SITEPACK_WINDOW + 1];
    int ns = 0, nwin = 0;
    for (int k = 1; k <= SITEPACK_WINDOW; k++) {
        uint64_t a = call - 4 * (uint64_t)k;
        if (hao_image_for_code(a) != im) break;
        uint32_t i = *(const uint32_t *)hao_code_ptr_im(im, a);
        decode(i, a, &o);
        /* control flow, or sp written; a store that also allocates the area (pre-index writeback of sp) is
         * the window's last instruction */
        bool alloc = o.mem && !o.load && o.base == 31 && o.wb_reg == 31 && o.wb < 0 && !((i >> 26) & 1) &&
                     !(o.rd & (bit(29) | bit(30))) && !hao_is_guest_function(a);   /* not a frame push */
        if (o.flow != F_NEXT || o.dst == 31 || (o.wb_reg == 31 && !alloc)) break;
        win[nwin = k] = i;
        if (o.mem && !o.load && o.base == 31) {
            bool pair = (i & 0x38000000) == 0x28000000, fp = (i >> 26) & 1;
            int w = pair ? o.bytes / 2 : o.bytes;
            /* a 16-byte SIMD store is two 8-byte words of data (taken below only where the callee reads them so) */
            int pieces = (pair ? 2 : 1) * (fp && w == 16 ? 2 : 1);
            if (fp && w == 16) w = 8;
            for (int p = 0; p < pieces; p++) {
                int64_t at = o.imm - (alloc ? o.wb : 0) + p * w;   /* from the sp at the call */
                if (at < 0 || at >= 8 * HAO_SITEPACK_MAX) continue;     /* below sp, or no argument we place */
                bool seen = false;
                for (int j = 0; j < ns && !seen; j++) {
                    if (st[j].off == at && st[j].w == w) seen = true;  /* the later store is the argument */
                    else if (at < st[j].off + st[j].w && st[j].off < at + w) return 0;   /* overlapping */
                }
                if (!seen) {
                    st[ns].off = (int16_t)at, st[ns].w = (uint8_t)w, st[ns].fp = fp, st[ns].k = (uint8_t)k;
                    st[ns].src = (uint8_t)(pair && 2 * p >= pieces ? (i >> 10) & 31 : i & 31);
                    ns++;
                }
            }
        } else if ((i & 0x3B200C00) == 0x38200800 && ((i >> 5) & 31) == 31 && !((i >> 22) & 1))
            return 0;                                                  /* a store to [sp, register] */
        if (alloc || hao_is_guest_function(a)) break;
    }
    for (int x = 1; x < ns; x++)                                       /* by offset */
        for (int y = x; y > 0 && st[y - 1].off > st[y].off; y--) { __typeof__(st[0]) t = st[y]; st[y] = st[y - 1]; st[y - 1] = t; }
    /* every general-register store at a multiple of 8: no layout of them is packed, unless one of them
     * holds several arguments (split below); until a split is tried such a site keeps the word copy
     * without a refusal */
    bool odd = false;
    for (int j = 0; j < ns; j++) odd |= !st[j].fp && (st[j].off & 7);
    hao_x86_stackuse u;
    bool shown = ns && callee_stackuse(native, &u), quiet = !odd;
    int n = shown ? u.nslots - 2 : ns;
#define REFUSE(m) do { if (!quiet) *why = (m); return 0; } while (0)
    if (!odd && !shown) return 0;
    if (shown && n <= 0) REFUSE("the callee reads no stack argument past x7");
    if (n > HAO_SITEPACK_MAX) REFUSE("too many stack arguments");
    bool packed = false;
    unsigned end = 0;
    int j = 0;
    for (int s = 0; j < n; s++) {
        if (s == ns) REFUSE("fewer stores than the callee's stack arguments (a merged store?)");
        unsigned at = (unsigned)st[s].off, w = st[s].w;
        if ((w != 1 && w != 2 && w != 4 && w != 8) || at % w) REFUSE("a store not naturally aligned");
        uint8_t r = shown ? u.width[j + 2] : 0;
        /* FP/SIMD data: an 8-byte word the callee reads as one 8-byte slot is placed like any other */
        if (st[s].fp && (w != 8 || r != 8)) REFUSE("an FP/SIMD store among the arguments");
        if (shown && !r) REFUSE("the callee does not read an argument's slot");
        if (shown && r != HAO_X86_WIDTH_UNKNOWN && r < w) {
            /* one store of several arguments: the callee's read widths at natural alignment fill it */
            unsigned p = at;
            int first = j;
            quiet = false;
            if (at != ((end + r - 1) & ~(r - 1u))) REFUSE("a hole among the stores");
            while (p < at + w && j < n) {
                r = u.width[j + 2];
                if (r != 1 && r != 2 && r != 4 && r != 8) REFUSE("a merged store with a slot of unknown width");
                p = (p + r - 1) & ~(r - 1u);
                if (p + r > at + w) break;
                off[j] = (uint16_t)p;
                width[j] = (uint8_t)r;
                packed |= p != 8u * (unsigned)j;
                p += r;
                j++;
            }
            if (p != at + w || j - first < 2) REFUSE("the callee reads a slot narrower than its store");
            end = p;
            continue;
        }
        if (at != ((end + w - 1) & ~(w - 1))) REFUSE("a hole among the stores");
        if (!shown && w > 1 && !sitepack_address(win, nwin, st[s].k, st[s].src))
            REFUSE("callee not readable, and a store wider than a byte may be merged arguments");
        off[j] = (uint16_t)at;
        width[j] = (uint8_t)w;
        packed |= at != 8u * (unsigned)j;
        end = at + w;
        j++;
    }
#undef REFUSE
    return packed ? n : 0;
}

/* How many SysV stack slots past x6 and x7 the native function NATIVE reads (its code shows them), or -1 */
int hao_swift_callee_stack_slots(uint64_t native)
{
    hao_x86_stackuse u;
    if (!callee_stackuse(native, &u)) return -1;
    return u.nslots > 2 ? u.nslots - 2 : 0;
}

int hao_swift_sitepack(const hao_cpu *c, uint64_t native, uint16_t *off, uint8_t *width)
{
    uint64_t lr = c->x[30], key[2] = {lr, native};
    if (!lr || (lr & 3) || !native) return 0;
    const uint64_t *e = hao_gmap_get(&sitepacks, key);
    int n, reg;
    uint16_t o[HAO_SITEPACK_MAX];
    uint8_t w[HAO_SITEPACK_MAX];
    const char *why = NULL;
    if (e) {
        uint64_t v = e[2];
        unsigned li = (unsigned)(v >> 16);
        n = (int)(v & 0xff);
        reg = (int)((v >> 8) & 0xff);
        if (n && li) for (int j = 0; j < n; j++) { o[j] = sitepack_layouts[li - 1].off[j]; w[j] = sitepack_layouts[li - 1].w[j]; }
        else if (n) n = sitepack_scan(lr - 4, native, o, w, &why);   /* the layout table was full: read the site again */
    } else {
        reg = sitepack_call(lr - 4, native);
        bool called = reg >= 0;
        n = reg < 0 ? 0 : sitepack_scan(lr - 4, native, o, w, &why);
        if (reg < 0) reg = SITEPACK_DIRECT;
        if (hao_verbose && !hao_opt.bridge_check) {
            /* guarded processes (no HAO_BRIDGE_CHECK): the decision once per site, with what the callee shows */
            hao_x86_stackuse u;
            bool shown = callee_stackuse(native, &u);
            uint64_t impl = dispatch_impl(untag(native));
            char t[8 * HAO_SITEPACK_MAX], cw[4 * HAO_X86_MAX_SLOTS];
            size_t l = 0, m = 0;
            for (int j = 0; j < n && l + 9 < sizeof t; j++) l += (size_t)snprintf(t + l, sizeof t - l, "%s%u.%u", j ? "," : "", o[j], w[j]);
            t[l] = 0;
            for (int j = 0; shown && j < u.nslots && m + 5 < sizeof cw; j++) m += (size_t)snprintf(cw + m, sizeof cw - m, "%s%u", j ? "," : "", u.width[j]);
            cw[m] = 0;
            char ib[48] = "";
            if (impl) snprintf(ib, sizeof ib, " (implementation %#llx)", (unsigned long long)impl);
            if (n || why || !called || (shown && u.nslots > 2))
                fprintf(HAO_DIAG, "hao: call-site layout at %#llx for %#llx%s: %s%s; callee %s, %d slots [%s]\n",
                        (unsigned long long)(lr - 4), (unsigned long long)native, ib,
                        !called ? "the site does not call it: word copy" : n ? "taken " : why ? "refused: " : "not packed: word copy",
                        n ? t : why ? why : "", shown ? "read" : "not readable", u.nslots, cw);
        }
        bool fresh;
        hao_crit_enter();
        pthread_mutex_lock(&sitepack_lock);
        uint64_t *s = hao_gmap_claim(&sitepacks, key, &fresh);
        if (s && fresh) {
            unsigned li = 0;
            if (n && sitepack_nlayouts < SITEPACK_LAYOUTS) {
                li = ++sitepack_nlayouts;
                for (int j = 0; j < n; j++) { sitepack_layouts[li - 1].off[j] = o[j]; sitepack_layouts[li - 1].w[j] = w[j]; }
            }
            s[2] = (uint64_t)n | (uint64_t)reg << 8 | (uint64_t)li << 16;
            hao_gmap_publish(&sitepacks, s, key);
        }
        pthread_mutex_unlock(&sitepack_lock);
        hao_crit_leave();
        if (n && hao_opt.bridge_check) {
            char t[8 * HAO_SITEPACK_MAX];
            size_t l = 0;
            for (int j = 0; j < n && l + 9 < sizeof t; j++) l += (size_t)snprintf(t + l, sizeof t - l, "%s%u.%u", j ? "," : "", o[j], w[j]);
            fprintf(HAO_DIAG, "hao: packed stack arguments from the call site at %#llx: %s\n", (unsigned long long)(lr - 4), t);
        } else if (why && hao_opt.bridge_check)
            fprintf(HAO_DIAG, "hao: packed stack arguments at %#llx refused (%s): word copy\n", (unsigned long long)(lr - 4), why);
    }
    if (n <= 0 || (reg != SITEPACK_DIRECT && untag(c->x[reg]) != untag(native))) return 0;
    memcpy(off, o, (size_t)n * sizeof *o);
    memcpy(width, w, (size_t)n);
    return n;
}
