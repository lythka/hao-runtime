/* tests/guest/sretfwd.sh: the native helper's functions (sretfwdhelper.c). No generator reads this
 * header. Five functions are stand-ins with the exported names and the 26A428 prototypes of real
 * native functions, because hao's aggregate table (src/aggspi_gen.inc, tools/aggspi.txt) is keyed
 * by name; their bodies are the helper's own and call nothing outside it. */
#include <stdint.h>

/* 64 bytes of uint64s and doubles, as SkyLight's SLSDisplayTiming (eight 8-byte slots; MEMORY on
 * SysV, an indirect result in x8 on arm64; not an HFA) */
typedef struct {
    uint64_t t0;
    uint64_t period;
    double rate;
    int64_t phase;
    uint64_t deadline;
    uint64_t last;
    double scale;
    uint64_t flags;
} sfh_timing_t;

typedef struct { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; } sfh_cmtime_t;   /* CMTime, 24 B */
typedef struct { void *obj; uint64_t index; int32_t type; uint8_t flag; } sfh_plistobj_t;          /* {?=^vQiC}, 24 B */
typedef struct { unsigned int val[8]; } sfh_token_t;                                              /* audit_token_t */

/* x8 -> x0, then a tail call: SkyLight's own wrapper, exactly */
sfh_timing_t SLSDisplayGetTiming(uint64_t display, int which);
/* the same shape, 24 bytes */
sfh_cmtime_t AVCFAssetGetDuration(void *asset);
/* x8 untouched into an sret tail call (arm64e `b`, x86_64 a call; Metadata's shape) */
sfh_plistobj_t _MDPlistGetRootPlistObjectFromPlist(void *plist);
/* x8 -> x0 for a call (bl), then more work and a return */
sfh_cmtime_t CMIOStreamClockConvertHostTimeToDeviceTime(uint64_t host_time, void *clock);
/* stores through x8 itself: clause S before P26, typed on main */
sfh_token_t SIUserCtxGetAuditToken(void *ctx);
/* SLSDisplayGetTiming's shape under a name no table types (the generator's gate only) */
sfh_timing_t sfh_get_timing(uint64_t display, int which);
/* no indirect result: a pointer argument the callee writes, returning a uint64 */
uint64_t sfh_fill_ptr(sfh_timing_t *out, uint64_t display, int which);
/* nor here: it returns the pointer (x86_64 returns rdi in rax as an sret function does) */
sfh_timing_t *sfh_fill_ptr_ret(sfh_timing_t *out, uint64_t display, int which);
extern uint64_t sfh_calls;
