/* Native stand-ins for tests/guest/hfasret.c: functions that RETURN an aggregate of three or
 * four doubles (an HFA: d0-d3 on arm64, no x8; MEMORY class on SysV: sret pointer in rdi, every
 * argument one register up). Under the plain register bridge the native side takes the guest's
 * first argument as the result buffer and reads the rest one register early (Dock:
 * SLSWindowIteratorGetBounds, faults in iterator_seek). Each stand-in has the
 * exported name and the 26A428 prototype of the function it stands for (hao's tables are keyed
 * by name); none of them loads or calls SkyLight, MapKit, UIKit or QuickLook code.
 *
 * Prototypes, read from both slices of the 26A428 caches (tools/codesig.py --encode, clause R):
 *   SLSWindowIteratorGetBounds / ...GetScreenRect  SkyLight: x86 rdi = sret, rsi iterator, rdx
 *       index (to iterator_seek); arm64e x0 iterator, x1 index, `ldp d0, d1 / d2, d3` at the ret
 *   MKMapRectMakeWithRadialDistance  MapKit: x86 xmm0-xmm2, arm64e v0-v2 (a coordinate and a
 *       distance), MKMapRect result
 *   UIEdgeInsetsMakeWithEdges  UIKitCore: x86 rsi edges, xmm0 inset; arm64e x0, d0; UIEdgeInsets
 *   QCVector_Make  three doubles in, three doubles out (24 B: still MEMORY on SysV)
 *   QLContentRect  QuickLook: x86 xmm0-xmm3, arm64e v0-v3 (two CGSizes), CGRect result (read by
 *       hand: the result comes from a callee on both sides)
 *   _MKMapRectThatFits  MapKit: x86 rdi = sret, the MKMapRect at 0x10(%rbp), xmm0/xmm1 (a size), the
 *       insets at 0x30(%rbp); arm64e v0-v3 (MKMapRect), v4/v5, the insets on the stack (an HFA that
 *       no longer fits v6/v7); the result is its callee's MKMapRect (no x8)
 *   UIAccessibilityConvertFrameToScreenCoordinates  UIKit UIAccessibility.h: CGRect (HFA on
 *       arm64, MEMORY on x86_64: stack) and a pointer in, CGRect out
 */
#include <stdio.h>
#include <stdlib.h>
#include "hfasrethelper.h"
struct HIter { long count; HRect r[3]; };

HIter *hfasret_iter_new(void)
{
    HIter *it = calloc(1, sizeof *it);
    it->count = 3;
    for (int i = 0; i < 3; i++) {
        HRect r = {1.5 + i, 2.25 + i, 100.0 * (i + 1), 50.0 * (i + 1)};
        it->r[i] = r;
    }
    return it;
}
HRect SLSWindowIteratorGetBounds(HIter *it, long idx)
{
    if (!it || idx < 0 || idx >= it->count) { HRect z = {-1, -1, -1, -1}; return z; }
    return it->r[idx];
}
HRect SLSWindowIteratorGetScreenRect(HIter *it, long idx)
{
    HRect r = SLSWindowIteratorGetBounds(it, idx);
    r.y = 1000 - r.y - r.h;
    return r;
}
HRect MKMapRectMakeWithRadialDistance(HCoord c, double d)
{
    HRect r = {c.lon * 1000 - d, c.lat * 1000 - d, 2 * d, 2 * d};
    return r;
}
HInsets UIEdgeInsetsMakeWithEdges(unsigned long edges, double inset)
{
    HInsets e = {edges & 1 ? inset : 0, edges & 2 ? inset : 0, edges & 4 ? inset : 0, edges & 8 ? inset : 0};
    return e;
}
HVec3 QCVector_Make(double a, double b, double c)
{
    HVec3 v = {a, b, c};
    return v;
}
HRect QLContentRect(HSize content, HSize bounds)
{
    double s = bounds.w / content.w < bounds.h / content.h ? bounds.w / content.w : bounds.h / content.h;
    HRect r = {(bounds.w - content.w * s) / 2, (bounds.h - content.h * s) / 2, content.w * s, content.h * s};
    return r;
}
HRect UIAccessibilityConvertFrameToScreenCoordinates(HRect r, void *view)
{
    double off = view ? 7.0 : 0.0;
    HRect o = {r.x + off, r.y + 2 * off, r.w, r.h};
    return o;
}
HRect _MKMapRectThatFits(HRect r, HSize s, HInsets e)
{
    HRect o = {r.x - e.left, r.y - e.top, r.w + e.left + e.right + s.w, r.h + e.top + e.bottom + s.h};
    return o;
}

/* GeoServices 26A428: ARM x0 is the object, d0-d3 the rectangle result;
 * x86 rdi is the hidden 32-byte result buffer and rsi is the object. The
 * object is compared, not dereferenced, so the untyped bridge fails safely:
 * it writes its result over region_storage instead of passing that pointer. */
static uint64_t region_storage[8];
void *hfasret_region_new(void)
{
    for (unsigned i = 0; i < 8; ++i) region_storage[i] = UINT64_C(0xa5a5a5a500000000) + i;
    return region_storage;
}
int hfasret_region_intact(void)
{
    for (unsigned i = 0; i < 8; ++i)
        if (region_storage[i] != UINT64_C(0xa5a5a5a500000000) + i) return 0;
    return 1;
}
HRect GEOMapRectForMapRegion(void *region)
{
    printf("region argument = %s\n", region == region_storage ? "correct" : "wrong");
    return (HRect){12.5, -24.25, 100.75, 200.125};
}
