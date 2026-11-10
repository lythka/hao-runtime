/* Native helper for tests/guest/blocks2.m: functions that call blocks.
 * Built universal (x86_64 + arm64 + arm64e). Under hao the guest's import
 * binds to the x86_64 slice, so these are native code calling guest blocks.
 * Every argument is a pointer, an integer or a double: the guest->native
 * call itself needs no struct marshalling; only the block calls do. */
#ifndef BLOCKHELPER_H
#define BLOCKHELPER_H
#include <CoreGraphics/CGGeometry.h>

typedef struct { double d; int i; } HMixed;              /* SSE + INT eightbytes on x86_64, x0/x1 on arm64 */
typedef struct { long a, b, c, d, e; } HBig;             /* 40 bytes: memory/sret on x86_64, by reference/x8 on arm64 */
typedef struct { float x, y, z; } HF3;                   /* float HFA of 3 */

typedef CGRect (^hb_rect_t)(CGRect r, double s);
typedef HMixed (^hb_mixed_t)(HMixed a, HMixed b);
typedef HBig (^hb_big_t)(HBig b, int k);
typedef HF3 (^hb_f3_t)(HF3 v, float s);
typedef long (^hb_many_t)(long a, long b, long c, long d, long e, long f, long g);
typedef double (^hb_fmix_t)(int a, double b, float c, long d, double e);
typedef int (^hb_int_t)(int);
typedef CGRect (^hb_outer_t)(hb_int_t inner, CGRect r);     /* extended signature @?<i@?i> */
typedef int (^hb_outer2_t)(int (^inner)(int (^)(int), int), int k); /* nested @?<i@?@?<i@?i>i> */

void hb_rect(hb_rect_t b, const CGRect *in, double s, CGRect *out);
void hb_mixed(hb_mixed_t b, double d1, int i1, double d2, int i2, HMixed *out);
void hb_big(hb_big_t b, long base, int k, HBig *out);
void hb_f3(hb_f3_t b, double x, double y, double z, double s, HF3 *out);
long hb_many(hb_many_t b, long base);
double hb_fmix(hb_fmix_t b, int a, double x);
/* Block_copy in native code, call the heap copy, hand it back (caller releases) */
hb_rect_t hb_copy_rect(hb_rect_t b, const CGRect *in, CGRect *out) __attribute__((ns_returns_retained));
void hb_outer(hb_outer_t b, const CGRect *in, int add, CGRect *out);
int hb_outer2(hb_outer2_t b, int k);
/* keep a copy, call it later from native code */
void hb_store(void (^b)(int));
void hb_fire(int v);
void hb_release(void);
/* the block arrives in the 9th argument (on the guest stack) */
long hb_arg9(long a, long b, long c, long d, long e, long f, long g, long h, long (^blk)(long));
#endif
