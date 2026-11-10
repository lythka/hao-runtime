/* An HFA result of three or four doubles from native functions no header declares (d0-d3 on
 * arm64, sret on SysV; Dock's SLSWindowIteratorGetBounds).
 * Each case calls stand-ins with the 26A428 prototypes (hfasrethelper.c) and prints the result;
 * canary words around each result buffer must survive, and the guest's outgoing argument area
 * is dirtied first so a shifted argument shows. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "hfasrethelper.h"
float _Complex __mulsc3(float, float, float, float);

__attribute__((noinline)) static void dirty(void)
{
    volatile uint64_t junk[64];
    for (int i = 0; i < 64; i++) junk[i] = 0xdead000000000000ull | (uint64_t)i;
    (void)junk[0];
}
#define CANARY 0x5ca1ab1e0ddba11ull
struct rbox { uint64_t lo; HRect r; uint64_t hi; };
struct ibox { uint64_t lo; HInsets r; uint64_t hi; };
struct vbox { uint64_t lo; HVec3 r; uint64_t hi; };
static int canaries(uint64_t lo, uint64_t hi, const char *what)
{
    if (lo == CANARY && hi == CANARY) return 1;
    printf("  %s: canary smashed lo=%llx hi=%llx\n", what, (unsigned long long)lo, (unsigned long long)hi);
    return 0;
}
static void pr(const char *what, HRect r) { printf("%s = %g %g %g %g\n", what, r.x, r.y, r.w, r.h); }

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "all";
    int all = !strcmp(c, "all");
    if (all || !strcmp(c, "georegion")) {
        void *region = hfasret_region_new();
        struct rbox b = {CANARY, {0}, CANARY};
        dirty();
        b.r = GEOMapRectForMapRegion(region);
        pr("georegion", b.r);
        canaries(b.lo, b.hi, "georegion");
        printf("region intact = %d\n", hfasret_region_intact());
    }
    if (all || !strcmp(c, "sls")) {
        HIter *it = hfasret_iter_new();
        for (long i = 0; i < 4; i++) {
            struct rbox b = {CANARY, {0}, CANARY};
            dirty();
            b.r = SLSWindowIteratorGetBounds(it, i);
            char w[48];
            snprintf(w, sizeof w, "bounds[%ld]", i);
            pr(w, b.r);
            canaries(b.lo, b.hi, w);
            dirty();
            b.r = SLSWindowIteratorGetScreenRect(it, i);
            snprintf(w, sizeof w, "screen[%ld]", i);
            pr(w, b.r);
            canaries(b.lo, b.hi, w);
        }
    }
    if (all || !strcmp(c, "radial")) {
        struct rbox b = {CANARY, {0}, CANARY};
        dirty();
        b.r = MKMapRectMakeWithRadialDistance((HCoord){37.25, -122.5}, 1500.0);
        pr("radial", b.r);
        canaries(b.lo, b.hi, "radial");
    }
    if (all || !strcmp(c, "edges")) {
        for (unsigned long e = 1; e < 16; e += 5) {
            struct ibox b = {CANARY, {0}, CANARY};
            dirty();
            b.r = UIEdgeInsetsMakeWithEdges(e, 4.5);
            printf("edges[%lu] = %g %g %g %g\n", e, b.r.top, b.r.left, b.r.bottom, b.r.right);
            canaries(b.lo, b.hi, "edges");
        }
    }
    if (all || !strcmp(c, "vec3")) {
        struct vbox b = {CANARY, {0}, CANARY};
        dirty();
        b.r = QCVector_Make(0.125, -3.0, 1e10);
        printf("vec3 = %g %g %g\n", b.r.a, b.r.b, b.r.c);
        canaries(b.lo, b.hi, "vec3");
    }
    if (all || !strcmp(c, "qlrect")) {
        struct rbox b = {CANARY, {0}, CANARY};
        dirty();
        b.r = QLContentRect((HSize){640, 480}, (HSize){128, 128});
        pr("qlrect", b.r);
        canaries(b.lo, b.hi, "qlrect");
    }
    if (all || !strcmp(c, "axframe")) {
        struct rbox b = {CANARY, {0}, CANARY};
        int view;
        dirty();
        b.r = UIAccessibilityConvertFrameToScreenCoordinates((HRect){10, 20, 30, 40}, &view);
        pr("axframe", b.r);
        canaries(b.lo, b.hi, "axframe");
        dirty();
        b.r = UIAccessibilityConvertFrameToScreenCoordinates((HRect){10, 20, 30, 40}, NULL);
        pr("axframe-null", b.r);
        canaries(b.lo, b.hi, "axframe-null");
    }
    if (all || !strcmp(c, "mkfits")) {
        struct rbox b = {CANARY, {0}, CANARY};
        dirty();
        b.r = _MKMapRectThatFits((HRect){100, 200, 300, 400}, (HSize){8, 16}, (HInsets){1, 2, 3, 4});
        pr("mkfits", b.r);
        canaries(b.lo, b.hi, "mkfits");
    }
    if (all || !strcmp(c, "cmul")) {
        /* compiler-rt's float _Complex multiply: s0/s1 on arm64, both packed in xmm0 on SysV */
        volatile float in[4] = {1.5f, -2.0f, 0.25f, 3.0f};
        dirty();
        float _Complex z = __mulsc3(in[0], in[1], in[2], in[3]);
        printf("cmul = %g %g\n", (double)__real__ z, (double)__imag__ z);
        dirty();
        z = __mulsc3(in[3], in[2], in[1], in[0]);
        printf("cmul2 = %g %g\n", (double)__real__ z, (double)__imag__ z);
    }
    return 0;
}
