/* Many distinct small guest functions, so a test can keep the translator
 * busy: every first call of one translates new blocks.
 *   fns[NFN]   int f(int)        (f1000 .. f9999)
 *   cmps[NCMP] qsort comparators (ascending; each new one is a new thunk) */
#ifndef FORKGEN_H
#define FORKGEN_H

/* about 100 instructions each, so a translation holds the JIT lock a while */
#define GEN_MIX(n, k) x = x * ((n) * 2 + (k) * 2 + 1) + (n) + (k); x ^= x >> (((n) + (k)) % 7 + 1);
#define GEN_MIX8(n, k) GEN_MIX(n, k) GEN_MIX(n, k + 1) GEN_MIX(n, k + 2) GEN_MIX(n, k + 3) \
                       GEN_MIX(n, k + 4) GEN_MIX(n, k + 5) GEN_MIX(n, k + 6) GEN_MIX(n, k + 7)
#define GEN_F(n) \
    static int f##n(int a) { unsigned x = (unsigned)a; GEN_MIX8(n, 0) GEN_MIX8(n, 8) GEN_MIX8(n, 16) return (int)(x & 0xffff); }
#define GEN_C(n) \
    static int c##n(const void *a, const void *b) { int d = *(const int *)a - *(const int *)b; return d * (n % 3 + 1); }
#define GEN_FP(n) f##n,
#define GEN_CP(n) c##n,

#define GEN10(M, p) M(p##0) M(p##1) M(p##2) M(p##3) M(p##4) M(p##5) M(p##6) M(p##7) M(p##8) M(p##9)
#define GEN100(M, p) GEN10(M, p##0) GEN10(M, p##1) GEN10(M, p##2) GEN10(M, p##3) GEN10(M, p##4) \
                     GEN10(M, p##5) GEN10(M, p##6) GEN10(M, p##7) GEN10(M, p##8) GEN10(M, p##9)
#define GEN1000(M, p) GEN100(M, p##0) GEN100(M, p##1) GEN100(M, p##2) GEN100(M, p##3) GEN100(M, p##4) \
                      GEN100(M, p##5) GEN100(M, p##6) GEN100(M, p##7) GEN100(M, p##8) GEN100(M, p##9)
#define GEN9000(M) GEN1000(M, 1) GEN1000(M, 2) GEN1000(M, 3) GEN1000(M, 4) GEN1000(M, 5) \
                   GEN1000(M, 6) GEN1000(M, 7) GEN1000(M, 8) GEN1000(M, 9)

GEN9000(GEN_F)
GEN1000(GEN_C, 1)

#define NFN 9000
#define NCMP 1000
static int (*const fns[NFN])(int) = { GEN9000(GEN_FP) };
static int (*const cmps[NCMP])(const void *, const void *) = { GEN1000(GEN_CP, 1) };

#endif
