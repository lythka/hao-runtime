/*
 * hao — os_log, os_signpost and os_log_pack records from guest images
 * (linked into libhao_identity.dylib beside src/process_identity.c, so its
 * __interpose tuples apply process-wide).
 *
 * libtrace records a message's format as an offset from the caller's
 * `__dso_handle`, and logd resolves it later from the uuidtext it harvests
 * from that image's file, cstring sections only. A guest image is not a
 * dyld image: every message, signpost name and activity label a guest image
 * logs showed as `<compose failure [UUID]>` (attributed to the main
 * executable's UUID at an offset outside it). That holds for the main
 * executable, guest dylibs and arm64e shared-cache images alike, in every
 * execution mode, and also for native code that logs on a guest's behalf
 * with the guest's dso (libswiftos's os_log() and os_signpost(), through
 * os_log_pack_send and _os_signpost_pack_send).
 *
 * For a guest dso each entry point below composes the message in process
 * with libtrace's own composer (_os_log_send_and_compose_impl with the
 * compose option only, os_log_pack_compose), which applies the format's
 * privacy annotations under the same private-data mode the native record
 * would have, and then calls the same native entry point with the main
 * executable's header and the format "%{public}s" from its __cstring
 * (src/front/stub.c, src/front/cli.c), the composed text as the one
 * argument. The message text, type, log object (subsystem and category),
 * activity and thread are the native ones; the sender stays the program in
 * the embedded route (the stub is the program's own file).
 *
 * What cannot be composed (static strings logd also resolves by offset)
 * stays a known difference: a guest signpost's name goes in front of its
 * message ("NAME: message") and the record's name is empty, and activity
 * descriptions and user-action labels are not rewritten.
 *
 * Every other dso (native callers, or before libhao registers its guest
 * test) reaches libtrace by a tail call, so the record keeps the caller's
 * return address as its pc.
 */
#include <dlfcn.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <os/log.h>
#include <os/signpost.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wpedantic"

/* libtrace SPI, from the 26A428 x86_64 libsystem_trace (argument use read from its code) */
extern char *_os_log_send_and_compose_impl(uint32_t options, const char **fmt_out, char *out, size_t outlen, void *dso,
                                           os_log_t log, os_log_type_t type, const char *fmt, uint8_t *buf,
                                           uint32_t size);
extern void _os_log_unreliable_impl(void *dso, os_log_t log, os_log_type_t type, const char *fmt, uint8_t *buf,
                                    uint32_t size);
extern void _os_signpost_emit_unreliably_with_name_impl(void *dso, os_log_t log, os_signpost_type_t type,
                                                        os_signpost_id_t spid, const char *name, const char *fmt,
                                                        uint8_t *buf, uint32_t size);
typedef struct hao_oslog_pack *os_log_pack_t;
extern void os_log_pack_send(os_log_pack_t pack, os_log_t log, os_log_type_t type);
extern char *os_log_pack_send_and_compose(os_log_pack_t pack, os_log_t log, os_log_type_t type, char *buf,
                                          size_t size);
extern char *os_log_pack_compose(os_log_pack_t pack, os_log_t log, os_log_type_t type, char *buf, size_t size);
extern void _os_signpost_pack_send(os_log_pack_t pack, os_log_t log, os_signpost_type_t type);

/* _os_log_pack_fill / _os_signpost_pack_fill: header 0x44 bytes, data after it */
struct hao_oslog_pack {
    uint64_t continuous_time;
    uint64_t wall_time[2];
    const void *mh;          /* 0x18 */
    const void *pc;          /* 0x20 */
    const char *format;      /* 0x28 */
    const char *name;        /* 0x30: signposts */
    uint64_t spid;           /* 0x38: signposts */
    uint16_t err;            /* 0x40 */
    uint16_t size;           /* 0x42 */
} __attribute__((packed));
_Static_assert(__builtin_offsetof(struct hao_oslog_pack, size) == 0x42, "os_log_pack layout");

extern const struct mach_header *_dyld_get_prog_image_header(void);

typedef int (*oslog_guest_fn)(const void *);
typedef int (*oslog_immutable_fn)(const void *, size_t);
static _Atomic(oslog_guest_fn) guest_hook;
static _Atomic(oslog_immutable_fn) immutable_hook;
static const void *emit_dso;    /* the main executable's header */
static const char *emit_fmt;    /* "%{public}s" in its __cstring; emit_fmt + 10 is "" */

/* A pointer to a literal, not an array: logd resolves formats only in an image's cstring sections, and a
 * const array lands in __TEXT,__const (every record from a front end without hao_oslog_format, such as a
 * stub linked before it, such as MenuBarAgent's, was <compose failure>) */
static const char *const local_fmt = "%{public}s";

/* libhao: FN tells whether an address is in a guest image, IMM whether a range lies in a guest image's
 * read-only segment. Set once, before guest code runs. */
void hao_identity_oslog_hook(oslog_guest_fn fn, oslog_immutable_fn imm)
{
    atomic_store(&immutable_hook, imm);
    const char *const *p = (const char *const *)dlsym(RTLD_MAIN_ONLY, "hao_oslog_format");
    if (p && *p && !strcmp(*p, local_fmt)) {
        emit_fmt = *p;
        emit_dso = _dyld_get_prog_image_header();
    } else {
        Dl_info di;
        emit_fmt = local_fmt;   /* not a hao front end: this library's own copy (sender libhao_identity) */
        emit_dso = dladdr(local_fmt, &di) ? di.dli_fbase : NULL;
    }
    if (emit_dso) atomic_store(&guest_hook, fn);
}

static inline bool from_guest(const void *dso)
{
    oslog_guest_fn f = atomic_load_explicit(&guest_hook, memory_order_acquire);
    return f && dso && f(dso);
}

/* libtrace's composer renders a mask as hex bytes ('0F A3 ...'); logd shows the same bytes in base64 */
static void mask_base64(char *s)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (char *m = strstr(s, "<mask."); m; m = strstr(m + 1, "<mask.")) {
        char *q = strstr(m, ": '");
        if (!q || memchr(m, '>', (size_t)(q - m))) continue;
        char *h = q + 3, *e = h;
        uint8_t bytes[64];
        size_t n = 0;
        while (n < sizeof bytes) {
            int hi = e[0], lo = hi ? e[1] : 0;
            if (!((hi >= '0' && hi <= '9') || (hi >= 'A' && hi <= 'F')) ||
                !((lo >= '0' && lo <= '9') || (lo >= 'A' && lo <= 'F')))
                break;
            bytes[n++] = (uint8_t)(((hi <= '9' ? hi - '0' : hi - 'A' + 10) << 4) | (lo <= '9' ? lo - '0' : lo - 'A' + 10));
            e += 2;
            if (*e == ' ' && e[1] != '\'') e++;
            else break;
        }
        if (!n || e[0] != '\'' || e[1] != '>') continue;
        char out[96];
        size_t o = 0;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)bytes[i] << 16 | (i + 1 < n ? (uint32_t)bytes[i + 1] << 8 : 0) | (i + 2 < n ? bytes[i + 2] : 0);
            out[o++] = b64[v >> 18 & 63];
            out[o++] = b64[v >> 12 & 63];
            out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
            out[o++] = i + 2 < n ? b64[v & 63] : '=';
        }
        if (o > (size_t)(e - h)) continue;
        memcpy(h, out, o);
        memmove(h + o, e, strlen(e) + 1);
    }
}

#define COMPOSE_MAX 2048

typedef void (*log_fn)(void *, os_log_t, os_log_type_t, const char *, uint8_t *, uint32_t);
typedef void (*signpost_fn)(void *, os_log_t, os_signpost_type_t, os_signpost_id_t, const char *, const char *,
                            uint8_t *, uint32_t);

static void put_string(uint8_t b[12], const char *s)
{
    b[0] = 2;       /* summary: a non-scalar item */
    b[1] = 1;       /* one item */
    b[2] = 0x22;    /* string, public */
    b[3] = 8;
    memcpy(b + 4, &s, 8);
}

static void emit_log(log_fn fn, os_log_t log, os_log_type_t type, char *msg)
{
    uint8_t b[12];
    mask_base64(msg);
    put_string(b, msg);
    fn((void *)emit_dso, log, type, emit_fmt, b, sizeof b);
}

static void emit_signpost(signpost_fn fn, os_log_t log, os_signpost_type_t type, os_signpost_id_t spid,
                          const char *name, char *msg)
{
    char text[COMPOSE_MAX + 128];
    uint8_t b[12];
    mask_base64(msg);
    const char *t = msg;
    if (name && name[0]) {
        size_t nl = strnlen(name, 120), ml = strlen(msg);
        if (ml > COMPOSE_MAX) ml = COMPOSE_MAX;
        memcpy(text, name, nl);
        if (ml) { memcpy(text + nl, ": ", 2); nl += 2; }
        memcpy(text + nl, msg, ml);
        text[nl + ml] = 0;
        t = text;
    }
    put_string(b, t);
    fn((void *)emit_dso, log, type, spid, emit_fmt + strlen(emit_fmt), emit_fmt, b, sizeof b);
}

/* The guest's own header goes to the composer: libtrace's defaults depend on the image's build version
 * (an unannotated string argument is shown for a 27.2 deployment target, redacted for a 27.0 one), and
 * dyld reads that version from the header it is given. */
static char *compose(char *out, size_t n, void *dso, os_log_t log, os_log_type_t type, const char *fmt, uint8_t *buf,
                     uint32_t size)
{
    return _os_log_send_and_compose_impl(2, NULL, out, n, dso, log, type, fmt ? fmt : "", buf, size);
}

static void guest_log(log_fn fn, void *dso, os_log_t log, os_log_type_t type, const char *fmt, uint8_t *buf, uint32_t size)
{
    int e = errno;
    if (os_log_type_enabled(log, type)) {
        char out[COMPOSE_MAX];
        errno = e;   /* %m */
        char *r = compose(out, sizeof out, dso, log, type, fmt, buf, size);
        if (r) emit_log(fn, log, type, r);
        if (r && r != out) free(r);
    }
    errno = e;
}

static void guest_signpost(signpost_fn fn, void *dso, os_log_t log, os_signpost_type_t type, os_signpost_id_t spid,
                           const char *name, const char *fmt, uint8_t *buf, uint32_t size)
{
    int e = errno;
    if (os_signpost_enabled(log)) {
        char out[COMPOSE_MAX];
        errno = e;
        char *r = compose(out, sizeof out, dso, log, OS_LOG_TYPE_DEFAULT, fmt, buf, size);
        if (r) emit_signpost(fn, log, type, spid, name, r);
        if (r && r != out) free(r);
    }
    errno = e;
}

#define LOG_IMPL(NAME)                                                                                         \
    static void hao##NAME(void *dso, os_log_t log, os_log_type_t type, const char *fmt, uint8_t *buf,          \
                          uint32_t size)                                                                       \
    {                                                                                                          \
        log_fn native = NAME; /* a pointer: the declarations say OS_NOT_TAIL_CALLED */                     \
        if (!from_guest(dso)) __attribute__((musttail)) return native(dso, log, type, fmt, buf, size);         \
        guest_log(NAME, dso, log, type, fmt, buf, size);                                                            \
    }
LOG_IMPL(_os_log_impl)
LOG_IMPL(_os_log_error_impl)
LOG_IMPL(_os_log_fault_impl)
LOG_IMPL(_os_log_debug_impl)
LOG_IMPL(_os_log_unreliable_impl)

#define SIGNPOST_IMPL(NAME)                                                                                    \
    static void hao##NAME(void *dso, os_log_t log, os_signpost_type_t type, os_signpost_id_t spid,             \
                          const char *name, const char *fmt, uint8_t *buf, uint32_t size)                      \
    {                                                                                                          \
        signpost_fn native = NAME;                                                                             \
        if (!from_guest(dso))                                                                                  \
            __attribute__((musttail)) return native(dso, log, type, spid, name, fmt, buf, size);               \
        guest_signpost(NAME, dso, log, type, spid, name, fmt, buf, size);                                           \
    }
SIGNPOST_IMPL(_os_signpost_emit_with_name_impl)
SIGNPOST_IMPL(_os_signpost_emit_unreliably_with_name_impl)

static char *hao_os_log_send_and_compose_impl(uint32_t options, const char **fmt_out, char *out, size_t outlen,
                                              void *dso, os_log_t log, os_log_type_t type, const char *fmt,
                                              uint8_t *buf, uint32_t size)
{
    if (!from_guest(dso))
        __attribute__((musttail)) return _os_log_send_and_compose_impl(options, fmt_out, out, outlen, dso, log, type,
                                                                        fmt, buf, size);
    int e = errno;
    char own[COMPOSE_MAX];
    bool want = options & 2;
    char *r = _os_log_send_and_compose_impl((options & ~1u) | 2, fmt_out, want ? out : own, want ? outlen : sizeof own,
                                            dso, log, type, fmt, buf, size);
    if ((options & 1) && r) {
        /* the caller may keep the composed text: send a copy with the mask rewritten */
        char m[COMPOSE_MAX];
        strlcpy(m, r, sizeof m);
        emit_log(type == OS_LOG_TYPE_FAULT ? _os_log_fault_impl : type == OS_LOG_TYPE_ERROR ? _os_log_error_impl
                                                                                         : _os_log_impl,
                 log, type, m);
    }
    errno = e;
    if (!want) {
        if (r && r != own) free(r);
        return NULL;
    }
    return r;
}

static char *pack_compose(os_log_pack_t pack, os_log_t log, os_log_type_t type, char *out, size_t n)
{
    return os_log_pack_compose(pack, log, type, out, n);
}

static void hao_os_log_pack_send(os_log_pack_t pack, os_log_t log, os_log_type_t type)
{
    if (!pack || !from_guest(pack->mh)) __attribute__((musttail)) return os_log_pack_send(pack, log, type);
    int e = errno;
    if (os_log_type_enabled(log, type)) {
        char out[COMPOSE_MAX];
        char *r = pack_compose(pack, log, type, out, sizeof out);
        if (r) emit_log(_os_log_impl, log, type, r);
        if (r && r != out) free(r);
    }
    errno = e;
}

static char *hao_os_log_pack_send_and_compose(os_log_pack_t pack, os_log_t log, os_log_type_t type, char *buf,
                                              size_t size)
{
    if (!pack || !from_guest(pack->mh))
        __attribute__((musttail)) return os_log_pack_send_and_compose(pack, log, type, buf, size);
    int e = errno;
    char *r = pack_compose(pack, log, type, buf, size);
    if (r) {
        char m[COMPOSE_MAX];
        strlcpy(m, r, sizeof m);
        emit_log(_os_log_impl, log, type, m);
    }
    errno = e;
    return r;
}

static void hao_os_signpost_pack_send(os_log_pack_t pack, os_log_t log, os_signpost_type_t type)
{
    if (!pack || !from_guest(pack->mh)) __attribute__((musttail)) return _os_signpost_pack_send(pack, log, type);
    int e = errno;
    if (os_signpost_enabled(log)) {
        char out[COMPOSE_MAX];
        char *r = pack_compose(pack, log, OS_LOG_TYPE_DEFAULT, out, sizeof out);
        if (r) emit_signpost(_os_signpost_emit_with_name_impl, log, type, (os_signpost_id_t)pack->spid, pack->name, r);
        if (r && r != out) free(r);
    }
    errno = e;
}

/* _os_log_internal(dso, log, type, format, ...): the format's arguments encoded as clang's
 * __builtin_os_log_format encodes them (summary, count, then descriptor/size/data items), then composed */
static size_t encode(const char *f, va_list ap, uint8_t *b, size_t cap)
{
    size_t o = 2;
    unsigned items = 0, summary = 0;
#define ITEM(desc, sz, src)                                                                                    \
    do {                                                                                                       \
        if (o + 2 + (sz) > cap || items == 255) goto done;                                                     \
        b[o] = (uint8_t)(desc);                                                                                \
        b[o + 1] = (uint8_t)(sz);                                                                              \
        memcpy(b + o + 2, (src), (sz));                                                                        \
        o += 2 + (sz);                                                                                         \
        items++;                                                                                               \
        if ((desc) & 1) summary |= 1;                                                                          \
        if ((desc) >> 4 && (desc) >> 4 != 1) summary |= 2;                                                     \
    } while (0)
    while ((f = strchr(f, '%'))) {
        f++;
        if (*f == '%') { f++; continue; }
        unsigned priv = 0;
        char mask[8] = {0};
        bool masked = false;
        if (*f == '{') {
            const char *e = strchr(f, '}');
            if (!e) break;
            for (const char *a = f + 1; a < e;) {
                while (a < e && (*a == ' ' || *a == ',')) a++;
                const char *z = a;
                while (z < e && *z != ',') z++;
                const char *t = z;
                while (t > a && t[-1] == ' ') t--;
                size_t n = (size_t)(t - a);
                if (n == 6 && !memcmp(a, "public", 6)) priv = 2;
                else if (n == 7 && !memcmp(a, "private", 7)) priv = 1;
                else if (n == 9 && !memcmp(a, "sensitive", 9)) priv = 5;
                else if (n > 5 && !memcmp(a, "mask.", 5)) {
                    masked = true;
                    memcpy(mask, a + 5, n - 5 < 8 ? n - 5 : 8);
                }
                a = z;
            }
            f = e + 1;
        }
        while (*f && strchr("-+ #0'", *f)) f++;
        if (*f == '*') {
            int w = va_arg(ap, int);
            ITEM(0, 4, &w);
            f++;
        } else
            while (*f >= '0' && *f <= '9') f++;
        bool prec = false;
        int precision = 0;
        if (*f == '.') {
            f++;
            prec = true;
            if (*f == '*') { precision = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') precision = precision * 10 + (*f++ - '0');
        }
        int longs = 0;
        for (;; f++) {
            if (*f == 'l' || *f == 'q' || *f == 'j' || *f == 'z' || *f == 't') longs++;
            else if (*f == 'h' || *f == 'L') {}
            else break;
        }
        char conv = *f;
        if (!conv) break;
        f++;
        if (masked) ITEM(0x70, 8, mask);
        switch (conv) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'c': case 'C':
            if (longs) { long long v = va_arg(ap, long long); ITEM(priv, 8, &v); }
            else { int v = va_arg(ap, int); ITEM(priv, 4, &v); }
            break;
        case 'p': { void *v = va_arg(ap, void *); ITEM(priv, 8, &v); break; }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v = va_arg(ap, double);   /* Apple arm64 long double is double */
            ITEM(priv, 8, &v);
            break;
        }
        case 's': case 'S': case 'P': {
            if (prec) ITEM(0x10, 4, &precision);
            void *v = va_arg(ap, void *);
            ITEM((conv == 'P' ? 0x30 : 0x20) | priv, 8, &v);
            break;
        }
        case '@': { void *v = va_arg(ap, void *); ITEM(0x40 | priv, 8, &v); break; }
        case 'm': ITEM(0x60 | priv, 0, b); break;
        case 'n': (void)va_arg(ap, void *); break;
        default: goto done;
        }
    }
done:
#undef ITEM
    b[0] = (uint8_t)summary;
    b[1] = (uint8_t)items;
    return o;
}

/* libhao's trap for guest calls of _os_log_internal (src/interpose.c), with the guest's variadic slots
 * as an x86_64 va_list. Not interposed: native callers keep libtrace's own encoder. */
void hao_identity_os_log_internal_v(void *dso, os_log_t log, os_log_type_t type, const char *fmt, va_list ap)
{
    int e = errno;
    uint8_t buf[1024];
    size_t n = encode(fmt, ap, buf, sizeof buf);
    errno = e;
    if (from_guest(dso)) guest_log(_os_log_impl, dso, log, type, fmt, buf, (uint32_t)n);
    else _os_log_impl(dso, log, type, fmt, buf, (uint32_t)n);
    errno = e;
}

/* libtrace shows an unannotated string argument only when it points into immutable memory, a string
 * literal ("plain %s" of a constant prints the text, of a heap copy <private>): it asks
 * _dyld_is_memory_immutable, which knows only dyld's images and the shared cache. A guest image's
 * read-only segments are as immutable as a native image's: hao never unloads a guest image. */
extern bool _dyld_is_memory_immutable(const void *addr, size_t length);
static bool hao_dyld_is_memory_immutable(const void *addr, size_t length)
{
    if (_dyld_is_memory_immutable(addr, length)) return true;
    oslog_immutable_fn f = atomic_load_explicit(&immutable_hook, memory_order_acquire);
    return f && f(addr, length);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } hao_oslog_interposers[] = {
    {(const void *)hao_os_log_impl, (const void *)_os_log_impl},
    {(const void *)hao_os_log_error_impl, (const void *)_os_log_error_impl},
    {(const void *)hao_os_log_fault_impl, (const void *)_os_log_fault_impl},
    {(const void *)hao_os_log_debug_impl, (const void *)_os_log_debug_impl},
    {(const void *)hao_os_log_unreliable_impl, (const void *)_os_log_unreliable_impl},
    {(const void *)hao_os_log_send_and_compose_impl, (const void *)_os_log_send_and_compose_impl},
    {(const void *)hao_os_signpost_emit_with_name_impl, (const void *)_os_signpost_emit_with_name_impl},
    {(const void *)hao_os_signpost_emit_unreliably_with_name_impl, (const void *)_os_signpost_emit_unreliably_with_name_impl},
    {(const void *)hao_os_log_pack_send, (const void *)os_log_pack_send},
    {(const void *)hao_os_log_pack_send_and_compose, (const void *)os_log_pack_send_and_compose},
    {(const void *)hao_os_signpost_pack_send, (const void *)_os_signpost_pack_send},
    {(const void *)hao_dyld_is_memory_immutable, (const void *)_dyld_is_memory_immutable},
};
#pragma clang diagnostic pop

/* for hao_identity_original (src/process_identity.c) */
const void *hao_oslog_original(const void *fn)
{
    for (size_t i = 0; i < sizeof hao_oslog_interposers / sizeof *hao_oslog_interposers; i++)
        if (hao_oslog_interposers[i].replacement == fn) return hao_oslog_interposers[i].original;
    return NULL;
}
