/*
 * Unit tests for src/a64_imm.c (logical immediates) and src/a64_cond.c
 * (ARM condition -> x86 condition). Run with `make unit`.
 */
#include "hao/a64_imm.h"
#include "hao/a64_cond.h"
#include "hao/cpu.h"
#include "hao/x86.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------ logical immediates */

typedef struct { unsigned sf, n, immr, imms; int ok; uint64_t val; const char *what; } imm_case;

static const imm_case imm_cases[] = {
    /* 64-bit element */
    { 1, 1,  0, 0x00, 1, 0x0000000000000001ull, "#1" },
    { 1, 1,  0, 0x3e, 1, 0x7fffffffffffffffull, "#0x7fff...ffff (S = 62)" },
    { 1, 1, 63, 0x3e, 1, 0xfffffffffffffffeull, "#~1 (ROR 63)" },
    { 1, 1,  1, 0x00, 1, 0x8000000000000000ull, "#1 << 63 (ROR 1)" },
    { 1, 1, 32, 0x1f, 1, 0xffffffff00000000ull, "#0xffffffff00000000" },
    { 1, 1,  0, 0x3f, 0, 0, "64 ones: reserved" },
    /* 32-bit element */
    { 1, 0,  0, 0x0f, 1, 0x0000ffff0000ffffull, "#0x0000ffff0000ffff" },
    { 1, 0, 16, 0x0f, 1, 0xffff0000ffff0000ull, "AND x, x, #0xffff0000ffff0000" },
    { 1, 0,  0, 0x07, 1, 0x000000ff000000ffull, "#0x000000ff000000ff" },
    { 1, 0,  0, 0x1f, 0, 0, "32 ones in a 32-bit element: reserved" },
    /* 16-, 8-, 4-, 2-bit elements */
    { 1, 0,  8, 0x27, 1, 0xff00ff00ff00ff00ull, "#0xff00ff00ff00ff00" },
    { 1, 0,  0, 0x30, 1, 0x0101010101010101ull, "#0x0101010101010101" },
    { 1, 0,  0, 0x38, 1, 0x1111111111111111ull, "#0x1111111111111111" },
    { 1, 0,  1, 0x39, 1, 0x9999999999999999ull, "#0x9999... (4-bit 0011 ROR 1)" },
    { 1, 0,  0, 0x3c, 1, 0x5555555555555555ull, "#0x5555555555555555" },
    { 1, 0,  1, 0x3c, 1, 0xaaaaaaaaaaaaaaaaull, "#0xaaaaaaaaaaaaaaaa" },
    { 1, 0, 63, 0x3c, 1, 0xaaaaaaaaaaaaaaaaull, "immr bits above the element are ignored" },
    { 1, 0,  0, 0x3d, 0, 0, "2-bit element all ones: reserved" },
    { 1, 0,  0, 0x3b, 0, 0, "4-bit element all ones: reserved" },
    { 1, 0,  0, 0x3e, 0, 0, "N:NOT(imms) = 1, element size 1: reserved" },
    { 1, 0,  0, 0x3f, 0, 0, "N:NOT(imms) = 0: reserved" },
    /* 32-bit operations */
    { 0, 0,  0, 0x1e, 1, 0x000000007fffffffull, "W: #0x7fffffff" },
    { 0, 0,  1, 0x00, 1, 0x0000000080000000ull, "W: #0x80000000" },
    { 0, 0,  8, 0x27, 1, 0x00000000ff00ff00ull, "W: ORR w, wzr, #0xff00ff00" },
    { 0, 0,  0, 0x3c, 1, 0x0000000055555555ull, "W: #0x55555555" },
    { 0, 0,  0, 0x1f, 0, 0, "W: all ones: reserved" },
    { 0, 1,  0, 0x00, 0, 0, "W: N = 1 is unallocated" },
    /* field ranges */
    { 1, 2,  0, 0x00, 0, 0, "N > 1" },
    { 1, 1, 64, 0x00, 0, 0, "immr > 63" },
    { 1, 1,  0, 0x40, 0, 0, "imms > 63" },
};

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void test_imm(void)
{
    int n = 0;
    for (size_t k = 0; k < sizeof imm_cases / sizeof imm_cases[0]; k++) {
        const imm_case *c = &imm_cases[k];
        uint64_t v = 0x0123456789abcdefull;
        bool ok = a64_decode_bitmask_imm(c->sf, c->n, c->immr, c->imms, &v);
        CHECK(ok == (bool)c->ok, "%s: sf=%u N=%u immr=%u imms=%#x returned %d", c->what, c->sf, c->n, c->immr, c->imms, ok);
        if (ok && c->ok) CHECK(v == c->val, "%s: got %#018llx want %#018llx", c->what, (unsigned long long)v, (unsigned long long)c->val);
        if (!ok) CHECK(v == 0x0123456789abcdefull, "%s: *out written on failure", c->what);
        n++;
    }

    /* Whole encoding space: count valid encodings and distinct values.
     * Element size e contributes (e - 1) * 64 encodings (S < e - 1, all 64
     * immr values) and e * (e - 1) distinct values, so 64-bit operations
     * have 7680 encodings / 5334 values and 32-bit ones 3648 / 1302. */
    static uint64_t vals[8192];
    for (unsigned sf = 0; sf < 2; sf++) {
        unsigned nenc = 0, nval = 0;
        for (unsigned bn = 0; bn < 2; bn++)
            for (unsigned immr = 0; immr < 64; immr++)
                for (unsigned imms = 0; imms < 64; imms++) {
                    uint64_t v;
                    if (!a64_decode_bitmask_imm(sf, bn, immr, imms, &v)) continue;
                    nenc++;
                    vals[nval++] = v;
                    uint64_t all = sf ? ~0ull : 0xffffffffull;
                    CHECK(v != 0 && v != all && (v & ~all) == 0, "sf=%u N=%u immr=%u imms=%u -> %#llx", sf, bn, immr, imms, (unsigned long long)v);
                }
        qsort(vals, nval, sizeof vals[0], cmp_u64);
        unsigned distinct = 0;
        for (unsigned k = 0; k < nval; k++) distinct += k == 0 || vals[k] != vals[k - 1];
        CHECK(nenc == (sf ? 7680u : 3648u), "sf=%u: %u valid encodings", sf, nenc);
        CHECK(distinct == (sf ? 5334u : 1302u), "sf=%u: %u distinct values", sf, distinct);
        printf("bitmask sf=%u: %u encodings, %u distinct values\n", sf, nenc, distinct);
    }
    printf("bitmask: %d table cases\n", n);
}

/* Bitfield masks: UBFM/SBFM/BFM computed from wmask/tmask must equal the
 * alias definitions (UBFX/SBFX/BFXIL when imms >= immr, UBFIZ/SBFIZ/BFI
 * otherwise), written directly from the Arm ARM alias descriptions. */
static uint64_t ror_w(uint64_t v, unsigned r, unsigned w)
{
    uint64_t m = w == 64 ? ~0ull : (1ull << w) - 1;
    v &= m; r %= w;
    return r ? ((v >> r) | (v << (w - r))) & m : v;
}
static void test_bitfield(void)
{
    static const uint64_t srcs[] = {0x0123456789abcdefull, 0xfedcba9876543210ull, 0x8000000000000001ull, ~0ull};
    unsigned nenc = 0;
    for (unsigned sf = 0; sf < 2; sf++)
        for (unsigned n = 0; n < 2; n++)
            for (unsigned immr = 0; immr < 64; immr++)
                for (unsigned imms = 0; imms < 64; imms++) {
                    uint64_t wm = 0, tm = 0;
                    bool ok = a64_decode_bitfield(sf, n, immr, imms, &wm, &tm);
                    bool want = n == sf && (sf || (immr < 32 && imms < 32));
                    CHECK(ok == want, "bitfield sf=%u N=%u immr=%u imms=%u: ok=%d", sf, n, immr, imms, ok);
                    if (!ok) continue;
                    nenc++;
                    unsigned W = sf ? 64 : 32;
                    uint64_t M = sf ? ~0ull : 0xffffffffull;
                    for (unsigned k = 0; k < 4; k++) {
                        uint64_t src = srcs[k] & M, dst = srcs[3 - k] & M, u, s, b;
                        uint64_t bot = ror_w(src, immr, W) & wm;
                        uint64_t top = ((src >> imms) & 1) ? M : 0;
                        uint64_t gu = bot & tm, gs = (top & ~tm) | (bot & tm);
                        uint64_t gb = (dst & ~tm) | (((dst & ~wm) | bot) & tm);
                        if (imms >= immr) {                         /* xFX / BFXIL: lsb = immr, width = imms - immr + 1 */
                            unsigned wd = imms - immr + 1;
                            uint64_t f = (src >> immr) & (wd == 64 ? ~0ull : (1ull << wd) - 1);
                            u = f;
                            s = (wd < W && ((f >> (wd - 1)) & 1)) ? (f | (M << wd)) & M : f;
                            b = (dst & ~(wd == 64 ? ~0ull : (1ull << wd) - 1)) | f;
                        } else {                                    /* xFIZ / BFI: lsb = W - immr, width = imms + 1 */
                            unsigned lsb = W - immr, wd = imms + 1;
                            uint64_t f = src & ((1ull << wd) - 1);
                            uint64_t sx = ((f >> (wd - 1)) & 1) ? (f | (~0ull << wd)) : f;
                            u = (f << lsb) & M;
                            s = (sx << lsb) & M;
                            b = (dst & ~(((1ull << wd) - 1) << lsb) & M) | u;
                        }
                        CHECK(gu == u && gs == s && (gb & M) == b, "bitfield sf=%u immr=%u imms=%u src=%#llx: ubfm %#llx/%#llx sbfm %#llx/%#llx bfm %#llx/%#llx",
                              sf, immr, imms, (unsigned long long)src, (unsigned long long)gu, (unsigned long long)u,
                              (unsigned long long)gs, (unsigned long long)s, (unsigned long long)gb, (unsigned long long)b);
                    }
                }
    CHECK(nenc == 64 * 64 + 32 * 32, "bitfield: %u valid encodings", nenc);
    printf("bitfield: %u encodings x 4 sources, UBFM/SBFM/BFM vs alias definitions\n", nenc);
}

/* ------------------------------------------------ conditions */

/* Arm ARM ConditionHolds, written independently of hao_cond. */
static bool arm_holds(unsigned cond, unsigned nzcv)
{
    bool N = nzcv >> 3 & 1, Z = nzcv >> 2 & 1, C = nzcv >> 1 & 1, V = nzcv & 1, r;
    switch (cond >> 1) {
    case 0: r = Z; break;
    case 1: r = C; break;
    case 2: r = N; break;
    case 3: r = V; break;
    case 4: r = C && !Z; break;
    case 5: r = N == V; break;
    case 6: r = N == V && !Z; break;
    default: r = true; break;
    }
    return (cond & 1) && cond != 15 ? !r : r;
}

/* Intel SDM Jcc condition evaluation on an RFLAGS value. */
static bool x86_holds(int cc, uint64_t f)
{
    bool CF = f & 1, ZF = f >> 6 & 1, SF = f >> 7 & 1, OF = f >> 11 & 1, PF = f >> 2 & 1, r;
    switch (cc >> 1) {
    case 0: r = OF; break;
    case 1: r = CF; break;
    case 2: r = ZF; break;
    case 3: r = CF || ZF; break;
    case 4: r = SF; break;
    case 5: r = PF; break;
    case 6: r = SF != OF; break;
    default: r = ZF || SF != OF; break;
    }
    return (cc & 1) ? !r : r;
}

static void test_cond(void)
{
    static const int want[16] = {
        XCC_E, XCC_NE, XCC_AE, XCC_B, XCC_S, XCC_NS, XCC_O, XCC_NO,
        XCC_A, XCC_BE, XCC_GE, XCC_L, XCC_G, XCC_LE, -1, -1,
    };
    for (unsigned cond = 0; cond < 16; cond++) {
        int x = a64_cond_to_xcc(cond);
        CHECK(x == want[cond], "cond %u -> xcc %d, want %d", cond, x, want[cond]);
        if (x < 0) continue;
        for (unsigned nzcv = 0; nzcv < 16; nzcv++) {
            hao_cpu c;
            hao_set_nzcv(&c, nzcv);
            CHECK(hao_nzcv(&c) == nzcv, "nzcv %#x does not round-trip through the flags image", nzcv);
            /* the RFLAGS translated code has after restoring the image (unrelated bits set: they must not matter) */
            uint64_t rf = hao_flags_to_rflags(c.flags, 0x8c1 ^ 0xfffff);
            CHECK(hao_flags_from_rflags(rf) == c.flags, "image %#llx does not round-trip through RFLAGS", (unsigned long long)c.flags);
            CHECK((c.flags & 0xff) <= 1, "image %#llx: low byte is not 0 or 1", (unsigned long long)c.flags);
            CHECK(x86_holds(x, rf) == arm_holds(cond, nzcv),
                  "cond %u nzcv %#x: x86 cc %d says %d, ARM says %d", cond, nzcv, x, x86_holds(x, rf), arm_holds(cond, nzcv));
            CHECK(hao_cond(nzcv, cond) == arm_holds(cond, nzcv), "hao_cond(%#x, %u)", nzcv, cond);
            CHECK(x86_holds(xcc_not(x), rf) == !arm_holds(cond, nzcv), "xcc_not for cond %u", cond);
        }
    }
    CHECK(a64_cond_to_xcc(16) == -1 && a64_cond_to_xcc(~0u) == -1, "out-of-range cond");
    printf("cond: 16 conditions x 16 NZCV values\n");
}

int main(void)
{
    test_imm();
    test_bitfield();
    test_cond();
    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
