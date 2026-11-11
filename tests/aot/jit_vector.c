/*
 * The translator's vector floating-point, shift/narrow/widen and structure
 * load/store translations against the interpreter (a64_exec), which the
 * oracle checks against native arm64. Each case runs one encoding (a
 * floating-point one sometimes followed by a second that reads its result),
 * alone and between a CMP and a CSEL that reads its NZCV, through both engines
 * from the same random state under one of the 16 FPCR RMode/DN/FZ settings
 * and with FPSR sticky flags clear or set, and compares every x and v
 * register, NZCV, the full FPSR (the IEEE cumulative flags included: the
 * oracle only counts those), FPCR, the pc, and the memory around the
 * address. Operands mix FP special values (zeros, subnormals, the minimum
 * normal, infinities, NaNs), operations that overflow, underflow or cancel
 * exactly, and random bits. Loads and stores run in the middle of a page
 * and across the end of a page followed by an inaccessible one: a faulting
 * access must leave the same registers and memory in both engines (the pc,
 * which the interpreter advances first, and NZCV, which translated code may
 * hold in EFLAGS, are not compared then).
 */
#include "hao/cpu.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int hao_jit_exec_code(hao_cpu *c, const uint32_t *code, int n, uint64_t base);

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static unsigned rr(unsigned n) { return (unsigned)(rnd() % n); }

static const uint32_t specials[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
    0x00800001, 0x00ffffff, 0x01000000, 0x7f7fffff, 0xff7fffff, 0x7f000000, 0x7f800000, 0xff800000,
    0x7fc00000, 0xffc00000, 0x7f800001, 0xffbfffff, 0x7fc00123, 0x3f800000, 0xbf800000, 0x3f800001,
    0x3fffffff, 0x34000000, 0x1f800000, 0x5f800000, 0x20000000, 0x1f000000, 0x00400000, 0x40490fdb,
};
static uint32_t fpval(void)
{
    switch (rr(4)) {
    case 0: return specials[rr(sizeof specials / sizeof *specials)];
    case 1: return (uint32_t)rnd();
    case 2: return (uint32_t)(rr(2) << 31 | (rr(254) + 1) << 23 | (rnd() & 0x7fffff));   /* normal */
    default: return (uint32_t)(rr(2) << 31 | (rr(2) ? rr(8) : 247 + rr(8)) << 23 | (rnd() & 0x7fffff)); /* near the ends */
    }
}

#define PG 16384
static uint8_t *mem;               /* page 0, 1: read/write; page 2: no access */
static uint8_t init_mem[2 * PG];

static sigjmp_buf jb;
static void on_fault(int s) { (void)s; siglongjmp(jb, 1); }

typedef struct { int rc; bool faulted; hao_cpu c; uint32_t fpsr, fpcr; uint8_t m[2 * PG]; } outcome;

static void run(outcome *o, const hao_cpu *in, const uint32_t *code, int n, uint64_t pc, bool jit)
{
    static hao_cpu c;
    c = *in;
    memcpy(mem, init_mem, sizeof init_mem);
    a64_fpcr_write(&c, in->fpcr);
    a64_fpsr_write(&c, in->fpsr);
    o->faulted = false;
    if (sigsetjmp(jb, 1) == 0) {
        if (jit) o->rc = hao_jit_exec_code(&c, code, n, pc);
        else {
            o->rc = HAO_EXIT_NONE;
            c.pc = pc;
            for (int k = 0; k < n; k++) {
                int r = a64_exec(&c, code[k], c.pc);
                if (r != HAO_EXIT_NONE && r != HAO_EXIT_BRANCH) { o->rc = r; break; }
            }
        }
    } else
        o->faulted = true;
    o->fpsr = a64_fpsr_read(&c);
    o->fpcr = a64_fpcr_read(&c);
    a64_fpsr_write(&c, 0);
    a64_fpcr_write(&c, 0);
    o->c = c;
    memcpy(o->m, mem, sizeof o->m);
}

static long cases, faults, undefs, fails, straddles;

/* insn (and insn2 after it, if not 0), alone or between CMP x0, x1 and
 * CSEL x2, x0, x1, HI */
static void check(uint32_t insn, uint32_t insn2, const hao_cpu *in, bool live)
{
    static outcome a, b;
    uint32_t code[4] = {0xeb01001fu, insn, insn2, 0x9a818002u};
    int n = insn2 ? 2 : 1;
    if (!insn2) code[2] = code[3];
    const uint32_t *p = live ? code : code + 1;
    n += live ? 2 : 0;
    uint64_t pc = 0x7300000000ull + 4 * rr(1024);
    run(&a, in, p, n, pc, false);
    run(&b, in, p, n, pc, true);
    const char *why = NULL;
    if (a.faulted != b.faulted) why = "fault";
    else if (!a.faulted && a.rc != b.rc) why = "exit";
    else if (memcmp(a.c.x, b.c.x, sizeof a.c.x)) why = "x";
    else if (memcmp(a.c.v, b.c.v, sizeof a.c.v)) why = "v";
    else if (memcmp(a.m, b.m, 2 * PG - 16)) why = "memory";
    else if (memcmp(a.m + 2 * PG - 16, b.m + 2 * PG - 16, 16)) {
        /* A register store that straddles the inaccessible page: translated
         * code's single move stores nothing, the interpreter's memcpy may
         * have stored the bytes before the page end (Arm leaves them
         * UNKNOWN); registers before it are stored by both. */
        for (int k = 2 * PG - 16; k < 2 * PG && !why; k++)
            if (a.m[k] != b.m[k] && !(a.faulted && b.m[k] == init_mem[k])) why = "memory";
        straddles += !why;
    }
    else if (!a.faulted && (a.c.pc != b.c.pc || hao_nzcv(&a.c) != hao_nzcv(&b.c))) why = "pc/nzcv";
    else if (a.fpsr != b.fpsr || a.fpcr != b.fpcr) why = "fpsr/fpcr";
    else if (a.rc == HAO_EXIT_UNDEF && a.c.exit_insn != b.c.exit_insn) why = "exit_insn";
    cases++;
    faults += a.faulted;
    undefs += !a.faulted && a.rc == HAO_EXIT_UNDEF;
    if (!why) return;
    if (fails++ < 20) {
        printf("MISMATCH %08x %08x%s: %s (interp rc %d%s fpsr %x, jit rc %d%s fpsr %x; fpcr %x)\n", insn, insn2, live ? " (NZCV live)" : "", why,
               a.rc, a.faulted ? " fault" : "", a.fpsr, b.rc, b.faulted ? " fault" : "", b.fpsr, in->fpcr);
        if (!strcmp(why, "memory")) {
            int lo = -1, hi = -1;
            for (int k = 0; k < 2 * PG; k++) if (a.m[k] != b.m[k]) { if (lo < 0) lo = k; hi = k; }
            printf("  memory differs at page-end offsets %d..%d, x[rn] at %lld\n", lo - 2 * PG, hi - 2 * PG,
                   (long long)(in->x[(insn >> 5) & 31] - (uint64_t)(uintptr_t)mem) - 2 * PG);
        }
        for (int k = 0; k < 32; k++)
            if (memcmp(&a.c.v[k], &b.c.v[k], 16) || memcmp(&in->v[k], &a.c.v[k], 16))
                printf("  v%-2d in %016llx:%016llx interp %016llx:%016llx jit %016llx:%016llx\n", k,
                       (unsigned long long)in->v[k].d[1], (unsigned long long)in->v[k].d[0],
                       (unsigned long long)a.c.v[k].d[1], (unsigned long long)a.c.v[k].d[0],
                       (unsigned long long)b.c.v[k].d[1], (unsigned long long)b.c.v[k].d[0]);
    }
}

static unsigned reg(void) { static const unsigned r[] = {0, 1, 2, 3, 7, 30, 31}; return r[rr(7)]; }

/* a random state: FP special values in the vector registers (with lanes of
 * Vm copied from Vn, negated or nudged for exact cancellations and equal
 * operands), x0/x1 for the CMP */
static void state(hao_cpu *c, unsigned rn, unsigned rm)
{
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 31; k++) c->x[k] = rnd();
    c->x[31] = rnd() & ~15ull;
    for (int k = 0; k < 32; k++) for (int e = 0; e < 4; e++) c->v[k].s[e] = rr(4) ? fpval() : (uint32_t)rnd();
    for (int e = 0; e < 4; e++)
        switch (rr(6)) {
        case 0: c->v[rm].s[e] = c->v[rn].s[e]; break;
        case 1: c->v[rm].s[e] = c->v[rn].s[e] ^ 0x80000000u; break;
        case 2: c->v[rm].s[e] = c->v[rn].s[e] ^ (1u << rr(23)); break;
        default: break;
        }
    hao_set_nzcv(c, rr(16));
    unsigned control = rr(16);   /* RMode, DN, FZ */
    c->fpcr = (control & 3) << 22 | (control & 4) << 23 | (control & 8) << 21;
    c->fpsr = rr(2) ? 0x0800009fu : 0;
}

static uint32_t enc_three_same(void)
{
    static const unsigned keys[] = {0x02, 0x22, 0x43, 0x00, 0x06, 0x20, 0x26, 0x04, 0x44, 0x64, 0x47, 0x01, 0x42, 0x65};
    unsigned key = keys[rr(sizeof keys / sizeof *keys)], dbl = rr(8) == 0, Q = rr(2) || dbl;
    return 0x0e200400u | Q << 30 | (key >> 6) << 29 | (((key >> 5) & 1) << 1 | dbl) << 22 | (0x18 | (key & 7)) << 11;
}
static uint32_t enc_indexed(void)
{
    unsigned size = rr(6) ? 2 : rr(4), U = rr(8) == 0;
    return 0x0f009000u | rr(2) << 30 | U << 29 | size << 22 | rr(2) << 21 | rr(2) << 11;   /* M and Rm below */
}

int main(void)
{
    mem = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED || mprotect(mem + 2 * PG, PG, PROT_NONE)) return 2;
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    for (unsigned k = 0; k < sizeof init_mem; k++) init_mem[k] = (uint8_t)rnd();
    long per[8] = {0};
    for (int round = 0; round < 60000; round++) {
        hao_cpu in;
        unsigned rd = reg(), rn = reg(), rm = reg(), kind = rr(8);
        uint32_t insn;
        switch (kind) {
        case 0: case 1: insn = enc_three_same() | rm << 16 | rn << 5 | rd; break;
        case 2: insn = enc_indexed() | (rm & 15) << 16 | (rm >> 4) << 20 | rn << 5 | rd; break;
        case 3: {   /* FABS/FNEG, XTN/SQXTUN (and their neighbours) */
            static const unsigned ops[] = {0x0F, 0x0F, 0x12, 0x12, 0x0B};
            insn = 0x0e200800u | rr(2) << 30 | rr(2) << 29 | rr(4) << 22 | ops[rr(5)] << 12 | rn << 5 | rd;
            break;
        }
        case 4: {   /* shift by immediate: vector and scalar */
            static const unsigned ops[] = {0x00, 0x00, 0x02, 0x04, 0x06, 0x0A, 0x10, 0x14, 0x08, 0x11};
            unsigned immhb = 1 + rr(127);
            insn = (rr(6) ? 0x0f000400u | rr(2) << 30 : 0x5f000400u) | rr(2) << 29 | immhb << 16 | ops[rr(10)] << 11 | rn << 5 | rd;
            break;
        }
        default: {  /* structure loads and stores */
            unsigned post = rr(2), Q = rr(2), L = rr(2), size = rr(4), rmv = post ? (rr(3) ? 31 : reg() & 15) : 0;
            if (kind == 5 || kind == 6) {
                static const unsigned opcs[] = {0x7, 0xA, 0x6, 0x2, 0x7, 0x2, 0x0, 0x8};
                insn = 0x0c000000u | Q << 30 | post << 23 | L << 22 | rmv << 16 | opcs[rr(8)] << 12 | size << 10;
            } else {
                static const unsigned opcs[] = {0, 2, 4, 6, 6, 1, 7};
                insn = 0x0d000000u | Q << 30 | post << 23 | L << 22 | (rr(6) == 0) << 21 | rmv << 16 | opcs[rr(7)] << 13 | rr(2) << 12 | size << 10;
            }
            if (rr(16) == 0) insn ^= 1u << (16 + rr(6));   /* a nonzero Rm field without post-index, ... */
            insn |= rn << 5 | rd;
            break;
        }
        }
        state(&in, rn, (insn >> 16) & 31);
        if (kind >= 5) {
            uint64_t a = (uint64_t)(uintptr_t)mem + (rr(2) ? PG + 64 * rr(200) + rr(64) : 2 * PG - 8 * (1 + rr(10)));
            in.x[rn] = a;   /* rn = 31: SP */
            unsigned xm = (insn >> 16) & 31;
            if (xm != 31 && xm != rn) in.x[xm] = (uint64_t)(int64_t)(rr(129) - 64);
        }
        /* a second vector FP instruction: one block, the first one's
         * result as an operand */
        uint32_t insn2 = 0;
        if (kind <= 2 && rr(2)) insn2 = (rr(2) ? enc_three_same() | rr(32) << 16 : enc_indexed() | rr(16) << 16) | rd << 5 | reg();
        for (int live = 0; live < 2; live++) check(insn, insn2, &in, live);
        per[kind]++;
    }
    printf("jit vector: %ld cases (%ld faulting, %ld of them stores straddling the page end; %ld undefined) [fp %ld, by element %ld, misc %ld, shift %ld, ld/st %ld], %ld mismatches\n",
           cases, faults, straddles, undefs, 2 * (per[0] + per[1]), 2 * per[2], 2 * per[3], 2 * per[4], 2 * (per[5] + per[6] + per[7]), fails);
    return fails ? 1 : 0;
}
