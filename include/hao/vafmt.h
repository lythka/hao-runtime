/* hao — format strings at the variadic and va_list bridges (src/vafmt.c). */
#ifndef HAO_VAFMT_H
#define HAO_VAFMT_H
#include <stdbool.h>
#include <stdint.h>

/* A function or ObjC selector whose format argument the SDK declares (generated outside this repository:
 * src/fmtattr_gen.inc). kind: 'p' printf, 's' scanf, 'w'/'W' wide printf/scanf, 'C' CFString/NSString,
 * 'o' os_log (never rewritten). fmt: 0-based argument index of the format (after self and _cmd for a
 * selector); first: 0-based index of the first variadic argument, -1 for a va_list consumer. */
typedef struct { const char *name; char kind; int8_t fmt, first; } hao_fmtattr;
const hao_fmtattr *hao_fmtattr_func(const char *name);   /* imported name, leading underscore */
const hao_fmtattr *hao_fmtattr_sel(const char *sel);

/* Apple arm64 long double is double. A format the native callee gets instead of the guest's: the L
 * length modifier of every floating conversion dropped (printf, CFString) or made l (scanf), which is
 * what the arm64 format means; any other byte is the guest's. hao_fmt_fix returns the format to pass
 * (the guest's when nothing changes); hao_fmt_done frees what it made. */
typedef struct { void *heap; const void *cf; } hao_fmtfix;
uint64_t hao_fmt_fix(char kind, uint64_t fmt, hao_fmtfix *fx);
void hao_fmt_done(hao_fmtfix *fx);
/* The same for a format that lies in a guest image (constant: the copy is made once and kept, so a
 * callee may keep the pointer). C strings ('p', 's') and constant CFStrings ('C'). */
uint64_t hao_fmt_fix_const(char kind, uint64_t fmt);

/* Slot types of the variadic arguments a printf-family or CFString format consumes, after hao_fmt_fix:
 * 'i' integer or pointer, 'd' double, 'L' a long double conversion (only for a format not fixed).
 * Returns the count, or -1 when the format cannot be typed (positional arguments, an unknown conversion,
 * more than max). */
int hao_fmt_types(char kind, uint64_t fmt, char *ty, int max);
/* the same scan over characters of width w (1, 2 or 4) */
int hao_fmt_types_chars(const void *s, int w, uint64_t len, char *ty, int max);

/* counters for hao_bridge_stats: formats rewritten, and calls whose variadic slots stayed untyped */
extern uint64_t hao_fmt_fixed, hao_fmt_untyped;
#endif
