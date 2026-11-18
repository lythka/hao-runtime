/* Per-type facts of the Swift value walk (Facts.h): src/swiftwalk_gen.inc,
 * included once per section. Under the test switch HAO_SWIFT_WALK_FACTS=FILE
 * (a HAO_TEST_SWITCHES build, an open process) a fixture file's rows are
 * looked up first, in any process, with no layout check: one row a line,
 *   type KEY VERDICT [XI-CLASS XI-OFFSET [UNCHECKED]]   (SAME, DYNAMIC, CONVERT or REFUSE; NONE, SAME, POINTER,
 *        OTHER; UNCHECKED: a SAME row on a type with guest parts, which no generated row names)
 *   image KEY CASE ARM64-IMAGE ARM64-MASK X86_64-IMAGE X86_64-MASK   (hex, as SWIFTWALK_IMAGES)
 * ("#" starts a comment; tests/guest/svfacts.facts). */
#include "Facts.h"
#include "hao/aot.h"
#include "hao/gen_stamp.h"
#include "hao/gmap.h"
#include "hao/opts.h"
#include "hao/runtime.h"
#include "hao/dlsym_once.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWIFT __attribute__((swiftcall))
#define SWIFTWALK_SAME WALK_FACT_SAME
#define SWIFTWALK_DYNAMIC WALK_FACT_DYNAMIC
#define SWIFTWALK_CONVERT WALK_FACT_CONVERT
#define SWIFTWALK_REFUSE WALK_FACT_REFUSE
#define SWIFTWALK_XI_NONE WALK_XI_NONE
#define SWIFTWALK_XI_SAME WALK_XI_SAME
#define SWIFTWALK_XI_POINTER WALK_XI_POINTER
#define SWIFTWALK_XI_OTHER WALK_XI_OTHER
static const struct {
    const char *key;
    char verdict;
    uint64_t size, stride, align;
    uint32_t flags, arm_xi, x86_xi;
    char xi_class;
    int32_t xi_offset;
    uint32_t payload, empty;
    int32_t first_child, children, first_image, images;
} types[] = {
#define SWIFTWALK_TYPES
#include "swiftwalk_gen.inc"
#undef SWIFTWALK_TYPES
    {NULL, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0},   /* the end; never matched */
};
/* A case image and its mask are SIZE bytes (the type's), NULs included. */
static const struct { int32_t type, which; const char *arm, *arm_mask, *x86, *x86_mask; } images[] = {
#define SWIFTWALK_IMAGES
#include "swiftwalk_gen.inc"
#undef SWIFTWALK_IMAGES
    {-1, 0, "", "", "", ""},
};
#define NTYPES (sizeof types / sizeof *types - 1)
#define NIMAGES (sizeof images / sizeof *images - 1)
/* The x86_64 shared cache the table was derived from (generated.lock's
 * cache_uuid for src/swiftwalk_gen.inc), until the table states it itself. */
#ifndef SWIFTWALK_X86_CACHE_UUID
#define SWIFTWALK_X86_CACHE_UUID "954AA5DE1C5D3D479921AF7AF681D333"
#endif

typedef struct { char verdict; char *key; unsigned first_image, nimages; char xi_class; int xi_offset; bool unchecked; } fixture_type;
typedef struct { unsigned which; unsigned char *arm, *arm_mask, *x86, *x86_mask; size_t n; } fixture_image;
static fixture_type *ftypes;
static fixture_image *fimages;
static unsigned nftypes, nfimages;
static bool fixture_loaded;

static hao_gmap by_type = HAO_GMAP_INIT(2, 1, 6, 14);   /* metadata -> row + 1 (fixture rows from 1 << 32), or 1 << 63: none */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void walk_facts_fork_child(void) { lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

/* Whether this process's x86_64 shared cache is the table's. */
static bool table_applies(void)
{
    static int state;   /* 0 unknown, 1 yes, 2 no */
    int s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (!s) {
        extern bool _dyld_get_shared_cache_uuid(unsigned char uuid[16]);
        unsigned char uuid[16];
        char text[33];
        static const char digits[] = "0123456789ABCDEF";
        s = 2;
        if (NTYPES && _dyld_get_shared_cache_uuid(uuid)) {
            for (unsigned k = 0; k < 16; ++k) { text[2 * k] = digits[uuid[k] >> 4]; text[2 * k + 1] = digits[uuid[k] & 15]; }
            text[32] = 0;
            if (!strcasecmp(text, SWIFTWALK_X86_CACHE_UUID)) s = 1;
        }
        __atomic_store_n(&state, s, __ATOMIC_RELEASE);
    }
    return s == 1;
}

static unsigned char *unhex(const char *t, size_t *n)
{
    size_t len = strlen(t);
    if (len % 2 || len / 2 > WALK_FACT_IMAGE_BYTES) return NULL;
    unsigned char *out = malloc(len / 2 + 1);
    for (size_t i = 0; out && i < len / 2; ++i) {
        unsigned b;
        if (sscanf(t + 2 * i, "%2x", &b) != 1) { free(out); return NULL; }
        out[i] = (unsigned char)b;
    }
    *n = len / 2;
    return out;
}

static void load_fixture_from(const char *path);
static void load_fixture(void)
{
    if (__atomic_load_n(&fixture_loaded, __ATOMIC_ACQUIRE)) return;
    load_fixture_from(hao_opt.swift_walk_facts);
}
/* Tests that load libhao into a native program (tests/host/swiftlayout):
 * the fixture at PATH, before the first lookup. A test build only. */
int walk_facts_load(const char *path)
{
    if (!hao_test_switches || !path || __atomic_load_n(&fixture_loaded, __ATOMIC_ACQUIRE)) return 0;
    load_fixture_from(path);
    return nftypes > 0;
}
static void load_fixture_from(const char *path)
{
    hao_crit_enter(); pthread_mutex_lock(&lock);
    if (!fixture_loaded && hao_test_switches && path && path[0]) {
        FILE *f = fopen(path, "r");
        if (!f) HAO_LOG(0, "HAO_SWIFT_WALK_FACTS: cannot open %s\n", path);
        char line[4096], key[2048], a[300], b[300], c[300], d[300], verdict[16];
        unsigned which;
        while (f && fgets(line, sizeof line, f) && nftypes < 1024 && nfimages < 1024) {
            char xi[16] = "NONE", mark[16] = "";
            int xi_offset = -1;
            if (sscanf(line, "type %2047s %15s %15s %d %15s", key, verdict, xi, &xi_offset, mark) >= 2) {
                fixture_type *g = realloc(ftypes, (nftypes + 1) * sizeof *ftypes);
                if (!g) break;
                ftypes = g;
                char v = !strcmp(verdict, "SAME") ? WALK_FACT_SAME : !strcmp(verdict, "DYNAMIC") ? WALK_FACT_DYNAMIC :
                         !strcmp(verdict, "CONVERT") ? WALK_FACT_CONVERT : WALK_FACT_REFUSE;
                char c = !strcmp(xi, "SAME") ? WALK_XI_SAME : !strcmp(xi, "POINTER") ? WALK_XI_POINTER :
                         !strcmp(xi, "OTHER") ? WALK_XI_OTHER : WALK_XI_NONE;
                ftypes[nftypes++] = (fixture_type){v, strdup(key), 0, 0, c, xi_offset, !strcmp(mark, "UNCHECKED")};
            } else if (sscanf(line, "image %2047s %u %299s %299s %299s %299s", key, &which, a, c, b, d) == 6) {
                size_t na = 0, nb = 0, nc = 0, nd = 0;
                unsigned char *arm = unhex(a, &na), *x86 = unhex(b, &nb), *am = unhex(c, &nc), *xm = unhex(d, &nd);
                fixture_image *g = realloc(fimages, (nfimages + 1) * sizeof *fimages);
                if (!g || !arm || !x86 || !am || !xm || na != nb || nc != na || nd != na) {
                    free(arm); free(x86); free(am); free(xm); if (g) fimages = g; continue;
                }
                fimages = g;
                for (unsigned i = 0; i < nftypes; ++i)
                    if (!strcmp(ftypes[i].key, key)) {
                        if (!ftypes[i].nimages) ftypes[i].first_image = nfimages;
                        if (ftypes[i].first_image + ftypes[i].nimages == nfimages) ftypes[i].nimages++;
                    }
                fimages[nfimages++] = (fixture_image){which, arm, am, x86, xm, na};
            }
        }
        if (f) fclose(f);
    }
    __atomic_store_n(&fixture_loaded, true, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock); hao_crit_leave();
}

const char *walk_fact_mangled(const void *type, uint64_t *length)
{
    typedef struct { const char *text; uintptr_t length; } pair;
    typedef pair SWIFT mangled_fn(const void *);
    mangled_fn *mangled = HAO_DLSYM("swift_getMangledTypeName");
    if (!type || !mangled) return NULL;
    pair p = mangled(type);
    if (!p.text || !p.length || p.length > 4096) return NULL;
    *length = p.length;
    return p.text;
}

static bool hexdigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
int walk_fact_key(const char *s, uint64_t n, char *out, uint64_t cap)
{
    uint64_t o = 0;
    for (uint64_t i = 0; i < n;) {
        if (s[i] >= '0' && s[i] <= '9') {   /* a run of digits: "<digits>$<hex>yXZ" is a private context */
            uint64_t j = i;
            while (j < n && s[j] >= '0' && s[j] <= '9') ++j;
            if (j < n && s[j] == '$') {
                uint64_t k = j + 1;
                while (k < n && hexdigit(s[k])) ++k;
                if (k > j + 1 && k + 3 <= n && !memcmp(s + k, "yXZ", 3)) {
                    if (o + 4 >= cap) return 0;
                    memcpy(out + o, "$yXZ", 4); o += 4;
                    i = k + 3;
                    continue;
                }
            }
            if (o + (j - i) >= cap) return 0;
            memcpy(out + o, s + i, j - i); o += j - i;
            i = j;
            continue;
        }
        if (o + 1 >= cap) return 0;
        out[o++] = s[i++];
    }
    out[o] = 0;
    return 1;
}

static bool peek(uint64_t address, void *out, size_t size)
{
    mach_vm_size_t got = 0;
    return address >= 4096 && address <= UINT64_MAX - size &&
        mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS &&
        got == size;
}
static bool guest_addr(uint64_t a) { return hao_image_for_addr(a) != NULL; }
/* An address in a native image dyld loaded: not the heap, not a guest image. */
static bool native_addr(uint64_t a)
{
    Dl_info i = {0};
    return a >= 4096 && !guest_addr(a) && dladdr((const void *)(uintptr_t)a, &i) && i.dli_fbase;
}
static bool native_code(uint64_t f) { return native_addr(f) && !hao_image_for_code(f) && !hao_guest_of_entry(f) && !hao_is_trap(f); }
typedef struct { const void *metadata; uintptr_t state; } facts_response;
typedef facts_response SWIFT generic_metadata_fn(uintptr_t, const void *const *, const void *);
typedef const void *conforms_fn(const void *, const void *);
typedef const void *SWIFT metatype_fn(const void *);
typedef facts_response SWIFT tuple_fn(uintptr_t, uintptr_t, const void *const *, const char *, const void *);
typedef const void *SWIFT existential_fn(uintptr_t, const void *, uintptr_t, const void *const *);
/* The generic parameter a requirement's mangled subject names, at depth 0
 * ("x", "q_", "q<n>_"); -1 for anything else. */
static int subject_param(uint64_t name)
{
    char t[8] = {0};
    if (!peek(name, t, 1)) return -1;
    if (t[0] == 'x') return peek(name + 1, t + 1, 1) && !t[1] ? 0 : -1;
    if (t[0] != 'q' || !peek(name, t, sizeof t - 1)) return -1;
    int n = 0, k = 1;
    if (t[k] == '_') return t[k + 1] ? -1 : 1;                           /* "x" is 0, "q_" 1, "q<n>_" n + 2 */
    while (t[k] >= '0' && t[k] <= '9' && k < 5) n = n * 10 + (t[k++] - '0');
    return k > 1 && t[k] == '_' && !t[k + 1] ? n + 2 : -1;
}
static bool rel32(uint64_t field, uint64_t *out)
{
    int32_t r;
    if (!peek(field, &r, 4) || !r) return false;
    *out = field + (int64_t)r;
    return true;
}
static bool native_identity(const void *type, unsigned depth);
/* A generic nominal TYPE of native descriptor DESC (generic header at
 * DESC + HEAD, key arguments at ARGS): the runtime's own metadata for its
 * arguments, each type argument native and each conformance the runtime's. */
static bool native_generic(const void *type, uint64_t desc, uint64_t head_at, uint64_t args_at, unsigned depth)
{
    uint16_t head[4];   /* GenericContextDescriptorHeader */
    if (!peek(desc + head_at, head, sizeof head) || head[0] > 16 || head[1] > 16 || head[2] > 32 || (head[3] & 1))
        return false;   /* no packs */
    unsigned char params[16];
    if (head[0] && !peek(desc + head_at + 8, params, head[0])) return false;
    uint64_t args[32];
    if (head[2] && !peek(args_at, args, 8ull * head[2])) return false;
    int slot_of[16];
    unsigned arg = 0;
    for (unsigned i = 0; i < head[0]; ++i) {
        slot_of[i] = -1;
        if (!(params[i] & 0x80)) continue;
        if (arg >= head[2]) return false;
        slot_of[i] = (int)arg;
        unsigned k = params[i] & 0x3f;
        if (k == 2) { ++arg; continue; }                               /* a value */
        if (k || !native_identity((const void *)(uintptr_t)args[arg++], depth + 1)) return false;
    }
    conforms_fn *conforms = HAO_DLSYM("swift_conformsToProtocol");
    uint64_t req = desc + head_at + 8 + ((head[0] + 3u) & ~3u);
    for (unsigned i = 0; i < head[1]; ++i, req += 12) {
        uint32_t rf;
        int32_t pr;
        uint64_t name, proto;
        if (!peek(req, &rf, 4)) return false;
        if (!(rf & 0x80)) continue;                                    /* no key argument */
        if ((rf & 0x1f) || arg >= head[2] || !rel32(req + 4, &name) || !peek(req + 8, &pr, 4) || !pr || (pr & 2))
            return false;                                              /* a Swift protocol conformance only */
        proto = req + 8 + (int64_t)(pr & ~3);
        if ((pr & 1) && !peek(proto, &proto, 8)) return false;
        int p = subject_param(name);
        if (p < 0 || p >= head[0] || slot_of[p] < 0 || (params[p] & 0x3f) || !native_addr(proto) || !conforms ||
            conforms((const void *)(uintptr_t)args[slot_of[p]], (const void *)(uintptr_t)proto) !=
                (const void *)(uintptr_t)args[arg++])
            return false;
    }
    if (arg != head[2]) return false;
    generic_metadata_fn *get = HAO_DLSYM("swift_getGenericMetadata");
    return get && get(0, (const void *const *)(uintptr_t)args_at, (const void *)(uintptr_t)desc).metadata == type;
}
/* A non-generic nominal TYPE of native descriptor DESC: in its image, or
 * what the descriptor's native access function returns. */
static bool native_plain(const void *type, uint64_t desc)
{
    uint64_t access;
    if (native_addr((uint64_t)(uintptr_t)type)) return true;
    if (!rel32(desc + 12, &access) || !native_code(access)) return false;
    return ((facts_response SWIFT (*)(uintptr_t))(uintptr_t)access)(0).metadata == type;
}
/* Whether TYPE is the native type its name says, proven positively: a SAME
 * row skips the walk, so a guest type spelled like a listed one (its own
 * module, a guest generic argument, metadata forged on the heap) must not
 * get it. A non-generic type's metadata lies in a native image; a generic
 * one's is what the native runtime returns for its native descriptor and its
 * key arguments, each generic argument native in turn and each conformance
 * the one the runtime finds. Its destroy witness is native code. */
static bool native_identity(const void *type, unsigned depth)
{
    uint64_t t = (uint64_t)(uintptr_t)type, m[2], vwt, destroy;
    if (depth > 8 || !peek(t - 8, &vwt, 8) || !peek(t, m, sizeof m) || guest_addr(t) || guest_addr(vwt) ||
        !peek(vwt + 8, &destroy, 8) || !native_code(destroy))
        return false;
    uint64_t kind = m[0];
    if (kind > 0x7ff) {   /* a class: an ObjC class in its image, or a Swift class by its descriptor */
        uint64_t data, desc;
        uint32_t flags, bounds[3];
        if (native_addr(t)) return true;
        if (!peek(t + 32, &data, 8) || !(data & 3) || !peek(t + 64, &desc, 8) || !native_addr(desc) ||
            !peek(desc, &flags, 4) || (flags & 31) != 16) return false;
        if (!(flags & 0x80)) return native_plain(type, desc);
        /* no resilient superclass, no negative members: arguments at positive size - immediate members */
        if ((flags & (3u << 28)) || !peek(desc + 24, bounds, sizeof bounds) || bounds[1] <= bounds[2] || bounds[1] > 4096)
            return false;
        return native_generic(type, desc, 52, t + 8ull * (bounds[1] - bounds[2]), depth);
    }
    switch (kind) {
    case 0x200: case 0x201: case 0x202: {
        uint64_t desc = m[1];
        uint32_t flags;
        if (!native_addr(desc) || !peek(desc, &flags, 4) || ((flags & 31) != 17 && (flags & 31) != 18)) return false;
        /* after the type's 28 bytes and the two rel32 of TypeGenericContextDescriptorHeader; arguments at word 2 */
        return flags & 0x80 ? native_generic(type, desc, 36, t + 16, depth) : native_plain(type, desc);
    }
    case 0x300: case 0x203: case 0x204: return native_addr(t);   /* builtin and foreign: in their image */
    case 0x301: {   /* the runtime's tuple of these elements and labels */
        uint64_t labels, e[16];
        char text[512] = {0};
        if (m[1] > 16 || !peek(t + 16, &labels, 8)) return false;
        if (labels) {
            unsigned n = 0;
            while (n < sizeof text - 1 && peek(labels + n, text + n, 1) && text[n]) ++n;
            if (n == sizeof text - 1 || text[n]) return false;
        }
        for (uint64_t i = 0; i < m[1]; ++i)
            if (!peek(t + 24 + 16 * i, &e[i], 8) || !native_identity((const void *)(uintptr_t)e[i], depth + 1)) return false;
        tuple_fn *get = HAO_DLSYM("swift_getTupleTypeMetadata");
        return get && get(0, m[1], (const void *const *)e, labels ? text : NULL, NULL).metadata == type;
    }
    case 0x302: return native_addr(vwt);   /* a function value: a code pointer and a context, whatever its signature */
    case 0x303: {   /* the runtime's existential of these native protocols */
        uint32_t flags = (uint32_t)m[1], n = (uint32_t)(m[1] >> 32);
        uint64_t at = t + 16, super = 0, p[16];
        if (n > 16) return false;
        if (flags & 0x40000000) {
            if (!peek(at, &super, 8) || !native_identity((const void *)(uintptr_t)super, depth + 1)) return false;
            at += 8;
        }
        if (n && !peek(at, p, 8ull * n)) return false;
        for (uint32_t i = 0; i < n; ++i) if (!native_addr(p[i] & ~UINT64_C(1))) return false;
        existential_fn *get = HAO_DLSYM("swift_getExistentialTypeMetadata");
        return get && get((flags & 0x80000000u) ? 1 : 0, (const void *)(uintptr_t)super, n, (const void *const *)p) == type;
    }
    case 0x304: case 0x306: {
        metatype_fn *get = kind == 0x304 ? HAO_DLSYM("swift_getMetatypeMetadata") : HAO_DLSYM("swift_getExistentialMetatypeMetadata");
        return get && native_identity((const void *)(uintptr_t)m[1], depth + 1) && get((const void *)(uintptr_t)m[1]) == type;
    }
    case 0x305: {
        metatype_fn *get = HAO_DLSYM("swift_getObjCClassMetadata");
        return get && native_addr(m[1]) && get((const void *)(uintptr_t)m[1]) == type;
    }
    default: return false;
    }
}

int walk_fact_of(const void *type, walk_fact *out)
{
    if (!type) return 0;
    load_fixture();
    bool table = table_applies();
    if (!table && !nftypes) return 0;   /* no row for this process: nothing to look up */
    const uint64_t *e = hao_gmap_get1(&by_type, (uint64_t)(uintptr_t)type);
    uint64_t found = e ? e[1] : 0;
    if (!e) {
        uint64_t n, vwt = 0;
        struct { uint64_t size, stride; uint32_t flags, extra; } v = {0};
        /* The value witness table's layout words first: a type whose table cannot be read has no row. */
        bool readable = peek((uint64_t)(uintptr_t)type - 8, &vwt, 8) && peek(vwt + 64, &v, sizeof v);
        const char *name = readable ? walk_fact_mangled(type, &n) : NULL;
        char key[4096];
        found = UINT64_C(1) << 63;
        if (name && walk_fact_key(name, n, key, sizeof key)) {
            for (unsigned i = 0; i < nftypes; ++i)
                if (!strcmp(ftypes[i].key, key)) { found = (UINT64_C(1) << 32) + i + 1; break; }
            for (long lo = 0, hi = (long)NTYPES - 1; table && found >> 63 && lo <= hi;) {   /* sorted by key */
                long mid = (lo + hi) / 2;
                int c = strcmp(key, types[mid].key);
                if (!c) {
                    /* The row describes this type only while its live layout is the row's. */
                    if (v.size == types[mid].size && v.stride == types[mid].stride && v.flags == types[mid].flags &&
                        v.extra == types[mid].x86_xi) found = (uint64_t)mid + 1;
                    break;
                }
                if (c < 0) hi = mid - 1; else lo = mid + 1;
            }
        }
        bool fixture = !(found >> 63) && (found >> 32);
        char verdict = found >> 63 ? 0 : fixture ? ftypes[(found & 0xffffffffu) - 1].verdict : types[found - 1].verdict;
        if (verdict == WALK_FACT_SAME && !(fixture && ftypes[(found & 0xffffffffu) - 1].unchecked) && !native_identity(type, 0))
            found = UINT64_C(1) << 63;
        hao_crit_enter(); pthread_mutex_lock(&lock);
        bool fresh;
        uint64_t *slot = hao_gmap_claim1(&by_type, (uint64_t)(uintptr_t)type, &fresh);
        if (slot && fresh) { slot[1] = found; hao_gmap_publish1(&by_type, slot, (uint64_t)(uintptr_t)type); }
        pthread_mutex_unlock(&lock); hao_crit_leave();
    }
    if (found >> 63) return 0;
    if (found >> 32) {
        const fixture_type *f = &ftypes[(found & 0xffffffffu) - 1];
        *out = (walk_fact){f->verdict, f->xi_class, f->xi_offset, f->nimages, (int)f->first_image, 1};
    } else {
        long at = (long)found - 1;
        int first = types[at].first_image, count = types[at].images;
        if (count < 0 || first < 0 || (uint64_t)first + (uint64_t)count > NIMAGES) count = 0;
        *out = (walk_fact){types[at].verdict, types[at].xi_class, types[at].xi_offset, (unsigned)count, first, 0};
    }
    return 1;
}

int walk_fact_image(const walk_fact *f, unsigned i, unsigned *which, unsigned char *arm, unsigned char *arm_mask,
                    unsigned char *x86, unsigned char *x86_mask, uint64_t size)
{
    if (i >= f->nimages || size > WALK_FACT_IMAGE_BYTES) return 0;
    if (f->fixture) {
        const fixture_image *g = &fimages[f->first_image + (int)i];
        if (g->n != size) return 0;
        *which = g->which;
        memcpy(arm, g->arm, size); memcpy(arm_mask, g->arm_mask, size);
        memcpy(x86, g->x86, size); memcpy(x86_mask, g->x86_mask, size);
        return 1;
    }
    int at = f->first_image + (int)i, t = images[at].type;
    if (t < 0 || (size_t)t >= NTYPES || types[t].size != size) return 0;
    memcpy(arm, images[at].arm, (size_t)size); memcpy(arm_mask, images[at].arm_mask, (size_t)size);
    memcpy(x86, images[at].x86, (size_t)size); memcpy(x86_mask, images[at].x86_mask, (size_t)size);
    *which = (unsigned)images[at].which;
    return 1;
}
