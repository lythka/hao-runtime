/* Native helper for tests/guest/fnslots.mm: native code that calls guest
 * functions through routes x0-x7 scanning does not see. Built universal
 * (x86_64 + arm64 + arm64e); under hao the guest binds to the x86_64 slice. */
#ifndef FNHELPER_H
#define FNHELPER_H
#include <CoreGraphics/CGGeometry.h>
#ifdef __cplusplus
extern "C" {
#endif

/* a callback table the guest keeps in static data */
typedef struct {
    const char *name;
    int (*apply)(int x);
    long (*fold)(long acc, int x);
} fh_ops;
/* sum over k < n of ops->fold(acc, ops->apply(k)) */
long fh_run_ops(const fh_ops *ops, int n);
/* call every table[k].apply(k) for k < count, n rounds; returns the sum */
long fh_run_table(const fh_ops *table, int count, int rounds);

/* a block inside a structure: copied and called natively (unretained, so
 * that ARC in the guest stores the stack literal itself, not a copy) */
typedef struct {
    int tag;
    __unsafe_unretained int (^blk)(int);
} fh_blkbox;
long fh_box_call(const fh_blkbox *box, int n);
/* a block as the 9th argument (on the stack under arm64 and x86_64) */
long fh_stack_block(long a, long b, long c, long d, long e, long f, long g, long h, int (^blk)(int), int n);

/* send -haoRect / -haoScaled:by: to obj from native code (the methods
 * return structs; the helpers return through *out so that the guest's call
 * of the helper itself needs no marshalling) */
void fh_rect(id obj, CGRect *out);
void fh_scaled(id obj, const CGRect *r, double s, CGRect *out);

/* C++ objects created by the guest, used natively through their vtables */
#ifdef __cplusplus
}
struct FhShape {
    virtual ~FhShape() {}
    virtual double area() const = 0;
    virtual int sides() const = 0;
    virtual long visit(long acc) const { return acc * 31 + sides(); }
};
extern "C" {
#endif
double fh_total_area(void *const *shapes, int n, int rounds);
long fh_visit_all(void *const *shapes, int n);
int fh_sides_sum(void *const *shapes, int n);
void fh_delete_all(void *const *shapes, int n);
#ifdef __cplusplus
}
#endif
#endif
