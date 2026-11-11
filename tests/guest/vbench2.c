/* Advanced SIMD kernel timings for the forms the translator emits with
 * SSSE3/SSE4.1/SSE4.2 instructions: TBL/TBX of one to four table registers,
 * EXT, MUL/MLA/MLS (vector and by element), FRINT* (vector and scalar),
 * the 64-bit compares, SMIN/SMAX/UMIN/UMAX and ABS. For hao --stats and
 * wall-time comparisons, not a test (vbench.c covers the SSE2 forms).
 *   clang -arch arm64e -O2 -o vbench2 tests/guest/vbench2.c */
#include <arm_neon.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

#define N 4096
static uint8_t bytes[N], out8[N];
static uint32_t words[N], out32[N];
static float fl[N], outf[N];
static double dbl[N / 2], outd[N / 2];
static int64_t q64[N / 2], outq[N / 2];

/* substitution through a 64-byte table (TBL of four registers), a 16- and a
 * 32-byte table (TBL of one and two) and TBX filling the bytes TBL left */
__attribute__((noinline)) static void shuffle(uint8_t *o, const uint8_t *in, const uint8_t *tab, int n)
{
    uint8x16x4_t t4 = vld1q_u8_x4(tab);
    uint8x16x2_t t2 = {{t4.val[1], t4.val[2]}};
    uint8x16_t mask = vdupq_n_u8(0x3f), prev = vdupq_n_u8(0);
    for (int i = 0; i < n; i += 16) {
        uint8x16_t x = vld1q_u8(in + i);
        uint8x16_t a = vqtbl4q_u8(t4, vandq_u8(x, mask));
        uint8x16_t b = vqtbl1q_u8(t4.val[3], x);
        uint8x16_t c = vqtbl2q_u8(t2, vshrq_n_u8(x, 2));
        b = vqtbx1q_u8(b, t4.val[0], vsubq_u8(x, vdupq_n_u8(16)));
        uint8x16_t s = vextq_u8(prev, x, 13);
        uint8x8_t h = vext_u8(vget_low_u8(x), vget_high_u8(prev), 5);
        prev = x;
        uint8x16_t r = veorq_u8(veorq_u8(a, b), veorq_u8(c, s));
        r = veorq_u8(r, vcombine_u8(h, vqtbl1_u8(t4.val[2], vget_low_u8(x))));
        vst1q_u8(o + i, r);
    }
}

/* integer multiply-accumulate: MUL/MLA/MLS .4S and .8H, by element */
__attribute__((noinline)) static void imul(uint32_t *o, const uint32_t *in, int n)
{
    uint32x4_t k = {0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du, 0x27d4eb2fu}, acc = vdupq_n_u32(1);
    uint16x8_t acc16 = vdupq_n_u16(7);
    for (int i = 0; i < n; i += 4) {
        uint32x4_t x = vld1q_u32(in + i);
        acc = vmlaq_u32(acc, x, k);
        acc = vmulq_u32(acc, vdupq_n_u32(0x01000193u));
        uint32x4_t y = vmlsq_laneq_u32(x, acc, k, 1);
        y = vmulq_laneq_u32(y, k, 3);
        acc16 = vmlaq_u16(acc16, vreinterpretq_u16_u32(y), vreinterpretq_u16_u32(x));
        acc16 = vmlaq_laneq_u16(acc16, vreinterpretq_u16_u32(x), vreinterpretq_u16_u32(k), 5);
        uint32x2_t lo = vmul_u32(vget_low_u32(y), vget_high_u32(acc));
        vst1q_u32(o + i, veorq_u32(vcombine_u32(lo, vget_high_u32(y)), vreinterpretq_u32_u16(acc16)));
    }
}

/* every rounding mode, vector single and double, scalar */
__attribute__((noinline)) static void rnd(float *o, double *od, const float *in, const double *ind, int n)
{
    for (int i = 0; i < n; i += 4) {
        float32x4_t x = vld1q_f32(in + i);
        float32x4_t r = vaddq_f32(vrndnq_f32(x), vrndmq_f32(x));
        r = vaddq_f32(r, vrndpq_f32(x));
        r = vsubq_f32(r, vrndq_f32(x));
        r = vaddq_f32(r, vrndaq_f32(x));
        r = vaddq_f32(r, vrndxq_f32(x));
        r = vsubq_f32(r, vrndiq_f32(x));
        float32x2_t h = vrndm_f32(vget_low_f32(x));
        r = vaddq_f32(r, vcombine_f32(h, vrndp_f32(vget_high_f32(x))));
        vst1q_f32(o + i, r);
    }
    for (int i = 0; i < n / 2; i += 2) {
        float64x2_t x = vld1q_f64(ind + i);
        float64x2_t r = vaddq_f64(vrndnq_f64(x), vrndmq_f64(x));
        r = vaddq_f64(r, vrndaq_f64(x));
        r = vsubq_f64(r, vrndq_f64(x));
        vst1q_f64(od + i, r);
    }
    for (int i = 0; i < n / 2; i++) {
        o[i] += floorf(in[i]) + ceilf(in[i + 1]) + truncf(in[i + 2]) + roundf(in[i + 3]) + rintf(in[i]);
        od[i] += floor(ind[i]) - ceil(ind[i]) + round(ind[i]) + nearbyint(ind[i]);
    }
}

/* 64-bit compares, min/max in every lane size, ABS */
__attribute__((noinline)) static void cmpmm(int64_t *o, const int64_t *in, uint32_t *o32, const uint32_t *in32, int n)
{
    int64x2_t lim = vdupq_n_s64(1ll << 40);
    for (int i = 0; i < n; i += 2) {
        int64x2_t x = vld1q_s64(in + i);
        uint64x2_t gt = vcgtq_s64(x, lim), hi = vcgtq_u64(vreinterpretq_u64_s64(x), vreinterpretq_u64_s64(lim));
        uint64x2_t z = vceqzq_s64(x), le = vcleq_s64(x, lim), hs = vcgeq_u64(vreinterpretq_u64_s64(lim), vreinterpretq_u64_s64(x));
        int64x2_t a = vabsq_s64(x);
        uint64x2_t m = veorq_u64(veorq_u64(gt, hi), veorq_u64(veorq_u64(z, le), hs));
        vst1q_s64(o + i, veorq_s64(a, vreinterpretq_s64_u64(m)));
    }
    for (int i = 0; i < n * 2; i += 4) {
        int32x4_t x = vld1q_s32((const int32_t *)in32 + i);
        int32x4_t r = vmaxq_s32(vminq_s32(x, vdupq_n_s32(1 << 20)), vdupq_n_s32(-(1 << 20)));
        uint32x4_t u = vminq_u32(vmaxq_u32(vreinterpretq_u32_s32(x), vdupq_n_u32(4096)), vdupq_n_u32(1u << 31));
        int8x16_t b = vmaxq_s8(vreinterpretq_s8_s32(x), vdupq_n_s8(-3));
        uint16x8_t h = vminq_u16(vreinterpretq_u16_s32(x), vdupq_n_u16(999));
        int32x4_t a = vabsq_s32(x);
        r = veorq_s32(r, vreinterpretq_s32_s8(vabsq_s8(b)));
        r = veorq_s32(r, vreinterpretq_s32_u16(h));
        vst1q_u32(o32 + i, veorq_u32(vreinterpretq_u32_s32(veorq_s32(r, a)), u));
    }
}

int main(int argc, char **argv)
{
    int reps = argc > 1 ? atoi(argv[1]) : 400;
    uint8_t tab[64];
    for (int i = 0; i < 64; i++) tab[i] = (uint8_t)(i * 37 + 11);
    for (int i = 0; i < N; i++) {
        bytes[i] = (uint8_t)(i * 7 + (i >> 5));
        words[i] = (uint32_t)i * 2654435761u;
        fl[i] = (float)(i % 211) * 0.37f - 37.5f;
    }
    for (int i = 0; i < N / 2; i++) {
        dbl[i] = (double)(i % 173) * 0.731 - 61.5;
        q64[i] = (int64_t)((uint64_t)i * 0x9e3779b97f4a7c15ull) >> (i & 31);
    }
    double t0 = now();
    for (int r = 0; r < reps; r++) shuffle(out8, bytes, tab, N);
    double t1 = now();
    for (int r = 0; r < reps; r++) imul(out32, words, N);
    double t2 = now();
    for (int r = 0; r < reps; r++) rnd(outf, outd, fl, dbl, N);
    double t3 = now();
    for (int r = 0; r < reps; r++) cmpmm(outq, q64, out32, words, N / 2);
    double t4 = now();
    uint64_t h = 1469598103934665603ull;
#define HASH(p, n) for (size_t k = 0; k < (n); k++) { h ^= ((const uint8_t *)(p))[k]; h *= 1099511628211ull; }
    HASH(out8, sizeof out8) HASH(out32, sizeof out32) HASH(outf, sizeof outf) HASH(outd, sizeof outd) HASH(outq, sizeof outq)
    printf("vbench2 hash=%016llx\n", (unsigned long long)h);
    fprintf(stderr, "shuffle %.3f  imul %.3f  round %.3f  cmpmm %.3f  total %.3f\n", t1 - t0, t2 - t1, t3 - t2, t4 - t3, t4 - t0);
    return 0;
}
