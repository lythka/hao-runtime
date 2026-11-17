/*
 * Native half of guest/ymmupper (tests/guest/ymmupper.sh). The x86_64 slice
 * leaves all ones in the upper halves of ymm8-15 and returns without
 * vzeroupper, as hand-written AVX code may, and reports which of those
 * uppers are nonzero. Without AVX state, and in the arm64 slice the native
 * reference uses, there are no uppers: nothing is dirtied, nothing reported.
 */
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__)
#include <cpuid.h>
static int avx(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & bit_AVX) || !(c & bit_OSXSAVE)) return 0;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}
void ymm_dirty(void)
{
    if (!avx()) return;
    __asm__ volatile("vcmpps $15, %%ymm8, %%ymm8, %%ymm8\n vcmpps $15, %%ymm9, %%ymm9, %%ymm9\n"
                     "vcmpps $15, %%ymm10, %%ymm10, %%ymm10\n vcmpps $15, %%ymm11, %%ymm11, %%ymm11\n"
                     "vcmpps $15, %%ymm12, %%ymm12, %%ymm12\n vcmpps $15, %%ymm13, %%ymm13, %%ymm13\n"
                     "vcmpps $15, %%ymm14, %%ymm14, %%ymm14\n vcmpps $15, %%ymm15, %%ymm15, %%ymm15"
                     ::: "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}
/* bit k: the upper half of ymm(8 + k) is not zero */
unsigned ymm_upper(void)
{
    if (!avx()) return 0;
    uint8_t y[8][32];
    __asm__ volatile("vmovdqu %%ymm8, 0(%0)\n vmovdqu %%ymm9, 32(%0)\n vmovdqu %%ymm10, 64(%0)\n"
                     "vmovdqu %%ymm11, 96(%0)\n vmovdqu %%ymm12, 128(%0)\n vmovdqu %%ymm13, 160(%0)\n"
                     "vmovdqu %%ymm14, 192(%0)\n vmovdqu %%ymm15, 224(%0)"
                     : : "r"(y) : "memory");
    static const uint8_t zero[16];
    unsigned m = 0;
    for (int k = 0; k < 8; k++) m |= (unsigned)(memcmp(y[k] + 16, zero, 16) != 0) << k;
    return m;
}
#else
void ymm_dirty(void) {}
unsigned ymm_upper(void) { return 0; }
#endif
/* Dirty the uppers, then call fn (a guest function under hao). */
unsigned ymm_call(unsigned (*fn)(void))
{
    ymm_dirty();
    return fn();
}
