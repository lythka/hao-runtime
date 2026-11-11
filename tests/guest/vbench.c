/* Vector floating-point and structure load/store kernel timings (single
 * precision .4S/.2S arithmetic, selects and compares, FMUL by element,
 * LD1/ST1 of one to four registers, LD1R), for hao --stats and wall-time
 * comparisons of the translator. No fused multiply-add: every product and
 * sum is a separate FMUL and FADD.
 *   clang -arch arm64e -O2 -o vbench tests/guest/vbench.c */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

#define N 4096
static float a[N], b[N], c[N], d[N];

__attribute__((noinline)) static void axpy(float *y, const float *x, float k, int n)
{
    float32x4_t kk = vld1q_dup_f32(&k);
    for (int i = 0; i < n; i += 16) {
        float32x4x4_t vx = vld1q_f32_x4(x + i), vy = vld1q_f32_x4(y + i);
        vy.val[0] = vaddq_f32(vy.val[0], vmulq_f32(vx.val[0], kk));
        vy.val[1] = vaddq_f32(vy.val[1], vmulq_f32(vx.val[1], kk));
        vy.val[2] = vsubq_f32(vy.val[2], vmulq_f32(vx.val[2], kk));
        vy.val[3] = vsubq_f32(vy.val[3], vmulq_f32(vx.val[3], kk));
        vst1q_f32_x4(y + i, vy);
    }
}

__attribute__((noinline)) static void clampmix(float *o, const float *x, const float *y, int n)
{
    float32x4_t lo = vdupq_n_f32(-4.0f), hi = vdupq_n_f32(4.0f);
    for (int i = 0; i < n; i += 8) {
        float32x4x2_t vx = vld1q_f32_x2(x + i), vy = vld1q_f32_x2(y + i);
        for (int k = 0; k < 2; k++) {
            float32x4_t s = vmaxnmq_f32(vminnmq_f32(vaddq_f32(vx.val[k], vy.val[k]), hi), lo);
            uint32x4_t gt = vcgtq_f32(vx.val[k], vy.val[k]);
            uint32x4_t ge = vcgeq_f32(vy.val[k], s);
            vx.val[k] = vbslq_f32(gt, s, vmulq_f32(vy.val[k], s));
            vx.val[k] = vbslq_f32(ge, vx.val[k], vmaxq_f32(vx.val[k], vminq_f32(s, vy.val[k])));
        }
        vst1q_f32_x2(o + i, vx);
    }
}

/* 4x4 matrix times vectors: FMUL by element, 64-bit and 128-bit forms */
__attribute__((noinline)) static void xform(float *o, const float *m, const float *v, int n)
{
    float32x4x4_t mm = vld1q_f32_x4(m);
    for (int i = 0; i < n; i += 4) {
        float32x4_t x = vld1q_f32(v + i);
        float32x4_t r = vmulq_laneq_f32(mm.val[0], x, 0);
        r = vaddq_f32(r, vmulq_laneq_f32(mm.val[1], x, 1));
        r = vaddq_f32(r, vmulq_laneq_f32(mm.val[2], x, 2));
        r = vaddq_f32(r, vmulq_laneq_f32(mm.val[3], x, 3));
        float32x2_t h = vmul_lane_f32(vget_low_f32(r), vget_high_f32(x), 1);
        vst1q_f32(o + i, r);
        vst1_f32(o + i, vadd_f32(h, vld1_f32(o + i)));
    }
}

int main(int argc, char **argv)
{
    int reps = argc > 1 ? atoi(argv[1]) : 400;
    for (int i = 0; i < N; i++) { a[i] = (float)(i % 97) * 0.03125f - 1.5f; b[i] = (float)(i % 31) * 0.25f + 0.5f; c[i] = 0; d[i] = 1.0f; }
    float m[16];
    for (int i = 0; i < 16; i++) m[i] = (float)(i + 1) * 0.0625f;
    double t0 = now();
    for (int r = 0; r < reps; r++) axpy(b, a, (r & 1) ? 0.5f : -0.5f, N);
    double t1 = now();
    for (int r = 0; r < reps; r++) clampmix(c, a, b, N);
    double t2 = now();
    for (int r = 0; r < reps; r++) xform(d, m, c, N);
    double t3 = now();
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < N; i++) {
        uint32_t u[3];
        memcpy(&u[0], &b[i], 4); memcpy(&u[1], &c[i], 4); memcpy(&u[2], &d[i], 4);
        for (int k = 0; k < 3; k++) { h ^= u[k]; h *= 1099511628211ull; }
    }
    printf("vbench hash=%016llx\n", (unsigned long long)h);
    fprintf(stderr, "axpy %.3f  clampmix %.3f  xform %.3f  total %.3f\n", t1 - t0, t2 - t1, t3 - t2, t3 - t0);
    return 0;
}
