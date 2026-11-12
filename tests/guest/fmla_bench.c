/* Stable normal operands, sixteen FMLA instructions per loop. Timing includes
 * loop/dispatch overhead; compare the checksum with native arm64. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
#define STEP3 "fmla v0.4s, v1.4s, v2.4s\n"
#define STEPE "fmla v0.4s, v1.4s, v2.s[3]\n"
#define REPEAT(x) x x x x x x x x x x x x x x x x
#define LOOP(step) __asm__ volatile( \
    "movi v0.4s, #0\nldr q1, [%[a]]\nldr q2, [%[b]]\n1:\n" REPEAT(step) \
    "subs %w[n], %w[n], #1\nb.ne 1b\nstr q0, [%[out]]" \
    : [n] "+r"(n) : [a] "r"(a), [b] "r"(b), [out] "r"(out) : "v0", "v1", "v2", "cc", "memory")
int main(int argc, char **argv)
{
    unsigned count = argc > 1 ? (unsigned)atoi(argv[1]) : 250000;
    if (!count || count > 1000000) return 2;
    const float a[4] = {0x1p-20f, 0x1p-20f, 0x1p-20f, 0x1p-20f}, b[4] = {1, 1, 1, 1};
    for (unsigned form = 0; form < 2; form++) {
        uint32_t out[4]; unsigned n = count;
        uint64_t start = ns();
        if (form) { LOOP(STEPE); } else { LOOP(STEP3); }
        uint64_t elapsed = ns() - start;
        printf("%s %08x %08x %08x %08x\n", form ? "element" : "three", out[0], out[1], out[2], out[3]);
        fprintf(stderr, "%s %.3f ns/FMLA (%u instructions)\n", form ? "element" : "three", (double)elapsed / (count * 16), count * 16);
    }
    const uint16_t ha[8] = {0x2400,0x2400,0x2400,0x2400,0x2400,0x2400,0x2400,0x2400};
    const uint16_t hb[8] = {0x3c00,0x3c00,0x3c00,0x3c00,0x3c00,0x3c00,0x3c00,0x3c00};
    for (unsigned form = 0; form < 2; form++) {
        /* Reset each loop: half precision cannot accumulate millions of
         * increments without saturating. The reset is included in timing. */
        const void *a = ha, *b = hb;
        uint32_t out[4]; unsigned n = count;
        uint64_t start = ns();
#define HALF_LOOP(step) __asm__ volatile( \
    "ldr q1, [%[a]]\nldr q2, [%[b]]\n1: movi v0.8h, #0\n" REPEAT(step) \
    "subs %w[n], %w[n], #1\nb.ne 1b\nstr q0, [%[out]]" \
    : [n] "+r"(n) : [a] "r"(a), [b] "r"(b), [out] "r"(out) : "v0", "v1", "v2", "cc", "memory")
        if (form) { HALF_LOOP("fmla v0.8h, v1.8h, v2.h[7]\n"); }
        else { HALF_LOOP("fmla v0.8h, v1.8h, v2.8h\n"); }
        uint64_t elapsed = ns() - start;
        printf("half-%s %08x %08x %08x %08x\n", form ? "element" : "three", out[0], out[1], out[2], out[3]);
        fprintf(stderr, "half-%s %.3f ns/FMLA (%u instructions)\n", form ? "element" : "three", (double)elapsed / (count * 16), count * 16);
    }
    return 0;
}
