/*
 * hao — format strings at the variadic and va_list bridges.
 *
 * Apple arm64 long double is double: the guest passes a %Lf argument as an
 * 8-byte double in an 8-byte slot, and gives scanf's %Lf an 8-byte buffer.
 * The native x86_64 printf reads an 80-bit x87 value from a 16-byte,
 * 16-aligned slot of the overflow area, and its scanf stores one. The
 * bridges therefore hand the native callee a copy of the format in which
 * every floating conversion's L is dropped (printf, CFString/NSString) or
 * made l (scanf): exactly what the arm64 format asks for, %La included
 * (an x87 value would print with the x86 hex mantissa). Nothing else in the
 * format changes, so positional arguments, %n and the conversion count stay
 * the guest's.
 *
 * Which argument is a format, and of which family, comes from the SDK's
 * format attributes (tools/gen_fmtattr.py -> fmtattr_gen.inc). The format
 * typing here (hao_fmt_types) also types the slots of variadics that have
 * no va_list variant, so their floating arguments go to XMM registers.
 */
#include "hao/runtime.h"
#include "hao/macho.h"
#include "hao/vafmt.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const hao_fmtattr fmt_funcs[] = {
#define HAO_FMTATTR_FUNCS
#include "fmtattr_gen.inc"
#undef HAO_FMTATTR_FUNCS
};
static const hao_fmtattr fmt_sels[] = {
#define HAO_FMTATTR_SELS
#include "fmtattr_gen.inc"
#undef HAO_FMTATTR_SELS
};

uint64_t hao_fmt_fixed, hao_fmt_untyped;

static const hao_fmtattr *find(const hao_fmtattr *t, size_t n, const char *name)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {                                     /* the generator writes them sorted (strcmp order) */
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, t[mid].name);
        if (!c) return &t[mid];
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
const hao_fmtattr *hao_fmtattr_func(const char *name) { return name ? find(fmt_funcs, sizeof fmt_funcs / sizeof *fmt_funcs, name) : NULL; }
const hao_fmtattr *hao_fmtattr_sel(const char *sel) { return sel ? find(fmt_sels, sizeof fmt_sels / sizeof *fmt_sels, sel) : NULL; }

/* ---- scanning */

static inline unsigned ch(const void *s, int w, uint64_t k)
{
    return w == 1 ? ((const uint8_t *)s)[k] : w == 2 ? ((const uint16_t *)s)[k] : ((const uint32_t *)s)[k];
}
static inline bool in(unsigned c, const char *set) { return c && c < 128 && strchr(set, (int)c); }

#define MAXL 64

/* Offsets of the L modifiers of floating conversions (at most MAXL; -1 if more). */
static int scan_L(const void *s, int w, uint64_t len, bool scanf_family, uint64_t *pos)
{
    int n = 0;
    for (uint64_t k = 0; k < len; k++) {
        if (ch(s, w, k) != '%') continue;
        if (++k >= len) break;
        if (ch(s, w, k) == '%') continue;
        uint64_t at[4];
        int nl = 0;
        while (k < len) {
            unsigned c = ch(s, w, k);
            if (c == 'L') { if (nl < 4) at[nl++] = k; k++; continue; }
            if (in(c, "-+ #0'*$.123456789hlqjztv,;:_")) { k++; continue; }
            break;
        }
        if (k >= len) break;
        unsigned c = ch(s, w, k);
        if (scanf_family && c == '[') {                   /* a scanset: ']' first (or after '^') is a member */
            k++;
            if (k < len && ch(s, w, k) == '^') k++;
            if (k < len && ch(s, w, k) == ']') k++;
            while (k < len && ch(s, w, k) != ']') k++;
            continue;
        }
        if (!in(c, "aAeEfFgG")) continue;
        for (int j = 0; j < nl; j++) {
            if (n == MAXL) return -1;
            pos[n++] = at[j];
        }
    }
    return n;
}

/* copy of s (width w, len units) with the L modifiers at pos removed (printf) or made l (scanf);
 * NUL-terminated; *newlen units without the terminator */
static void *rewrite(const void *s, int w, uint64_t len, bool scanf_family, const uint64_t *pos, int n, uint64_t *newlen)
{
    uint8_t *d = malloc((size_t)(len + 1) * (size_t)w);
    if (!d) return NULL;
    uint64_t o = 0;
    int j = 0;
    for (uint64_t k = 0; k < len; k++) {
        unsigned c = ch(s, w, k);
        if (j < n && pos[j] == k) {
            j++;
            if (!scanf_family) continue;
            c = 'l';
        }
        if (w == 1) d[o] = (uint8_t)c; else if (w == 2) ((uint16_t *)d)[o] = (uint16_t)c; else ((uint32_t *)d)[o] = c;
        o++;
    }
    memset(d + o * (size_t)w, 0, (size_t)w);
    *newlen = o;
    return d;
}

/* ---- CoreFoundation, reached only for a CFString the guest already holds */

typedef const void *cfref;
typedef struct { long loc, len; } cfrange;
static struct {
    long (*len)(cfref);
    void (*chars)(cfref, cfrange, uint16_t *);
    cfref (*make)(cfref, const uint16_t *, long);
    void (*release)(cfref);
    bool ok;
} cf;
static bool cf_init(void)
{
    if (__atomic_load_n(&cf.ok, __ATOMIC_ACQUIRE)) return true;
    *(void **)&cf.len = dlsym(RTLD_DEFAULT, "CFStringGetLength");
    *(void **)&cf.chars = dlsym(RTLD_DEFAULT, "CFStringGetCharacters");
    *(void **)&cf.make = dlsym(RTLD_DEFAULT, "CFStringCreateWithCharacters");
    *(void **)&cf.release = dlsym(RTLD_DEFAULT, "CFRelease");
    if (!cf.len || !cf.chars || !cf.make || !cf.release) return false;
    __atomic_store_n(&cf.ok, true, __ATOMIC_RELEASE);
    return true;
}

#define CF_MAXLEN (1 << 20)

/* a new CFString (+1) for the fixed format, or NULL when the format needs nothing */
static cfref cf_fix_chars(const void *chars, int w, uint64_t len)
{
    uint64_t pos[MAXL], nl;
    int n = scan_L(chars, w, len, false, pos);
    if (n <= 0) return NULL;
    uint16_t *d = rewrite(chars, w, len, false, pos, n, &nl);
    if (!d) return NULL;
    uint16_t *u = d;
    if (w == 1) {                                         /* an 8-bit constant: widen */
        u = malloc((size_t)(nl + 1) * 2);
        if (!u) { free(d); return NULL; }
        for (uint64_t k = 0; k < nl; k++) u[k] = ((const uint8_t *)d)[k];
    }
    cfref r = cf.make(NULL, u, (long)nl);
    if (u != d) free(u);
    free(d);
    return r;
}

static cfref cf_fix(cfref s)
{
    if (!s || !cf_init()) return NULL;
    long len = cf.len(s);
    if (len <= 0 || len > CF_MAXLEN) return NULL;
    uint16_t stackbuf[256], *b = len <= 256 ? stackbuf : malloc((size_t)len * 2);
    if (!b) return NULL;
    cf.chars(s, (cfrange){0, len}, b);
    cfref out = NULL;
    bool any = false;
    for (long k = 0; k < len && !any; k++) any = b[k] == 'L';
    if (any) out = cf_fix_chars(b, 2, (uint64_t)len);
    if (b != stackbuf) free(b);
    return out;
}

/* a constant CFString in a guest image (its isa is __CFConstantStringClassReference) */
static bool guest_const_cfstring(uint64_t a)
{
    static uint64_t cfconst;
    if (!cfconst) cfconst = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, "__CFConstantStringClassReference");
    hao_image *im = (a & 7) ? NULL : hao_image_for_addr(a);
    return im && cfconst && a + 32 <= hao_image_extent(im, a) && *(const uint64_t *)(uintptr_t)a == cfconst;
}
uint64_t hao_fmt_fix_const(char kind, uint64_t fmt);

/* ---- per-call fix */

uint64_t hao_fmt_fix(char kind, uint64_t fmt, hao_fmtfix *fx)
{
    fx->heap = NULL;
    fx->cf = NULL;
    if (!fmt) return fmt;
    uint64_t pos[MAXL], nl;
    switch (kind) {
    case 'p': case 's': {
        const char *s = (const char *)(uintptr_t)fmt;
        if (!strchr(s, 'L')) return fmt;
        uint64_t len = strlen(s);
        int n = scan_L(s, 1, len, kind == 's', pos);
        if (n <= 0) return fmt;
        fx->heap = rewrite(s, 1, len, kind == 's', pos, n, &nl);
        break;
    }
    case 'w': case 'W': {
        const wchar_t *s = (const wchar_t *)(uintptr_t)fmt;
        if (!wcschr(s, L'L')) return fmt;
        uint64_t len = wcslen(s);
        int n = scan_L(s, 4, len, kind == 'W', pos);
        if (n <= 0) return fmt;
        fx->heap = rewrite(s, 4, len, kind == 'W', pos, n, &nl);
        break;
    }
    case 'C':
        if (guest_const_cfstring(fmt)) return hao_fmt_fix_const('C', fmt);   /* scanned once, not per call */
        fx->cf = cf_fix((cfref)(uintptr_t)fmt);
        if (fx->cf) { __atomic_fetch_add(&hao_fmt_fixed, 1, __ATOMIC_RELAXED); return (uint64_t)(uintptr_t)fx->cf; }
        return fmt;
    default:
        return fmt;
    }
    if (!fx->heap) return fmt;
    __atomic_fetch_add(&hao_fmt_fixed, 1, __ATOMIC_RELAXED);
    if (hao_verbose > 1) fprintf(stderr, "hao: format %s: long double conversions made double\n", (const char *)(uintptr_t)fmt);
    return (uint64_t)(uintptr_t)fx->heap;
}

void hao_fmt_done(hao_fmtfix *fx)
{
    free(fx->heap);
    if (fx->cf) cf.release(fx->cf);
    fx->heap = NULL;
    fx->cf = NULL;
}

/* ---- formats in guest images: one copy each, kept. Lock-free (a CAS on the key); the insertion
 * runs in a hao critical section, so a guest signal handler on the same thread never waits for it. */

#define CONST_TAB 4096
static struct { uint64_t key, val; } ctab[CONST_TAB];

static uint64_t const_make(char kind, uint64_t fmt)
{
    uint64_t pos[MAXL], nl;
    if (kind == 'C') {
        /* a constant CFString: isa, info (0x10: UTF-16), chars, length (as varsniff reads it) */
        const uint64_t *o = (const uint64_t *)(uintptr_t)fmt;
        hao_image *ci = hao_image_for_addr(o[2]);
        bool wide = (o[1] & 0x10) != 0;
        if (!ci || o[3] > CF_MAXLEN || o[2] + o[3] * (wide ? 2 : 1) > hao_image_extent(ci, o[2]) || !cf_init()) return 0;
        return (uint64_t)(uintptr_t)cf_fix_chars((const void *)(uintptr_t)o[2], wide ? 2 : 1, o[3]);
    }
    const char *s = (const char *)(uintptr_t)fmt;
    uint64_t len = strlen(s);
    int n = scan_L(s, 1, len, kind == 's', pos);
    return n > 0 ? (uint64_t)(uintptr_t)rewrite(s, 1, len, kind == 's', pos, n, &nl) : 0;
}

uint64_t hao_fmt_fix_const(char kind, uint64_t fmt)
{
    if (!fmt || (kind != 'p' && kind != 's' && kind != 'C') || (kind == 'C' && !guest_const_cfstring(fmt))) return fmt;
    uint32_t h = (uint32_t)((fmt >> 3) * 2654435761u) & (CONST_TAB - 1);
    for (uint32_t k = 0; k < CONST_TAB; k++) {
        uint32_t i = (h + k) & (CONST_TAB - 1);
        uint64_t key = __atomic_load_n(&ctab[i].key, __ATOMIC_ACQUIRE);
        if (key == fmt) {
            uint64_t v;
            while (!(v = __atomic_load_n(&ctab[i].val, __ATOMIC_ACQUIRE))) {}   /* being made by another thread */
            return v == 1 ? fmt : v;
        }
        if (key) continue;
        hao_crit_enter();
        uint64_t zero = 0;
        if (!__atomic_compare_exchange_n(&ctab[i].key, &zero, fmt, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            hao_crit_leave();
            k--;                                          /* look at this slot again */
            continue;
        }
        uint64_t v = const_make(kind, fmt);
        if (v) __atomic_fetch_add(&hao_fmt_fixed, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&ctab[i].val, v ? v : 1, __ATOMIC_RELEASE);   /* 1: the format needs nothing */
        hao_crit_leave();
        return v ? v : fmt;
    }
    uint64_t v = const_make(kind, fmt);                    /* table full: still exact, the copy is not shared */
    return v ? v : fmt;
}

/* ---- slot typing */

int hao_fmt_types_chars(const void *s, int w, uint64_t len, char *ty, int max)
{
    int n = 0;
    for (uint64_t k = 0; k < len; k++) {
        if (ch(s, w, k) != '%') continue;
        k++;
        if (k < len && ch(s, w, k) == '%') continue;
        while (k < len && in(ch(s, w, k), "-+ #0'")) k++;
        if (k < len && ch(s, w, k) == '*') { if (n >= max) return -1; ty[n++] = 'i'; k++; }
        while (k < len && ch(s, w, k) >= '0' && ch(s, w, k) <= '9') k++;
        if (k < len && ch(s, w, k) == '$') return -1;     /* positional arguments */
        if (k < len && ch(s, w, k) == '.') {
            k++;
            if (k < len && ch(s, w, k) == '*') { if (n >= max) return -1; ty[n++] = 'i'; k++; }
            while (k < len && ch(s, w, k) >= '0' && ch(s, w, k) <= '9') k++;
        }
        bool L = false;
        while (k < len && in(ch(s, w, k), "hlqLjztv")) { if (ch(s, w, k) == 'L') L = true; k++; }
        unsigned c = k < len ? ch(s, w, k) : 0;
        if (!c || n >= max) return -1;
        if (in(c, "aAeEfFgG")) ty[n++] = L ? 'L' : 'd';
        else if (in(c, "diouxXcCsSpn@DOU")) ty[n++] = 'i';
        else return -1;
    }
    return n;
}

int hao_fmt_types(char kind, uint64_t fmt, char *ty, int max)
{
    if (!fmt) return -1;
    if (kind == 'p') { const char *s = (const char *)(uintptr_t)fmt; return hao_fmt_types_chars(s, 1, strlen(s), ty, max); }
    if (kind == 'w') { const wchar_t *s = (const wchar_t *)(uintptr_t)fmt; return hao_fmt_types_chars(s, 4, wcslen(s), ty, max); }
    if (kind != 'C' || !cf_init()) return -1;
    cfref s = (cfref)(uintptr_t)fmt;
    long len = cf.len(s);
    if (len < 0 || len > CF_MAXLEN) return -1;
    uint16_t stackbuf[256], *b = len <= 256 ? stackbuf : malloc((size_t)len * 2 + 2);
    if (!b) return -1;
    cf.chars(s, (cfrange){0, len}, b);
    int n = hao_fmt_types_chars(b, 2, (uint64_t)len, ty, max);
    if (b != stackbuf) free(b);
    return n;
}
