#import "objcimphelper.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static double last_sum;
double hao_last_sum(void) { return last_sum; }
@implementation HaoLayer {
    HRect _b;
    HTransform _t;
    HPoint _p;
}
- (void)setBounds:(HRect)r { _b = r; }
- (HRect)bounds { return _b; }
- (void)setTransform:(HTransform)t { _t = t; }
- (HTransform)transform { return _t; }
- (void)setPosition:(HPoint)p { _p = p; }
- (HPoint)position { return _p; }
- (BOOL)isWide { return _b.size.w > _b.size.h; }
- (HTriple)triple { return (HTriple){(long)_b.origin.x, (long)_b.size.w, 7}; }
- (float)scale:(float)f rect:(HRect)r { return f * (float)(r.size.w + r.size.h); }
+ (HRectBlock)rectBlock
{
    double k = 2;
    return [^(HRect r) { last_sum = k * (r.origin.x + r.origin.y + r.size.w + r.size.h); } copy];
}
@end
HRectBlock hao_rect_block(void)
{
    double k = 3;
    return [^(HRect r) { last_sum = k * (r.origin.x + 10 * r.origin.y + 100 * r.size.w + 1000 * r.size.h); } copy];
}

/* hao_split_block: global blocks laid out by hand (the Block ABI's literal and descriptor), so that
 * their signature can be placed anywhere */
struct split_desc { unsigned long reserved, size; const char *sig; };
struct split_lit { void *isa; int flags, reserved; void *invoke; const struct split_desc *desc; };
extern void *_NSConcreteGlobalBlock[];
#define SPLIT_INV(i) static void split_inv##i(void *b, HRect r) \
    { (void)b; last_sum = (i + 1) * (r.origin.x + 10 * r.origin.y + 100 * r.size.w + 1000 * r.size.h); }
SPLIT_INV(0) SPLIT_INV(1) SPLIT_INV(2) SPLIT_INV(3) SPLIT_INV(4) SPLIT_INV(5) SPLIT_INV(6) SPLIT_INV(7)
static void *const split_invs[HAO_SPLIT_CASES] = {split_inv0, split_inv1, split_inv2, split_inv3,
                                                  split_inv4, split_inv5, split_inv6, split_inv7};
static struct split_desc split_descs[HAO_SPLIT_CASES];
static struct split_lit split_lits[HAO_SPLIT_CASES];
HRectBlock hao_split_block(int i)
{
    static char *pages;
    const char *shortsig = "v40@?0{?={?=dd}{?=dd}}8";   /* clang's encoding of HRectBlock */
    const char *longsig = "v40@?0{HaoSplitBlockRectangleWithAVeryLongTagName={HaoSplitBlockPointTag=dd}{?=dd}}8";
    static const int before[HAO_SPLIT_CASES] = {1, 2, 8, 16, 22, 0, 0, 0};   /* bytes before the boundary */
    if (i < 0 || i >= HAO_SPLIT_CASES) return NULL;
    size_t pg = (size_t)getpagesize();
    if (!pages) {
        void *m = mmap(NULL, 2 * HAO_SPLIT_CASES * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (m == MAP_FAILED) return NULL;
        pages = m;
    }
    char *boundary = pages + (2 * (size_t)i + 1) * pg, *at;
    const char *sig = i == 7 ? longsig : shortsig;
    if (i == 5) at = boundary - strlen(sig);              /* only the NUL in the next page */
    else if (i == 6) at = boundary - strlen(sig) - 1;     /* the NUL is the page's last byte */
    else if (i == 7) at = boundary - 40;
    else at = boundary - before[i];
    memcpy(at, sig, strlen(sig) + 1);
    split_descs[i] = (struct split_desc){0, sizeof(struct split_lit), at};
    split_lits[i] = (struct split_lit){_NSConcreteGlobalBlock, (1 << 28) | (1 << 30), 0, split_invs[i], &split_descs[i]};
    return (__bridge HRectBlock)(void *)&split_lits[i];
}
