/*
 * hao — std::string parameters of native C++ functions outside libc++
 * (plan kind 'Z', hao/bridge.h; the table is src/cxxstr_gen.inc, whose generator
 * is not part of this repository).
 *
 * Apple arm64 libc++ uses the alternate string layout, x86_64 libc++ the
 * standard one (libc++ <string>, _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT):
 *
 *   arm64   long  {data, size, cap | 1 << 63}     cap: the allocation's size
 *           short {chars[23]}, byte 23 = size (bit 7 clear)
 *   x86_64  long  {cap | 1, size, data}           cap: the allocation's size (even)
 *           short byte 0 = size << 1, chars from byte 1
 *
 * Guest code builds its strings inline (libc++'s inline members), so no
 * libc++ function sees them before a native library that is not libc++
 * does (LocationSupport, ProtocolBuffer, caulk: they carry ObjC and stay
 * native). For each std::string parameter the
 * plan names (by const reference, by value: a non-trivial class, passed by
 * address on both ABIs; by reference: written back) the call gets a native
 * string built by native libc++'s own exported members (__init, ~basic_string)
 * in place of the guest's, and destroys it afterwards. A string the callee
 * may write is copied back into the guest's string with libc++'s assign
 * semantics (its own buffer when it fits, else a new one from operator new,
 * which is native libc++abi's and the heap guest libc++ allocates from).
 * Native code that throws destroys the native strings on the way out
 * (cleanup; this file is built with -fexceptions); the guest string is then
 * left as it was.
 */
#include "hao/runtime.h"
#include "hao/gen_stamp.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name, *enc, *why; } cxxstr_ent;
static const cxxstr_ent cxxstr_tab[] = {
#include "cxxstr_gen.inc"
};

void hao_cxxstr_names(hao_name_fn cb, void *ctx)
{
    for (size_t k = 0; k < sizeof cxxstr_tab / sizeof *cxxstr_tab; k++) cb(cxxstr_tab[k].name, ctx);
}

int hao_cxxstr_lookup(const char *name, const char **enc, const char **why)
{
    size_t lo = 0, hi = sizeof cxxstr_tab / sizeof *cxxstr_tab;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = strcmp(name, cxxstr_tab[mid].name);
        if (!r) {
            if (cxxstr_tab[mid].enc && hao_cxxstr_enc_ok(cxxstr_tab[mid].enc)) { *enc = cxxstr_tab[mid].enc; return 1; }
            *why = cxxstr_tab[mid].why;
            return -1;
        }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

typedef char *(*demangle_fn)(const char *, char *, size_t *, int *);

static demangle_fn cxx_demangler(void)
{
    static demangle_fn dem;
    if (!dem) dem = (demangle_fn)dlsym(RTLD_DEFAULT, "__cxa_demangle");
    return dem;
}

/* The demangled signature names one of libc++'s string, function or string-stream templates. This
 * reads native libc++abi's spelling of the name (the qualification std::__1 can be a substitution in
 * the mangled form, which is not resolved here); hao_cxx_demangle_selfcheck pins that spelling. */
static bool divergent_text(const char *d)
{
    return strstr(d, "std::__1::basic_string<") || strstr(d, "std::__1::function<") || strstr(d, "stringstream<") ||
           strstr(d, "std::__1::basic_stringbuf<");
}

/* Fixed names and whether divergent_text holds for the demangler's spelling of each. */
static const struct { const char *sym; bool divergent; } spellings[] = {
    {"__ZN3hao1fERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", true},
    {"__ZN3hao1fENSt3__18functionIFvvEEE", true},
    {"__ZN3hao1fERNSt3__118basic_stringstreamIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", true},
    {"__ZN3hao1fERNSt3__115basic_stringbufIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", true},
    {"__ZN3hao1fEPKc", false},
};

int hao_cxx_demangle_selfcheck(char *why, size_t n)
{
    demangle_fn dem = cxx_demangler();
    if (!dem) return -1;
    int bad = 0;
    why[0] = 0;
    for (size_t k = 0; k < sizeof spellings / sizeof *spellings; k++) {
        int st = 0;
        char *d = dem(spellings[k].sym + 1, NULL, NULL, &st);
        if (!d || st || divergent_text(d) != spellings[k].divergent)
            if (!bad++) snprintf(why, n, "__cxa_demangle(%s) = \"%s\": std::string parameters %s", spellings[k].sym + 1,
                                 d && !st ? d : "(fails)", spellings[k].divergent ? "not recognised" : "recognised wrongly");
        free(d);
    }
    return bad;
}

const char *hao_cxxstr_divergent(const char *name, const char *lib)
{
    if (strncmp(name, "__Z", 3)) return NULL;
    const char *enc = NULL, *why = NULL;
    int t = hao_cxxstr_lookup(name, &enc, &why);
    if (t > 0) return NULL;
    if (t < 0) return why;
    /* libc++ (guest normally; native only with HAO_NATIVE_LIBCXX, where its own interface is the
     * point) and libc++abi (no std::string in its interface) are not this class */
    const char *b = lib ? strrchr(lib, '/') : NULL;
    b = b ? b + 1 : lib;
    if (b && (!strncmp(b, "libc++.", 7) || !strncmp(b, "libc++abi", 9))) return NULL;
    /* cheap filter on the mangled name before demangling: every spelling names the template */
    if (!strstr(name, "12basic_stringI") && !strstr(name, "8functionI") && !strstr(name, "stringstreamI") &&
        !strstr(name, "15basic_stringbufI"))
        return NULL;
    demangle_fn dem = cxx_demangler();
    if (!dem) return NULL;
    int st = 0;
    char *d = dem(name + 1, NULL, NULL, &st);
    if (!d || st) { free(d); return NULL; }
    const char *r = NULL;
    if (divergent_text(d))
        r = "a std::string, std::function or string stream (layout differs between arm64 and x86_64) in a native C++ "
            "signature cxxstr_gen.inc does not type";
    free(d);
    return r;
}

/* ---------------------------------------------------------------- strings */

#define SSO 22                       /* chars a short string holds, both layouts */
#define ARM_LONG (1ull << 63)

typedef struct { uint64_t w[3]; } nstr;    /* an x86_64 libc++ std::string */

typedef void (*init_fn)(nstr *, const char *, size_t);
typedef void (*dtor_fn)(nstr *);
typedef void *(*new_fn)(size_t);
typedef void (*del_fn)(void *);
static init_fn n_init;
static dtor_fn n_dtor;
static new_fn n_new;
static del_fn n_del;

static void resolve(void)
{
    if (__atomic_load_n(&n_del, __ATOMIC_ACQUIRE)) return;
    n_init = (init_fn)dlsym(RTLD_DEFAULT, "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm");
    n_dtor = (dtor_fn)dlsym(RTLD_DEFAULT, "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev");
    n_new = (new_fn)dlsym(RTLD_DEFAULT, "_Znwm");
    del_fn d = (del_fn)dlsym(RTLD_DEFAULT, "_ZdlPv");
    if (!n_init || !n_dtor || !n_new || !d) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "cxxstr: native libc++ string members not found; cannot convert std::string arguments\n");
        abort();
    }
    __atomic_store_n(&n_del, d, __ATOMIC_RELEASE);
}

/* the guest string's characters and size (arm64 alternate layout) */
static const char *g_view(const uint64_t *g, size_t *n)
{
    if (g[2] & ARM_LONG) { *n = (size_t)g[1]; return (const char *)(uintptr_t)g[0]; }
    *n = ((const uint8_t *)g)[23] & 0x7f;
    return (const char *)g;
}

/* the native string's characters and size (x86_64 standard layout) */
static const char *n_view(const nstr *s, size_t *n)
{
    if (s->w[0] & 1) { *n = (size_t)s->w[1]; return (const char *)(uintptr_t)s->w[2]; }
    *n = ((const uint8_t *)s)[0] >> 1;
    return (const char *)s + 1;
}

/* guest string = (p, n), as basic_string::assign(const char *, size_t) does it */
static void g_assign(uint64_t *g, const char *p, size_t n)
{
    size_t cur;
    const char *old = g_view(g, &cur);
    if (cur == n && !memcmp(old, p, n)) return;
    bool lng = g[2] & ARM_LONG;
    size_t cap = lng ? (size_t)(g[2] & ~ARM_LONG) - 1 : SSO;
    if (n <= cap) {
        char *d = lng ? (char *)(uintptr_t)g[0] : (char *)g;
        memmove(d, p, n);
        d[n] = 0;
        if (lng) g[1] = n;
        else ((uint8_t *)g)[23] = (uint8_t)n;
        return;
    }
    size_t alloc = (n + 1 + 7) & ~(size_t)7;     /* libc++'s __recommend: n + 1 rounded to 8 */
    char *d = n_new(alloc);
    memcpy(d, p, n);
    d[n] = 0;
    if (lng) n_del((void *)(uintptr_t)g[0]);
    g[0] = (uint64_t)(uintptr_t)d;
    g[1] = n;
    g[2] = alloc | ARM_LONG;
}

typedef struct { nstr s[6]; uint8_t live[6]; } cxxstr_frame;

static void frame_cleanup(cxxstr_frame *f)
{
    for (int k = 0; k < 6; k++)
        if (f->live[k]) { f->live[k] = 0; n_dtor(&f->s[k]); }
}

void hao_cxxstr_call(hao_cpu *c, uint64_t pc, const char *enc)
{
    resolve();
    __attribute__((cleanup(frame_cleanup))) cxxstr_frame f;
    memset(f.live, 0, sizeof f.live);
    uint64_t g[6] = {0};
    const char *roles = enc + 1;
    int nr = (int)strlen(roles);
    for (int k = 0; k < nr && k < 6; k++) {
        if (roles[k] == '-') continue;
        g[k] = c->x[k];
        size_t n;
        const char *p = g_view((const uint64_t *)(uintptr_t)g[k], &n);
        n_init(&f.s[k], p, n);
        f.live[k] = 1;
        c->x[k] = (uint64_t)(uintptr_t)&f.s[k];
    }
    uint64_t self = c->x[0];
    hao_native_call(pc, c, 16);
    if (enc[0] == 'P') c->x[0] = self;
    for (int k = 0; k < nr && k < 6; k++) {
        if (roles[k] != 'w') continue;
        size_t n;
        const char *p = n_view(&f.s[k], &n);
        g_assign((uint64_t *)(uintptr_t)g[k], p, n);
    }
}
