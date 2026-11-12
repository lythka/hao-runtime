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
#include "../oracle/fma_cases.h"
#include "../oracle/fma16_cases.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>

int hao_jit_exec_code(hao_cpu *c, const uint32_t *code, int n, uint64_t base);

/* The unit/mutation build redirects only the translator's helper target to
 * this counter. Production code is unchanged; result comparisons still run
 * the real descriptor helper. This also tests declines whose final half
 * result happens to be insensitive to a lost binary64 tail. */
static unsigned vector_helper_calls;
int jit_vector_helper(hao_cpu *c, uint64_t descriptor, uint64_t pc)
{
    vector_helper_calls++;
    return a64_exec_helper(c, descriptor, pc);
}


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

/* a double around the integers: specials, halves and quarters, values near
 * 2^52 where every double is integral, and random bits */
static uint64_t dval(void)
{
    static const uint64_t ds[] = {
        0, 0x8000000000000000ull, 1, 0x800fffffffffffffull, 0x0010000000000000ull, 0x8010000000000000ull,
        0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000001ull, 0xfff4000000000123ull,
        0x3fe0000000000000ull, 0xbfe0000000000000ull, 0x3fdfffffffffffffull, 0x3fefffffffffffffull, 0x4330000000000000ull,
        0x432fffffffffffffull, 0xc32fffffffffffffull, 0x7fefffffffffffffull, 0x3ff8000000000000ull, 0xc004000000000000ull,
    };
    double d;
    uint64_t v;
    switch (rr(4)) {
    case 0: return ds[rr(sizeof ds / sizeof *ds)];
    case 1: return rnd();
    default:
        d = (double)((int)rr(200) - 100) + (double)rr(8) * 0.125 + (rr(4) ? 0 : (double)(rnd() & 0xfffff) * 0x1p-20);
        if (rr(4) == 0) d = ldexp(d, (int)rr(120) - 60);
        memcpy(&v, &d, 8);
        return v;
    }
}
/* a single around the integers (the same, in single precision) */
static uint32_t sval(void)
{
    uint32_t v;
    float f;
    if (rr(2)) return fpval();
    f = (float)((int)rr(200) - 100) + (float)rr(8) * 0.125f + (rr(4) ? 0 : (float)(rnd() & 0xfff) * 0x1p-12f);
    if (rr(4) == 0) f = ldexpf(f, (int)rr(60) - 30);
    memcpy(&v, &f, 4);
    return v;
}

static uint32_t enc_three_same(void)
{
    static const unsigned keys[] = {0x02, 0x22, 0x43, 0x00, 0x06, 0x20, 0x26, 0x04, 0x44, 0x64, 0x47, 0x01, 0x42, 0x65, 0x01, 0x21};
    unsigned key = keys[rr(sizeof keys / sizeof *keys)], dbl = rr(8) == 0, Q = rr(2) || dbl;
    return 0x0e200400u | Q << 30 | (key >> 6) << 29 | (((key >> 5) & 1) << 1 | dbl) << 22 | (0x18 | (key & 7)) << 11;
}
/* FRINT* (vector), every key; reserved ones included */
static uint32_t enc_frint(void)
{
    return 0x0e218800u | rr(2) << 30 | rr(2) << 29 | rr(4) << 22 | rr(2) << 12;
}
static uint32_t enc_indexed(void)
{
    unsigned size = rr(6) ? 2 : rr(4), U = rr(8) == 0;
    static const unsigned ops[] = {1, 5, 9};
    return 0x0f000000u | ops[rr(3)] << 12 | rr(2) << 30 | U << 29 | size << 22 | rr(2) << 21 | rr(2) << 11;   /* M and Rm below */
}

int main(void)
{
    mem = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED || mprotect(mem + 2 * PG, PG, PROT_NONE)) return 2;
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    for (unsigned k = 0; k < sizeof init_mem; k++) init_mem[k] = (uint8_t)rnd();
    /* Every allocated permute, byte-distinct random states and aliases. */
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 4; size++)
        for (unsigned op = 1; op < 8; op++) for (unsigned k = 0; k < 64; k++) {
            if (!(op & 3) || (!q && size == 3)) continue;
            unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
            hao_cpu in; state(&in, rn, rm);
            for (int r = 0; r < 32; r++) for (int b = 0; b < 16; b++) in.v[r].b[b] = (uint8_t)rnd();
            uint32_t i = 0x0e000800 | q << 30 | size << 22 | op << 12 | rm << 16 | rn << 5 | rd;
            check(i, 0, &in, false); check(i, 0, &in, true);
        }
    for (unsigned q = 0; q < 2; q++) for (unsigned sz = 0; sz < 3; sz++)
        for (unsigned form = 0; form < 5; form++) for (unsigned k = 0; k < 128; k++) {
            if (form == 0 && sz == 2 && !q) continue;
            unsigned rd = reg(), rn = k & 1 ? rd : reg();
            hao_cpu in; state(&in, rn, rn);
            in.v[rn].d[0] = rnd(); in.v[rn].d[1] = rnd();
            uint32_t i = form == 0 ? 0x0e31b800 : 0x0e202800 | ((form - 1) & 1) << 29 | (form >= 3 ? 0x4000 : 0);
            i |= q << 30 | sz << 22 | rn << 5 | rd;
            check(i, 0, &in, false); check(i, 0, &in, true);
        }
    /* Pairwise sums and extrema reductions, aliases and full FP state. */
    static const uint32_t pair_forms[] = {
        0x2e20d400, 0x6e20d400, 0x6e60d400, 0x7e30d800, 0x7e70d800,
        0x6e30c800, 0x6eb0c800, 0x6e30f800, 0x6eb0f800
    };
    for (unsigned control = 0; control < 16; control++)
        for (unsigned f = 0; f < sizeof pair_forms / sizeof *pair_forms; f++)
            for (unsigned k = 0; k < 128; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                hao_cpu in; state(&in, rn, rm); in.fpcr = control << 22;
                if (f == 2 || f == 4) for (int e = 0; e < 2; e++) { in.v[rn].d[e] = dval(); in.v[rm].d[e] = dval(); }
                if (k < 4) for (int e = 0; e < 4; e++) {
                    in.v[rn].s[e] = 0x3f800000u + (unsigned)e * 0x800000u;
                    in.v[rm].s[e] = 0x41000000u + (unsigned)e * 0x100000u;
                }
                uint32_t i = pair_forms[f] | rn << 5 | rd | (f < 3 ? rm << 16 : 0);
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0;
                    check(i, 0, &in, live);
                    if (k == 0 && vector_helper_calls) {
                        if (fails++ < 20) printf("MISMATCH pair admission %08x: %u helper calls\n", i, vector_helper_calls);
                    }
                }
            }
    /* Every signed low-byte shift amount, random ignored count bits,
     * lane widths, aliases, and NZCV liveness. Oversized shifts and the
     * signed -128 corner must not wrap through an x86 integer shift. */
    for (unsigned sz = 2; sz <= 3; sz++) for (unsigned q = 0; q < 2; q++)
        for (unsigned u = 0; u < 2; u++) for (unsigned byte = 0; byte < 256; byte++)
            for (unsigned alias = 0; alias < 4; alias++) {
                if (sz == 3 && !q) continue;
                unsigned rd = reg(), rn = alias & 1 ? rd : reg(), rm = alias & 2 ? rn : reg();
                hao_cpu in; state(&in, rn, rm);
                in.v[rn].d[0] = rnd(); in.v[rn].d[1] = rnd();
                in.v[rm].d[0] = rnd(); in.v[rm].d[1] = rnd();
                for (unsigned b = 0; b < (q ? 16u : 8u); b += 1u << sz)
                    in.v[rm].b[b] = (uint8_t)(byte + b * 7);
                uint32_t i = 0x0e204400 | q << 30 | u << 29 | sz << 22 | rm << 16 | rn << 5 | rd;
                check(i, 0, &in, false); check(i, 0, &in, true);
            }
    for (unsigned control = 0; control < 16; control++)
        for (unsigned q = 0; q < 2; q++) for (unsigned k = 0; k < 256; k++) {
            unsigned rd = reg(), rn = k > 1 && (k & 1) ? rd : 1, rm = k > 1 && (k & 2) ? rn : 2;
            hao_cpu in; state(&in, rn, rm); in.fpcr = control << 22;
            if (k < 2) for (int e = 0; e < 4; e++) {
                in.v[rn].s[e] = k ? 0x00800000 : 0x40c00000;
                in.v[rm].s[e] = 0x40000000;
            }
            uint32_t i = 0x2e20fc00 | q << 30 | rm << 16 | rn << 5 | rd;
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (k < 2 && vector_helper_calls != k) {
                    if (fails++ < 20) printf("MISMATCH division admission %08x fpcr %x: %u helper calls\n", i, in.fpcr, vector_helper_calls);
                }
            }
        }
    /* Scalar indexed single multiply/fused forms and double multiply:
     * all lane indices and controls, high Vm, aliases and directed FMA tails.
     * Unused NaN lanes must not send ordinary scalar arithmetic to a helper. */
    for (unsigned control = 0; control < 16; control++)
        for (unsigned form = 0; form < 4; form++) for (unsigned idx = 0; idx < (form == 3 ? 2u : 4u); idx++)
            for (unsigned k = 0; k < 96; k++) {
                unsigned rd = k < 2 ? 0 : reg(), rn = k < 2 ? 1 : (k & 1 ? rd : reg());
                unsigned rm = k < 2 ? 17 : (k & 2 ? rn : reg());
                hao_cpu in; state(&in, rn, rm); in.fpcr = control << 22;
                if (form == 3) {
                    in.v[rn].d[0] = dval(); in.v[rm].d[idx] = dval();
                } else {
                    fma_ops f = fma_case(&seed, k % FC_N);
                    in.v[rn].s[0] = f.a; in.v[rm].s[idx] = f.b; in.v[rd].s[0] = f.c;
                }
                if (k < 2) {
                    for (int e = 0; e < 4; e++) {
                        in.v[rn].s[e] = 0x7f800001u; in.v[rm].s[e] = 0x7f800001u; in.v[rd].s[e] = 0x7f800001u;
                    }
                    if (form == 3) { in.v[rn].d[0] = 0x4000000000000000ull; in.v[rm].d[idx] = 0x4008000000000000ull; }
                    else { in.v[rn].s[0] = 0x40000000; in.v[rm].s[idx] = 0x40400000; in.v[rd].s[0] = 0x40800000; }
                }
                unsigned op = form == 1 ? 1 : form == 2 ? 5 : 9;
                uint32_t i = 0x5f000000 | (form == 3 ? 3u : 2u) << 22 | op << 12 | rm << 16 | rn << 5 | rd;
                i |= (form == 3 ? idx : idx >> 1) << 11 | (form == 3 ? 0 : (idx & 1) << 21);
                vector_helper_calls = 0;
                check(i, 0, &in, false); check(i, 0, &in, true);
                if (k < 2 && vector_helper_calls) {
                    printf("MISMATCH %08x scalar indexed ordinary path called helper\n", i); fails++;
                }
            }
    /* Every fused form, lane index, high Vm, alias pattern and FPCR setting.
     * Directed near-ties detect removing the odd step; random bits alone
     * almost never land close enough to a binary32 midpoint. */
    for (unsigned control = 0; control < 16; control++)
        for (unsigned form = 0; form < 20; form++)
            for (unsigned k = 0; k < 144; k++) {
                unsigned q = form & 1, sub = (form >> 1) & 1;
                int idx = form < 4 ? -1 : (int)((form - 4) >> 2);
                unsigned rd = reg(), rn = reg(), rm = reg();
                hao_cpu in;
                state(&in, rn, rm);
                in.fpcr = control << 22;
                for (unsigned lane = 0; lane < 4; lane++) {
                    fma_ops o = fma_case(&seed, k % FC_N);
                    in.v[rd].s[lane] = o.c;
                    in.v[rn].s[lane] = o.a;
                    in.v[rm].s[lane] = o.b;
                }
                uint32_t insn = idx < 0 ? 0x0e20cc00u | sub << 23 :
                    0x0f801000u | sub << 14 | (unsigned)(idx >> 1) << 11 | (unsigned)(idx & 1) << 21;
                insn |= q << 30 | rm << 16 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    check(insn, 0, &in, live);
                    if (k % 8 == 0) check(insn, 0x4e20d400u | rd << 16 | rn << 5 | rd, &in, live);
                }
            }
    /* Half fused forms: H:L:M, low Vm bank, all controls, aliases, ordinary
     * and special inputs, exactness declines, and live NZCV. FZ16/AHP are
     * compared with the existing helper, not claimed as native support. */
    for (unsigned control = 0; control < 24; control++)
        for (unsigned form = 0; form < 36; form++)
            for (unsigned k = 0; k < 128; k++) {
                unsigned q = form & 1, sub = (form >> 1) & 1;
                int idx = form < 4 ? -1 : (int)((form - 4) >> 2);
                unsigned rd = reg(), rn = reg(), rm = reg();
                if (idx >= 0) rm &= 15;
                if (k == 22) { rd = 0; rn = 1; rm = 2; }
                hao_cpu in;
                state(&in, rn, rm);
                in.fpcr = control < 16 ? control << 22 :
                    (control & 3) << 22 | (1u << (control < 20 ? 19 : 26));
                for (unsigned lane = 0; lane < 8; lane++) {
                    fma16_ops o = fma16_case(&seed, k < 22 ? k : k + lane);
                    in.v[rd].h[lane] = o.c;
                    in.v[rn].h[lane] = o.a;
                    in.v[rm].h[lane] = o.b;
                }
                if (k == 22) {
                    /* Earlier pairs round inexactly; only the last active
                     * lane loses binary64 precision and must roll back. */
                    for (unsigned l = 0; l < 8; l++) {
                        in.v[0].h[l] = 0x3c00;
                        in.v[1].h[l] = 0x3c00;
                        in.v[2].h[l] = 0x0401;
                    }
                    in.v[0].h[q ? 7 : 3] = 0x7bff;
                    in.v[1].h[q ? 7 : 3] = 0x0401;
                }
                if (idx >= 0 && k >= 22 && !(k % 4))
                    for (unsigned l = 0; l < 8; l++) in.v[rm].h[l] = 0x3000 + l * 0x101;
                uint32_t insn = idx < 0 ? 0x0e400c00u | sub << 23 :
                    0x0f001000u | sub << 14 | (unsigned)(idx >> 2) << 11 |
                    (unsigned)((idx >> 1) & 1) << 21 | (unsigned)(idx & 1) << 20;
                insn |= q << 30 | rm << 16 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0;
                    check(insn, 0, &in, live);
                    if ((k == 22 && vector_helper_calls != 1) ||
                        (k == 0 && control < 16 && vector_helper_calls != 0)) {
                        if (fails++ < 20) printf("MISMATCH half admission %08x fpcr %x: %u helper calls\n", insn, in.fpcr, vector_helper_calls);
                    }
                    /* Adjacent FP32 code must rebuild its own constants. */
                    if (k % 8 == 0) check(insn, 0x4e20d400u | rd << 16 | rn << 5 | rd, &in, live);
                }
            }
    /* A maximum-length block must fit the emitter's scratch/cache budget,
     * including both pairs of the largest fused translation. */
    for (unsigned form = 0; form < 6; form++) {
        hao_cpu in;
        state(&in, 1, 2);
        for (unsigned r = 0; r < 3; r++)
            for (unsigned lane = 0; lane < 4; lane++) in.v[r].f[lane] = 0.25f;
        if (form >= 2) for (unsigned r = 0; r < 3; r++)
            for (unsigned l = 0; l < 8; l++) in.v[r].h[l] = 0x3400;
        uint32_t code[98];
        code[0] = 0xeb01001fu;
        for (unsigned k = 1; k <= 96; k++) code[k] = (uint32_t[]){0x4e22cc20u, 0x4fa21820u, 0x4e420c20u, 0x4f321820u, 0x4ec20c20u, 0x4f325820u}[form];
        if (form >= 2) code[0] = code[1]; /* first block: 96 half expansions */
        code[97] = 0x9a818002u;
        static outcome a, b;
        run(&a, &in, code, 98, 0x7400000000ull, false);
        run(&b, &in, code, 98, 0x7400000000ull, true);
        if (a.faulted || b.faulted || a.rc != b.rc || a.c.pc != b.c.pc ||
            memcmp(a.c.x, b.c.x, sizeof a.c.x) || memcmp(a.c.v, b.c.v, sizeof a.c.v) ||
            hao_nzcv(&a.c) != hao_nzcv(&b.c) || a.fpsr != b.fpsr || a.fpcr != b.fpcr) {
            printf("MISMATCH maximum FMLA block form %u: rc %d/%d\n", form, a.rc, b.rc);
            fails++;
        }
        cases++;
    }
    long per[12] = {0};
    for (int round = 0; round < 100000; round++) {
        hao_cpu in;
        unsigned rd = reg(), rn = reg(), rm = reg(), kind = rr(12);
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
        case 8: {   /* integer three-same: CMGT CMGE CMEQ CMTST CMHI CMHS, [SU]MAX/MIN, MUL MLA MLS (PMUL) */
            static const unsigned ops[] = {0x06, 0x07, 0x0C, 0x0D, 0x11, 0x12, 0x13, 0x10};
            insn = 0x0e200400u | rr(2) << 30 | rr(2) << 29 | rr(4) << 22 | rm << 16 | ops[rr(8)] << 11 | rn << 5 | rd;
            break;
        }
        case 9:     /* two-reg misc: compares with zero, ABS, NEG; FRINT* (vector) */
            if (rr(2)) insn = 0x0e200800u | rr(2) << 30 | rr(2) << 29 | rr(4) << 22 | (0x08 + rr(4)) << 12 | rn << 5 | rd;
            else insn = enc_frint() | rn << 5 | rd;
            break;
        case 10:    /* TBL/TBX of 1-4 registers, EXT */
            if (rr(3)) insn = 0x0e000000u | rr(2) << 30 | rm << 16 | rr(4) << 13 | rr(2) << 12 | rn << 5 | rd;
            else insn = 0x2e000000u | rr(2) << 30 | rm << 16 | rr(16) << 11 | rn << 5 | rd;
            break;
        case 11:    /* MUL/MLA/MLS by element (and neighbours); scalar FRINT* */
            if (rr(2)) {
                static const unsigned ops[] = {0x0, 0x4, 0x8, 0x8, 0x0, 0x4, 0x2, 0xA};
                insn = 0x0f000000u | rr(2) << 30 | rr(2) << 29 | (rr(4) ? 1 + rr(2) : rr(4)) << 22 | rr(2) << 21 | rm << 16 | ops[rr(8)] << 12 | rr(2) << 11 | rn << 5 | rd;
            } else
                insn = 0x1e244000u | (rr(4) ? rr(2) : 2 + rr(2)) << 22 | rr(8) << 15 | rn << 5 | rd;
            break;
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
        if (kind == 9 || kind == 11)                          /* values to round, in the lane size of bit 22 */
            for (int k = 0; k < 32; k++)
                if (k == (int)rn || rr(4)) {
                    if ((insn >> 22) & 1) { in.v[k].d[0] = dval(); in.v[k].d[1] = dval(); }
                    else for (int e = 0; e < 4; e++) in.v[k].s[e] = sval();
                }
        if (kind == 8 && rr(2))                               /* equal and sign-flipped lanes for the compares */
            for (int e = 0; e < 16; e++) {
                unsigned xm = (insn >> 16) & 31;
                switch (rr(4)) {
                case 0: in.v[xm].b[e] = in.v[rn].b[e]; break;
                case 1: in.v[xm].b[e] = in.v[rn].b[e] ^ 0x80; break;
                default: break;
                }
            }
        if (kind == 10) {                                     /* table indices, mostly in or near range */
            unsigned xm = (insn >> 16) & 31;
            for (int e = 0; e < 16; e++) if (rr(4)) in.v[xm].b[e] = (uint8_t)(rr(4) ? rr(72) : 0x70 + rr(32) * 4);
        }
        if (kind >= 5 && kind <= 7) {
            uint64_t a = (uint64_t)(uintptr_t)mem + (rr(2) ? PG + 64 * rr(200) + rr(64) : 2 * PG - 8 * (1 + rr(10)));
            in.x[rn] = a;   /* rn = 31: SP */
            unsigned xm = (insn >> 16) & 31;
            if (xm != 31 && xm != rn) in.x[xm] = (uint64_t)(int64_t)(rr(129) - 64);
        }
        /* a second vector FP instruction: one block, the first one's
         * result as an operand */
        uint32_t insn2 = 0;
        if (kind <= 2 && rr(2)) insn2 = (rr(2) ? enc_three_same() | rr(32) << 16 : enc_indexed() | rr(16) << 16) | rd << 5 | reg();
        /* FRINT* after or before single-precision vector FP: the constants
         * one leaves for the next (tctx.vfp_consts) */
        if (kind <= 2 && insn2 && rr(4) == 0) insn2 = enc_frint() | rd << 5 | reg();
        if (kind == 9 && (insn & 0x0e218800u) == 0x0e218800u && rr(2))
            insn2 = (rr(2) ? enc_three_same() | rr(32) << 16 : enc_frint()) | rd << 5 | reg();
        for (int live = 0; live < 2; live++) check(insn, insn2, &in, live);
        per[kind]++;
    }
    printf("jit vector: %ld cases (%ld faulting, %ld of them stores straddling the page end; %ld undefined) [fp %ld, by element %ld, misc %ld, shift %ld, ld/st %ld,"
           " integer %ld, misc/frint %ld, tbl/ext %ld, by element/scalar frint %ld], %ld mismatches\n",
           cases, faults, straddles, undefs, 2 * (per[0] + per[1]), 2 * per[2], 2 * per[3], 2 * per[4], 2 * (per[5] + per[6] + per[7]),
           2 * per[8], 2 * per[9], 2 * per[10], 2 * per[11], fails);
    return fails ? 1 : 0;
}
