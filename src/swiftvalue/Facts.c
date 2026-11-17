/* Per-type facts of the Swift value walk (Facts.h): src/swiftwalk_gen.inc,
 * included once per section. Under the test switch HAO_SWIFT_WALK_FACTS=FILE
 * (a HAO_TEST_SWITCHES build, an open process) a fixture file's rows are
 * looked up first, in any process, with no layout check: one row a line,
 *   type KEY VERDICT [XI-CLASS XI-OFFSET]   (SAME, DYNAMIC, CONVERT or REFUSE; NONE, SAME, POINTER, OTHER)
 *   image KEY CASE ARM64-IMAGE ARM64-MASK X86_64-IMAGE X86_64-MASK   (hex, as SWIFTWALK_IMAGES)
 * ("#" starts a comment; tests/guest/svfacts.facts). */
#include "Facts.h"
#include "hao/aot.h"
#include "hao/gen_stamp.h"
#include "hao/gmap.h"
#include "hao/opts.h"
#include "hao/runtime.h"
#include "hao/dlsym_once.h"
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

typedef struct { char verdict; char *key; unsigned first_image, nimages; char xi_class; int xi_offset; } fixture_type;
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
            char xi[16] = "NONE";
            int xi_offset = -1;
            if (sscanf(line, "type %2047s %15s %15s %d", key, verdict, xi, &xi_offset) >= 2) {
                fixture_type *g = realloc(ftypes, (nftypes + 1) * sizeof *ftypes);
                if (!g) break;
                ftypes = g;
                char v = !strcmp(verdict, "SAME") ? WALK_FACT_SAME : !strcmp(verdict, "DYNAMIC") ? WALK_FACT_DYNAMIC :
                         !strcmp(verdict, "CONVERT") ? WALK_FACT_CONVERT : WALK_FACT_REFUSE;
                char c = !strcmp(xi, "SAME") ? WALK_XI_SAME : !strcmp(xi, "POINTER") ? WALK_XI_POINTER :
                         !strcmp(xi, "OTHER") ? WALK_XI_OTHER : WALK_XI_NONE;
                ftypes[nftypes++] = (fixture_type){v, strdup(key), 0, 0, c, xi_offset};
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

int walk_fact_of(const void *type, walk_fact *out)
{
    if (!type) return 0;
    load_fixture();
    bool table = table_applies();
    if (!table && !nftypes) return 0;   /* no row for this process: nothing to look up */
    const uint64_t *e = hao_gmap_get1(&by_type, (uint64_t)(uintptr_t)type);
    uint64_t found = e ? e[1] : 0;
    if (!e) {
        uint64_t n;
        const char *name = walk_fact_mangled(type, &n);
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
                    const struct { void *fn[8]; uint64_t size, stride; uint32_t flags, extra; } *v = ((const void *const *)type)[-1];
                    if (v->size == types[mid].size && v->stride == types[mid].stride && v->flags == types[mid].flags &&
                        v->extra == types[mid].x86_xi) found = (uint64_t)mid + 1;
                    break;
                }
                if (c < 0) hi = mid - 1; else lo = mid + 1;
            }
        }
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
