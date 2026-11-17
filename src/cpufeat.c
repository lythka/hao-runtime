/*
 * hao — the host's optional x86 features (hao/cpufeat.h). The probe runs
 * once; decoding, naming and profile parsing are pure.
 */
#include "hao/cpufeat.h"
#include <cpuid.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XCR0_AVX    0x06u            /* SSE and AVX (YMM upper) state */
#define XCR0_AVX512 0xE6u            /* those plus opmask, ZMM_Hi256, Hi16_ZMM */

uint64_t hao_cpu_decode(const hao_cpuid_regs *r)
{
    uint32_t c = r->l1_ecx, b7 = r->max_leaf >= 7 ? r->l7_ebx : 0, c7 = r->max_leaf >= 7 ? r->l7_ecx : 0;
    uint32_t e1 = r->max_ext >= 0x80000001u ? r->e1_ecx : 0;
    bool osx = c >> 27 & 1;
    bool avx_state = osx && (r->xcr0 & XCR0_AVX) == XCR0_AVX;
    bool avx512_state = osx && (r->xcr0 & XCR0_AVX512) == XCR0_AVX512;
    uint64_t f = 0;
    if (c >> 25 & 1) f |= HAO_CPU_AES;
    if (c >> 1 & 1) f |= HAO_CPU_PCLMUL;
    if (c >> 30 & 1) f |= HAO_CPU_RDRAND;
    if (c >> 22 & 1) f |= HAO_CPU_MOVBE;
    if (b7 >> 3 & 1) f |= HAO_CPU_BMI1;
    if (b7 >> 8 & 1) f |= HAO_CPU_BMI2;
    if (b7 >> 19 & 1) f |= HAO_CPU_ADX;
    if (b7 >> 29 & 1) f |= HAO_CPU_SHA;
    if (c7 >> 8 & 1) f |= HAO_CPU_GFNI;
    if (e1 >> 5 & 1) f |= HAO_CPU_LZCNT;
    if (avx_state && (c >> 28 & 1)) {
        f |= HAO_CPU_AVX;
        if (c >> 29 & 1) f |= HAO_CPU_F16C;
        if (c >> 12 & 1) f |= HAO_CPU_FMA;
        if (b7 >> 5 & 1) f |= HAO_CPU_AVX2;
        if (c7 >> 9 & 1) f |= HAO_CPU_VAES;
        if (c7 >> 10 & 1) f |= HAO_CPU_VPCLMUL;
        if (avx512_state && (b7 >> 16 & 1)) {
            f |= HAO_CPU_AVX512F;
            if (b7 >> 17 & 1) f |= HAO_CPU_AVX512DQ;
            if (b7 >> 30 & 1) f |= HAO_CPU_AVX512BW;
            if (b7 >> 31 & 1) f |= HAO_CPU_AVX512VL;
        }
    }
    return f;
}

static uint64_t host_word;
static pthread_once_t host_once = PTHREAD_ONCE_INIT;
static void host_probe(void)
{
    hao_cpuid_regs r;
    unsigned a, b, c, d;
    memset(&r, 0, sizeof r);
    r.max_leaf = __get_cpuid_max(0, NULL);
    r.max_ext = __get_cpuid_max(0x80000000u, NULL);
    if (__get_cpuid(1, &a, &b, &c, &d)) { r.l1_ecx = c; r.l1_edx = d; }
    if (r.max_leaf >= 7) { __cpuid_count(7, 0, a, b, c, d); r.l7_ebx = b; r.l7_ecx = c; }
    if (r.max_ext >= 0x80000001u) { __cpuid(0x80000001u, a, b, c, d); r.e1_ecx = c; }
    if (r.l1_ecx >> 27 & 1) {                   /* OSXSAVE: XGETBV is enabled */
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        r.xcr0 = (uint64_t)hi << 32 | lo;
    }
    host_word = hao_cpu_decode(&r);
}

uint64_t hao_cpu_host(void)
{
    pthread_once(&host_once, host_probe);
    return host_word;
}

static const char *const names[] = {"aes", "pclmul", "avx", "f16c", "rdrand", "fma", "movbe", "avx2", "bmi1", "bmi2",
                                    "lzcnt", "adx", "sha", "gfni", "vaes", "vpclmulqdq", "avx512f", "avx512dq",
                                    "avx512bw", "avx512vl"};
_Static_assert(sizeof names / sizeof *names == 20 && HAO_CPU_ALL == (1ull << 20) - 1, "one name per feature bit");

const char *hao_cpu_names(uint64_t mask, char *out, size_t n)
{
    size_t l = 0;
    if (n) out[0] = 0;
    for (unsigned k = 0; k < 64 && n; k++) {
        if (!(mask >> k & 1)) continue;
        int w = k < sizeof names / sizeof *names ? snprintf(out + l, n - l, "%s%s", l ? "," : "", names[k])
                                                 : snprintf(out + l, n - l, "%sbit%u", l ? "," : "", k);
        if (w < 0 || (size_t)w >= n - l) break;
        l += (size_t)w;
    }
    return out;
}

bool hao_cpu_profile_parse(const char *spec, uint64_t host, uint64_t *out, char *why, size_t n)
{
    if (!strcmp(spec, "baseline")) { *out = host & HAO_CPU_BASELINE; return true; }
    if (!strcmp(spec, "host")) { *out = host; return true; }
    const char *p = spec[0] == '0' && (spec[1] == 'x' || spec[1] == 'X') ? spec + 2 : spec;
    char *e;
    unsigned long long m = *p && p[0] != '-' && p[0] != '+' ? strtoull(p, &e, 16) : 0;
    if (!*p || p[0] == '-' || p[0] == '+' || *e) {
        snprintf(why, n, "--cpu=%s: expected baseline, host or a hexadecimal feature mask", spec);
        return false;
    }
    *out = host & (uint64_t)m;
    return true;
}
