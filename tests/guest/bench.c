#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static int cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
int main(void) {
    double t0 = now();
    /* sieve */
    int N = 5000000; char *s = calloc(N, 1); int primes = 0;
    for (int i = 2; i < N; i++) if (!s[i]) { primes++; for (long j = (long)i * i; j < N; j += i) s[j] = 1; }
    double t1 = now();
    /* matrix multiply (doubles) */
    int M = 160; double *a = malloc(M*M*8), *b = malloc(M*M*8), *c = calloc(M*M, 8);
    for (int i = 0; i < M*M; i++) { a[i] = i % 7 * 0.5; b[i] = i % 5 * 0.25; }
    for (int i = 0; i < M; i++) for (int k = 0; k < M; k++) { double x = a[i*M+k]; for (int j = 0; j < M; j++) c[i*M+j] += x * b[k*M+j]; }
    double t2 = now();
    /* hashing / integer mix */
    uint64_t h = 1469598103934665603ull; for (int i = 0; i < 20000000; i++) { h ^= (uint64_t)i; h *= 1099511628211ull; h = (h << 13) | (h >> 51); }
    double t3 = now();
    /* qsort with guest comparator (native->guest callbacks) */
    int n = 300000; int *v = malloc(n * 4); srand(1); for (int i = 0; i < n; i++) v[i] = rand();
    qsort(v, n, 4, cmp);
    double t4 = now();
    printf("primes=%d c=%.1f h=%llx sorted=%d\n", primes, c[M*M-1], (unsigned long long)h, v[0] <= v[n-1]);
    fprintf(stderr, "sieve %.3f  matmul %.3f  hash %.3f  qsort-callbacks %.3f  total %.3f\n", t1-t0, t2-t1, t3-t2, t4-t3, t4-t0);
    return 0;
}
