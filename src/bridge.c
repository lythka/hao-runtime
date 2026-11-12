/*
 * hao — the typed guest/native boundary, runtime part (hao/bridge.h,
 * docs/architecture.md): the plan table, binding from interface units
 * and the compiled-in tables, first-call binding of bare native addresses,
 * statistics and HAO_BRIDGE_CHECK.
 *
 * Memory. Plans, interned names and tables come from mmap'd arenas, never
 * from malloc: a guest signal handler on a native thread may run while that
 * thread holds the malloc locks (signals.c, bba7c03), and the call path of
 * such a handler reads the table. Reads take no lock. Writers hold
 * bridge_lock inside a hao critical section; the table grows by publishing a
 * larger copy, and old copies stay mapped for readers still probing them.
 * Interface units are mapped at bind time (image loading), read-only, for
 * as long as the process lives.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include "hao/vafmt.h"
#include "hao/gmap.h"
#include "hao/gen_stamp.h"
#include <sys/sysctl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

hao_plan_table *hao_plan_tab;
bool hao_bridge_counting;

static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t bridge_once = PTHREAD_ONCE_INIT;
static bool ready;

/* fork child: see the lock comment in jit.c */
static pthread_mutex_t labels_lock = PTHREAD_MUTEX_INITIALIZER;   /* the name sources by address (labels_at) */
void hao_bridge_fork_child(void)
{
    bridge_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    labels_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

static void L(void) { hao_crit_enter(); pthread_mutex_lock(&bridge_lock); }
static void U(void) { pthread_mutex_unlock(&bridge_lock); hao_crit_leave(); }

/* ------------------------------------------------------------- arena */

#define ARENA_CHUNK (256u << 10)
static uint8_t *arena_p, *arena_end;
static uint64_t arena_bytes;

static void *map_zero(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* under bridge_lock */
static void *arena_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (n > ARENA_CHUNK) return NULL;
    if (!arena_p || (size_t)(arena_end - arena_p) < n) {
        uint8_t *c = map_zero(ARENA_CHUNK);
        if (!c) return NULL;
        arena_p = c;
        arena_end = c + ARENA_CHUNK;
        arena_bytes += ARENA_CHUNK;
    }
    void *r = arena_p;
    arena_p += n;
    return r;
}

static const char *arena_str(const char *s)
{
    if (!s) return NULL;
    size_t l = strlen(s);
    if (l > 4096) l = 4096;
    char *d = arena_alloc(l + 1);
    if (!d) return NULL;
    memcpy(d, s, l);
    d[l] = 0;
    return d;
}

/* ------------------------------------------------------------- table */

#define TAB_MIN_BITS 12      /* 4096 slots: the old fixed set's size */
static uint64_t st_tab_bytes, st_grows;

static hao_plan_table *tab_new(unsigned bits)
{
    size_t n = (size_t)1 << bits, bytes = sizeof(hao_plan_table) + n * sizeof(hao_plan_slot);
    hao_plan_table *t = map_zero(bytes);
    if (!t) return NULL;
    t->mask = (uint32_t)(n - 1);
    t->shift = 64 - bits;
    t->limit = (uint32_t)(n / 4 * 3);     /* a quarter stays empty: probes stay short and end */
    st_tab_bytes += bytes;
    return t;
}

static hao_plan_slot *tab_slot(hao_plan_table *t, uint64_t a)
{
    uint32_t h = (uint32_t)((a * 0x9E3779B97F4A7C15ull) >> t->shift);
    for (;;) {
        hao_plan_slot *s = &t->s[h];
        if (!s->key || s->key == a) return s;
        h = (h + 1) & t->mask;
    }
}

/* Is `p` a better plan than `old` for the same entry point? Typed beats
 * UNKNOWN; otherwise the first plan stays (the same function bound under
 * two names, or noted twice). */
static bool better(const hao_plan *p, const hao_plan *old)
{
    /* or the same class, now known to be a Swift async function (note_async_entry) */
    return (old->cls == HAO_BR_UNKNOWN && p->cls != HAO_BR_UNKNOWN) ||
           (p->cls == old->cls && (p->flags & ~old->flags & (HAO_PLAN_SWIFTASYNC | HAO_PLAN_ASYNCARG)));
}

/* under bridge_lock: insert or upgrade; returns the plan now in the table. Every change ends
 * the native-target caches' generation (hao/runtime.h hao_ncache_invalidate): a thread that
 * remembered the address's previous plan (or its absence) looks it up again. */
static const hao_plan *tab_put(uint64_t a, const hao_plan *p)
{
    hao_plan_table *t = hao_plan_tab;
    if (!t) return NULL;
    hao_plan_slot *s = tab_slot(t, a);
    if (s->key == a) {
        const hao_plan *old = s->plan;
        if (!better(p, old)) return old;
        __atomic_store_n(&s->plan, p, __ATOMIC_RELEASE);
        hao_ncache_invalidate();
        return p;
    }
    if (t->used >= t->limit) {
        unsigned bits = 64 - t->shift + 1;
        hao_plan_table *n = bits <= 28 ? tab_new(bits) : NULL;
        if (!n) {
            static int warned;
            if (!warned++) { HAO_LOG(0, "bridge plan table cannot grow: later entry points are UNKNOWN\n"); }
            return NULL;
        }
        for (uint32_t k = 0; k <= t->mask; k++)
            if (t->s[k].key) { hao_plan_slot *d = tab_slot(n, t->s[k].key); *d = t->s[k]; n->used++; }
        n->prev = t;
        st_grows++;
        __atomic_store_n(&hao_plan_tab, n, __ATOMIC_RELEASE);
        t = n;
        s = tab_slot(t, a);
    }
    s->plan = p;
    __atomic_store_n(&s->key, a, __ATOMIC_RELEASE);
    t->used++;
    hao_ncache_invalidate();
    return p;
}

/* ------------------------------------------------------- static plans */

/* Shared by every UNKNOWN entry point while nothing counts per symbol. */
static const hao_plan unknown_import = {.cls = HAO_BR_UNKNOWN, .argmask = HAO_BR_SCAN_ALL};
static const hao_plan unknown_bare = {.cls = HAO_BR_UNKNOWN, .argmask = HAO_BR_SCAN_ALL, .flags = HAO_PLAN_DLADDR};

/* bind counts: edges (one per resolved import) by class and source */
static uint64_t st_bind_cls[HAO_BR_NCLASS], st_bind_src[HAO_BRS_N], st_bare, st_bare_named, st_bare_typed, st_bare_skipped;
static uint64_t st_conflicts, st_bare_redirected, st_thunk_missing, st_thunk_halves;
static uint64_t st_alias_typed, st_alias_refused;   /* entry points typed by another name of their address */

/* ------------------------------------------------------ interface units */

/* A unit's records by code identity: (offset from the library's mach header,
 * record index), sorted by offset, for every record whose name the library's
 * own trie exports at an address (a record under a name it re-exports, or
 * does not export, is found by that name only: named[] has its bit clear). */
typedef struct { uint32_t off, rec; } addr_ent;
typedef struct {
    uint64_t base; uint8_t uuid[16]; const char *path; hao_if_view v; bool have;
    addr_ent *ax; uint32_t nax, cap;
    uint8_t *placed;                  /* bit per record: in ax */
} native_lib;
/* mach header -> native_lib * (arena); written and read under bridge_lock */
static hao_gmap libs = HAO_GMAP_INIT(2, 1, 8, 24);
static int units_loaded, units_rejected;
static bool units_on;
static hao_aot_dir if_dir;

static void units_init(void)
{
    if (hao_opt.no_ifunits) return;
    char why[512];
    int nd = hao_if_dirs_for(hao_env_tier(), hao_opt.if_dir, &if_dir, why, sizeof why);
    if (!nd) { if (why[0]) HAO_LOG(1, "%s\n", why); return; }
    /* nothing to bind against without at least one unit: skip the per-import library lookup */
    DIR *d = opendir(if_dir.path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strlen(e->d_name) == 32 && strspn(e->d_name, "0123456789abcdef") == 32) { units_on = true; break; }
    closedir(d);
    if (units_on) HAO_LOG(1, "interface units from %s\n", if_dir.path);
}

static bool header_uuid(const struct mach_header_64 *h, uint8_t uuid[16])
{
    if (h->magic != MH_MAGIC_64 || h->sizeofcmds > (1u << 24)) return false;
    const uint8_t *p = (const uint8_t *)(h + 1), *end = p + h->sizeofcmds;
    for (uint32_t k = 0; k < h->ncmds && p + sizeof(struct load_command) <= end; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > (uint64_t)(end - p)) return false;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            memcpy(uuid, ((const struct uuid_command *)(const void *)lc)->uuid, 16);
            return true;
        }
        p += lc->cmdsize;
    }
    return false;
}

static uint64_t trie_uleb(const uint8_t **p, const uint8_t *end, bool *ok)
{
    uint64_t r = 0;
    for (unsigned sh = 0; *p < end && sh < 64; sh += 7) {
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) return r;
    }
    *ok = false;
    return 0;
}

/* ------------------------------------------------- export tries
 *
 * A native image's export trie (<mach-o/loader.h> LC_DYLD_EXPORTS_TRIE, or
 * the dyld_info export area), read in its mapped __LINKEDIT. Plans are keyed
 * by code identity: the defining image and the offset of the function from
 * its mach header. Every name source (interface unit records, the trap
 * table, the compiled-in tables) is resolved to that key through the tries
 * of the images that export the names, so a function reached under any of
 * its names (a renamed re-export, an alias at the same address, a bare
 * address) finds the same records. The walks never call dyld and allocate
 * nothing; anything malformed ends them. */
typedef struct { const uint8_t *start, *end; } trie_t;

static bool trie_of(const struct mach_header_64 *h, trie_t *t)
{
    if (h->magic != MH_MAGIC_64 || h->sizeofcmds > (1u << 24)) return false;
    const uint8_t *p = (const uint8_t *)(h + 1), *end = p + h->sizeofcmds;
    uint64_t text_vm = 0, le_vm = 0, le_off = 0, le_size = 0;
    uint32_t toff = 0, tsize = 0;
    bool have_text = false, have_le = false;
    for (uint32_t k = 0; k < h->ncmds && p + sizeof(struct load_command) <= end; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > (uint64_t)(end - p)) return false;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (!strncmp(sg->segname, "__TEXT", 16)) { text_vm = sg->vmaddr; have_text = true; }
            if (!strncmp(sg->segname, "__LINKEDIT", 16)) { le_vm = sg->vmaddr; le_off = sg->fileoff; le_size = sg->filesize; have_le = true; }
        } else if (lc->cmd == LC_DYLD_EXPORTS_TRIE && lc->cmdsize >= sizeof(struct linkedit_data_command)) {
            const struct linkedit_data_command *ld = (const void *)lc;
            toff = ld->dataoff; tsize = ld->datasize;
        } else if ((lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) && lc->cmdsize >= sizeof(struct dyld_info_command)) {
            const struct dyld_info_command *di = (const void *)lc;
            toff = di->export_off; tsize = di->export_size;
        }
        p += lc->cmdsize;
    }
    if (!have_text || !have_le || !tsize || toff < le_off || (uint64_t)toff + tsize > le_off + le_size) return false;
    uint64_t slide = (uint64_t)(uintptr_t)h - text_vm;
    t->start = (const uint8_t *)(uintptr_t)(le_vm + slide + (toff - le_off));
    t->end = t->start + tsize;
    return true;
}

/* A terminal's export info: REGULAR or re-export/resolver (the latter two
 * need dyld to resolve). */
enum { EX_NONE, EX_ADDR, EX_INDIRECT };
static int trie_info(const uint8_t *q, const uint8_t *end, uint64_t *off)
{
    bool ok = true;
    uint64_t flags = trie_uleb(&q, end, &ok);
    if (!ok) return EX_NONE;
    if (flags & (EXPORT_SYMBOL_FLAGS_REEXPORT | EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)) return EX_INDIRECT;
    if ((flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) != EXPORT_SYMBOL_FLAGS_KIND_REGULAR) return EX_NONE;
    *off = trie_uleb(&q, end, &ok);
    return ok ? EX_ADDR : EX_NONE;
}

/* Inspect original exports, including shared-cache images whose extracted
 * symbol tables omit ABI data. The visitor sees only direct regular exports;
 * an incomplete/malformed traversal never constitutes a successful proof. */
static bool trie_visit(const trie_t *t, const uint8_t *node, char name[4096],
                       size_t length, uintptr_t base, hao_native_export_visitor visit,
                       void *context, unsigned *budget, unsigned depth)
{
    if (!*budget || depth > 512 || node >= t->end) return false;
    --*budget;
    bool ok = true;
    const uint8_t *q = node;
    uint64_t term = trie_uleb(&q,t->end,&ok);
    if (!ok || term > (uint64_t)(t->end - q)) return false;
    if (term) {
        uint64_t off = 0;
        int kind = trie_info(q,q + term,&off);
        if (kind == EX_NONE) return false;
        if (kind == EX_ADDR) {
            if (off > UINTPTR_MAX - base) return false;
            name[length] = 0;
            visit(context,name,(const void *)(base + off));
        }
    }
    q += term;
    if (q >= t->end) return false;
    unsigned count = *q++;
    for (unsigned k = 0; k < count; ++k) {
        size_t len = strnlen((const char *)q,(size_t)(t->end - q));
        if (!len || len == (size_t)(t->end - q) || len >= 4096 - length) return false;
        memcpy(name + length,q,len);
        q += len + 1;
        uint64_t child = trie_uleb(&q,t->end,&ok);
        if (!ok || child >= (uint64_t)(t->end - t->start) ||
            !trie_visit(t,t->start + child,name,length + len,base,visit,context,budget,depth + 1)) return false;
    }
    return true;
}

bool hao_bridge_native_exports(const void *address, hao_native_export_visitor visit, void *context)
{
    Dl_info info = {0};
    trie_t t;
    if (!address || !visit || !dladdr(address,&info) || !info.dli_fbase ||
        !trie_of(info.dli_fbase,&t)) return false;
    unsigned budget = 1u << 22;
    char name[4096];
    return trie_visit(&t,t.start,name,0,(uintptr_t)info.dli_fbase,visit,context,&budget,0);
}

/*
 * Every name of a sorted list (strcmp order, no duplicates) that the trie
 * exports, in one walk: a node is entered only with the names that share the
 * path to it, so the cost is the trie's nodes on those names' paths, not the
 * names times the trie. hit(ctx, i, kind, off) for each name i the trie
 * exports. budget bounds the nodes a malformed (cyclic) trie can make it
 * visit.
 */
typedef const char *(*trie_name_fn)(const void *ctx, size_t i);
typedef void (*trie_hit_fn)(void *ctx, size_t i, int kind, uint64_t off);

static size_t name_bound(trie_name_fn nm, const void *ctx, size_t lo, size_t hi, size_t pl, const char *e, size_t el, bool upper)
{
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        int c = strncmp(nm(ctx, m) + pl, e, el);
        if (upper ? c <= 0 : c < 0) lo = m + 1; else hi = m;
    }
    return lo;
}

static void trie_walk_node(const trie_t *t, const uint8_t *node, trie_name_fn nm, const void *nctx, size_t lo, size_t hi,
                           size_t pl, trie_hit_fn hit, void *hctx, unsigned *budget, unsigned depth)
{
    if (!*budget || depth > 512 || node >= t->end) return;
    --*budget;
    bool ok = true;
    const uint8_t *q = node;
    uint64_t term = trie_uleb(&q, t->end, &ok);
    if (!ok || term > (uint64_t)(t->end - q)) return;
    if (term && !nm(nctx, lo)[pl]) {        /* the name equal to the path sorts first */
        uint64_t off = 0;
        int kind = trie_info(q, q + term, &off);
        if (kind != EX_NONE) hit(hctx, lo, kind, off);
        if (++lo >= hi) return;
    }
    const uint8_t *c = q + term;
    if (c >= t->end) return;
    unsigned n = *c++;
    for (unsigned k = 0; k < n && c < t->end && lo < hi; k++) {
        const char *e = (const char *)c;
        size_t el = strnlen(e, (size_t)(t->end - c));
        if (el == (size_t)(t->end - c)) return;
        c += el + 1;
        uint64_t child = trie_uleb(&c, t->end, &ok);
        if (!ok || child >= (uint64_t)(t->end - t->start)) return;
        if (!el) continue;
        size_t a = name_bound(nm, nctx, lo, hi, pl, e, el, false);
        if (a == hi || strncmp(nm(nctx, a) + pl, e, el)) continue;
        size_t b = name_bound(nm, nctx, a, hi, pl, e, el, true);
        trie_walk_node(t, t->start + child, nm, nctx, a, b, pl + el, hit, hctx, budget, depth + 1);
    }
}

static void trie_walk(const trie_t *t, trie_name_fn nm, const void *nctx, size_t n, trie_hit_fn hit, void *hctx)
{
    unsigned budget = 1u << 22;
    if (n) trie_walk_node(t, t->start, nm, nctx, 0, n, 0, hit, hctx, &budget, 0);
}

static void unit_reject(const char *name, const char *why)
{
    units_rejected++;
    if (hao_verbose || hao_bridge_counting) { HAO_LOG(0, "interface unit %s/%s rejected: %s\n", if_dir.path, name, why); }
}

static const char *unit_name(const void *ctx, size_t i)
{
    const hao_if_view *v = ctx;
    return v->str + v->recs[i].name;
}

static void unit_hit(void *ctx, size_t i, int kind, uint64_t off)
{
    native_lib *l = ctx;
    if (kind != EX_ADDR || off >> 32 || l->nax >= l->cap) return;
    l->ax[l->nax++] = (addr_ent){(uint32_t)off, (uint32_t)i};
    l->placed[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static int ax_cmp(const void *a, const void *b)
{
    const addr_ent *x = a, *y = b;
    return x->off != y->off ? (x->off < y->off ? -1 : 1) : (x->rec < y->rec ? -1 : x->rec > y->rec);
}

/* The unit's records at their addresses in the library (the unit is keyed by
 * the library's LC_UUID, so a name resolves to exactly one function there):
 * one walk of the library's export trie with the records' sorted names. */
static void unit_index(native_lib *l)
{
    trie_t t;
    uint32_t n = l->v.nrecs;
    if (!n || !trie_of((const struct mach_header_64 *)(uintptr_t)l->base, &t)) return;
    l->ax = map_zero((size_t)n * sizeof *l->ax);
    l->placed = map_zero((size_t)n / 8 + 1);
    if (!l->ax || !l->placed) { l->ax = NULL; return; }
    l->cap = n;
    trie_walk(&t, unit_name, &l->v, n, unit_hit, l);
    qsort(l->ax, l->nax, sizeof *l->ax, ax_cmp);
}

static bool unit_placed(const native_lib *l, const hao_if_rec *r)
{
    size_t i = (size_t)(r - l->v.recs);
    return l->placed && (l->placed[i >> 3] >> (i & 7) & 1);
}

static void unit_load(native_lib *l)
{
    char name[40], why[400];
    for (int k = 0; k < 16; k++) snprintf(name + 2 * k, 3, "%02x", l->uuid[k]);
    struct stat st;
    int fd = hao_aot_open_trusted(&if_dir, name, &st, why, sizeof why);
    if (fd < 0) { if (why[0]) unit_reject(name, why); return; }
    if (st.st_size < (off_t)sizeof(hao_if_hdr) || (uint64_t)st.st_size > HAO_IF_MAX_FILE) {
        close(fd);
        unit_reject(name, "size out of range");
        return;
    }
    /* Mapped, not read: the unit's pages stay clean file pages, outside the
     * process's footprint (a daemon binds units of tens of libraries). The
     * file passed the same trust rules as an AOT cache (root's, or the
     * user's own for an open-tier process), so whoever could change it
     * after the check below could have planted it in the first place. */
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { unit_reject(name, "cannot map it"); return; }
    if (hao_ifunit_check(m, (uint64_t)st.st_size, l->uuid, &l->v, why, sizeof why) != 0) {
        munmap(m, (size_t)st.st_size);
        unit_reject(name, why);
        return;
    }
    l->have = true;
    units_loaded++;
    unit_index(l);
    HAO_LOG(1, "interface unit %s/%s (%s): %u records, %u at their addresses\n", if_dir.path, name,
                             l->path, l->v.nrecs, l->nax);
}

/* Two records for one entry point agree (same class, kind, encoding, mask and thunk pair). */
/* The same conversion (class, kind, signature, thunk pair). Masks are not part
 * of it: two names of one function whose records name different slots that
 * may hold guest code are scanned for the union of them (mask_merge). */
static bool rec_same(const hao_if_view *v, const hao_if_rec *a, const hao_if_rec *b)
{
    uint8_t conv = HAO_IFR_SIG | HAO_IFR_THUNK;
    if (a->cls != b->cls || a->kind != b->kind || ((a->flags ^ b->flags) & conv)) return false;
    if ((a->flags & HAO_IFR_SIG || a->kind == HAO_IF_KIND_STACKPACK || a->kind == 'Z') && strcmp(v->str + a->enc, v->str + b->enc)) return false;
    if ((a->flags & HAO_IFR_THUNK) && strcmp(v->str + a->thunk, v->str + b->thunk)) return false;
    return true;
}

/* The unit of the native library at mach header `base` (loaded once). Called
 * at bind time (image loading) and from first-call binding, never in a
 * signal handler. */
static const native_lib *lib_for(uint64_t base, const char *path)
{
    if (!units_on || !base) return NULL;
    L();
    native_lib *l = NULL;
    bool fresh;
    uint64_t *s = hao_gmap_claim1(&libs, base, &fresh);
    if (s && !fresh) l = (native_lib *)(uintptr_t)s[1];
    else {
        /* a library left out would bind without its unit */
        if (!s || !(l = arena_alloc(sizeof *l))) hao_oom("interface unit libraries");
        s[1] = (uint64_t)(uintptr_t)l;
        hao_gmap_publish1(&libs, s, base);
        l->base = base;
        l->path = arena_str(path);
        /* once per library, under the lock so that a concurrent first-call
         * bind never sees a half-loaded entry */
        if (header_uuid((const struct mach_header_64 *)(uintptr_t)base, l->uuid)) unit_load(l);
    }
    if (l && !l->have) l = NULL;
    U();
    return l;
}

static const hao_if_view *unit_for(uint64_t base, const char *path)
{
    const native_lib *l = lib_for(base, path);
    return l ? &l->v : NULL;
}

/* The argument and result masks of records of one function: the union, and
 * the full scan when one of them has no authoritative mask. */
typedef struct { uint16_t argmask; uint8_t resmask; bool unmasked; } mask_merge;
static void mask_add(mask_merge *m, const hao_if_rec *r)
{
    if (!(r->flags & HAO_IFR_MASKED)) m->unmasked = true;
    m->argmask |= r->argmask;
    m->resmask |= r->resmask;
}

/* The record of the function at `off` in library l: any record whose name the
 * library exports there, a typed one before an UNKNOWN one (which only carries
 * its reason). Two typed ones that disagree set *conflict; *mm gets the typed
 * ones' masks. */
static const hao_if_rec *unit_rec_at(const native_lib *l, uint64_t off, const char **name2, bool *conflict, mask_merge *mm)
{
    *conflict = false;
    *name2 = NULL;
    if (!l || !l->ax || off >> 32) return NULL;
    uint32_t lo = 0, hi = l->nax;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2;
        if (l->ax[m].off < off) lo = m + 1; else hi = m;
    }
    if (lo >= l->nax || l->ax[lo].off != off) return NULL;
    const hao_if_rec *r = &l->v.recs[l->ax[lo].rec];
    if (r->cls != HAO_BR_UNKNOWN) mask_add(mm, r);
    for (uint32_t k = lo + 1; k < l->nax && l->ax[k].off == off; k++) {
        const hao_if_rec *o = &l->v.recs[l->ax[k].rec];
        if (o->cls == HAO_BR_UNKNOWN) continue;
        mask_add(mm, o);
        if (r->cls == HAO_BR_UNKNOWN) r = o;
        else if (!rec_same(&l->v, r, o)) { *conflict = true; *name2 = l->v.str + o->name; }
    }
    return r;
}

/* --------------------------------------------------------------- init */

/* The native function a libhao_identity.dylib interposer stands for
 * (src/process_identity.c: execve, posix_spawn, getsectiondata, ...): guest
 * imports bind to the interposer, which has the original's type. */
static const void *(*identity_original)(const void *);
extern const struct mach_header *dyld_image_header_containing_address(const void *addr);   /* libdyld */
extern const char *dyld_image_path_containing_address(const void *addr);
static uint64_t typed_at(uint64_t native)
{
    return identity_original ? (uint64_t)(uintptr_t)identity_original((const void *)(uintptr_t)native) : native;
}

/* The generated tables' source build against this system's (hao/gen_stamp.h). */
bool hao_gen_stamp_check(char *running, size_t n)
{
    size_t len = n;
    if (!n) return false;
    if (sysctlbyname("kern.osversion", running, &len, NULL, 0) != 0 || !len) running[0] = 0;
    running[n - 1] = 0;
    return !strcmp(running, HAO_GEN_OS_BUILD);
}

static void init_once(void)
{
    if (hao_verbose) {
        char build[32];
        if (!hao_gen_stamp_check(build, sizeof build)) {
            HAO_LOG(0, "generated tables describe macOS %s (%s), table format %d; this system is %s\n",
                    HAO_GEN_OS_VERSION, HAO_GEN_OS_BUILD, HAO_GEN_FORMAT, build[0] ? build : "an unknown build");
        }
        char why[512];
        if (hao_cxx_demangle_selfcheck(why, sizeof why) > 0) {
            HAO_LOG(0, "demangler spelling changed: %s\n", why);
        }
    }
    identity_original = (const void *(*)(const void *))dlsym(RTLD_DEFAULT, "hao_identity_original");
    hao_bridge_counting = hao_opt.bridge_stats || hao_opt.bridge_check;
    L();
    hao_plan_tab = tab_new(TAB_MIN_BITS);
    U();
    units_init();
    if (hao_opt.bridge_stats) hao_bridge_stats_atexit();
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}

void hao_bridge_init(void) { pthread_once(&bridge_once, init_once); }

/* Descriptor exports can be absent from dladdr's local symbol table even
 * though dlsym resolves them exactly (SwiftUICore in the 26A428 x86 cache).
 * Use the UUID unit's names and exact export addresses, never nearest names.
 * Cache by descriptor and unit; no dyld operation runs under bridge_lock. */
static hao_gmap swift_requirements = HAO_GMAP_INIT(4, 2, 6, 16);

const hao_swift_reverse *hao_bridge_swift_requirement(uint64_t requirement,
                                                   const hao_swift_result_signature **result)
{
    if (result) *result = NULL;
    if (hao_opt.no_ifunits || hao_opt.no_thunks) return NULL;
    hao_bridge_init();
    const void *address = (const void *)(uintptr_t)requirement;
    const struct mach_header *header = dyld_image_header_containing_address(address);
    const char *path = dyld_image_path_containing_address(address);
    if (!header || !path) return NULL;
    const hao_if_view *v = unit_for((uint64_t)(uintptr_t)header, path);
    if (!v) return NULL;
    uint64_t key[2] = {requirement, (uint64_t)(uintptr_t)v};
    const uint64_t *cached = hao_gmap_get(&swift_requirements, key);
    if (cached) {
        if (result) *result = (const void *)(uintptr_t)cached[3];
        return (const void *)(uintptr_t)cached[2];
    }
    void *handle = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) return NULL;
    const hao_swift_reverse *reverse = NULL;
    const hao_swift_result_signature *semantic = NULL;
    const char *matched_key = NULL;
    for (uint32_t k = 0; k < v->hdr->nrecs; k++) {
        const hao_if_rec *r = &v->recs[k];
        if (r->cls != HAO_BR_TYPED || !(r->flags & HAO_IFR_THUNK)) continue;
        const char *symbol = v->str + r->name;
        size_t n = strlen(symbol);
        if (n < 6 || n > 1000 || strncmp(symbol, "_$s", 3) || strcmp(symbol + n - 2, "Tj")) continue;
        char descriptor[1024];
        memcpy(descriptor, symbol + 1, n); /* dlsym omits the Mach-O underscore */
        descriptor[n - 2] = 'q';
        if (dlsym(handle, descriptor) != address) continue;
        void *function = dlsym(handle, symbol + 1);
        if (!function || dyld_image_header_containing_address(function) != header) continue;
        const char *pair = v->str + r->thunk;
        /* Two exported aliases may identify one requirement, but they must
         * agree on the complete compiler contract, not just its registers. */
        if (matched_key && strcmp(matched_key, pair)) { reverse = NULL; semantic = NULL; break; }
        matched_key = pair;
        reverse = hao_thunks_reverse(pair);
        semantic = reverse ? hao_thunks_result(pair, reverse) : NULL;
    }
    dlclose(handle);
    L();
    bool fresh;
    uint64_t *slot = hao_gmap_claim(&swift_requirements, key, &fresh);
    if (slot && fresh) {
        slot[2] = (uint64_t)(uintptr_t)reverse;
        slot[3] = (uint64_t)(uintptr_t)semantic;
        hao_gmap_publish(&swift_requirements, slot, key);
    }
    U();
    if (result) *result = semantic;
    return reverse;
}

/* ------------------------------------------- name sources by address
 *
 * The trap table (hao_interpose_names: every name an interposer answers)
 * and the compiled-in tables (hao_tables_names) are keyed by name. Each of
 * their names is resolved in every loaded native image's export trie, once
 * per image (one guided walk per image: 0.16 ms for a command-line tool's 45
 * images, 1.2 ms for AppKit's 430, with 2,122 names, on this host under
 * Rosetta), and the address it names is recorded: a re-export or resolver
 * entry through dyld (dlsym on that image). A function reached under any
 * other name, or none, then finds the trap or the table entry of the names
 * its address has (labels_at): the trap applies to every name of the
 * trapped function, and a table line written under the definer's name types
 * its renamed re-exports and same-address aliases. Images loaded later are
 * walked when the image count changes. No lock of dyld's is taken under
 * labels_lock (dyld runs initializers, which can reach guest code, with its
 * own held). */
enum { LBL_TRAP = 1, LBL_TABLE = 2 };
typedef struct { const char *name; uint8_t kinds; } lbl_name;
typedef struct { uint64_t addr; uint32_t idx; } lbl_slot;
static lbl_name *lbl_names;
static size_t lbl_n, lbl_cap;
static lbl_slot *lbl_tab;
static uint32_t lbl_mask, lbl_used;
static const struct mach_header_64 **lbl_seen;
static uint32_t lbl_nseen, lbl_capseen, lbl_count;
static bool lbl_ready;
/* the name table (lbl_names, lbl_n) is complete; it never changes again, so its entries are read
 * without labels_lock. lbl_ready only says a refresh of the images finished. */
static bool lbl_names_ready;
static uint64_t st_lbl_images, st_lbl_addrs, st_trap_alias, st_lbl_ns, st_lbl_ind, st_lbl_ind_ns;

static void lbl_add_name(const char *name, void *ctx)
{
    uint8_t k = (uint8_t)(uintptr_t)ctx;
    if (!name || name[0] != '_') return;
    if (lbl_n == lbl_cap) {
        size_t nc = lbl_cap ? 2 * lbl_cap : 2048;
        lbl_name *n = map_zero(nc * sizeof *n);
        if (!n) return;
        if (lbl_names) { memcpy(n, lbl_names, lbl_n * sizeof *n); munmap(lbl_names, lbl_cap * sizeof *n); }
        lbl_names = n; lbl_cap = nc;
    }
    lbl_names[lbl_n++] = (lbl_name){name, k};
}

static int lbl_cmp(const void *a, const void *b) { return strcmp(((const lbl_name *)a)->name, ((const lbl_name *)b)->name); }
static const char *lbl_name_at(const void *ctx, size_t i) { (void)ctx; return lbl_names[i].name; }

/* under labels_lock */
static void lbl_put(uint64_t a, uint32_t idx)
{
    if (!a) return;
    if (!lbl_tab || (lbl_used + 1) * 4 > (lbl_mask + 1) * 3) {
        uint32_t ns = lbl_tab ? 2 * (lbl_mask + 1) : 4096;
        lbl_slot *n = map_zero((size_t)ns * sizeof *n);
        if (!n) return;
        for (uint32_t k = 0; lbl_tab && k <= lbl_mask; k++) {
            if (!lbl_tab[k].addr) continue;
            uint32_t h = (uint32_t)((lbl_tab[k].addr * 0x9E3779B97F4A7C15ull) >> 40) & (ns - 1);
            while (n[h].addr) h = (h + 1) & (ns - 1);
            n[h] = lbl_tab[k];
        }
        if (lbl_tab) munmap(lbl_tab, (size_t)(lbl_mask + 1) * sizeof *lbl_tab);
        lbl_tab = n; lbl_mask = ns - 1;
    }
    uint32_t h = (uint32_t)((a * 0x9E3779B97F4A7C15ull) >> 40) & lbl_mask;
    for (; lbl_tab[h].addr; h = (h + 1) & lbl_mask)
        if (lbl_tab[h].addr == a && lbl_tab[h].idx == idx) return;
    lbl_tab[h] = (lbl_slot){a, idx};
    lbl_used++;
    st_lbl_addrs++;
}

typedef struct { const struct mach_header_64 *h; const char *path; uint32_t *ind; uint32_t nind, capind; } lbl_scan;

static void lbl_hit(void *ctx, size_t i, int kind, uint64_t off)
{
    lbl_scan *sc = ctx;
    if (kind == EX_ADDR) lbl_put((uint64_t)(uintptr_t)sc->h + off, (uint32_t)i);
    else if (sc->ind && sc->nind < sc->capind) sc->ind[sc->nind++] = (uint32_t)i;
}

static bool lbl_seen_has(const struct mach_header_64 *h)
{
    for (uint32_t k = 0; k < lbl_nseen; k++) if (lbl_seen[k] == h) return true;
    return false;
}

static void labels_init(void)
{
    hao_interpose_names(lbl_add_name, (void *)(uintptr_t)LBL_TRAP);
    hao_tables_names(lbl_add_name, (void *)(uintptr_t)LBL_TABLE);
    qsort(lbl_names, lbl_n, sizeof *lbl_names, lbl_cmp);
    size_t w = 0;
    for (size_t k = 0; k < lbl_n; k++) {
        if (w && !strcmp(lbl_names[w - 1].name, lbl_names[k].name)) lbl_names[w - 1].kinds |= lbl_names[k].kinds;
        else lbl_names[w++] = lbl_names[k];
    }
    lbl_n = w;
}

/* Walk the images dyld lists that no walk has seen. Not in a signal handler. */
static void labels_refresh(void)
{
    uint32_t n = _dyld_image_count();
    if (__atomic_load_n(&lbl_ready, __ATOMIC_ACQUIRE) && __atomic_load_n(&lbl_count, __ATOMIC_ACQUIRE) == n) return;
    /* dyld's lists are read before labels_lock is taken */
    size_t bytes = ((size_t)n + 1) * (sizeof(void *) * 2);
    const void **imgs = map_zero(bytes);
    if (!imgs) return;
    uint32_t m = 0;
    for (uint32_t k = 0; k < n; k++) {
        const struct mach_header *h = _dyld_get_image_header(k);
        const char *p = _dyld_get_image_name(k);
        if (h && h->magic == MH_MAGIC_64) { imgs[2 * m] = h; imgs[2 * m + 1] = p; m++; }
    }
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    enum { NIND = 256 };
    struct { const char *path; uint32_t idx; } ind[NIND];
    uint32_t nind = 0;
    hao_crit_enter();
    pthread_mutex_lock(&labels_lock);
    if (!lbl_names_ready) { labels_init(); __atomic_store_n(&lbl_names_ready, true, __ATOMIC_RELEASE); }
    for (uint32_t k = 0; k < m; k++) {
        const struct mach_header_64 *h = imgs[2 * k];
        if (lbl_seen_has(h)) continue;
        if (lbl_nseen == lbl_capseen) {
            uint32_t nc = lbl_capseen ? 2 * lbl_capseen : 1024;
            const struct mach_header_64 **ns = map_zero(nc * sizeof *ns);
            if (!ns) break;
            if (lbl_seen) { memcpy(ns, lbl_seen, lbl_nseen * sizeof *ns); munmap(lbl_seen, lbl_capseen * sizeof *ns); }
            lbl_seen = ns; lbl_capseen = nc;
        }
        lbl_seen[lbl_nseen++] = h;
        st_lbl_images++;
        trie_t t;
        if (!trie_of(h, &t)) continue;
        uint32_t idx[64];
        lbl_scan sc = {h, imgs[2 * k + 1], idx, 0, 64};
        trie_walk(&t, lbl_name_at, NULL, lbl_n, lbl_hit, &sc);
        for (uint32_t j = 0; j < sc.nind && nind < NIND; j++) ind[nind].path = sc.path, ind[nind++].idx = idx[j];
    }
    pthread_mutex_unlock(&labels_lock);
    hao_crit_leave();
    /* re-exports and resolvers: where dyld binds the name exported by that image */
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    st_lbl_ind += nind;
    for (uint32_t k = 0; k < nind; k++) {
        if (!ind[k].path) continue;
        void *hd = dlopen(ind[k].path, RTLD_LAZY | RTLD_NOLOAD);
        if (!hd) continue;
        uint64_t a = typed_at((uint64_t)(uintptr_t)dlsym(hd, lbl_names[ind[k].idx].name + 1));
        dlclose(hd);
        if (!a) continue;
        hao_crit_enter();
        pthread_mutex_lock(&labels_lock);
        lbl_put(a, ind[k].idx);
        pthread_mutex_unlock(&labels_lock);
        hao_crit_leave();
    }
    munmap(imgs, bytes);
    __atomic_fetch_add(&st_lbl_ind_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st_lbl_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
    __atomic_store_n(&lbl_count, n, __ATOMIC_RELEASE);
    __atomic_store_n(&lbl_ready, true, __ATOMIC_RELEASE);
}

/* The names of the sources whose names resolve to `a`, of the given kinds;
 * returns how many (at most max). */
static int labels_at(uint64_t a, uint8_t kinds, const char **out, int max)
{
    labels_refresh();
    int n = 0;
    hao_crit_enter();
    pthread_mutex_lock(&labels_lock);
    if (lbl_tab && a)
        for (uint32_t h = (uint32_t)((a * 0x9E3779B97F4A7C15ull) >> 40) & lbl_mask; lbl_tab[h].addr; h = (h + 1) & lbl_mask)
            if (lbl_tab[h].addr == a && (lbl_names[lbl_tab[h].idx].kinds & kinds) && n < max) out[n++] = lbl_names[lbl_tab[h].idx].name;
    pthread_mutex_unlock(&labels_lock);
    hao_crit_leave();
    return n;
}

/*
 * The trap that answers the native function at `native` under any of its
 * names: an interposer answers one of the names its address has. 1 with
 * *trap, 0 for none, -1 when two of its names have different traps (the
 * caller refuses it; *why says so).
 */
static int trap_at(uint64_t native, uint64_t *trap, const char **tname)
{
    const char *nm[8];
    int n = labels_at(typed_at(native), LBL_TRAP, nm, 8);
    uint64_t found = 0;
    *tname = NULL;
    for (int k = 0; k < n; k++) {
        uint64_t tr = 0;
        hao_crit_enter();
        bool ok = hao_interpose_lookup(nm[k], &tr) && hao_is_trap(tr) && tr != native;
        hao_crit_leave();
        if (!ok) continue;
        if (found && tr != found) return -1;
        found = tr;
        *tname = nm[k];
    }
    *trap = found;
    return found != 0;
}

int hao_bridge_trap_at(uint64_t native, uint64_t *trap)
{
    hao_bridge_init();
    const char *tn;
    *trap = 0;
    int r = native ? trap_at(native, trap, &tn) : 0;
    if (r > 0) __atomic_fetch_add(&st_trap_alias, 1, __ATOMIC_RELAXED);
    return r;
}

/* ---------------------------------------------------------- binding */

static const char *cls_name(int c)
{
    static const char *const n[] = {"UNKNOWN", "EXACT", "TYPED", "REFUSED"};
    return c >= 0 && c < HAO_BR_NCLASS ? n[c] : "?";
}

/* A plan for `native` from the unit record r (if typed) or the tables. */
/* A Swift async function: Swift emits an async function pointer record
 * NAME + "Tu" for every async function and dispatch thunk, and the native
 * library exports it with the function (the x86_64 library's own metadata). */
static bool swift_async(const char *gname)
{
    if (strncmp(gname, "_$s", 3) || !strstr(gname, "Ya")) return false;
    char afp[1024];
    size_t n = strlen(gname + 1);
    if (n + 3 > sizeof afp) return false;
    memcpy(afp, gname + 1, n);
    memcpy(afp + n, "Tu", 3);
    return dlsym(RTLD_DEFAULT, afp) != NULL;
}

static void make_plan(hao_plan *p, uint64_t native, const char *gname, const hao_if_view *v, const hao_if_rec *r, int tsrc,
                      char tkind, const char *tenc)
{
    memset(p, 0, sizeof *p);
    p->native = native;
    p->argmask = HAO_BR_SCAN_ALL;
    if (swift_async(gname)) p->flags |= HAO_PLAN_SWIFTASYNC;
    if (hao_swift_async_param(gname)) p->flags |= HAO_PLAN_ASYNCARG;
    uint64_t gh = 0, nh = 0;
    bool thunk = r && (r->flags & HAO_IFR_THUNK) && !hao_opt.no_thunks && hao_thunks_pair(v->str + r->thunk, &gh, &nh);
    if (r && (r->flags & HAO_IFR_THUNK) && !thunk) __atomic_fetch_add(&st_thunk_missing, 1, __ATOMIC_RELAXED);
    if (r && (r->flags & HAO_IFR_THUNK) && !thunk && !(r->flags & HAO_IFR_SIG)) {
        /* The unit requires this adapter and supplies no abi.c equivalent.
         * Losing the pair does not make the raw calling convention safe. */
        p->cls = HAO_BR_REFUSED;
        p->src = HAO_BRS_UNIT;
        p->why = hao_opt.no_thunks ? "required generated thunk pair disabled (HAO_NO_THUNKS)" :
                                    "required generated thunk pair not loaded";
    } else if (r && r->cls != HAO_BR_UNKNOWN) {
        p->cls = r->cls;
        p->src = HAO_BRS_UNIT;
        p->resmask = r->resmask;
        if (r->flags & HAO_IFR_MASKED) { p->flags |= HAO_PLAN_MASKED; p->argmask = r->argmask; }
        if (r->flags & HAO_IFR_PROVEN) p->flags |= HAO_PLAN_PROVEN;
        if (thunk) {
            p->thunk_guest = (const void *)(uintptr_t)gh;
            p->thunk_native = (const void *)(uintptr_t)nh;
            p->tp[0] = native;
            p->tp[1] = nh;
            p->tp[2] = (uint64_t)(uintptr_t)hao_bridge_code_arg;
        } else if (r->flags & HAO_IFR_SIG) { p->kind = 'A'; p->enc = v->str + r->enc; }
        else if (r->kind == HAO_IF_KIND_STACKPACK && (p->flags & HAO_PLAN_SWIFTASYNC)) {
            /* a swifttailcc callee pops its own stack arguments (swiftcc.c): no repack; the
             * generator never types one */
            p->cls = HAO_BR_UNKNOWN;
            p->src = 0;
            p->why = "stack pack of an async function";
        } else {
            p->kind = (char)r->kind;
            if (r->kind == HAO_IF_KIND_STACKPACK) p->enc = v->str + r->enc;
            if (p->kind == 'Z') p->enc = v->str + r->enc;   /* std::string roles (hao_ifunit_check validated them) */
        }
        if (r->why) p->why = v->str + r->why;
        if (tsrc && !thunk && (tkind != p->kind || (tenc && (!p->enc || strcmp(tenc, p->enc))))) {
            __atomic_fetch_add(&st_conflicts, 1, __ATOMIC_RELAXED);
            if (hao_opt.bridge_check) {
                HAO_LOG(0, "bridge: %s: unit says %s kind %c, the compiled-in table kind %c%s%s\n", gname,
                        cls_name(r->cls), p->kind ? p->kind : '-',
                        tkind ? tkind : '-', tenc ? " " : "", tenc ? tenc : "");
            }
        }
    } else if (!tsrc && hao_swift_fixed_abi(gname)) {
        p->cls = HAO_BR_EXACT;
        p->src = HAO_BRS_SWIFTABI;
        p->flags |= HAO_PLAN_MASKED;
        p->argmask = 0;
    } else if (tsrc) {
        p->cls = HAO_BR_TYPED;
        p->src = (uint8_t)tsrc;
        p->kind = tkind;
        p->enc = tenc;
    } else {
        p->cls = HAO_BR_UNKNOWN;
        if (r && r->why) p->why = v->str + r->why;
    }
}

/* The compiled-in sources by guest import name (interpose.c tables, then a
 * Swift async narrow result). */
static int table_type(const char *gname, char *kind, const char **enc)
{
    int src = hao_tables_plan(gname, kind, enc);
    if (src) return src;
    char sk = hao_swift_async_result_kind(gname);
    if (sk) { *kind = sk; *enc = NULL; return HAO_BRS_SWIFTASYNC; }
    const char *why = NULL;
    if (hao_cxxstr_lookup(gname, enc, &why) > 0) { *kind = 'Z'; return HAO_BRS_CXXSTR; }   /* before this-return: enc has it */
    *enc = NULL;
    if (hao_cxx_this_return(gname)) { *kind = 'P'; *enc = NULL; return HAO_BRS_THISRET; }
    return 0;
}

/* ------------------------------------------- Itanium C++ mangled names
 *
 * Just enough of the Itanium C++ ABI's mangling grammar (5.1) to read the
 * components of a function's nested name from the mangled form itself:
 * types, template arguments, substitutions and the special names are
 * skipped by their syntax. Each step returns the position after what it
 * read, or NULL for what it does not read (an expression, a local name, a
 * function-parameter reference, ...), and then nothing is concluded from
 * the mangled form. Substitutions are skipped, not resolved: a component
 * that is one says only that it is one. */
typedef struct {
    char kind;          /* 'n' source name, 'c' constructor, 'd' destructor, 'o' operator, 's' substitution, 't' other */
    char code;          /* constructor/destructor: '0'..'5' (C1, CI1, D2, ...) */
    const char *id;     /* 'n': the identifier, len bytes (not NUL-terminated) */
    size_t len;
} cx_comp;

static const char *cx_type(const char *p, int d);

static bool cx_digit(char c) { return c >= '0' && c <= '9'; }

static const char *cx_source_name(const char *p, const char **id, size_t *len)
{
    if (!cx_digit(*p)) return NULL;
    size_t n = 0;
    while (cx_digit(*p)) {
        n = n * 10 + (size_t)(*p++ - '0');
        if (n > 4096) return NULL;
    }
    for (size_t k = 0; k < n; k++) if (!p[k]) return NULL;
    if (id) { *id = p; *len = n; }
    return p + n;
}

/* [<seq-id>] _ (base 36: digits and capitals), as in S_, S1_, T_, T0_ */
static const char *cx_seq(const char *p)
{
    while (cx_digit(*p) || (*p >= 'A' && *p <= 'Z')) p++;
    return *p == '_' ? p + 1 : NULL;
}

static const char *cx_template_args(const char *p, int d);

/* L <type> <value> E; L _Z <encoding> E is not read */
static const char *cx_expr_primary(const char *p, int d)
{
    p++;
    if (*p == '_' || *p == 'Z') return NULL;
    if (!(p = cx_type(p, d))) return NULL;
    while (*p && *p != 'E') {
        if (!cx_digit(*p) && !(*p >= 'a' && *p <= 'f') && *p != 'n' && *p != 'p' && *p != '.') return NULL;
        p++;
    }
    return *p == 'E' ? p + 1 : NULL;
}

static const char *cx_template_arg(const char *p, int d)
{
    if (p[0] == 'T' && p[1] == 'y') return cx_template_arg(p + 2, d);   /* a type parameter's declaration */
    if (p[0] == 'T' && p[1] == 'n') {                                   /* a non-type parameter's: its type */
        if (!(p = cx_type(p + 2, d))) return NULL;
        return cx_template_arg(p, d);
    }
    switch (*p) {
    case 'L': return cx_expr_primary(p, d);
    case 'J':
        for (p++; *p != 'E';)
            if (!*p || !(p = cx_template_arg(p, d))) return NULL;
        return p + 1;
    case 'X': return NULL;       /* an expression */
    default: return cx_type(p, d);
    }
}

/* I <template-arg>+ E */
static const char *cx_template_args(const char *p, int d)
{
    if (++d > 64) return NULL;
    for (p++; *p != 'E';)
        if (!*p || !(p = cx_template_arg(p, d))) return NULL;
    return p + 1;
}

/* <unqualified-name> [<abi-tags>] */
static const char *cx_unqualified(const char *p, int d, cx_comp *out)
{
    cx_comp c = {.kind = 't'};
    if (p[0] == 'L' && cx_digit(p[1])) p++;                 /* internal linkage */
    if (cx_digit(*p)) {
        c.kind = 'n';
        p = cx_source_name(p, &c.id, &c.len);
    } else if (p[0] == 'C' && p[1] == 'I' && (p[2] == '1' || p[2] == '2')) {   /* inheriting constructor: CI1 <base> */
        c.kind = 'c', c.code = p[2];
        p = cx_type(p + 3, d);
    } else if (p[0] == 'C' && p[1] >= '1' && p[1] <= '5') {
        c.kind = 'c', c.code = p[1];
        p += 2;
    } else if (p[0] == 'D' && p[1] >= '0' && p[1] <= '5') {
        c.kind = 'd', c.code = p[1];
        p += 2;
    } else if (p[0] == 'D' && p[1] == 'C') {               /* structured binding: DC <source-name>+ E */
        for (p += 2; *p != 'E';)
            if (!(p = cx_source_name(p, NULL, NULL))) return NULL;
        p++;
    } else if (p[0] == 'U' && p[1] == 't') {               /* unnamed type: Ut [<number>] _ */
        for (p += 2; cx_digit(*p); p++) {}
        p = *p == '_' ? p + 1 : NULL;
    } else if (p[0] == 'U' && p[1] == 'l') {               /* lambda: Ul <parameter types> E [<number>] _ */
        for (p += 2; p && *p != 'E';) p = *p ? cx_type(p, d) : NULL;
        if (!p) return NULL;
        for (p++; cx_digit(*p); p++) {}
        p = *p == '_' ? p + 1 : NULL;
    } else if (p[0] >= 'a' && p[0] <= 'z' && p[1]) {        /* an operator */
        c.kind = 'o', c.id = p, c.len = 2;
        if (p[0] == 'c' && p[1] == 'v') p = cx_type(p + 2, d);
        else if (p[0] == 'l' && p[1] == 'i') p = cx_source_name(p + 2, NULL, NULL);
        else if (p[0] == 'v' && cx_digit(p[1])) p = cx_source_name(p + 2, NULL, NULL);
        else if ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z')) p += 2;
        else return NULL;
    } else
        return NULL;
    while (p && *p == 'B') p = cx_source_name(p + 1, NULL, NULL);   /* ABI tags */
    if (p && out) *out = c;
    return p;
}

/* N [<CV-qualifiers>] [<ref-qualifier>] <prefix> <unqualified-name> E, at 'N'. last[0] and last[1]: the
 * last two components (kind 0: none); *cv_const: the K qualifier (a const member function). */
static const char *cx_nested(const char *p, int d, cx_comp last[2], bool *cv_const)
{
    cx_comp prev = {0}, cur = {0};
    bool k = false;
    if (++d > 64) return NULL;
    for (p++; *p == 'r' || *p == 'V' || *p == 'K'; p++) k |= *p == 'K';
    if (*p == 'R' || *p == 'O') p++;
    while (*p != 'E') {
        cx_comp c = {.kind = 's'};
        if (!*p) return NULL;
        if (*p == 'I') {                                   /* the template arguments of the component before */
            if (!cur.kind || !(p = cx_template_args(p, d))) return NULL;
            continue;
        }
        if (*p == 'M') { p++; continue; }                  /* a data-member prefix */
        if (p[0] == 'S' && p[1] == 't') {                   /* ::std */
            c.kind = 'n', c.id = "std", c.len = 3;
            p += 2;
        } else if (p[0] == 'S' && p[1] && strchr("absiod", p[1]))
            p += 2;
        else if (p[0] == 'S')
            p = cx_seq(p + 1);
        else if (p[0] == 'T' && (p[1] == '_' || cx_digit(p[1])))
            p = cx_seq(p + 1);
        else if (p[0] == 'D' && (p[1] == 't' || p[1] == 'T'))
            return NULL;                                   /* decltype */
        else
            p = cx_unqualified(p, d, &c);
        if (!p) return NULL;
        prev = cur;
        cur = c;
    }
    if (!cur.kind) return NULL;
    if (last) { last[0] = prev; last[1] = cur; }
    if (cv_const) *cv_const = k;
    return p + 1;
}

static const char *cx_type(const char *p, int d)
{
    if (++d > 64 || !*p) return NULL;
    char c = *p;
    if (strchr("vwbcahstijlmxynofdegz", c)) return p + 1;
    switch (c) {
    case 'r': case 'V': case 'K': case 'P': case 'R': case 'O': case 'C': case 'G':
        return cx_type(p + 1, d);
    case 'U':                                              /* vendor qualifier U <source-name> [<template-args>] <type> */
        if (!(p = cx_source_name(p + 1, NULL, NULL))) return NULL;
        if (*p == 'I' && !(p = cx_template_args(p, d))) return NULL;
        return cx_type(p, d);
    case 'u':                                              /* vendor type */
        if (!(p = cx_source_name(p + 1, NULL, NULL))) return NULL;
        return *p == 'I' ? cx_template_args(p, d) : p;
    case 'F':                                              /* F [Y] <types> [<ref-qualifier>] E */
        if (*++p == 'Y') p++;
        while (*p != 'E') {
            if ((*p == 'R' || *p == 'O') && p[1] == 'E') { p++; break; }
            if (!*p || !(p = cx_type(p, d))) return NULL;
        }
        return p + 1;
    case 'A':                                              /* A [<number>] _ <type> */
        for (p++; cx_digit(*p); p++) {}
        return *p == '_' ? cx_type(p + 1, d) : NULL;
    case 'M':                                              /* M <class type> <member type> */
        if (!(p = cx_type(p + 1, d))) return NULL;
        return cx_type(p, d);
    case 'T':
        if (p[1] == 's' || p[1] == 'u' || p[1] == 'e') return cx_type(p + 2, d);   /* elaborated: struct, union, enum */
        if (!(p = cx_seq(p + 1))) return NULL;            /* a template parameter */
        return *p == 'I' ? cx_template_args(p, d) : p;
    case 'S':
        if (p[1] == 't') p = cx_unqualified(p + 2, d, NULL);
        else if (p[1] && strchr("absiod", p[1])) p += 2;
        else p = cx_seq(p + 1);
        return p && *p == 'I' ? cx_template_args(p, d) : p;
    case 'N':
        return cx_nested(p, d, NULL, NULL);
    case 'D':
        switch (p[1]) {
        case 'd': case 'e': case 'f': case 'h': case 'i': case 's': case 'u': case 'a': case 'c': case 'n':
            return p + 2;
        case 'F':                                          /* DF <number> _ | b | x */
            for (p += 2; cx_digit(*p); p++) {}
            return *p == '_' || *p == 'b' || *p == 'x' ? p + 1 : NULL;
        case 'B': case 'U':                                /* _BitInt: DB <number> _ */
            for (p += 2; cx_digit(*p); p++) {}
            return *p == '_' ? p + 1 : NULL;
        case 'p':                                          /* pack expansion */
            return cx_type(p + 2, d);
        case 'v':                                          /* vector: Dv <number> _ <type> */
            if (!cx_digit(p[2])) return NULL;
            for (p += 2; cx_digit(*p); p++) {}
            return *p == '_' ? cx_type(p + 1, d) : NULL;
        case 'x': case 'o':                                /* transaction_safe / noexcept function type */
            return cx_type(p + 2, d);
        case 'w':                                          /* dynamic exception specification */
            for (p += 2; *p != 'E';)
                if (!*p || !(p = cx_type(p, d))) return NULL;
            return cx_type(p + 1, d);
        default:
            return NULL;
        }
    default:
        if (cx_digit(c) || (c == 'L' && cx_digit(p[1]))) {   /* a class or enum: <unqualified-name> [<template-args>] */
            if (!(p = cx_unqualified(p, d, NULL))) return NULL;
            return *p == 'I' ? cx_template_args(p, d) : p;
        }
        return NULL;
    }
}

/*
 * C++ constructors and destructors. In a mangled name (Itanium C++ ABI
 * 5.1.4.3) a constructor is <ctor-dtor-name> C1/C2/C3 or CI1/CI2 <base>
 * and a destructor D0/D1/D2, as the last component of a nested name
 * (N ... E); destructors take no parameters (Ev). The ARM C++ ABI that
 * Apple arm64 follows returns `this` from constructors and from complete
 * and base-object destructors (not from the deleting D0, nor the allocating
 * C3), constructor templates included, and guest code compiled for it uses
 * x0 after such a call; x86_64 returns void. The nested name is read from
 * the mangled form (cx_nested), not from a demangler's spelling of it.
 */
bool hao_cxx_this_return(const char *name)
{
    if (strncmp(name, "__ZN", 4)) return false;
    cx_comp last[2];
    const char *params = cx_nested(name + 3, 0, last, NULL);
    if (!params) return false;
    if (last[1].kind == 'd') return (last[1].code == '1' || last[1].code == '2') && !strcmp(params, "v");
    return last[1].kind == 'c' && (last[1].code == '1' || last[1].code == '2');
}

static bool cx_is(const cx_comp *c, const char *id)
{
    return c->kind == 'n' && c->len == strlen(id) && !memcmp(c->id, id, c->len);
}

static const hao_plan *publish(const hao_plan *tmp, const char *name)
{
    const hao_plan *r;
    L();
    bool share = tmp->cls == HAO_BR_UNKNOWN && !hao_bridge_counting && !tmp->why && !tmp->redirect &&
                 !(tmp->flags & (HAO_PLAN_SWIFTASYNC | HAO_PLAN_ASYNCARG | HAO_PLAN_PROBE));
    if (share) r = tab_put(tmp->native, (tmp->flags & HAO_PLAN_DLADDR) ? &unknown_bare : &unknown_import);
    else {
        hao_plan *p = arena_alloc(sizeof *p);
        if (!p) { U(); return NULL; }
        *p = *tmp;
        /* names: for reports and diagnostics only */
        if (hao_bridge_counting || p->cls == HAO_BR_REFUSED) p->name = arena_str(name);
        r = tab_put(tmp->native, p);
    }
    U();
    return r;
}

/* An async function pointer import (NAME + "Tu", a native record {rel32
 * Function, u32 ExpectedContextSize}, ABI/Executor.h): guest code calls
 * the function it names through the record, at an address that need not be
 * an exported symbol. That entry point gets a plan marked async, named
 * after the function. */
static void note_async_entry(const char *gname, uint64_t afp)
{
    size_t n = strlen(gname);
    if (n < 6 || strncmp(gname, "_$s", 3) || strcmp(gname + n - 2, "Tu") || (afp & 3)) return;
    Dl_info da, df;
    int32_t rel;
    if (!dladdr((void *)(uintptr_t)afp, &da) || !da.dli_fbase) return;
    memcpy(&rel, (const void *)(uintptr_t)afp, 4);
    uint64_t fn = afp + (uint64_t)(int64_t)rel;
    if (!dladdr((void *)(uintptr_t)fn, &df) || df.dli_fbase != da.dli_fbase) return;   /* the record's own library */
    const hao_plan *old = hao_bridge_lookup(fn);
    if (old && (old->flags & HAO_PLAN_SWIFTASYNC)) return;
    hao_plan tmp;
    if (old) tmp = *old;   /* a shared UNKNOWN plan has no address of its own */
    else memset(&tmp, 0, sizeof tmp), tmp.argmask = HAO_BR_SCAN_ALL, tmp.cls = HAO_BR_UNKNOWN;
    tmp.native = fn;
    tmp.flags |= HAO_PLAN_SWIFTASYNC;
    char name[1024];
    snprintf(name, sizeof name, "%.*s", (int)(n - 2), gname);
    publish(&tmp, old && old->name ? old->name : name);
}


static bool tab_same(char k1, const char *e1, char k2, const char *e2)
{
    return k1 == k2 && (e1 || e2 ? e1 && e2 && !strcmp(e1, e2) : true);
}

/*
 * The type of the native function at `at` (in the image at dh) from every
 * name it has: the unit records of every name its library exports at that
 * address, the import's own name (uname for the unit, tname for the tables;
 * a record under a name the library does not export at an address is found
 * by that name only), and the table lines of every name that resolves to the
 * address (labels_at). Precedence: the import's own name, then the others;
 * two that disagree set conflict (the call is refused: two names of one
 * function can only disagree when a generator is wrong).
 */
typedef struct {
    const hao_if_view *v;
    const hao_if_rec *r;
    int tsrc;
    char tkind;
    const char *tenc, *conflict, *other;   /* other: a name other than the import's that typed it */
    mask_merge mm;                          /* the masks of every typed record of the function */
    const char *cxx_why;                    /* another name of the address is a std::string function no table converts */
} addr_type;

static void type_at(uint64_t at, const struct mach_header_64 *dh, const char *path, const char *tname, const char *uname,
                    addr_type *o)
{
    memset(o, 0, sizeof *o);
    if (tname) o->tsrc = table_type(tname, &o->tkind, &o->tenc);
    const native_lib *l = dh && units_on ? lib_for((uint64_t)(uintptr_t)dh, path) : NULL;
    if (l) {
        o->v = &l->v;
        const char *n2;
        bool uc;
        const hao_if_rec *ra = unit_rec_at(l, at - (uint64_t)(uintptr_t)dh, &n2, &uc, &o->mm);
        const hao_if_rec *rn = uname ? hao_ifunit_find(&l->v, uname) : NULL;
        if (rn && unit_placed(l, rn)) rn = NULL;     /* at its own address: in ra's group, or another function */
        if (rn && rn->cls != HAO_BR_UNKNOWN) mask_add(&o->mm, rn);
        if (rn && ra && rn->cls == HAO_BR_UNKNOWN && ra->cls != HAO_BR_UNKNOWN) rn = NULL;   /* a reason only */
        if (uc) o->conflict = "two names of the function are typed differently (interface unit)";
        else if (ra && rn && ra->cls != HAO_BR_UNKNOWN && !rec_same(&l->v, ra, rn))
            o->conflict = "the import and another name of the function are typed differently (interface unit)";
        o->r = rn ? rn : ra;
        if (!rn && ra && ra->cls != HAO_BR_UNKNOWN && (!uname || strcmp(l->v.str + ra->name, uname))) o->other = l->v.str + ra->name;
        if (uc && !o->other) o->other = n2;
    }
    const char *lb[8];
    int n = labels_at(at, LBL_TABLE, lb, 8);
    for (int k = 0; k < n; k++) {
        if (tname && !strcmp(lb[k], tname)) continue;
        char kk = 0;
        const char *ke = NULL;
        int ks = table_type(lb[k], &kk, &ke);
        if (!ks) {
            const char *ce = NULL, *cw = NULL;
            if (hao_cxxstr_lookup(lb[k], &ce, &cw) < 0 && !o->cxx_why) o->cxx_why = cw;
            continue;
        }
        if (!o->tsrc) { o->tsrc = ks; o->tkind = kk; o->tenc = ke; if (!o->other) o->other = lb[k]; }
        else if (!tab_same(o->tkind, o->tenc, kk, ke) && !o->conflict) {
            o->conflict = "two names of the function are typed differently (compiled-in tables)";
            o->other = lb[k];
        }
    }
}

/* A unit plan: the union of the masks of every typed record of the function
 * (the plan's own record among them). */
static void apply_masks(hao_plan *p, const addr_type *o)
{
    if (p->src != HAO_BRS_UNIT) return;
    p->resmask |= o->mm.resmask;
    if (o->mm.unmasked) { p->flags &= (uint8_t)~HAO_PLAN_MASKED; p->argmask = HAO_BR_SCAN_ALL; }
    else if (p->flags & HAO_PLAN_MASKED) p->argmask |= o->mm.argmask;
}

static void alias_report(const char *gname, const addr_type *o)
{
    if (!o->conflict) return;
    __atomic_fetch_add(&st_alias_refused, 1, __ATOMIC_RELAXED);
    if (hao_opt.bridge_check) {
        HAO_LOG(0, "bridge: REFUSED %s (also named %s): %s\n", gname, o->other ? o->other : "?", o->conflict);
    }
}

/* A native C++ function outside libc++ with a std::string (std::function, string stream) in its
 * signature that no unit or table types (this-return alone does not convert them): its arguments cannot be right (src/cxxstr.c), so the call
 * aborts with the reason instead of running on another layout. */
static void cxx_refuse(hao_plan *p, const char *name, const char *why)
{
    p->cls = HAO_BR_REFUSED;
    p->why = why;
    p->kind = 0;
    p->enc = NULL;
    if (hao_opt.bridge_check) {
        HAO_LOG(0, "bridge: REFUSED %s: %s\n", name, why);
    }
}

static void cxx_fail_closed(hao_plan *p, const char *name, const char *lib)
{
    const char *why = hao_cxxstr_divergent(name, lib);
    if (why) cxx_refuse(p, name, why);
}

int hao_bridge_bind(const char *gname, const char *nname, uint64_t native)
{
    hao_bridge_init();
    if (!native) return HAO_BR_UNKNOWN;
    uint64_t at = typed_at(native);
    const struct mach_header_64 *dh = (const struct mach_header_64 *)dyld_image_header_containing_address((const void *)(uintptr_t)at);
    addr_type o;
    type_at(at, dh, dh ? dyld_image_path_containing_address((const void *)(uintptr_t)at) : NULL, gname, nname ? nname : gname, &o);
    /* native_sym binds a function with a trap under another name to the trap (hao_bridge_trap_at) */
    uint64_t trap = 0;
    const char *tn;
    int tr = trap_at(native, &trap, &tn);
    if (tr < 0) o.conflict = "two names of the function have different traps";
    hao_plan tmp;
    make_plan(&tmp, native, gname, o.v, o.r, o.tsrc, o.tkind, o.tenc);
    apply_masks(&tmp, &o);
    if (tr > 0) {
        tmp.cls = HAO_BR_TYPED;
        tmp.src = HAO_BRS_TRAP;
        tmp.redirect = trap;
    }
    if (o.conflict) {   /* fail closed: aborts with the reason on the call */
        tmp.cls = HAO_BR_REFUSED;
        tmp.why = o.conflict;
    } else if (o.other && tmp.cls != HAO_BR_UNKNOWN) __atomic_fetch_add(&st_alias_typed, 1, __ATOMIC_RELAXED);
    alias_report(gname, &o);
    if ((tmp.cls == HAO_BR_UNKNOWN || tmp.src == HAO_BRS_THISRET) && dh) {
        cxx_fail_closed(&tmp, nname ? nname : gname, dyld_image_path_containing_address((const void *)(uintptr_t)at));
        if (tmp.cls != HAO_BR_REFUSED && o.cxx_why) cxx_refuse(&tmp, gname, o.cxx_why);
    }
    const hao_plan *p = publish(&tmp, nname ? nname : gname);
    note_async_entry(gname, native);
    int cls = p ? p->cls : HAO_BR_UNKNOWN;
    __atomic_fetch_add(&st_bind_cls[tmp.cls], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st_bind_src[tmp.src], 1, __ATOMIC_RELAXED);
    return cls;
}

void hao_bridge_bind_trap(const char *name, int src)
{
    (void)name;
    __atomic_fetch_add(&st_bind_cls[HAO_BR_TYPED], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st_bind_src[src < HAO_BRS_N ? src : HAO_BRS_TRAP], 1, __ATOMIC_RELAXED);
}

void hao_bridge_note(uint64_t native, char kind, const char *enc, int src)
{
    hao_bridge_init();
    if (!native || !kind) return;
    const hao_plan *old = hao_bridge_lookup(native);
    if (old && old->cls != HAO_BR_UNKNOWN) return;      /* known (same function, same type) */
    hao_plan tmp = {.native = native, .argmask = HAO_BR_SCAN_ALL, .cls = HAO_BR_TYPED, .src = (uint8_t)src, .kind = kind,
                    .enc = enc, .calls = old ? old->calls : 0, .flags = old ? (old->flags & HAO_PLAN_DLADDR) : 0};
    publish(&tmp, old ? old->name : NULL);
}

/* The native half of a generated thunk pair: its only argument is the
 * guest half's slot array (no guest code), and its plan must exist before
 * the first call so that no first-call bind or scan runs for it. */
void hao_bridge_note_native_half(uint64_t nh)
{
    hao_bridge_init();
    const hao_plan *old = hao_bridge_lookup(nh);
    if (old && old->cls == HAO_BR_EXACT) return;
    hao_plan tmp = {.native = nh, .argmask = 0, .cls = HAO_BR_EXACT, .src = HAO_BRS_THUNK, .flags = HAO_PLAN_MASKED};
    __atomic_fetch_add(&st_thunk_halves, 1, __ATOMIC_RELAXED);
    publish(&tmp, "(thunk native half)");
}

/* A guest signal handler is running on this thread (signals.c sets
 * sig_depth to the handler's segment depth; a longjmp out of the handler
 * leaves it stale, which the depth check clears). */
static bool in_signal_handler(hao_thread *t)
{
    int d = t->sig_depth;
    if (d && t->depth < d) { t->sig_depth = 0; return false; }
    return d != 0;
}
bool hao_bridge_in_signal_handler(hao_thread *t) { return in_signal_handler(t); }

/*
 * A native libc++ object that reached guest code (an exception thrown by
 * native code, an object a native library handed over) and whose virtual
 * function returns a std::basic_string by value: the result goes through x8
 * on arm64 and a hidden rdi argument on x86_64, and the string's layout
 * differs (libc++'s alternate layout on arm64), so no register bridge can
 * make the call correct (route REFUSED). From
 * the SDK's c++/v1 headers: error_category::message(int) (<system_error>),
 * numpunct::do_grouping/do_truename/do_falsename,
 * moneypunct::do_grouping/do_curr_symbol/do_positive_sign/do_negative_sign,
 * collate::do_transform and messages::do_get (<locale>), and their
 * overriders in libc++. With guest libc++ guest code reaches such a function
 * only through a native object's vtable or dlsym.
 */
static const char *libcxx_string_result(const char *name, const char *lib)
{
    size_t ll = lib ? strlen(lib) : 0;
    if (ll < 15 || strcmp(lib + ll - 15, "/libc++.1.dylib") || strncmp(name, "__ZNK", 5)) return NULL;
    static const char *const facet_fns[] = {"do_grouping", "do_truename", "do_falsename", "do_curr_symbol",
                                            "do_positive_sign", "do_negative_sign", "do_transform"};
    cx_comp last[2];
    bool is_const = false;
    const char *params = cx_nested(name + 3, 0, last, &is_const);   /* N K <class> <function> E <params> */
    if (!params) return NULL;
    if (cx_is(&last[1], "message"))
        return is_const && !strcmp(params, "i") ? "error_category::message returns std::string by value" : NULL;
    if (cx_is(&last[1], "do_get") && last[0].kind == 'n' && last[0].len >= 8 && !memcmp(last[0].id, "messages", 8))
        return "messages::do_get returns std::basic_string by value";
    for (size_t k = 0; k < sizeof facet_fns / sizeof *facet_fns; k++)
        if (cx_is(&last[1], facet_fns[k])) return "a locale facet's virtual function returns std::basic_string by value";
    return NULL;
}

char hao_bridge_kind_for_enc(const char *enc)
{
    if (!enc) return 0;
    return !hao_sig_simple(enc) ? 'A' : hao_narrowret_kind_for_enc(enc);
}

/* ObjC-typed first-call plans: methods and blocks typed, calls through a method plan with another
 * selector (rechecked), and those whose method had another encoding or none */
static uint64_t st_objc_imp, st_objc_block, st_objc_recheck, st_objc_other, st_objc_unseen;

/* The ObjC runtime's record of what guest code calls at native pc, from the
 * call's own registers: a native block in x0 whose invoke is pc (a block is
 * always its invoke's first argument), else the method at or above x0's class
 * whose IMP for selector x1 is pc. The encoding is the native runtime's, the
 * one trap_msgsend marshals the same methods by. */
static bool type_by_objc(const hao_cpu *c, uint64_t pc, hao_plan *p)
{
    int src = HAO_BRS_BLOCKSIG;
    uint64_t sel = 0;
    const char *enc = hao_native_block_sig(c->x[0], pc);
    if (!enc) {
        enc = hao_objc_imp_types(c->x[0], c->x[1], pc);
        src = HAO_BRS_OBJCIMP;
        sel = c->x[1];
    }
    if (!enc) return false;
    uint64_t vt = src == HAO_BRS_OBJCIMP ? hao_objc_variadic_imp_trap(pc, true) : 0;
    if (vt) {   /* a variadic method: its encoding names only the fixed arguments */
        p->cls = HAO_BR_TYPED;
        p->src = HAO_BRS_VARSNIFF;
        p->redirect = vt;
        p->flags &= ~HAO_PLAN_PROBE;
        p->why = NULL;
        __atomic_fetch_add(&st_objc_imp, 1, __ATOMIC_RELAXED);
        return true;
    }
    char kind = hao_bridge_kind_for_enc(enc);
    const char *copy = NULL;
    if (kind == 'A') {
        if (strlen(enc) > 4096) return false;   /* arena_str's limit: never a cut encoding */
        L();
        copy = arena_str(enc);
        U();
        if (!copy) return false;
    }
    p->cls = HAO_BR_TYPED;
    p->src = (uint8_t)src;
    p->kind = kind;
    p->enc = copy;
    p->sel = sel;
    p->flags &= ~HAO_PLAN_PROBE;
    p->why = NULL;
    __atomic_fetch_add(src == HAO_BRS_OBJCIMP ? &st_objc_imp : &st_objc_block, 1, __ATOMIC_RELAXED);
    return true;
}

static const char *sel_name(uint64_t s) { return s ? sel_getName((SEL)(uintptr_t)s) : "?"; }

/* encodings equal but for offsets, qualifiers and quoted class names: the same ABI */
static bool same_types(const char *a, const char *b)
{
    for (;;) {
        while (*a && (strchr("0123456789rnNoORV", *a) || *a == '"')) {
            if (*a == '"') { a = strchr(a + 1, '"'); if (!a) return false; }
            a++;
        }
        while (*b && (strchr("0123456789rnNoORV", *b) || *b == '"')) {
            if (*b == '"') { b = strchr(b + 1, '"'); if (!b) return false; }
            b++;
        }
        if (*a != *b) return false;
        if (!*a) return true;
        a++, b++;
    }
}

const hao_plan *hao_bridge_objc_recheck(const struct hao_cpu *c, const hao_plan *p, hao_plan *tmp)
{
    __atomic_fetch_add(&st_objc_recheck, 1, __ATOMIC_RELAXED);
    const char *enc = hao_objc_imp_types(c->x[0], c->x[1], p->native);
    char kind = enc ? hao_bridge_kind_for_enc(enc) : 0;
    if (enc && kind == p->kind && (kind != 'A' || same_types(enc, p->enc))) return p;
    if (!enc) {
        /* no method shows this IMP for this receiver: one IMP's code has one ABI, the plan's */
        __atomic_fetch_add(&st_objc_unseen, 1, __ATOMIC_RELAXED);
        return p;
    }
    *tmp = *p;
    /* one IMP behind methods of different types (a forwarding or shared implementation): this call's own */
    if (!__atomic_fetch_add(&st_objc_other, 1, __ATOMIC_RELAXED) || hao_opt.bridge_check) {
        HAO_LOG(0, "bridge: native IMP %#llx typed %s for %s, called as %s with %s: typed per call\n",
                (unsigned long long)p->native, p->enc ? p->enc : "(register-exact)", sel_name(p->sel), enc,
                sel_name(c->x[1]));
    }
    tmp->kind = kind;
    tmp->enc = enc;   /* the runtime's string: alive while its class is, for this one call */
    return tmp;
}

void hao_bridge_note_imp(uint64_t imp, const char *enc, uint64_t sel)
{
    hao_bridge_init();
    if (!imp || !enc || hao_image_for_code(imp) || hao_guest_of_entry(imp)) return;   /* guest IMPs are guest code */
    const hao_plan *old = hao_bridge_lookup(imp);
    if (old && old->cls != HAO_BR_UNKNOWN) return;
    hao_thread *t = hao_current_peek();
    if (t && in_signal_handler(t)) return;   /* no prologue decoding there: its first call types it */
    uint64_t vt = hao_objc_variadic_imp_trap(imp, true);
    char kind = vt ? 0 : hao_bridge_kind_for_enc(enc);
    if (kind == 'A' && strlen(enc) > 4096) return;
    hao_plan tmp = {.native = imp, .argmask = HAO_BR_SCAN_ALL, .cls = HAO_BR_TYPED, .src = vt ? HAO_BRS_VARSNIFF : HAO_BRS_OBJCIMP,
                    .kind = kind, .sel = sel, .redirect = vt, .calls = old ? old->calls : 0,
                    .flags = old ? (old->flags & HAO_PLAN_DLADDR) : 0};
    if (kind == 'A') {
        L();
        tmp.enc = arena_str(enc);
        U();
        if (!tmp.enc) return;
    }
    __atomic_fetch_add(&st_objc_imp, 1, __ATOMIC_RELAXED);
    publish(&tmp, old ? old->name : NULL);
}

/* First call of a native address no import bound (dlsym, a native function
 * pointer, a native block's invoke): by its address, the same sources as
 * hao_bridge_bind, then the ObjC runtime's record of it (type_by_objc);
 * dladdr names it (a label: reports, varsniff, the libc++ string-result rule).
 * A target that is not host code fails closed (not_host_code). */
static const char *not_host_code(uint64_t pc)
{
    /* arm64 code the guest generated (a JIT): hao translates registered images only */
    if (hao_guest_exec_mapped(pc))
        return "the target is in memory the guest mapped executable itself (arm64 code generated at run time, which hao does not run)";
    /* the leaf entry: the shared cache is a submap whose outer entry reads r-- */
    vm_region_submap_info_data_64_t info;
    natural_t depth = 0;
    for (;;) {
        mach_vm_address_t a = pc;
        mach_vm_size_t size = 0;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &cnt);
        if (kr != KERN_SUCCESS || a > pc) return "nothing is mapped at the target";
        if (!info.is_submap || depth >= 8) break;
        depth++;
    }
    if (!(info.protection & VM_PROT_EXECUTE))
        return "the target is not executable host memory (guest code outside a loaded image, such as arm64 generated at run time, or data)";
    return NULL;
}

static const hao_plan *bind_bare(hao_thread *t, uint64_t pc)
{
    const char *bad = not_host_code(pc);
    if (bad) {
        char label[32];
        snprintf(label, sizeof label, "%#llx", (unsigned long long)pc);
        hao_plan rp = {.native = pc, .argmask = HAO_BR_SCAN_ALL, .cls = HAO_BR_REFUSED, .flags = HAO_PLAN_DLADDR, .why = bad};
        const hao_plan *p = publish(&rp, label);
        if (!p) hao_bridge_refused(t, &rp);   /* never run it as x86 code, recorded or not */
        return p;
    }
    Dl_info di;
    uint64_t at = typed_at(pc);
    hao_crit_enter();          /* a guest signal handler waits until dyld's lock is free again */
    int ok = dladdr((void *)(uintptr_t)at, &di);
    hao_crit_leave();
    __atomic_fetch_add(&st_bare, 1, __ATOMIC_RELAXED);
    char name[512];
    bool exact = ok && di.dli_sname && di.dli_saddr && (uint64_t)(uintptr_t)di.dli_saddr == at;
    if (exact) {
        snprintf(name, sizeof name, "_%s", di.dli_sname);
        __atomic_fetch_add(&st_bare_named, 1, __ATOMIC_RELAXED);
    } else if (ok && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        snprintf(name, sizeof name, "%s`%s+%#llx", b ? b + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?",
                 (unsigned long long)(at - (uint64_t)(uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase)));
    } else snprintf(name, sizeof name, "%#llx", (unsigned long long)pc);
    /* a function hao interposes, under any of its names: the call goes to its trap */
    uint64_t trap = 0;
    const char *tn;
    int isrc = 0, tr = trap_at(pc, &trap, &tn);
    if (tr > 0) isrc = HAO_BRS_TRAP;
    else if (!tr && exact) {
        hao_crit_enter();
        if (hao_varsniff_bind(name, pc, &trap)) isrc = HAO_BRS_VARSNIFF;
        hao_crit_leave();
    }
    if (isrc && trap && trap != pc) {
        hao_plan tp = {.native = pc, .argmask = HAO_BR_SCAN_ALL, .cls = HAO_BR_TYPED, .src = (uint8_t)isrc,
                       .flags = HAO_PLAN_DLADDR, .redirect = trap};
        __atomic_fetch_add(&st_bare_typed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&st_bare_redirected, 1, __ATOMIC_RELAXED);
        return publish(&tp, name);
    }
    addr_type o;
    type_at(at, ok ? (const struct mach_header_64 *)di.dli_fbase : NULL, ok ? di.dli_fname : NULL, exact ? name : NULL,
            exact ? name : NULL, &o);
    if (tr < 0) o.conflict = "two names of the function have different traps";
    hao_plan tmp;
    make_plan(&tmp, pc, name, o.v, o.r, o.tsrc, o.tkind, o.tenc);
    apply_masks(&tmp, &o);
    tmp.flags |= HAO_PLAN_DLADDR;
    const char *sr = exact && !o.r ? libcxx_string_result(name, di.dli_fname) : NULL;
    if (o.conflict) sr = o.conflict;
    else if (o.other && tmp.cls != HAO_BR_UNKNOWN) __atomic_fetch_add(&st_alias_typed, 1, __ATOMIC_RELAXED);
    alias_report(name, &o);
    if (sr) {   /* fail closed: aborts with the reason on the call */
        tmp.cls = HAO_BR_REFUSED;
        tmp.why = sr;
    } else if (tmp.cls == HAO_BR_UNKNOWN || tmp.src == HAO_BRS_THISRET) {
        if (exact) cxx_fail_closed(&tmp, name, di.dli_fname);
        if (tmp.cls != HAO_BR_REFUSED && o.cxx_why) cxx_refuse(&tmp, name, o.cxx_why);
    }
    if (tmp.cls == HAO_BR_UNKNOWN && !sr && !tmp.redirect && !type_by_objc(&t->cpu, pc, &tmp))
        tmp.flags |= HAO_PLAN_PROBE;
    if (tmp.cls != HAO_BR_UNKNOWN) __atomic_fetch_add(&st_bare_typed, 1, __ATOMIC_RELAXED);
    return publish(&tmp, name);
}

#define OBJC_PROBES HAO_PLAN_OBJC_PROBES   /* hao/bridge.h; hao_bridge_plan_settled */

const hao_plan *hao_bridge_plan_for_call(hao_thread *t, uint64_t pc)
{
    const hao_plan *p = hao_bridge_lookup(pc);
    if (p && (p->flags & HAO_PLAN_PROBE) && p->probes < OBJC_PROBES && !in_signal_handler(t)) {
        hao_plan tmp = *p;
        if (__atomic_fetch_add(&((hao_plan *)p)->probes, 1, __ATOMIC_RELAXED) < OBJC_PROBES && type_by_objc(&t->cpu, pc, &tmp)) {
            tmp.calls = p->calls;
            const hao_plan *n = publish(&tmp, p->name);
            if (n) return n;
        }
        return p;
    }
    if (p || !__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return p;
    if (in_signal_handler(t)) { __atomic_fetch_add(&st_bare_skipped, 1, __ATOMIC_RELAXED); return NULL; }
    return bind_bare(t, pc);
}

/* ------------------------------------------------------ diagnostics */

void hao_bridge_count(hao_thread *t, const hao_plan *p, uint64_t pc)
{
    if (!p) return;
    uint64_t n = __atomic_fetch_add(&((hao_plan *)p)->calls, 1, __ATOMIC_RELAXED);
    if (n || !hao_opt.bridge_check || p->cls != HAO_BR_UNKNOWN || in_signal_handler(t)) return;
    if (__atomic_exchange_n(&((hao_plan *)p)->reported, 1, __ATOMIC_RELAXED)) return;
    char where[1100];
    HAO_LOG(0, "bridge: UNKNOWN %s (%#llx)%s%s first call, lr=%#llx%s\n", p->name ? p->name : "?", (unsigned long long)pc,
            p->why ? ": " : "", p->why ? p->why : "", (unsigned long long)t->cpu.x[30],
            hao_diag_guest_where(t->cpu.x[30], where, sizeof where));
}

void hao_bridge_check_mask(hao_thread *t, const hao_plan *p)
{
    unsigned skipped = hao_args_code_slots(t) & ~(unsigned)p->argmask;
    if (!skipped) return;
    char where[1100];
    HAO_LOG(0, "bridge: CHECK %s: argument slots %#x hold guest code addresses the plan's mask %#x skips, lr=%#llx%s\n",
            p->name ? p->name : "?", skipped, p->argmask, (unsigned long long)t->cpu.x[30],
            hao_diag_guest_where(t->cpu.x[30], where, sizeof where));
    if (hao_opt.bridge_abort) abort();
}

void hao_bridge_refused(hao_thread *t, const hao_plan *p)
{
    char where[1100];
    HAO_LOG(0, "bridge: REFUSED call to %s (%#llx): %s; lr=%#llx%s\n", p->name ? p->name : "?", (unsigned long long)p->native,
            p->why ? p->why : "no reason recorded", (unsigned long long)t->cpu.x[30],
            hao_diag_guest_where(t->cpu.x[30], where, sizeof where));
    fflush(HAO_DIAG);
    if (hao_opt.bridge_stats) hao_bridge_stats();
    abort();
}

typedef struct { const char *name; uint64_t calls; } top_ent;
static int top_cmp(const void *a, const void *b)
{
    uint64_t x = ((const top_ent *)a)->calls, y = ((const top_ent *)b)->calls;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* One line per process with the plan counts, and (with counting on) the
 * UNKNOWN symbols called most:
 *   hao: bridge PROG[PID]: bound E/T/R/U (edges), symbols E/T/R/U, called ..., calls ...
 */
void hao_bridge_stats(void)
{
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    hao_diag_blk blk;   /* one write: the rows stay under their header in the log */
    FILE *f = hao_diag_block(&blk);
    const hao_plan_table *t = __atomic_load_n(&hao_plan_tab, __ATOMIC_ACQUIRE);
    uint64_t sym[HAO_BR_NCLASS] = {0}, called[HAO_BR_NCLASS] = {0}, calls[HAO_BR_NCLASS] = {0}, masked = 0;
    uint64_t proven = 0, proven_calls = 0;
    enum { TOP = 25 };
    static top_ent top[TOP + 1];
    int ntop = 0;
    for (uint32_t k = 0; t && k <= t->mask; k++) {
        if (!t->s[k].key) continue;
        const hao_plan *p = t->s[k].plan;
        sym[p->cls]++;
        if (p->flags & HAO_PLAN_MASKED) masked++;
        uint64_t c = __atomic_load_n(&p->calls, __ATOMIC_RELAXED);
        if (p->flags & HAO_PLAN_PROVEN) { proven++; proven_calls += c; }
        if (!c) continue;
        called[p->cls]++;
        calls[p->cls] += c;
        if (p->cls != HAO_BR_UNKNOWN) continue;
        top[ntop++] = (top_ent){p->name ? p->name : "?", c};
        qsort(top, (size_t)ntop, sizeof *top, top_cmp);
        if (ntop > TOP) ntop = TOP;
    }
    fprintf(f, "hao: bridge %s[%d]: bound exact=%llu typed=%llu refused=%llu unknown=%llu (traps=%llu varsniff=%llu unit=%llu "
               "tables=%llu); entry points exact=%llu typed=%llu refused=%llu unknown=%llu masked=%llu; ",
            getprogname(), (int)getpid(), (unsigned long long)st_bind_cls[HAO_BR_EXACT], (unsigned long long)st_bind_cls[HAO_BR_TYPED],
            (unsigned long long)st_bind_cls[HAO_BR_REFUSED], (unsigned long long)st_bind_cls[HAO_BR_UNKNOWN],
            (unsigned long long)st_bind_src[HAO_BRS_TRAP], (unsigned long long)st_bind_src[HAO_BRS_VARSNIFF],
            (unsigned long long)st_bind_src[HAO_BRS_UNIT],
            (unsigned long long)(st_bind_src[HAO_BRS_STACKARGS] + st_bind_src[HAO_BRS_SIGDB] + st_bind_src[HAO_BRS_NARROWRET] +
                                 st_bind_src[HAO_BRS_SWIFTASYNC] + st_bind_src[HAO_BRS_THISRET] + st_bind_src[HAO_BRS_SWIFTABI] +
                                 st_bind_src[HAO_BRS_CXXSTR]),
            (unsigned long long)sym[HAO_BR_EXACT], (unsigned long long)sym[HAO_BR_TYPED], (unsigned long long)sym[HAO_BR_REFUSED],
            (unsigned long long)sym[HAO_BR_UNKNOWN], (unsigned long long)masked);
    fprintf(f, "proven=%llu/%llu (entry points/calls, subset of exact/typed); ",
            (unsigned long long)proven, (unsigned long long)proven_calls);
    if (hao_bridge_counting)
        fprintf(f, "called exact=%llu/%llu typed=%llu/%llu unknown=%llu/%llu (symbols/calls); ", (unsigned long long)called[HAO_BR_EXACT],
                (unsigned long long)calls[HAO_BR_EXACT], (unsigned long long)called[HAO_BR_TYPED], (unsigned long long)calls[HAO_BR_TYPED],
                (unsigned long long)called[HAO_BR_UNKNOWN], (unsigned long long)calls[HAO_BR_UNKNOWN]);
    /* variadic formats (vafmt.c): long double conversions rewritten, and calls whose slots stayed untyped */
    /* and signatures abi.c declined (malformed, too large: the register bridge took the call) */
    extern uint64_t hao_abi_refused;
    fprintf(f, "formats fixed=%llu untyped=%llu; signatures refused=%llu; ", (unsigned long long)__atomic_load_n(&hao_fmt_fixed, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hao_fmt_untyped, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hao_abi_refused, __ATOMIC_RELAXED));
    /* native variadic ObjC methods objc_msgSend reached by their prologue (objc.c msgsend_varsniff) */
    fprintf(f, "objc variadic methods=%llu calls=%llu untyped=%llu declined=%llu; ",
            (unsigned long long)__atomic_load_n(&hao_objcva_imps, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hao_objcva_calls, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hao_objcva_untyped, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hao_objcva_declined, __ATOMIC_RELAXED));
    /* arm64e cache selector strings objc_msgSend mapped to native selectors (dsc.c hao_dsc_sel_map) */
    fprintf(f, "objc cache selectors mapped=%llu; ", (unsigned long long)__atomic_load_n(&hao_dsc_sels_mapped, __ATOMIC_RELAXED));
    fprintf(f, "first-call binds=%llu (named %llu, typed %llu, to an interposer %llu, skipped in signal handlers %llu); "
               "units=%d (rejected %d), thunk pairs %llu (keys not loaded %llu); table=%u/%u slots, %llu KB plans\n",
            (unsigned long long)st_bare, (unsigned long long)st_bare_named, (unsigned long long)st_bare_typed,
            (unsigned long long)st_bare_redirected, (unsigned long long)st_bare_skipped, units_loaded, units_rejected,
            (unsigned long long)st_thunk_halves, (unsigned long long)st_thunk_missing, t ? t->used : 0, t ? t->mask + 1 : 0,
            (unsigned long long)(arena_bytes >> 10));
    if (st_objc_imp || st_objc_block || st_objc_recheck)
        fprintf(f, "hao: bridge: objc-typed first calls: methods=%llu blocks=%llu; other-selector calls rechecked=%llu "
                   "(another type %llu, no method %llu)\n",
                (unsigned long long)st_objc_imp, (unsigned long long)st_objc_block, (unsigned long long)st_objc_recheck,
                (unsigned long long)st_objc_other, (unsigned long long)st_objc_unseen);
    if (st_conflicts) fprintf(f, "hao: bridge: %llu unit records disagree with the compiled-in tables (HAO_BRIDGE_CHECK=1 names them)\n",
                              (unsigned long long)st_conflicts);
    if (st_alias_typed || st_alias_refused || st_trap_alias)
        fprintf(f, "hao: bridge: %llu entry points typed by another name of their address (renamed re-exports, aliases), "
                   "%llu refused (typed differently under two names), %llu imports bound to the trap of another name\n",
                (unsigned long long)st_alias_typed, (unsigned long long)st_alias_refused, (unsigned long long)st_trap_alias);
    if (hao_verbose && st_lbl_images)
        fprintf(f, "hao: bridge: name sources by address: %zu names, %llu images walked, %llu addresses (%llu through dyld), %llu us (dyld %llu us)\n", lbl_n,
                (unsigned long long)st_lbl_images, (unsigned long long)st_lbl_addrs, (unsigned long long)st_lbl_ind,
                (unsigned long long)(st_lbl_ns / 1000), (unsigned long long)(st_lbl_ind_ns / 1000));
    for (int k = 0; k < ntop; k++)
        fprintf(f, "hao: bridge: unknown %10llu  %s\n", (unsigned long long)top[k].calls, top[k].name);
    hao_diag_block_end(&blk);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* Daemons never exit and PID 1's stderr is the console: with
 * hao_bridge_stats=1 every process also reports after 8 seconds (a smoke
 * run's timeout is 10) and then every two minutes while its counts change. */
static void *bridge_periodic(void *arg)
{
    (void)arg;
    hao_signals_block_async();
    uint64_t last = 0;
    for (unsigned wait = 8;; wait = 120) {
        sleep(wait);
        uint64_t now = st_bare;
        for (int k = 0; k < HAO_BR_NCLASS; k++) now += st_bind_cls[k];
        const hao_plan_table *t = __atomic_load_n(&hao_plan_tab, __ATOMIC_ACQUIRE);
        for (uint32_t k = 0; t && k <= t->mask; k++)
            if (t->s[k].key) now += __atomic_load_n(&t->s[k].plan->calls, __ATOMIC_RELAXED);
        if (now == last) continue;
        last = now;
        hao_bridge_stats();
    }
    return NULL;
}

void hao_bridge_stats_atexit(void)
{
    static int once;
    if (once++) return;
    atexit(hao_bridge_stats);
    pthread_t th;
    if (pthread_create(&th, NULL, bridge_periodic, NULL) == 0) pthread_detach(th);
}
