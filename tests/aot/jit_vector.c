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
static bool fp_env_check;
static bool x87_unmasked;
static unsigned x87_seed; /* directed system-register checks only */
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
static void on_fault(int s) { if (s == SIGFPE) __asm__ volatile("fnclex"); siglongjmp(jb, 1); }

typedef struct { int rc; bool faulted; hao_cpu c; uint32_t fpsr, fpcr; uint16_t x87_round; uint8_t m[2 * PG]; } outcome;

static void run(outcome *o, const hao_cpu *in, const uint32_t *code, int n, uint64_t pc, bool jit)
{
    static hao_cpu c;
    uint16_t saved_cw = 0;
    if (x87_unmasked) __asm__ volatile("fnstcw %0" : "=m"(saved_cw));
    c = *in;
    memcpy(mem, init_mem, sizeof init_mem);
    a64_fpcr_write(&c, in->fpcr);
    a64_fpsr_write(&c, in->fpsr);
    if (x87_seed) {
        uint32_t env[7];
        __asm__ volatile("fnstenv %0" : "=m"(env));
        env[1] = (env[1] & ~0x3fu) | x87_seed;
        if (x87_unmasked) { env[0] &= ~1u; env[1] |= 0x8080; } /* pending unmasked invalid */
        __asm__ volatile("fldenv %0" : : "m"(env));
    }
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
    __asm__ volatile("fnstcw %0" : "=m"(o->x87_round));
    o->x87_round &= 0x0c00;
    a64_fpsr_write(&c, 0);
    if (x87_unmasked) __asm__ volatile("fldcw %0" : : "m"(saved_cw));
    a64_fpcr_write(&c, 0);
    o->c = c;
    memcpy(o->m, mem, sizeof o->m);
}

static long cases, faults, undefs, fails, straddles;

/* insn (and insn2 after it, if not 0), alone or between CMP x0, x1 and
 * CSEL x2, x0, x1, HI */
static void check(uint32_t insn, uint32_t insn2, const hao_cpu *in, bool live)
{
    /* Mutation runs need one witnessed mismatch; the passing control and
     * ordinary unit runs still exercise the complete corpus. */
    static int fail_fast = -1;
    if (fail_fast < 0) fail_fast = getenv("HAO_VECTOR_FAIL_FAST") != NULL;
    if (fail_fast && fails) { printf("MISMATCH mutation detected after %ld failures\n", fails); exit(1); }
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
    else if ((insn & 0xbf000000u) == 0x0c000000u &&
             (((insn >> 12) & 15) == 0 || ((insn >> 12) & 15) == 4 || ((insn >> 12) & 15) == 8) &&
             memcmp(a.m, b.m, sizeof a.m)) why = "memory"; /* interleaved: exact partial writes */
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
    else if (fp_env_check && a.x87_round != b.x87_round) why = "x87 rounding";
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

/* Integer tail: lane widths, halves, aliases, QC and live NZCV. */
static void integer_tail_case(uint32_t i, unsigned rn, unsigned rm, unsigned k)
{
    static const uint64_t edges[] = {0, ~0ull, 0x8000800080008000ull, 0x8000000080000000ull,
        0x8000000000000000ull, 0x7fff7fff7fff7fffull, 0x7fffffff7fffffffull,
        0x000000ff00000100ull, 0x0000ffff00010000ull, 0x7fffffff80000000ull};
    hao_cpu in; state(&in, rn, rm);
    if (k < 20) for (unsigned r = 0; r < 32; r++) {
        in.v[r].d[0] = edges[(k / 2 + (k & 1 ? r : 0)) % 10];
        in.v[r].d[1] = edges[(k / 2 + (k & 1 ? r + 1 : 0)) % 10];
    }
    in.fpsr = k & 1 ? 0x0800009f : 0;
    for (unsigned live = 0; live < 2; live++) {
        vector_helper_calls = 0; check(i, 0, &in, live);
        if (vector_helper_calls && fails++ < 20)
            printf("MISMATCH integer tail helper %08x: %u calls\n", i, vector_helper_calls);
    }
}
static void integer_tail(void)
{
    for (unsigned q = 0; q < 2; q++) for (unsigned u = 0; u < 2; u++)
        for (unsigned size = 0; size < 3; size++) for (unsigned op = 0; op < 13; op++) {
            if (op > 3 && op != 8 && op != 10 && op != 12) continue;
            for (unsigned k = 0; k < 32; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                integer_tail_case(0x0e200000u | q << 30 | u << 29 | size << 22 | op << 12 | rm << 16 | rn << 5 | rd, rn, rm, k);
            }
        }
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 3; size++)
        for (unsigned form = 0; form < 7; form++) for (unsigned k = 0; k < 32; k++) {
            unsigned op = (unsigned[]){0,0,1,0x12,0x14,0x14,0x13}[form];
            unsigned u = (unsigned[]){0,1,0,1,0,1,1}[form];
            if ((form == 1 && size >= 2) || (form == 2 && size >= 1)) continue;
            unsigned rd = reg(), rn = k & 1 ? rd : reg();
            integer_tail_case(0x0e200800u | q << 30 | u << 29 | size << 22 | op << 12 | rn << 5 | rd, rn, rn, k);
        }
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 4; size++)
        for (unsigned op = 8; op <= 10; op += 2) for (unsigned shift = 0; shift < (8u << size); shift++) {
            if (size == 3 && !q) continue;
            unsigned imm = op == 8 ? (16u << size) - shift - 1 : (8u << size) + shift;
            for (unsigned k = 0; k < 4; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg();
                integer_tail_case(0x2f000400u | q << 30 | imm << 16 | op << 11 | rn << 5 | rd, rn, rn, k + 20);
            }
        }
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 1; size <= 2; size++)
        for (unsigned round = 0; round < 2; round++) for (int idx = -1; idx < (size == 1 ? 8 : 4); idx++)
            for (unsigned k = 0; k < 32; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                if (idx >= 0 && size == 1) rm &= 15;
                uint32_t i = idx < 0 ? 0x0e20b400u | round << 29 | rm << 16 :
                    0x0f00c000u | round << 12 | rm << 16 |
                    (size == 1 ? (unsigned)(idx & 1) << 20 | (unsigned)((idx >> 1) & 1) << 21 | (unsigned)(idx >> 2) << 11 :
                        (unsigned)(idx & 1) << 21 | (unsigned)(idx >> 1) << 11);
                integer_tail_case(i | q << 30 | size << 22 | rn << 5 | rd, rn, rm, k);
            }
    for (unsigned k = 0; k < 64; k++) {
        unsigned rd = reg(), rn = k & 1 ? rd : reg();
        integer_tail_case(0x5ef1b800u | rn << 5 | rd, rn, rn, k);
    }
    for (unsigned sz = 0; sz < 2; sz++) for (unsigned load = 0; load < 2; load++)
        for (unsigned mode = 0; mode < 4; mode++) for (unsigned k = 0; k < 20; k++) {
            hao_cpu in; state(&in, 1, 2);
            in.x[1] = (uintptr_t)mem + (k < 10 ? PG + k : 2 * PG - (k % 3));
            uint32_t i = 0x3c000020u | sz << 30 | load << 22;
            if (mode == 2) i |= 1u << 24; else i |= mode << 10;
            check(i, 0, &in, false); check(i, 0, &in, true);
        }
}

/* Conversion-specific limits: each mode, alias, inactive lane, sticky flag,
 * saturation/NaN decline and half rounding boundary has complete-state checks. */
static void conversion_cases(void)
{
    static const unsigned ops[] = {0,1,4,5,8,9,16,17,24,25,2,3};
    static const uint16_t hs[] = {0,0x8000,1,0x3bff,0x3800,0x3c00,0xbc00,0x3e00,0xc100,0x7bff,0x7c00,0x7d01,0x7e00};
    for (unsigned type = 0; type < 4; type++) for (unsigned sf = 0; sf < 2; sf++)
        for (unsigned op = 0; op < sizeof ops / sizeof *ops; op++) for (unsigned k = 0; k < 32; k++) {
            unsigned rn = k & 1 ? 3 : 31, rd = k & 2 ? rn : 4;
            hao_cpu in; state(&in, rn, rn); in.fpcr = (k & 15) << 22;
            in.v[rn].d[0] = dval();
            if (type == 0) in.v[rn].s[0] = sval();
            if (type == 3) in.v[rn].h[0] = hs[k % (sizeof hs / sizeof *hs)];
            in.x[rn] = k & 4 ? rnd() : (uint64_t)((int)k - 16);
            uint32_t i = 0x1e200000 | type << 22 | sf << 31 | ops[op] << 16 | rn << 5 | rd;
            check(i, 0, &in, false); check(i, 0, &in, true);
        }
    static const unsigned vops[] = {0x1a,0x1b,0x1c,0x3a,0x3b,0x1d};
    for (unsigned scalar = 0; scalar < 2; scalar++) for (unsigned q = 0; q < 2; q++)
        for (unsigned dbl = 0; dbl < 2; dbl++) for (unsigned u = 0; u < 2; u++)
            for (unsigned op = 0; op < sizeof vops / sizeof *vops; op++) for (unsigned k = 0; k < 32; k++) {
                unsigned rn = 3, rd = k & 1 ? 3 : 4;
                hao_cpu in; state(&in, rn, rn); in.fpcr = (k & 15) << 22;
                for (int lane = 0; lane < 2; lane++) in.v[rn].d[lane] = dval();
                if (!dbl) for (int lane = 0; lane < 4; lane++) in.v[rn].s[lane] = sval();
                uint32_t i = (scalar ? 0x5e200800 : 0x0e200800) | q << 30 | u << 29 | dbl << 22 |
                    (vops[op] >> 5) << 23 | (vops[op] & 31) << 12 | rn << 5 | rd;
                check(i, 0, &in, false); check(i, 0, &in, true);
            }
    static const uint32_t halfops[] = {0x0e79d800,0x2e79d800,0x0ef9b800,0x2ef9b800,0x0e79a800,0x2e79a800};
    for (unsigned q = 0; q < 2; q++) for (unsigned op = 0; op < 6; op++) for (unsigned k = 0; k < 64; k++) {
        hao_cpu in; state(&in, 3, 3); in.fpcr = (k & 15) << 22;
        for (int lane = 0; lane < 8; lane++) in.v[3].h[lane] = k & 1 ? (uint16_t)rnd() : hs[(k + lane) % (sizeof hs / sizeof *hs)];
        uint32_t i = halfops[op] | q << 30 | 3 << 5 | (k & 2 ? 3 : 4);
        check(i, 0, &in, false); check(i, 0, &in, true);
    }
    for (unsigned type = 0; type < 4; type++) for (unsigned op = 4; op < 8; op++) for (unsigned k = 0; k < 64; k++) {
        hao_cpu in; state(&in, 3, 3); in.fpcr = (k & 15) << 22;
        in.v[3].d[0] = dval();
        if (type == 0) in.v[3].s[0] = sval();
        if (type == 3) in.v[3].h[0] = k & 1 ? (uint16_t)rnd() : hs[k % (sizeof hs / sizeof *hs)];
        uint32_t i = 0x1e204000 | type << 22 | op << 15 | 3 << 5 | (k & 2 ? 3 : 4);
        check(i, 0, &in, false); check(i, 0, &in, true);
    }
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 4; size++)
        for (unsigned u = 0; u < 2; u++) for (unsigned wide = 0; wide < 2; wide++) for (unsigned k = 0; k < 32; k++) {
            hao_cpu in; state(&in, 3, 3); in.fpcr = (k & 15) << 22;
            for (int lane = 0; lane < 2; lane++) in.v[3].d[lane] = dval();
            if (size == 0) for (int lane = 0; lane < 4; lane++) in.v[3].s[lane] = sval();
            uint32_t i = 0x0e216800 | q << 30 | u << 29 | size << 22 | wide << 12 | 3 << 5 | (k & 1 ? 3 : 4);
            check(i, 0, &in, false); check(i, 0, &in, true);
        }
    /* Saturating after an inexact ROUND must not leak PE: architectural
     * FPToFixed raises IOC alone for these operands. */
    static const uint64_t bad_double[] = {0x41dfffffffe00000ull, 0xbfe0000000000000ull};
    static const uint32_t bad_ops[] = {0x1e600000u, 0x1e710000u};
    for (unsigned k = 0; k < 2; k++) for (unsigned sticky = 0; sticky < 2; sticky++) {
        hao_cpu in; state(&in, 3, 3); in.fpcr = 0; in.fpsr = sticky ? 0x0800009f : 0;
        in.v[3].d[0] = bad_double[k];
        check(bad_ops[k] | 3 << 5 | 4, 0, &in, false);
        check(bad_ops[k] | 3 << 5 | 4, 0, &in, true);
    }
    for (unsigned bit = 0; bit < 2; bit++) {
        hao_cpu in; state(&in, 3, 3); in.fpcr = 1u << (bit ? 26 : 19);
        memset(&in.v[3], 0, sizeof in.v[3]);
        unsigned before = vector_helper_calls;
        check(0x1e23c064, 0, &in, true);
        if (vector_helper_calls == before) { fprintf(stderr, "MISMATCH conversion unsupported control admission\n"); fails++; }
    }
    /* Ordinary inputs must really avoid the helper; late saturation must
     * restore PE raised by earlier fractional lanes before retrying. */
    static const uint32_t inline_ops[] = {0x4ea1b800,0x6ea1b800,0x4e21a800,0x4ea1a800,0x4e21b800,
        0x5e21d800,0x7e21d800,0x4e61d800,0x6e61d800,0x4e79d800,0x6e79d800,0x4ef9b800,0x6ef9b800,
        0x1e240000,0x9e690000,0x1e23c000,0x1ee24000,0x1ee2c000};
    for (unsigned k = 0; k < sizeof inline_ops / sizeof *inline_ops; k++) {
        hao_cpu in; state(&in, 3, 3); in.fpcr = 0;
        memset(&in.v[3], 0, sizeof in.v[3]); in.x[3] = 1;
        unsigned before = vector_helper_calls;
        check(inline_ops[k] | 3 << 5 | 4, 0, &in, true);
        if (vector_helper_calls != before) { fprintf(stderr, "MISMATCH conversion ordinary helper %08x\n", inline_ops[k]); fails++; }
    }
}

/* Half products and sums are exact in D before one integer H rounding. */
static void half_arithmetic_cases(void)
{
    static const uint16_t hv[] = {0,0x8000,1,0x3ff,0x400,0x401,0x3555,0x3800,0x3bff,0x3c00,0x3c01,
        0x3e00,0x4000,0xbc00,0x7bfe,0x7bff,0xfbff,0x7c00,0xfc00,0x7d01,0x7e01};
    static const uint32_t ops[] = {0x0e401400,0x0ec01400,0x2e401c00,0x0e403400,0x0ec03400,0x0e400400,0x0ec00400};
    for (unsigned q = 0; q < 2; q++) for (unsigned op = 0; op < 7; op++) for (unsigned k = 0; k < 256; k++) {
        unsigned rn = 3, rm = k & 1 ? 3 : 17, rd = k & 2 ? rn : k & 4 ? rm : 4;
        hao_cpu in; state(&in, rn, rm); in.fpcr = (k & 15) << 22;
        for (int lane = 0; lane < 8; lane++) {
            in.v[rn].h[lane] = k & 16 ? (uint16_t)rnd() : hv[(k + lane) % (sizeof hv / sizeof *hv)];
            in.v[rm].h[lane] = k & 32 ? (uint16_t)rnd() : hv[(k / 3 + lane) % (sizeof hv / sizeof *hv)];
        }
        uint32_t i = ops[op] | q << 30 | rm << 16 | rn << 5 | rd;
        check(i, 0, &in, false); check(i, 0, &in, true);
    }
    for (unsigned q = 0; q < 2; q++) for (unsigned idx = 0; idx < 8; idx++) for (unsigned k = 0; k < 64; k++) {
        unsigned rn = 17, rm = 3, rd = k & 1 ? rn : k & 2 ? rm : 4;
        hao_cpu in; state(&in, rn, rm); in.fpcr = (k & 15) << 22;
        for (int lane = 0; lane < 8; lane++) {
            in.v[rn].h[lane] = hv[(k + lane) % (sizeof hv / sizeof *hv)];
            in.v[rm].h[lane] = hv[(k / 3 + lane) % (sizeof hv / sizeof *hv)];
        }
        uint32_t i = 0x0f009000 | q << 30 | ((idx >> 2) & 1) << 11 | ((idx >> 1) & 1) << 21 |
                     (idx & 1) << 20 | rm << 16 | rn << 5 | rd;
        check(i, 0, &in, false); check(i, 0, &in, true);
    }
    static const unsigned scalar_ops[] = {0,2,3,4,5,6,7};
    for (unsigned op = 0; op < 7; op++) for (unsigned k = 0; k < 128; k++) {
        hao_cpu in; state(&in, 3, 17); in.fpcr = (k & 15) << 22;
        in.v[3].h[0] = hv[k % (sizeof hv / sizeof *hv)];
        in.v[17].h[0] = hv[(k / 3) % (sizeof hv / sizeof *hv)];
        uint32_t i = 0x1ee00800 | scalar_ops[op] << 12 | 17 << 16 | 3 << 5 | (k & 1 ? 3 : 4);
        check(i, 0, &in, false); check(i, 0, &in, true);
    }
    for (unsigned op = 0; op < 7; op++) for (unsigned control = 0; control < 16; control++) {
        hao_cpu in; state(&in, 3, 17); in.fpcr = control << 22;
        for (int lane = 0; lane < 8; lane++) { in.v[3].h[lane] = lane & 1 ? 0x3c01 : 0x400; in.v[17].h[lane] = lane & 1 ? 0x3bff : 0x400; }
        unsigned before = vector_helper_calls;
        check(ops[op] | 1u << 30 | 17 << 16 | 3 << 5 | 4, 0, &in, true);
        if (vector_helper_calls != before) { fprintf(stderr, "MISMATCH half arithmetic ordinary helper\n"); fails++; }
    }
}

/* FNSTCW is non-waiting, whereas FLDCW can deliver a pending unmasked
 * x87 exception. Rewriting unchanged FPCR must follow x87_rc_set's skip
 * path; native long-double code can leave this pending state. */
static void fpcr_pending_exception(void)
{
    hao_cpu warm; state(&warm, 1, 2); check(0xd503201f, 0, &warm, false);
    x87_unmasked = true; x87_seed = 1; fp_env_check = true;
    for (unsigned mode = 0; mode < 4; mode++) for (unsigned live = 0; live < 2; live++) {
        hao_cpu in; state(&in, 1, 2); in.fpcr = mode << 22; in.x[17] = in.fpcr;
        check(0xd51b4411, 0, &in, live);
    }
    x87_unmasked = false; x87_seed = 0; fp_env_check = false;
}

static void fp_system_tail(void)
{
    fp_env_check = true;
    for (unsigned cond = 0; cond < 16; cond++) for (unsigned flags = 0; flags < 16; flags++)
        for (unsigned dbl = 0; dbl < 2; dbl++) for (unsigned signal = 0; signal < 2; signal++)
            for (unsigned k = 0; k < 8; k++) {
                hao_cpu in; state(&in, 1, 2); hao_set_nzcv(&in, flags);
                if (k < 4) { in.v[1].df[0] = k == 0 ? -0.0 : 1.0; in.v[2].df[0] = k == 1 ? -1.0 : 2.0;
                    if (!dbl) { in.v[1].s[0] = (uint32_t[]){0x80000000,0x3f800000,0x40000000,0x40400000}[k]; in.v[2].s[0] = 0x40000000; }
                } else {
                    in.v[1].d[0] = dbl ? (uint64_t[]){1,0x7ff0000000000000ull,0x7ff8000000000001ull,0x7ff0000000000001ull}[k-4] :
                        (uint32_t[]){1,0x7f800000,0x7fc00001,0x7f800001}[k-4];
                }
                in.fpcr = ((cond + flags + k) & 15) << 22;
                uint32_t i = 0x1e220420u | dbl << 22 | cond << 12 | signal << 4 | ((flags + 3) & 15);
                vector_helper_calls = 0; check(i, 0, &in, false);
                if (k < 4 && vector_helper_calls && fails++ < 20) printf("MISMATCH FCCMP ordinary helper %08x\n", i);
                check(i, 0, &in, true);
            }
    for (unsigned kind = 0; kind < 4; kind++) for (unsigned rtidx = 0; rtidx < 3; rtidx++)
        for (unsigned k = 0; k < 128; k++) {
            unsigned rt = (unsigned[]){0,17,31}[rtidx];
            hao_cpu in; state(&in, 1, 2);
            in.x[rt] = k < 32 ? 1ull << k : rnd();
            in.fpcr = (k & 15) << 22; in.fpsr = k & 1 ? 0x0800009f : 0;
            x87_seed = (k >> 1) & 0x3d;
            uint32_t i = (uint32_t[]){0xd53b4400,0xd51b4400,0xd53b4420,0xd51b4420}[kind] | rt;
            for (unsigned live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (vector_helper_calls && fails++ < 20) printf("MISMATCH FP system helper %08x\n", i);
            }
        }
    x87_seed = 0;
    fp_env_check = false;
}

static void estimate_tail(void)
{
    for (unsigned root = 0; root < 2; root++) for (unsigned dbl = 0; dbl < 2; dbl++)
        for (unsigned form = 0; form < 3; form++) {
            bool scalar = form == 2; unsigned q = form != 0;
            if (dbl && !q) continue;
            uint32_t op = (scalar ? 0x5ea1d800u : 0x0ea1d800u) | q << 30 | root << 29 | dbl << 22;
            unsigned mbits = dbl ? 52 : 23, maxexp = dbl ? 2047 : 255;
            for (unsigned idx = 0; idx < 256; idx++) for (unsigned parity = 0; parity < 2; parity++)
                for (unsigned edge = 0; edge < 2; edge++) {
                    unsigned rd = reg(), rn = idx & 1 ? rd : reg();
                    hao_cpu in; state(&in, rn, rn); in.fpcr = (idx & 15) << 22;
                    uint64_t bits = (uint64_t)(maxexp / 2 - 1 + parity) << mbits | (uint64_t)idx << (mbits - 8);
                    if (edge) bits |= (1ull << (mbits - 8)) - 1;
                    if (!root && (idx & 2)) bits |= 1ull << (dbl ? 63 : 31);
                    for (unsigned lane = 0; lane < (dbl ? 2 : 4); lane++) {
                        if (dbl) in.v[rn].d[lane] = bits; else in.v[rn].s[lane] = (uint32_t)bits;
                    }
                    for (unsigned live = 0; live < 2; live++) {
                        vector_helper_calls = 0; check(op | rn << 5 | rd, 0, &in, live);
                        if (vector_helper_calls && fails++ < 20) printf("MISMATCH estimate ordinary helper %08x\n", op);
                    }
                }
            for (unsigned control = 0; control < 16; control++) for (unsigned k = 0; k < 24; k++) {
                hao_cpu in; state(&in, 1, 2); in.fpcr = control << 22;
                unsigned exp = (unsigned[]){0,1,maxexp-3,maxexp-2,maxexp-1,maxexp}[k / 4];
                uint64_t bits = (uint64_t)exp << mbits | (k & 1 ? 1 : 0);
                if (k & 2) bits |= 1ull << (dbl ? 63 : 31);
                for (unsigned lane = 0; lane < (dbl ? 2 : 4); lane++) {
                    if (dbl) in.v[1].d[lane] = bits; else in.v[1].s[lane] = (uint32_t)bits;
                }
                check(op | 1u << 5, 0, &in, false); check(op | 1u << 5, 0, &in, true);
            }
            /* A later special lane must decline without an earlier store. */
            if (!scalar) {
                hao_cpu in; state(&in, 1, 2);
                if (dbl) { in.v[1].df[0] = 1; in.v[1].d[1] = 1; }
                else { for (int lane=0; lane<4; lane++) in.v[1].f[lane]=1; in.v[1].s[q ? 3 : 1]=1; }
                check(op | 1u << 5 | 1u, 0, &in, true);
            }
        }
}

int main(void)
{
    mem = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED || mprotect(mem + 2 * PG, PG, PROT_NONE)) return 2;
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    signal(SIGFPE, on_fault);
    for (unsigned k = 0; k < sizeof init_mem; k++) init_mem[k] = (uint8_t)rnd();
    /* Scalar fused signs/addend selection and fused reciprocal steps. */
    static const uint32_t tail_fused[] = {0x1f000000, 0x1f008000, 0x1f200000, 0x1f208000,
        0x1e208800, 0x0e20fc00, 0x4e20fc00, 0x0ea0fc00, 0x4ea0fc00, 0x5e20fc00, 0x5ea0fc00};
    for (unsigned ctl = 0; ctl < 16; ctl++) for (unsigned f = 0; f < 11; f++)
        for (unsigned k = 0; k < 160; k++) {
            unsigned rd = reg(), rn = reg(), rm = reg(), ra = reg();
            hao_cpu in; state(&in, rn, rm); in.fpcr = ctl << 22;
            for (int lane = 0; lane < 4; lane++) {
                fma_ops o = fma_case(&seed, k % FC_N);
                in.v[ra].s[lane] = o.c; in.v[rn].s[lane] = o.a; in.v[rm].s[lane] = o.b;
            }
            if (!k) for (unsigned r = 0; r < 32; r++) for (int lane = 0; lane < 4; lane++) in.v[r].s[lane] = 0x3f000000;
            uint32_t i = tail_fused[f] | rn << 5 | rm << 16 | rd | (f < 4 ? ra << 10 : 0);
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (!k && vector_helper_calls) { if (fails++ < 20) printf("MISMATCH tail fused admission %08x\n", i); }
            }
        }


    fpcr_pending_exception();

    estimate_tail();
    fp_system_tail();
    integer_tail();

    conversion_cases();

    /* Interleaved structures: every arrangement, post-index form, wrapped
     * register lists and every possible partial-access boundary. Loads must
     * publish no vector until the complete transfer has succeeded. */
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 4; size++)
        for (unsigned regs = 2; regs <= 4; regs++) for (unsigned load = 0; load < 2; load++)
            for (unsigned post = 0; post < 3; post++) for (unsigned k = 0; k < 68; k++) {
                unsigned rn = k & 1 ? 31 : 7, rt = k & 2 ? 31 : 3;
                unsigned opc = regs == 2 ? 8 : regs == 3 ? 4 : 0;
                unsigned rm = post == 2 ? (k & 4 ? rn : 6) : post ? 31 : 0;
                uint32_t i = 0x0c000000u | q << 30 | (post != 0) << 23 | load << 22 |
                             rm << 16 | opc << 12 | size << 10 | rn << 5 | rt;
                hao_cpu in; state(&in, rn, rn);
                in.x[6] = (uint64_t)(int64_t)(k - 32);
                in.x[rn] = (uint64_t)(uintptr_t)(mem + (k < 65 ? 2 * PG - k : PG + k));
                unsigned before = vector_helper_calls;
                check(i, 0, &in, false); check(i, 0, &in, true);
                if ((q || size != 3) && (load || size == 0 || k >= 65) && vector_helper_calls != before) {
                    printf("MISMATCH %08x: interleaved structure unexpectedly used helper\n", i); fails++;
                }
            }
    /* Double arithmetic and S/D square roots: all controls, aliases,
     * nonfinite/tiny inputs, exact zeros, saturation boundaries and declines. */
    static const uint32_t wide_forms[] = {0x4e60d400, 0x4ee0d400, 0x6e60dc00, 0x6e60fc00,
                                         0x6ee1f800, 0x6ea1f800, 0x2ea1f800};
    for (unsigned f = 0; f < 7; f++) for (unsigned control = 0; control < 16; control++)
        for (unsigned k = 0; k < 192; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
            hao_cpu in; state(&in, rn, rm); in.fpcr = control << 22;
            bool dbl = f < 5;
            for (int r = 0; r < 32; r++) {
                if (dbl) for (int e = 0; e < 2; e++) in.v[r].d[e] = dval();
                else for (int e = 0; e < 4; e++) in.v[r].s[e] = sval();
            }
            if (k < 4) {
                for (int e = 0; e < (dbl ? 2 : 4); e++) {
                    if (dbl) { in.v[rn].d[e] = k == 1 ? 0 : k == 2 ? 0x0010000000000000ull : 0x4010000000000000ull;
                               in.v[rm].d[e] = k == 1 ? 0 : k == 2 ? 0x4000000000000000ull : 0x4010000000000000ull; }
                    else { in.v[rn].s[e] = k == 1 ? 0 : k == 2 ? 1 : 0x40800000; }
                }
            }
            uint32_t i = wide_forms[f] | rn << 5 | rd | (f < 4 ? rm << 16 : 0);
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (k == 0 && vector_helper_calls) {
                    if (fails++ < 20) printf("MISMATCH %08x: wide ordinary arithmetic used helper\n", i);
                }
                /* An exact subnormal division result requires fallback even
                 * when no architectural bit changes after removing admission. */
                if (k == 2 && f == 3 && rn != rm && vector_helper_calls != 1) {
                    if (fails++ < 20) printf("MISMATCH %08x: wide tiny result bypassed helper\n", i);
                }
            }
            if (k % 8 == 0) check(i, 0x4e20d400u | rd << 16 | rn << 5 | rd, &in, true);
        }

    /* FABD rounds subtraction before taking absolute value; inactive lanes
     * must not affect FPSR. Exercise all controls, aliases and sticky flags. */
    static const uint32_t abd_forms[] = {0x2ea0d400, 0x6ea0d400, 0x6ee0d400, 0x7ea0d400, 0x7ee0d400};
    for (unsigned f = 0; f < 5; f++) for (unsigned ctl = 0; ctl < 16; ctl++)
        for (unsigned k = 0; k < 192; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
            hao_cpu in; state(&in, rn, rm); in.fpcr = ctl << 22;
            bool dbl = f == 2 || f == 4;
            for (unsigned r = 0; r < 32; r++) {
                if (dbl) for (int e = 0; e < 2; e++) in.v[r].d[e] = dval();
                else for (int e = 0; e < 4; e++) in.v[r].s[e] = sval();
            }
            if (k < 4) {
                for (int e = 0; e < (dbl ? 2 : 4); e++) {
                    if (dbl) { in.v[rn].d[e] = 0x3ff0000000000000ull; in.v[rm].d[e] = 0x4000000000000000ull; }
                    else { in.v[rn].s[e] = 0x3f800000; in.v[rm].s[e] = 0x40000000; }
                }
            }
            uint32_t i = abd_forms[f] | rn << 5 | rm << 16 | rd;
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (k < 4 && vector_helper_calls) {
                    if (fails++ < 20) printf("MISMATCH %08x: ordinary FABD used helper\n", i);
                }
            }
        }

    /* Saturating add/sub: boundary bits, all sizes, scalar and both vector
     * widths; aliasing, upper clearing, live flags and preexisting QC. */
    for (unsigned scalar = 0; scalar < 2; scalar++) for (unsigned q = 0; q < 2; q++)
        for (unsigned u = 0; u < 2; u++) for (unsigned sub = 0; sub < 2; sub++)
            for (unsigned size = 0; size < 4; size++) for (unsigned k = 0; k < 96; k++) {
                if (scalar && !q) continue;
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                hao_cpu in; state(&in, rn, rm);
                uint64_t sign = 1ull << ((8u << size) - 1);
                uint64_t vals[] = {0, 1, sign - 1, sign, sign + 1, ~0ull, ~1ull};
                unsigned es = 1u << size;
                for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16 / es; e++) {
                    uint64_t v = k < 49 ? vals[r == rm ? k / 7 : k % 7] : rnd();
                    memcpy(&in.v[r].b[e * es], &v, es);
                }
                uint32_t i = (scalar ? 0x5e200c00 : 0x0e200c00) | q << 30 | u << 29 |
                             size << 22 | sub << 13 | rm << 16 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0; check(i, 0, &in, live);
                    if ((scalar || size != 3 || q) && vector_helper_calls) {
                        if (fails++ < 20) printf("MISMATCH %08x: saturating add/sub used helper\n", i);
                    }
                }
            }

    /* Absolute differences: signed extreme distance, unsigned order,
     * accumulation wraparound, high-half selection and every register alias. */
    for (unsigned lng = 0; lng < 2; lng++) for (unsigned q = 0; q < 2; q++)
        for (unsigned u = 0; u < 2; u++) for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned size = 0; size < 3; size++) for (unsigned k = 0; k < 96; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                hao_cpu in; state(&in, rn, rm);
                unsigned es = 1u << size; uint64_t sign = 1ull << (8 * es - 1);
                uint64_t vals[] = {0, 1, sign - 1, sign, sign + 1, ~0ull, ~1ull};
                for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16 / es; e++) {
                    uint64_t v = k < 49 ? vals[r == rm ? k / 7 : k % 7] : rnd();
                    memcpy(&in.v[r].b[e * es], &v, es);
                }
                uint32_t i = (lng ? 0x0e200000 | (acc ? 5 : 7) << 12 : 0x0e200400 | (14 + acc) << 11) |
                             q << 30 | u << 29 | size << 22 | rm << 16 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0; check(i, 0, &in, live);
                    if (vector_helper_calls && fails++ < 20) printf("MISMATCH %08x: absolute difference used helper\n", i);
                }
            }

    /* High narrowing arithmetic: wide carry/borrow, rounding carry,
     * source aliases and preservation of the destination's low half. */
    for (unsigned q = 0; q < 2; q++) for (unsigned round = 0; round < 2; round++)
        for (unsigned sub = 0; sub < 2; sub++) for (unsigned size = 0; size < 3; size++)
            for (unsigned k = 0; k < 96; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                hao_cpu in; state(&in, rn, rm);
                unsigned es = 2u << size; uint64_t half = 1ull << (4 * es - 1);
                uint64_t vals[] = {0, 1, half - 1, half, half + 1, ~0ull, ~1ull};
                for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16 / es; e++) {
                    uint64_t v = k < 49 ? vals[r == rm ? k / 7 : k % 7] : rnd();
                    memcpy(&in.v[r].b[e * es], &v, es);
                }
                uint32_t i = 0x0e204000 | q << 30 | round << 29 | size << 22 | sub << 13 |
                             rm << 16 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0; check(i, 0, &in, live);
                    if (vector_helper_calls && fails++ < 20) printf("MISMATCH %08x: high narrowing used helper\n", i);
                }
            }

    /* ADDP pair ordering, wraparound, high registers and source aliases. */
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 4; size++)
        for (unsigned k = 0; k < 128; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
            hao_cpu in; state(&in, rn, rm);
            for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16; e++)
                in.v[r].b[e] = k < 8 ? (uint8_t)(r * 17 + e * (k + 1)) : (uint8_t)rnd();
            uint32_t i = 0x0e20bc00 | q << 30 | size << 22 | rm << 16 | rn << 5 | rd;
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if ((size != 3 || q) && vector_helper_calls && fails++ < 20)
                    printf("MISMATCH %08x: vector ADDP used helper\n", i);
            }
        }

    /* Indexed vector double FMUL: both D lanes, high Vm, all aliases,
     * inactive special inputs, tiny results, controls and live NZCV. */
    for (unsigned idx = 0; idx < 2; idx++) for (unsigned ctl = 0; ctl < 16; ctl++)
        for (unsigned k = 0; k < 192; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
            hao_cpu in; state(&in, rn, rm); in.fpcr = ctl << 22;
            for (unsigned r = 0; r < 32; r++) for (int e = 0; e < 2; e++) in.v[r].d[e] = dval();
            if (k < 4) {
                in.v[rn].d[0] = in.v[rn].d[1] = 0x4000000000000000ull;
                in.v[rm].d[idx] = 0x3fe0000000000000ull;
                if (rn != rm) in.v[rm].d[1 - idx] = 0x7ff0000000000001ull;
            }
            uint32_t i = 0x4fc09000 | idx << 11 | rm << 16 | rn << 5 | rd;
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (k < 4 && vector_helper_calls && fails++ < 20)
                    printf("MISMATCH %08x: indexed double FMUL used helper\n", i);
            }
            if (k < 8) { check(i | 1u << 21, 0, &in, true); check(i & ~(1u << 30), 0, &in, true); }
        }

    /* Double compares, register and zero forms: all controls, NaNs,
     * subnormals, signed zeros, register aliases and helper admission. */
    static const uint32_t cmpd_forms[] = {0x4e60e400, 0x6e60e400, 0x6ee0e400,
        0x4ee0c800, 0x6ee0c800, 0x4ee0d800, 0x6ee0d800, 0x4ee0e800};
    for (unsigned f = 0; f < 8; f++) for (unsigned ctl = 0; ctl < 16; ctl++)
        for (unsigned k = 0; k < 128; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
            hao_cpu in; state(&in, rn, rm); in.fpcr = ctl << 22;
            for (unsigned r = 0; r < 32; r++) for (int e = 0; e < 2; e++) in.v[r].d[e] = dval();
            if (k < 4) {
                in.v[rn].d[0] = 0x3ff0000000000000ull; in.v[rn].d[1] = 0xbff0000000000000ull;
                in.v[rm].d[0] = 0x4000000000000000ull; in.v[rm].d[1] = 0x8000000000000000ull;
            }
            uint32_t i = cmpd_forms[f] | rn << 5 | rd | (f < 3 ? rm << 16 : 0);
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (k < 4 && vector_helper_calls && fails++ < 20)
                    printf("MISMATCH %08x: double compare used helper\n", i);
            }
        }

    /* Signed, unsigned and mixed byte products; all selected four-byte
     * groups, matrix rows/columns, accumulator wraparound and aliases. */
    static const uint32_t dot_forms[] = {0x0e809400, 0x2e809400, 0x0e809c00,
        0x0f80e000, 0x2f80e000, 0x0f80f000, 0x0f00f000,
        0x0e80a400, 0x2e80a400, 0x0e80ac00};
    for (unsigned f = 0; f < 10; f++) for (unsigned q = 0; q < 2; q++)
        for (unsigned idx = 0; idx < (f >= 3 && f < 7 ? 4u : 1u); idx++)
            for (unsigned k = 0; k < 128; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), rm = k & 2 ? rn : reg();
                hao_cpu in; state(&in, rn, rm);
                static const uint8_t vals[] = {0, 1, 127, 128, 129, 254, 255};
                for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16; e++)
                    in.v[r].b[e] = k < 49 ? vals[r == rm ? k / 7 : k % 7] : (uint8_t)rnd();
                uint32_t i = dot_forms[f] | q << 30 | (idx / 2) << 11 | (idx % 2) << 21 |
                             rm << 16 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0; check(i, 0, &in, live);
                    if ((f < 7 || q) && vector_helper_calls && fails++ < 20)
                        printf("MISMATCH %08x: dot/matrix product used helper\n", i);
                }
            }

    /* Every immediate narrowing shift, both halves and scalar saturation.
     * The values straddle saturation and rounding boundaries, including
     * unsigned 64-bit carry and inactive lanes that would otherwise set QC. */
    for (unsigned scalar = 0; scalar < 2; scalar++) for (unsigned q = scalar; q < 2; q++)
        for (unsigned u = 0; u < 2; u++) for (unsigned op = 0x10; op <= 0x13; op++)
            for (unsigned size = 0; size < 4; size++)
                for (unsigned sh = 1; sh <= (8u << size); sh++) for (unsigned k = 0; k < 16; k++) {
                    unsigned rd = reg(), rn = k & 1 ? rd : reg(), bits = 8u << size;
                    hao_cpu in; state(&in, rn, rn);
                    unsigned es = size < 3 ? 2u << size : 8;
                    uint64_t max = bits == 64 ? ~0ull : (1ull << bits) - 1, sign = 1ull << (es * 8 - 1);
                    unsigned shift = sh & 63;
                    uint64_t half = 1ull << (sh - 1);
                    uint64_t vals[] = {0, 1, ~0ull, sign - 1, sign, sign + 1,
                        half - 1, half, half + 1, max << shift, (max << shift) + half,
                        (max + 1) << shift, (max >> 1) << shift, ((max >> 1) << shift) + half,
                        -(1ull << (bits - 1)) << shift, rnd()};
                    for (unsigned e = 0; e < 16 / es; e++) {
                        uint64_t v = vals[(k + e) % 16]; memcpy(&in.v[rn].b[e * es], &v, es);
                    }
                    uint32_t i = (scalar ? 0x5f000400u : 0x0f000400u) | q << 30 | u << 29 |
                                 (2 * bits - sh) << 16 | op << 11 | rn << 5 | rd;
                    for (int live = 0; live < 2; live++) {
                        vector_helper_calls = 0; check(i, 0, &in, live);
                        if (size < 3 && !(scalar && op < 0x12 && !u) && vector_helper_calls && fails++ < 20)
                            printf("MISMATCH %08x: immediate narrowing used helper\n", i);
                    }
                }

    /* All integer reductions and reserved neighbours. Negative-only
     * maxima and positive-only minima must never see padded zero lanes. */
    static const unsigned reduce_ops[] = {3, 0x0a, 0x1a, 0x1b, 2, 4, 0x0b, 0x19};
    for (unsigned f = 0; f < 8; f++) for (unsigned q = 0; q < 2; q++)
        for (unsigned u = 0; u < 2; u++) for (unsigned size = 0; size < 4; size++)
            for (unsigned k = 0; k < 128; k++) {
                unsigned rd = reg(), rn = k & 1 ? rd : reg(), es = 1u << size;
                hao_cpu in; state(&in, rn, rn);
                uint64_t sign = 1ull << (8 * es - 1), vals[] = {0, 1, sign - 1, sign, sign + 1, ~0ull, ~1ull};
                for (unsigned e = 0; e < 16 / es; e++) {
                    uint64_t v = k < 7 ? vals[k] : k < 14 ? vals[(k + e) % 7] : rnd();
                    memcpy(&in.v[rn].b[e * es], &v, es);
                }
                uint32_t i = 0x0e300800u | q << 30 | u << 29 | size << 22 | reduce_ops[f] << 12 | rn << 5 | rd;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0; check(i, 0, &in, live);
                    if (f < 4 && !(f == 3 && u) && size < 3 && (size != 2 || q) && vector_helper_calls && fails++ < 20)
                        printf("MISMATCH %08x: integer reduction used helper\n", i);
                }
            }

    half_arithmetic_cases();

    /* Every AES round/column transform, high registers and aliases.
     * Size/op neighbours must remain undefined. */
    for (unsigned op = 3; op <= 8; op++) for (unsigned size = 0; size < 4; size++)
        for (unsigned k = 0; k < 128; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg();
            hao_cpu in; state(&in, rn, rn);
            for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16; e++) in.v[r].b[e] = (uint8_t)rnd();
            uint32_t i = 0x4e280800u | op << 12 | size << 22 | rn << 5 | rd;
            check(i, 0, &in, false); check(i, 0, &in, true);
        }
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
    static const uint32_t zero_forms[] = {0x0ea0c800, 0x2ea0c800, 0x0ea0d800, 0x2ea0d800, 0x0ea0e800};
    for (unsigned control = 0; control < 16; control++) for (unsigned f = 0; f < 5; f++)
        for (unsigned q = 0; q < 2; q++) for (unsigned k = 0; k < 128; k++) {
            unsigned rd = reg(), rn = k & 1 ? rd : reg();
            hao_cpu in; state(&in, rn, rn); in.fpcr = control << 22;
            if (k < 2) for (int e = 0; e < 4; e++) in.v[rn].s[e] = k ? 1 : (e & 1 ? 0x80000000 : 0);
            uint32_t i = zero_forms[f] | q << 30 | rn << 5 | rd;
            for (int live = 0; live < 2; live++) {
                vector_helper_calls = 0; check(i, 0, &in, live);
                if (k < 2 && vector_helper_calls != k) {
                    if (fails++ < 20) printf("MISMATCH zero comparison admission %08x: %u helper calls\n", i, vector_helper_calls);
                }
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
    /* A minimum subnormal product disappears completely in RN binary64
     * beside a large exact half addend. Round-to-odd must retain IXC even
     * though the final half value is unchanged. Every active lane has this
     * shape, so unrelated inexact lanes cannot hide a missing flag. */
    for (unsigned ctl = 0; ctl < 16; ctl++) for (unsigned q = 0; q < 2; q++)
        for (unsigned sub = 0; sub < 2; sub++) for (unsigned idx = 0; idx < 8; idx++)
            for (unsigned signs = 0; signs < 4; signs++) for (unsigned sticky = 0; sticky < 2; sticky++) {
                hao_cpu in; state(&in, 17, 15); in.fpcr = ctl << 22;
                in.fpsr = sticky ? 0x0800009f : 0;
                for (unsigned l = 0; l < 8; l++) {
                    in.v[17].h[l] = l < (q ? 8u : 4u) ? 1 | ((signs & 1) << 15) : 0x7c01;
                    in.v[31].h[l] = l < (q ? 8u : 4u) ? 0x7bff | ((signs >> 1) << 15) : 0x7c01;
                    in.v[15].h[l] = 0x7c01;
                }
                in.v[15].h[idx] = 1;
                uint32_t i = 0x0f001000u | q << 30 | sub << 14 | (idx >> 2) << 11 |
                             ((idx >> 1) & 1) << 21 | (idx & 1) << 20 | 15 << 16 | 17 << 5 | 31;
                for (int live = 0; live < 2; live++) {
                    vector_helper_calls = 0; check(i, 0, &in, live);
                    if (vector_helper_calls != (unsigned)((ctl & 3) != 0) && fails++ < 20)
                        printf("MISMATCH %08x: subnormal product admission\n", i);
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
                     * lane loses binary64 precision: RN rounds to odd, directed modes roll back. */
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
                    bool finite = true;
                    for (unsigned lane = 0; lane < (q ? 8u : 4u); lane++)
                        finite &= (in.v[rd].h[lane] & 0x7c00) != 0x7c00 &&
                                  (in.v[rn].h[lane] & 0x7c00) != 0x7c00 &&
                                  (in.v[rm].h[idx < 0 ? lane : (unsigned)idx] & 0x7c00) != 0x7c00;
                    if ((k == 22 && vector_helper_calls != (unsigned)(control >= 16 || (control & 3))) ||
                        (control < 16 && !(control & 3) && finite && vector_helper_calls) ||
                        (k == 0 && control < 16 && vector_helper_calls != 0)) {
                        if (fails++ < 20) printf("MISMATCH half admission %08x fpcr %x: %u helper calls\n", insn, in.fpcr, vector_helper_calls);
                    }
                    /* Adjacent FP32 code must rebuild its own constants. */
                    if (k % 8 == 0) check(insn, 0x4e20d400u | rd << 16 | rn << 5 | rd, &in, live);
                }
            }
    /* A maximum-length block must fit the emitter's scratch/cache budget,
     * including both pairs of the largest fused translation. */
    for (unsigned form = 0; form < 8; form++) {
        hao_cpu in;
        state(&in, 1, 2);
        for (unsigned r = 0; r < 3; r++)
            for (unsigned lane = 0; lane < 4; lane++) in.v[r].f[lane] = 0.25f;
        if (form >= 2) for (unsigned r = 0; r < 3; r++)
            for (unsigned l = 0; l < 8; l++) in.v[r].h[l] = 0x3400;
        if (form >= 6) in.x[7] = (uint64_t)(uintptr_t)(mem + PG);
        uint32_t code[98];
        code[0] = 0xeb01001fu;
        for (unsigned k = 1; k <= 96; k++) code[k] = (uint32_t[]){0x4e22cc20u, 0x4fa21820u, 0x4e420c20u, 0x4f321820u, 0x4ec20c20u, 0x4f325820u, 0x4c4000e0u, 0x4c0000e0u}[form];
        if (form >= 2) code[0] = code[1]; /* first block: 96 half expansions */
        code[97] = 0x9a818002u;
        static outcome a, b;
        run(&a, &in, code, 98, 0x7400000000ull, false);
        run(&b, &in, code, 98, 0x7400000000ull, true);
        if (a.faulted || b.faulted || a.rc != b.rc || a.c.pc != b.c.pc ||
            memcmp(a.c.x, b.c.x, sizeof a.c.x) || memcmp(a.c.v, b.c.v, sizeof a.c.v) ||
            hao_nzcv(&a.c) != hao_nzcv(&b.c) || a.fpsr != b.fpsr || a.fpcr != b.fpcr ||
            memcmp(a.m, b.m, sizeof a.m)) {
            printf("MISMATCH maximum vector block form %u: rc %d/%d\n", form, a.rc, b.rc);
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
    /* FEAT_AFP's FIZ, AH and NEP (FPCR bits 0-2) are RES0: hao neither
     * advertises nor models them, so MSR FPCR drops them in the interpreter
     * and in the JIT's inline FPCR write alike (a64_simd.c FPCR_WRITABLE). */
    for (int jit = 0; jit < 2; jit++) {
        hao_cpu in;
        memset(&in, 0, sizeof in);
        in.x[0] = 0x07ff9f07;                         /* every writable bit, AFP's included */
        const uint32_t msr = 0xD51B4400u;             /* msr fpcr, x0 */
        outcome o;
        run(&o, &in, &msr, 1, 0x100000, jit);
        if (o.fpcr & 7) { printf("MISMATCH %s msr fpcr kept FEAT_AFP bits: fpcr %x\n", jit ? "jit" : "interp", o.fpcr); fails++; }
    }
    printf("jit vector: %ld cases (%ld faulting, %ld of them stores straddling the page end; %ld undefined) [fp %ld, by element %ld, misc %ld, shift %ld, ld/st %ld,"
           " integer %ld, misc/frint %ld, tbl/ext %ld, by element/scalar frint %ld], %ld mismatches\n",
           cases, faults, straddles, undefs, 2 * (per[0] + per[1]), 2 * per[2], 2 * per[3], 2 * per[4], 2 * (per[5] + per[6] + per[7]),
           2 * per[8], 2 * per[9], 2 * per[10], 2 * per[11], fails);
    return fails ? 1 : 0;
}
