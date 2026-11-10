/* No GUI services: a native fixture exports the production SPI name and ABI.
 * Its helper is in another translation unit, so the wrapper must forward x8. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { uint64_t q[8]; } Timing;
typedef struct { int64_t value; int32_t scale; uint32_t flags; int64_t epoch; } Time;
typedef struct { Time start, duration; } Range;
typedef struct { double a, b, c, d, tx, ty; } Transform;
typedef struct { uint64_t q[199]; } Gamepad;
Timing SLSDisplayGetTiming(uint64_t, int);
Transform SLSSpaceGetTransform(unsigned, uint64_t, unsigned *);
Time AVCFAssetGetDuration(void *);
Time AVCFPlayerItemIntegratedTimelineInspection_Get_CurrentTime(void *);
Time AVCFPlayerItemIntegratedTimelineSnapshot_Get_Duration(void *);
Range AVCFTimedMetadataGroupGetTimeRange(void *);
Time CMIOStreamClockConvertHostTimeToDeviceTime(uint64_t, const void *);
Gamepad GCExtendedGamepadInitInfoMake(void);
/* x8 passed through to a tail call (clause S, x8_to) */
typedef struct { uint64_t q[4]; } LSVersion;
typedef struct { double w, h; } Size;
typedef struct { uint64_t q[7]; } PixelInfo;
LSVersion _LSVersionNumberMakeWithString(const void *);
Range AVCFAssetTrackGetTimeRange(void *);
Transform _UIScaleTransformForAspectFitOfSizeInTargetSize(Size, Size);
PixelInfo MTLPixelFormatGetInfo(uint64_t);
void fill_bytes(void *, unsigned, uint64_t);

#if defined(HELPER)
void fill_timing(Timing *p, uint64_t tick, int display) {
    for (unsigned i = 0; i < 8; ++i)
        p->q[i] = tick + (uint64_t)(int64_t)display * 17 + i * 101;
}
void fill_bytes(void *p, unsigned n, uint64_t seed) {
    unsigned char *b = p;
    for (unsigned i = 0; i < n; ++i) b[i] = (unsigned char)((seed >> (i % 8 * 8)) + i * 37);
}
#elif defined(LIBRARY)
extern void fill_timing(Timing *, uint64_t, int);
Timing SLSDisplayGetTiming(uint64_t tick, int display) {
    Timing r;
    fill_timing(&r, tick, display);
    return r;
}
Transform SLSSpaceGetTransform(unsigned cid, uint64_t space, unsigned *status) {
    *status = cid + (unsigned)space;
    Transform r;
    fill_bytes(&r, sizeof r, space ^ cid);
    return r;
}
#define TIME_GETTER(name, salt) \
    Time name(void *p) { Time r; fill_bytes(&r, sizeof r, *(uint64_t *)p + salt); return r; }
TIME_GETTER(AVCFAssetGetDuration, 11)
TIME_GETTER(AVCFPlayerItemIntegratedTimelineInspection_Get_CurrentTime, 23)
TIME_GETTER(AVCFPlayerItemIntegratedTimelineSnapshot_Get_Duration, 41)
Range AVCFTimedMetadataGroupGetTimeRange(void *p) {
    Range r; fill_bytes(&r, sizeof r, *(uint64_t *)p); return r;
}
Time CMIOStreamClockConvertHostTimeToDeviceTime(uint64_t ticks, const void *p) {
    Time r; fill_bytes(&r, sizeof r, *(const uint64_t *)p ^ ticks); return r;
}
Gamepad GCExtendedGamepadInitInfoMake(void) {
    Gamepad r; fill_bytes(&r, sizeof r, 0x12345678); return r;
}
LSVersion _LSVersionNumberMakeWithString(const void *p) {
    LSVersion r; fill_bytes(&r, sizeof r, *(const uint64_t *)p + 3); return r;
}
Range AVCFAssetTrackGetTimeRange(void *p) {
    Range r; fill_bytes(&r, sizeof r, *(uint64_t *)p + 5); return r;
}
Transform _UIScaleTransformForAspectFitOfSizeInTargetSize(Size s, Size t) {
    Transform r = {s.w, s.h, t.w, t.h, s.w * 2 - t.h, t.w * 0.5 + s.h};
    return r;
}
PixelInfo MTLPixelFormatGetInfo(uint64_t format) {
    PixelInfo r; fill_bytes(&r, sizeof r, format * 31); return r;
}
#else
static void dump(const void *p, unsigned n) {
    const unsigned char *b = p;
    for (unsigned i = 0; i < n; ++i) printf("%02x", b[i]);
    putchar('\n');
}
#define CHECK(type, expr) do { \
    struct { uint64_t before; type value; uint64_t after; } v; \
    memset(&v, 0xa5, sizeof v); \
    v.value = (expr); \
    if (v.before != UINT64_C(0xa5a5a5a5a5a5a5a5) || v.after != v.before) return 2; \
    dump(&v.value, sizeof v.value); \
} while (0)
int main(int argc, char **argv) {
    if (argc != 2) return 3;
    unsigned which = (unsigned)atoi(argv[1]);
    const uint64_t ticks[] = {0, 1, UINT64_C(0x123456789abcdef0)};
    const int displays[] = {0x042731c0, -7, 8};
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t input[256];
        memset(input, 0x5a, sizeof input);
        input[0] = ticks[i];
        unsigned status = 0;
        switch (which) {
        case 0: CHECK(Timing, SLSDisplayGetTiming(ticks[i], displays[i])); break;
        case 1:
            CHECK(Transform, SLSSpaceGetTransform((unsigned)displays[i], ticks[i], &status));
            if (status != (unsigned)displays[i] + (unsigned)ticks[i]) return 5;
            break;
        case 2: CHECK(Time, AVCFAssetGetDuration(input)); break;
        case 3: CHECK(Time, AVCFPlayerItemIntegratedTimelineInspection_Get_CurrentTime(input)); break;
        case 4: CHECK(Time, AVCFPlayerItemIntegratedTimelineSnapshot_Get_Duration(input)); break;
        case 5: CHECK(Range, AVCFTimedMetadataGroupGetTimeRange(input)); break;
        case 6: CHECK(Time, CMIOStreamClockConvertHostTimeToDeviceTime(ticks[i] + 7, input)); break;
        case 7: CHECK(Gamepad, GCExtendedGamepadInitInfoMake()); break;
        case 8: CHECK(LSVersion, _LSVersionNumberMakeWithString(input)); break;
        case 9: CHECK(Range, AVCFAssetTrackGetTimeRange(input)); break;
        case 10: {
            Size a = {(double)displays[i], 1.5 * i}, b = {-0.25 * i, (double)(ticks[i] & 0xffff)};
            CHECK(Transform, _UIScaleTransformForAspectFitOfSizeInTargetSize(a, b));
            break;
        }
        case 11: CHECK(PixelInfo, MTLPixelFormatGetInfo(ticks[i] + 80)); break;
        default: return 4;
        }
        if (input[0] != ticks[i]) return 6;
        for (unsigned j = 1; j < 256; ++j) if (input[j] != UINT64_C(0x5a5a5a5a5a5a5a5a)) return 6;
    }
    return 0;
}
#endif
