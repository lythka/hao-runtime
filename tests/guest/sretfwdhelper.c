/* Native helper for tests/guest/sretfwd.sh: functions that return an aggregate through the hidden
 * result pointer (x8 on arm64, rdi returned in rax on x86_64) without storing through x8 themselves,
 * and controls. SkyLight's SLSDisplayGetTiming (26A428) is the model:
 *   arm64e  mov x2, x1; mov x1, x0; mov x0, x8; mov w3, #0; b display_get_timing_common
 *   x86_64  push %rbx; mov %rdi, %rbx; xor %ecx, %ecx; call display_get_timing_common; mov %rbx, %rax
 * Under the plain register bridge x8 never reaches rdi, and the callee writes the 64-byte result
 * through the guest's first argument (in WindowManager: a write to address 0).
 * The exported names are stand-ins (sretfwdhelper.h); the bodies call nothing outside this file.
 * sretfwd.sh checks each function's shape in both slices before it runs anything. The results
 * depend on every argument, so a shifted or dropped argument shows. */
#include <stdint.h>
#include <stdbool.h>
#include "sretfwdhelper.h"

uint64_t sfh_calls;

/* the out pointer is an ordinary first argument (x0 / rdi), as display_get_timing_common's */
__attribute__((noinline, visibility("hidden")))
void sfh_fill_timing(sfh_timing_t *out, uint64_t display, int which, bool legacy)
{
    out->t0 = display * 3 + 1;
    out->period = 16666667u + (uint64_t)which;
    out->rate = 60.0 / (double)(which + 1);
    out->phase = -(int64_t)display - which;
    out->deadline = display << 20 | (uint64_t)(uint32_t)which;
    out->last = legacy ? 0xdead : 0x1e57;
    out->scale = (double)display * 0.5 + 0.25;
    out->flags = 0xf1a90000u | (uint64_t)(legacy ? 2 : 1) << 8 | (uint64_t)(uint8_t)which;
}

__attribute__((noinline, visibility("hidden")))
void sfh_fill_time(sfh_cmtime_t *out, uintptr_t from, uint64_t t, bool device)
{
    out->value = (int64_t)(t * 7 + (from & 0xffff));
    out->timescale = device ? 48000 : 600;
    out->flags = 1u | (device ? 0x10u : 0u);
    out->epoch = (int64_t)(from >> 4) - 3;
}

/* an sret callee of its own: x8 / rdi is its hidden result pointer */
__attribute__((noinline, visibility("hidden")))
sfh_plistobj_t sfh_make_plistobj(void *plist, int depth)
{
    sfh_plistobj_t r = { (char *)plist + 0x40, (uintptr_t)plist >> 3, depth + 0x1d, (uint8_t)((uintptr_t)plist | 1) };
    return r;
}

/* fwdcall's work after the call, out of line so that it touches no register x8 could be */
__attribute__((noinline, visibility("hidden")))
void sfh_note(uint64_t t)
{
    __atomic_fetch_add(&sfh_calls, (uint64_t)(t > 0 ? 1 : 2), __ATOMIC_RELAXED);
}

sfh_timing_t SLSDisplayGetTiming(uint64_t display, int which)
{
    sfh_timing_t r;
    sfh_fill_timing(&r, display, which, false);
    return r;
}

sfh_cmtime_t AVCFAssetGetDuration(void *asset)
{
    sfh_cmtime_t r;
    sfh_fill_time(&r, (uintptr_t)asset, 0x600, false);
    return r;
}

sfh_plistobj_t _MDPlistGetRootPlistObjectFromPlist(void *plist)
{
    return sfh_make_plistobj(plist, 0);
}

sfh_cmtime_t CMIOStreamClockConvertHostTimeToDeviceTime(uint64_t host_time, void *clock)
{
    sfh_cmtime_t r;
    volatile int done;
    sfh_fill_time(&r, (uintptr_t)clock, host_time, true);
    sfh_note(host_time);
    done = 1;              /* a store to the frame: the note is a call, not a tail call */
    (void)done;
    return r;
}

sfh_token_t SIUserCtxGetAuditToken(void *ctx)
{
    sfh_token_t t;
    for (int i = 0; i < 8; i++)
        t.val[i] = (unsigned)((uintptr_t)ctx >> (i * 4)) * 0x9e3779b1u + (unsigned)i;
    return t;
}

sfh_timing_t sfh_get_timing(uint64_t display, int which)
{
    sfh_timing_t r;
    sfh_fill_timing(&r, display, which, false);
    return r;
}

uint64_t sfh_fill_ptr(sfh_timing_t *out, uint64_t display, int which)
{
    sfh_fill_timing(out, display, which, false);
    return display * 1000 + (uint64_t)which;
}

sfh_timing_t *sfh_fill_ptr_ret(sfh_timing_t *out, uint64_t display, int which)
{
    sfh_fill_timing(out, display, which, true);
    return out;
}
