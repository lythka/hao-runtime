/* Exhaust every finite binary16 interval: endpoints and the double values
 * immediately below/at/above its midpoint, both signs and all four modes.
 * fp_cvt's finite path calls the unchanged round_pack_mode reference. */
#include "a64_fp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static unsigned long cases, failures;
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double value(unsigned h)
{
    unsigned e = h >> 10, m = h & 1023;
    return ldexp((double)(e ? m + 1024 : m), e ? (int)e - 25 : -24);
}
static void check(uint64_t v, unsigned mode)
{
    hao_cpu c = {0};
    unsigned sticky = cases & 1 ? 0x0800009fu : 0;
    fpctx x = {&c, mode, false, false};
    a64_fpsr_write(&c, sticky);
    uint16_t ref = (uint16_t)fp_cvt(&x, v, 8, 2);
    unsigned rf = a64_fpsr_read(&c), flags = sticky;
    uint16_t got = fp_round16_exact(v, mode, &flags);
    cases++;
    if (ref != got || rf != flags || a64_fpsr_read(&c) != rf) {
        if (failures++ < 10) printf("MISMATCH round16 %016llx mode %u: %04x/%x != %04x/%x\n",
                (unsigned long long)v, mode, ref, rf, got, flags);
    }
}
int main(void)
{
    for (unsigned h = 0; h < 0x7c00; h++) {
        double lo = value(h), hi = h == 0x7bff ? 65536 : value(h + 1);
        uint64_t a = bits(lo), b = bits(hi), mid = bits((lo + hi) / 2);
        uint64_t points[] = {a, a + 1, mid - 1, mid, mid + 1, b - 1, b};
        for (unsigned k = 0; k < 7; k++)
            for (unsigned sign = 0; sign < 2; sign++)
                for (unsigned mode = 0; mode < 4; mode++) check(points[k] | (uint64_t)sign << 63, mode);
    }
    uint64_t rng = 0x315e89ab46de0211ull;
    for (unsigned k = 0; k < 200000; k++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        uint64_t v = rng;
        if ((v >> 52 & 2047) == 2047) v ^= 1ull << 52;
        for (unsigned mode = 0; mode < 4; mode++) check(v, mode);
    }
    a64_fpsr_write(&(hao_cpu){0}, 0);
    printf("round16: %lu cases, %lu mismatches\n", cases, failures);
    return failures != 0;
}
