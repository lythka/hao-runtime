/* Real host-library C functions whose Apple arm64 and x86_64 SysV ABIs differ,
 * one shape class per case (tests/guest/ifunits.sh): narrow results, >16-byte
 * aggregates by value and as results (sret), HFA results, mixed SSE/INTEGER
 * aggregates, packed stack arguments past the 8th, struct { double, double }
 * results (controls: the register bridge already places them), a callback that
 * returns a 24-byte struct, and variadics with %Lf.
 *   ifunits list         the case names
 *   ifunits CASE         that case's lines ("CASE label: values")
 * Every value printed is deterministic and the same natively on arm64; no
 * pointer is printed. Each case runs in its own process, so a wrong call
 * (a crash) costs only its own lines.
 * The narrow results are printed through the int the arm64 caller sees: Apple
 * arm64 callers trust the callee to have extended w0 (clang uses it directly),
 * x86_64 callees leave the bits above al undefined. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreText/CoreText.h>
#include <CoreVideo/CoreVideo.h>
#include <Accelerate/Accelerate.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static const char *cur;
#define P(label, ...) (printf("%s %s: ", cur, label), printf(__VA_ARGS__), putchar('\n'))

static void pcmtime(const char *label, CMTime t)
{
    P(label, "value=%lld timescale=%d flags=%#x epoch=%lld", (long long)t.value, t.timescale, t.flags,
      (long long)t.epoch);
}

static void prect(const char *label, CGRect r)
{
    P(label, "%.4f %.4f %.4f %.4f", r.origin.x, r.origin.y, r.size.width, r.size.height);
}

static void pcfstr(const char *label, CFStringRef s)
{
    char b[512] = "(null)";
    if (s) CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8);
    P(label, "%s", b);
}

static CMTimeRange range(int64_t start, int64_t dur, int32_t ts)
{
    return CMTimeRangeMake(CMTimeMake(start, ts), CMTimeMake(dur, ts));
}

/* ---------------------------------------------------------------- narrow results */

static void narrow_hlg(void)
{
    CGColorSpaceRef hlg = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_HLG);
    int a = CGColorSpaceIsHLGBased(hlg), b = CGColorSpaceIsPQBased(hlg);
    P("CGColorSpaceIsHLGBased(HLG)", "%#x", a);
    P("CGColorSpaceIsPQBased(HLG)", "%#x", b);
}

static void narrow_pq(void)
{
    CGColorSpaceRef pq = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
    int a = CGColorSpaceIsHLGBased(pq), b = CGColorSpaceIsPQBased(pq);
    P("CGColorSpaceIsHLGBased(PQ)", "%#x", a);
    P("CGColorSpaceIsPQBased(PQ)", "%#x", b);
}

static void narrow_cmtag(void)
{
    CMTag t = CMTagMakeWithSInt64Value(kCMTagCategory_ChannelID, 42);
    int a = CMTagHasSInt64Value(t), b = CMTagIsValid(t), c = CMTagCategoryEqualToTagCategory(t, t);
    P("CMTagHasSInt64Value", "%#x", a);
    P("CMTagIsValid", "%#x", b);
    P("CMTagCategoryEqualToTagCategory", "%#x", c);
}

static void narrow_blockbuffer(void)
{
    CMBlockBufferRef bb = NULL;
    OSStatus st = CMBlockBufferCreateEmpty(NULL, 0, 0, &bb);
    int a = CMBlockBufferIsEmpty(bb);
    P("CMBlockBufferIsEmpty", "st=%d %#x", (int)st, a);
}

/* ------------------------------------------- aggregates > 16 bytes by value and as results */

static void sret_cmtime_epoch(void)
{
    pcmtime("CMTimeMakeWithEpoch(1000,600,7)", CMTimeMakeWithEpoch(1000, 600, 7));
}

static void agg_cmtime_fold(void)
{
    pcmtime("CMTimeFoldIntoRange(13s,[1s,+5s])", CMTimeFoldIntoRange(CMTimeMake(1300, 100), range(100, 500, 100)));
    pcmtime("CMTimeFoldIntoRange(0.5s,[1s,+5s])", CMTimeFoldIntoRange(CMTimeMake(50, 100), range(100, 500, 100)));
}

static void agg_cmtime_mapdur(void)
{
    pcmtime("CMTimeMapDurationFromRangeToRange(2s,10s->30s)",
            CMTimeMapDurationFromRangeToRange(CMTimeMake(2, 1), range(0, 10, 1), range(5, 30, 1)));
}

static void agg_cmtimerange_contains(void)
{
    int a = CMTimeRangeContainsTimeRange(range(0, 10, 1), range(2, 3, 1));
    int b = CMTimeRangeContainsTimeRange(range(0, 10, 1), range(8, 3, 1));
    P("CMTimeRangeContainsTimeRange(yes)", "%#x", a);
    P("CMTimeRangeContainsTimeRange(no)", "%#x", b);
}

static void sret_cmtimemapping(void)
{
    CMTimeMapping m = CMTimeMappingMakeEmpty(range(30, 90, 30));
    pcmtime("CMTimeMappingMakeEmpty.source.start", m.source.start);
    pcmtime("CMTimeMappingMakeEmpty.target.start", m.target.start);
    pcmtime("CMTimeMappingMakeEmpty.target.duration", m.target.duration);
}

static void agg_cmtimemapping_desc(void)
{
    CMTimeMapping m = CMTimeMappingMake(range(0, 10, 1), range(20, 10, 1));
    CFStringRef s = CMTimeMappingCopyDescription(NULL, m);
    pcfstr("CMTimeMappingCopyDescription", s);
}

static void sret_gregorian_units(void)
{
    CFTimeZoneRef gmt = CFTimeZoneCreateWithTimeIntervalFromGMT(NULL, 0);
    CFGregorianUnits u = CFAbsoluteTimeGetDifferenceAsGregorianUnits(86400.0 * 400 + 3723.5, 0.0, gmt,
                                                                     kCFGregorianAllUnits);
    P("CFAbsoluteTimeGetDifferenceAsGregorianUnits", "y=%d mo=%d d=%d h=%d mi=%d s=%.3f", (int)u.years,
      (int)u.months, (int)u.days, (int)u.hours, (int)u.minutes, u.seconds);
}

/* {SInt32; SInt8 x4; double}: INTEGER + SSE on x86_64, two GP registers on arm64 */
static void mixed_gregorian_valid(void)
{
    CFGregorianDate ok = {2026, 11, 4, 12, 30, 15.0}, bad = {2026, 11, 4, 12, 30, 75.0};
    int a = CFGregorianDateIsValid(ok, kCFGregorianAllUnits);
    int b = CFGregorianDateIsValid(bad, kCFGregorianUnitsSeconds);
    P("CFGregorianDateIsValid(15s)", "%#x", a);
    P("CFGregorianDateIsValid(75s)", "%#x", b);
}

static void sret_affine_components(void)
{
    CGAffineTransformComponents c = {{2.0, 3.0}, 0.25, 0.5, {10.0, -4.0}};
    CGAffineTransform t = CGAffineTransformMakeWithComponents(c);
    P("CGAffineTransformMakeWithComponents", "%.6f %.6f %.6f %.6f %.6f %.6f", t.a, t.b, t.c, t.d, t.tx, t.ty);
}

/* ------------------------------------------------------------------- HFA results */

static void hfa_clean_aperture(void)
{
    CMVideoFormatDescriptionRef d = NULL;
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_H264, 640, 480, NULL, &d);
    P("CMVideoFormatDescriptionCreate", "st=%d", (int)st);
    prect("CMVideoFormatDescriptionGetCleanAperture", CMVideoFormatDescriptionGetCleanAperture(d, true));
}

static void hfa_font_bbox(void)
{
    CGFontRef f = CGFontCreateWithFontName(CFSTR("Helvetica"));
    prect("CGFontGetFontBBox(Helvetica)", f ? CGFontGetFontBBox(f) : CGRectNull);
}

static void hfa_optical_bounds(void)
{
    CTFontRef f = CTFontCreateWithName(CFSTR("Helvetica"), 24.0, NULL);
    UniChar ch[3] = {'h', 'a', 'o'};
    CGGlyph g[3];
    CTFontGetGlyphsForCharacters(f, ch, g, 3);
    CGRect r[3];
    prect("CTFontGetOpticalBoundsForGlyphs", CTFontGetOpticalBoundsForGlyphs(f, g, r, 3, 0));
    prect("CTFontGetOpticalBoundsForGlyphs[1]", r[1]);
}

static void hfa_sincosf(void)
{
    struct __float2 a = __sincosf_stret(0.5f), b = __sincospif_stret(0.125f);
    P("__sincosf_stret(0.5)", "%a %a", a.__sinval, a.__cosval);
    P("__sincospif_stret(0.125)", "%a %a", b.__sinval, b.__cosval);
}

/* --------------------------------------------- struct { double, double } (controls) */

static void dd_sincos(void)
{
    struct __double2 a = __sincos_stret(0.5), b = __sincospi_stret(0.125);
    P("__sincos_stret(0.5)", "%a %a", a.__sinval, a.__cosval);
    P("__sincospi_stret(0.125)", "%a %a", b.__sinval, b.__cosval);
}

static void dd_cvsize(void)
{
    CVPixelBufferRef pb = NULL;
    CVReturn r = CVPixelBufferCreate(NULL, 320, 200, kCVPixelFormatType_32BGRA, NULL, &pb);
    CGSize e = CVImageBufferGetEncodedSize(pb), d = CVImageBufferGetDisplaySize(pb);
    P("CVImageBufferGetEncodedSize", "r=%d %.1f %.1f", r, e.width, e.height);
    P("CVImageBufferGetDisplaySize", "%.1f %.1f", d.width, d.height);
}

/* ------------------------------------------- packed stack arguments past the 8th */

/* 11 and 12 arguments: divisor (int32), bias (int32) and backgroundColor (uint8)
 * share arm64 stack words; x86_64 gives each its own 8-byte slot */
static void conv_planar8(int bias)
{
    static uint8_t in[8 * 8], out[8 * 8];
    for (int i = 0; i < 64; i++) in[i] = (uint8_t)(i * 37 % 251);
    static const int16_t k[9] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
    vImage_Buffer s = {in, 8, 8, 8}, d = {out, 8, 8, 8};
    memset(out, 0, sizeof out);
    vImage_Error e = bias < 0 ? vImageConvolve_Planar8(&s, &d, NULL, 0, 0, k, 3, 3, 16, 200, kvImageBackgroundColorFill)
                              : vImageConvolveWithBias_Planar8(&s, &d, NULL, 0, 0, k, 3, 3, 16, bias, 200,
                                                               kvImageBackgroundColorFill);
    unsigned sum = 0;
    for (int i = 0; i < 64; i++) sum = sum * 31 + out[i];
    P(bias < 0 ? "vImageConvolve_Planar8" : "vImageConvolveWithBias_Planar8", "err=%ld out[0]=%u out[9]=%u sum=%#x",
      (long)e, out[0], out[9], sum);
}

static void stack_vimage_conv(void) { conv_planar8(-1); }
static void stack_vimage_bias(void) { conv_planar8(40); }

/* 12 arguments: shouldInterpolate (bool) and intent (int) share one arm64 stack word */
static void stack_cgimage_headroom(void)
{
    static uint8_t px[4 * 4 * 4];
    for (int i = 0; i < (int)sizeof px; i++) px[i] = (uint8_t)(i * 7);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, sizeof px, NULL);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
    CGImageRef im = CGImageCreateWithContentHeadroom(2.0f, 4, 4, 8, 32, 16, cs, kCGImageAlphaPremultipliedLast, dp,
                                                     NULL, true, kCGRenderingIntentRelativeColorimetric);
    P("CGImageCreateWithContentHeadroom", "%s", im ? "image" : "NULL");
    if (!im) return;
    P("image", "w=%zu h=%zu bpc=%zu bpp=%zu bpr=%zu interpolate=%d intent=%d alpha=%u headroom=%.3f",
      CGImageGetWidth(im), CGImageGetHeight(im), CGImageGetBitsPerComponent(im), CGImageGetBitsPerPixel(im),
      CGImageGetBytesPerRow(im), (int)CGImageGetShouldInterpolate(im), (int)CGImageGetRenderingIntent(im),
      (unsigned)CGImageGetAlphaInfo(im), CGImageGetContentHeadroom(im));
}

/* ------------------------------------------ callbacks returning a 24-byte struct */

static CMTime cb_pts(CMBufferRef buf, void *refcon)
{
    int v = 0;
    CFNumberGetValue((CFNumberRef)buf, kCFNumberIntType, &v);
    return CMTimeMake(v * 10 + (int)(intptr_t)refcon, 100);
}

static CMTime cb_dur(CMBufferRef buf, void *refcon)
{
    (void)buf; (void)refcon;
    return CMTimeMake(25, 100);
}

static CFComparisonResult cb_cmp(CMBufferRef a, CMBufferRef b, void *refcon)
{
    (void)refcon;
    return CFNumberCompare((CFNumberRef)a, (CFNumberRef)b, NULL);
}

static void cb_bufferqueue(void)
{
    static CMBufferCallbacks cbs;      /* packed (4): filled at run time, the linker refuses unaligned pointers */
    cbs.version = 0;
    cbs.refcon = (void *)3;
    cbs.getDecodeTimeStamp = cb_pts;
    cbs.getPresentationTimeStamp = cb_pts;
    cbs.getDuration = cb_dur;
    cbs.compare = cb_cmp;
    CMBufferQueueRef q = NULL;
    OSStatus st = CMBufferQueueCreate(NULL, 0, &cbs, &q);
    P("CMBufferQueueCreate", "st=%d", (int)st);
    if (st) return;
    for (int v = 5; v >= 3; v--) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
        CMBufferQueueEnqueue(q, n);
        CFRelease(n);
    }
    pcmtime("CMBufferQueueGetDuration", CMBufferQueueGetDuration(q));
    pcmtime("CMBufferQueueGetFirstPresentationTimeStamp", CMBufferQueueGetFirstPresentationTimeStamp(q));
    pcmtime("CMBufferQueueGetMaxPresentationTimeStamp", CMBufferQueueGetMaxPresentationTimeStamp(q));
}

/* -------------------------------------------------------- variadics with %Lf */

static void var_lf(void)
{
    CFStringRef s = CFStringCreateWithFormat(NULL, NULL, CFSTR("%Lf|%.3Lf|%d|%.2f"), 1.25L, 2.5L, 7, 0.75);
    pcfstr("CFStringCreateWithFormat(%Lf)", s);
    char b[128];
    snprintf(b, sizeof b, "%Lf|%.3Le|%d", 1.25L, 31.5L, 9);
    P("snprintf(%Lf)", "%s", b);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"narrow-hlg", narrow_hlg},
    {"narrow-pq", narrow_pq},
    {"narrow-cmtag", narrow_cmtag},
    {"narrow-blockbuffer", narrow_blockbuffer},
    {"sret-cmtime-epoch", sret_cmtime_epoch},
    {"agg-cmtime-fold", agg_cmtime_fold},
    {"agg-cmtime-mapdur", agg_cmtime_mapdur},
    {"agg-cmtimerange-contains", agg_cmtimerange_contains},
    {"sret-cmtimemapping", sret_cmtimemapping},
    {"agg-cmtimemapping-desc", agg_cmtimemapping_desc},
    {"sret-gregorian-units", sret_gregorian_units},
    {"mixed-gregorian-valid", mixed_gregorian_valid},
    {"sret-affine-components", sret_affine_components},
    {"hfa-clean-aperture", hfa_clean_aperture},
    {"hfa-font-bbox", hfa_font_bbox},
    {"hfa-optical-bounds", hfa_optical_bounds},
    {"hfa-sincosf", hfa_sincosf},
    {"dd-sincos", dd_sincos},
    {"dd-cvsize", dd_cvsize},
    {"stack-vimage-conv", stack_vimage_conv},
    {"stack-vimage-bias", stack_vimage_bias},
    {"stack-cgimage-headroom", stack_cgimage_headroom},
    {"cb-bufferqueue", cb_bufferqueue},
    {"var-lf", var_lf},
};

int main(int argc, char **argv)
{
    int n = (int)(sizeof cases / sizeof *cases);
    setvbuf(stdout, NULL, _IOLBF, 0);     /* the lines before a crash still count */
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (int i = 0; i < n; i++) puts(cases[i].name);
        return 0;
    }
    for (int i = 0; i < n; i++)
        if (!strcmp(argv[1], cases[i].name)) {
            cur = cases[i].name;
            cases[i].fn();
            fflush(stdout);
            return 0;
        }
    fprintf(stderr, "ifunits: no case %s\n", argv[1]);
    return 2;
}
