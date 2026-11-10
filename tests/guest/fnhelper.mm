/* Native side of tests/guest/fnslots.mm (see fnhelper.h). */
#import <Foundation/Foundation.h>
#include <Block.h>
#include "fnhelper.h"

long fh_run_ops(const fh_ops *ops, int n)
{
    long acc = 0;
    for (int k = 0; k < n; k++) acc = ops->fold(acc, ops->apply(k));
    return acc;
}

long fh_run_table(const fh_ops *table, int count, int rounds)
{
    long s = 0;
    for (int r = 0; r < rounds; r++)
        for (int k = 0; k < count; k++) s += table[k].apply(k + r);
    return s;
}

long fh_box_call(const fh_blkbox *box, int n)
{
    int (^b)(int) = Block_copy(box->blk);
    long s = box->tag;
    for (int k = 0; k < n; k++) s += b(k);
    int (^c)(int) = Block_copy(b);     /* a copy of the heap copy: a retain */
    s += c(1000);
    Block_release(c);
    Block_release(b);
    return s;
}

long fh_stack_block(long a, long b, long c, long d, long e, long f, long g, long h, int (^blk)(int), int n)
{
    long s = a + b + c + d + e + f + g + h;
    for (int k = 0; k < n; k++) s += blk(k);
    return s;
}

@interface NSObject (FhRect)
- (CGRect)haoRect;
- (CGRect)haoScaled:(CGRect)r by:(double)s;
@end

void fh_rect(id obj, CGRect *out) { *out = [obj haoRect]; }
void fh_scaled(id obj, const CGRect *r, double s, CGRect *out) { *out = [obj haoScaled:*r by:s]; }

double fh_total_area(void *const *shapes, int n, int rounds)
{
    double t = 0;
    for (int r = 0; r < rounds; r++)
        for (int k = 0; k < n; k++) t += static_cast<const FhShape *>(shapes[k])->area();
    return t;
}

long fh_visit_all(void *const *shapes, int n)
{
    long acc = 7;
    for (int k = 0; k < n; k++) acc = static_cast<const FhShape *>(shapes[k])->visit(acc);
    return acc;
}

int fh_sides_sum(void *const *shapes, int n)
{
    int s = 0;
    for (int k = 0; k < n; k++) s += static_cast<const FhShape *>(shapes[k])->sides();
    return s;
}

void fh_delete_all(void *const *shapes, int n)
{
    for (int k = 0; k < n; k++) delete static_cast<FhShape *>(shapes[k]);
}
