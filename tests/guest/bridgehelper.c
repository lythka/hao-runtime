/* Native helper for tests/guest/bridgefix.m (see bridgehelper.h). */
#include "bridgehelper.h"
#include <objc/message.h>
#include <objc/runtime.h>

long bh_call_rects(bh_rects_t b)
{
    return b(CGRectMake(1, 2, 3, 4), CGRectMake(5, 6, 7, 8), CGRectMake(9, 10, 11, 12), 4242);
}

double bh_call_ld(bh_ld_t b, double x)
{
    return (double)(b(x) * 2);
}

static bh_big72 bh_make72(long seed)
{
    bh_big72 a;
    for (int k = 0; k < 9; k++) a.v[k] = seed * 100 + k;
    return a;
}

long bh_sum304(const bh_big304 *r)
{
    long s = 0;
    for (int k = 0; k < 38; k++) s = s * 31 + r->v[k];
    return s;
}

bh_big304 bh_big_imp(void *self, void *cmd, bh_big72 a, long i)
{
    (void)self; (void)cmd;
    bh_big304 r;
    for (int k = 0; k < 38; k++) r.v[k] = a.v[k % 9] * (k + 1) + i;
    return r;
}

long bh_call_big(bh_big_t b)
{
    bh_big304 r = b(bh_make72(7), 1000);
    return bh_sum304(&r);
}

long bh_send_big(void *obj, const char *sel)
{
#if defined(__x86_64__)
#define MSGSEND_STRET objc_msgSend_stret    /* a result in memory needs the stret entry on x86_64 */
#else
#define MSGSEND_STRET objc_msgSend
#endif
    bh_big304 r = ((bh_big304(*)(void *, SEL, bh_big72, long))MSGSEND_STRET)(obj, sel_registerName(sel), bh_make72(9), 2000);
    return bh_sum304(&r);
}

typedef struct { unsigned long location, length; } bh_range;
unsigned long bh_send_nosplit(void *obj, const char *sel)
{
    bh_range r = {11, 22};
    return ((unsigned long (*)(void *, SEL, void *, void *, unsigned long, bh_range))objc_msgSend)(
        obj, sel_registerName(sel), obj, (void *)0, 3, r);
}
