/* tests/guest/sretfwd.sh: calls one sretfwdhelper.c function per process (argv[1] names the case)
 * and prints every field of the result with the canaries on both sides of it. The first argument
 * is a small integer (a display ID, or a fake object pointer the helper never dereferences): a
 * callee that takes it for the result pointer faults. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sretfwdhelper.h"

#define CANARY 0xc0ffee00c0ffee00ull
#define U(x) ((unsigned long long)(x))

static void pt(const char *c, const sfh_timing_t *t)
{
    printf("%s t0=%llx period=%llu rate=%.17g phase=%lld deadline=%llx last=%llx scale=%.17g flags=%llx\n", c,
           U(t->t0), U(t->period), t->rate, (long long)t->phase, U(t->deadline), U(t->last), t->scale, U(t->flags));
}

static void pm(const char *c, const sfh_cmtime_t *t)
{
    printf("%s value=%lld timescale=%d flags=%x epoch=%lld\n", c, (long long)t->value, t->timescale, t->flags,
           (long long)t->epoch);
}

static void canaries(const uint64_t *pre, const uint64_t *post)
{
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (pre[i] != CANARY + i || post[i] != CANARY + 4 + i) ok = 0;
    if (ok) { printf("canaries ok\n"); return; }
    for (int i = 0; i < 4; i++) printf("canary pre[%d]=%llx post[%d]=%llx\n", i, U(pre[i]), i, U(post[i]));
}

#define BOX(T) struct { uint64_t pre[4]; T r; uint64_t post[4]; }
#define INIT(b) do { memset(&(b).r, 0x5a, sizeof (b).r); \
    for (int i = 0; i < 4; i++) { (b).pre[i] = CANARY + i; (b).post[i] = CANARY + 4 + i; } } while (0)

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    static volatile uint64_t display = 0x2a;   /* volatile: nothing folds across the call */
    static volatile int which = 3;
    void *obj = (void *)(uintptr_t)display;
    if (!strcmp(c, "fwd64")) {
        static BOX(sfh_timing_t) b; INIT(b);
        b.r = SLSDisplayGetTiming(display, which);
        pt(c, &b.r); canaries(b.pre, b.post);
    } else if (!strcmp(c, "fwd24")) {
        static BOX(sfh_cmtime_t) b; INIT(b);
        b.r = AVCFAssetGetDuration(obj);
        pm(c, &b.r); canaries(b.pre, b.post);
    } else if (!strcmp(c, "fwdx8")) {
        static BOX(sfh_plistobj_t) b; INIT(b);
        b.r = _MDPlistGetRootPlistObjectFromPlist((char *)obj + 0x280);
        printf("%s obj=%llx index=%llx type=%d flag=%u\n", c, U((uintptr_t)b.r.obj), U(b.r.index), b.r.type, b.r.flag);
        canaries(b.pre, b.post);
    } else if (!strcmp(c, "fwdcall")) {
        static BOX(sfh_cmtime_t) b; INIT(b);
        b.r = CMIOStreamClockConvertHostTimeToDeviceTime(display, obj);
        pm(c, &b.r); canaries(b.pre, b.post);
        b.r = CMIOStreamClockConvertHostTimeToDeviceTime(0, (char *)obj + 0x10000);
        pm(c, &b.r); canaries(b.pre, b.post);
        printf("calls=%llu\n", U(sfh_calls));
    } else if (!strcmp(c, "store")) {
        static BOX(sfh_token_t) b; INIT(b);
        b.r = SIUserCtxGetAuditToken(obj);
        printf("%s", c);
        for (int i = 0; i < 8; i++) printf(" %08x", b.r.val[i]);
        printf("\n"); canaries(b.pre, b.post);
    } else if (!strcmp(c, "ptr")) {
        static BOX(sfh_timing_t) b; INIT(b);
        uint64_t v = sfh_fill_ptr(&b.r, display, which);
        printf("ptr returned %llu\n", U(v));
        pt(c, &b.r); canaries(b.pre, b.post);
    } else if (!strcmp(c, "ptrret")) {
        static BOX(sfh_timing_t) b; INIT(b);
        sfh_timing_t *p = sfh_fill_ptr_ret(&b.r, display, which);
        printf("ptrret returned %s\n", p == &b.r ? "out" : "another pointer");
        pt(c, &b.r); canaries(b.pre, b.post);
    } else {
        fprintf(stderr, "usage: sretfwd fwd64|fwd24|fwdx8|fwdcall|store|ptr|ptrret\n");
        return 2;
    }
    return 0;
}
