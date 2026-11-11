/*
 * hao — GSS-API structures whose layout differs between the architectures.
 *
 * The SDK's GSS-API headers (GSS.framework, Kerberos.framework, usr/include/gssapi) are compiled under
 * `#pragma pack(push,2)` on x86_64 only, so the native libraries see
 *   gss_OID_desc                 {u32 length @0; void *elements @4}           12 bytes (arm64: elements @8, 16)
 *   gss_channel_bindings_struct  u32, buffer @4, u32 @20, buffer @24, @40     56 bytes (arm64: 64)
 * and an OID set's element array has a 12-byte stride (arm64: 16). Each side reads the other's OIDs from the
 * wrong offsets: sshd-auth's ssh_gssapi_supported_oids passes its static arm64 OIDs to the native
 * gss_test_oid_set_member, which then memcmps through the upper half of the elements pointer (SIGSEGV), and
 * guest code reading a native OID set (gss_indicate_mechs) or GSS_C_NT_* reads garbage.
 *
 * src/gss_gen.inc types every function and variable of those headers (its generator is not part of this
 * repository). The guest only ever sees arm64-layout objects:
 *   - a native OID or OID set reaching the guest (output parameter, result, exported variable) is given a
 *     shadow in arm64 layout, one per native object, refreshed each time it crosses again; a shadow crossing
 *     back is replaced by its native object, so identity (pointer comparisons, release) is kept;
 *   - an OID the guest built itself is replaced by a native copy interned by content (never freed: OIDs are
 *     few and native code may keep the pointer, as gss_import_name keeps the name type);
 *   - an OID set or channel bindings the guest built itself is converted into a native copy for the call;
 *   - gss_add_oid_set_member and gss_release_oid_set modify the guest's set variable (RFC 2744 "modify"):
 *     the native set behind it is updated or released and the shadow follows;
 *   - the gss_iter_creds callbacks receive shadows (the iteration is synchronous: Heimdal's gss_iter_creds_f
 *     calls back before it returns).
 */
#include "hao/runtime.h"
#include "hao/gen_stamp.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t length, pad; uint64_t elements; } goid;                       /* arm64 */
typedef struct __attribute__((packed, aligned(2))) { uint32_t length; uint64_t elements; } noid;   /* x86_64 */
typedef struct { uint64_t count, elements; } oset;          /* both; elements: goid[] or noid[] */
typedef struct { uint64_t length, value; } gbuf;
typedef struct { uint32_t ia, p0; gbuf iaddr; uint32_t aa, p1; gbuf aaddr, appdata; } gcb;
typedef struct __attribute__((packed, aligned(2))) { uint32_t ia; gbuf iaddr; uint32_t aa; gbuf aaddr, appdata; } ncb;
_Static_assert(sizeof(noid) == 12 && sizeof(goid) == 16, "gss_OID_desc layouts");
_Static_assert(sizeof(ncb) == 56 && sizeof(gcb) == 64, "gss_channel_bindings_struct layouts");

static const struct { const char *name, *kinds; char ret; } funcs[] = {
#define HAO_GSS_FUNCS
#include "gss_gen.inc"
#undef HAO_GSS_FUNCS
};
static const struct { const char *name; char kind; } vars[] = {
#define HAO_GSS_DATA
#include "gss_gen.inc"
#undef HAO_GSS_DATA
};
static void *native_fn[sizeof funcs / sizeof *funcs];

/* ---- pointer maps (open addressing, never shrink; a removed key leaves a tombstone) */
typedef struct { uint64_t *k, *v; size_t cap, n; } pmap;
#define TOMB 1ull

static uint64_t *pm_slot(pmap *m, uint64_t key, bool insert)
{
    if (insert && (m->n + 1) * 2 > m->cap) {
        pmap o = *m;
        m->cap = o.cap ? o.cap * 2 : 64;
        m->k = hao_alloc_check(calloc(m->cap, 8), "GSS-API map"); m->v = hao_alloc_check(calloc(m->cap, 8), "GSS-API map"); m->n = 0;
        for (size_t i = 0; i < o.cap; i++)
            if (o.k[i] > TOMB) { *pm_slot(m, o.k[i], true) = o.v[i]; }
        free(o.k); free(o.v);
    }
    if (!m->cap) return NULL;
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & (m->cap - 1), tomb = (size_t)-1;
    for (;; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == key) return &m->v[i];
        if (m->k[i] == TOMB && tomb == (size_t)-1) tomb = i;
        if (!m->k[i]) {
            if (!insert) return NULL;
            if (tomb != (size_t)-1) i = tomb;
            m->k[i] = key; m->v[i] = 0; m->n++;
            return &m->v[i];
        }
    }
}
static uint64_t pm_get(pmap *m, uint64_t key) { uint64_t *s = pm_slot(m, key, false); return s ? *s : 0; }
static void pm_del(pmap *m, uint64_t key)
{
    uint64_t *s = pm_slot(m, key, false);
    if (s) { m->k[s - m->v] = TOMB; *s = 0; }
}

static pmap oid_n2g, oid_g2n, set_n2g, set_g2n;
static struct interned { struct interned *next; noid *n; } *interned;
static pthread_mutex_t gss_lock = PTHREAD_MUTEX_INITIALIZER;
static void L(void) { hao_crit_enter(); pthread_mutex_lock(&gss_lock); }
static void U(void) { pthread_mutex_unlock(&gss_lock); hao_crit_leave(); }
void hao_gss_fork_child(void) { gss_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

/* ---- OIDs */
static uint64_t oid_to_guest_l(const noid *n)
{
    if (!n) return 0;
    goid *g = (goid *)(uintptr_t)pm_get(&oid_n2g, (uint64_t)(uintptr_t)n);
    if (!g) {
        g = hao_alloc_check(calloc(1, sizeof *g), "GSS-API OIDs");
        *pm_slot(&oid_n2g, (uint64_t)(uintptr_t)n, true) = (uint64_t)(uintptr_t)g;
        *pm_slot(&oid_g2n, (uint64_t)(uintptr_t)g, true) = (uint64_t)(uintptr_t)n;
    }
    g->length = n->length;
    g->elements = n->elements;
    return (uint64_t)(uintptr_t)g;
}

static const noid *oid_to_native_l(uint64_t ga)
{
    if (!ga) return NULL;
    uint64_t n = pm_get(&oid_g2n, ga);
    if (n) return (const noid *)(uintptr_t)n;
    const goid *g = (const goid *)(uintptr_t)ga;
    const void *bytes = (const void *)(uintptr_t)g->elements;
    for (struct interned *i = interned; i; i = i->next)
        if (i->n->length == g->length && (!g->length || !memcmp((const void *)(uintptr_t)i->n->elements, bytes, g->length)))
            return i->n;
    struct interned *i = hao_alloc_check(malloc(sizeof *i), "GSS-API OIDs");
    i->n = hao_alloc_check(malloc(sizeof *i->n), "GSS-API OIDs");
    void *copy = hao_alloc_check(malloc(g->length ? g->length : 1), "GSS-API OIDs");
    if (g->length) memcpy(copy, bytes, g->length);
    i->n->length = g->length;
    i->n->elements = (uint64_t)(uintptr_t)copy;
    i->next = interned;
    interned = i;
    return i->n;
}

/* ---- OID sets */
static uint64_t set_to_guest_l(const oset *n)
{
    if (!n) return 0;
    oset *g = (oset *)(uintptr_t)pm_get(&set_n2g, (uint64_t)(uintptr_t)n);
    if (!g) {
        g = hao_alloc_check(calloc(1, sizeof *g), "GSS-API OID sets");
        *pm_slot(&set_n2g, (uint64_t)(uintptr_t)n, true) = (uint64_t)(uintptr_t)g;
        *pm_slot(&set_g2n, (uint64_t)(uintptr_t)g, true) = (uint64_t)(uintptr_t)n;
    }
    goid *ge = hao_alloc_check(realloc((void *)(uintptr_t)g->elements, (n->count ? n->count : 1) * sizeof *ge), "GSS-API OID sets");
    const noid *ne = (const noid *)(uintptr_t)n->elements;
    for (uint64_t k = 0; k < n->count; k++) ge[k] = (goid){ne[k].length, 0, ne[k].elements};
    g->count = n->count;
    g->elements = (uint64_t)(uintptr_t)ge;
    return (uint64_t)(uintptr_t)g;
}

static void set_drop_l(uint64_t ga)
{
    uint64_t n = pm_get(&set_g2n, ga);
    if (!n) return;
    pm_del(&set_g2n, ga);
    pm_del(&set_n2g, n);
    oset *g = (oset *)(uintptr_t)ga;
    free((void *)(uintptr_t)g->elements);
    free(g);
}

/* A guest-built set as a temporary native one (freed by the caller), or the native set behind a shadow. */
static oset *set_to_native_l(uint64_t ga, bool *tmp)
{
    *tmp = false;
    if (!ga) return NULL;
    uint64_t n = pm_get(&set_g2n, ga);
    if (n) return (oset *)(uintptr_t)n;
    const oset *g = (const oset *)(uintptr_t)ga;
    oset *t = hao_alloc_check(malloc(sizeof *t), "GSS-API OID sets");
    noid *e = hao_alloc_check(malloc((g->count ? g->count : 1) * sizeof *e), "GSS-API OID sets");
    const goid *ge = (const goid *)(uintptr_t)g->elements;
    for (uint64_t k = 0; k < g->count; k++) e[k] = *oid_to_native_l((uint64_t)(uintptr_t)&ge[k]);
    t->count = g->count;
    t->elements = (uint64_t)(uintptr_t)e;
    *tmp = true;
    return t;
}

static void tmp_free(oset *t) { free((void *)(uintptr_t)t->elements); free(t); }

/* ---- callbacks of gss_iter_creds(_f) */
typedef struct { uint64_t fn, ctx; bool block; } iter_ctx;
static void iter_tramp(void *c, const noid *oid, void *cred)
{
    iter_ctx *x = c;
    L(); uint64_t g = oid_to_guest_l(oid); U();
    uint64_t a[3];
    int n = 0;
    a[n++] = x->ctx;   /* the guest's context, or its block */
    a[n++] = g;
    a[n++] = (uint64_t)(uintptr_t)cred;
    hao_call_guest(hao_current(), x->fn, a, n);
}

/* guest function at a (plain or thunked entry), else 0 */
static uint64_t guest_fn(uint64_t a)
{
    if (!a) return 0;
    uint64_t g = hao_guest_of_entry(a);
    if (g) return g;
    return hao_image_for_code(a) ? a : 0;
}

/* ---- the trap */
typedef uint64_t (*fn13)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void trap_gss(hao_thread *t, uint64_t idx)
{
    hao_cpu *c = &t->cpu;
    const char *kinds = funcs[idx].kinds;
    int n = (int)strlen(kinds);
    uint64_t a[16] = {0}, local[16] = {0}, old[16] = {0};
    oset *tmpset[16] = {0};
    ncb cbs[16];
    iter_ctx ictx[2];
    int ni = 0;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < n; k++) a[k] = k < 8 ? c->x[k] : sp[k - 8];   /* stack parameters are 8 bytes (gss_gen.inc) */
    void *fn = native_fn[idx];
    L();
    for (int k = 0; k < n; k++) {
        uint64_t *p = (uint64_t *)(uintptr_t)a[k];
        bool tmp;
        switch (kinds[k]) {
        case 'o': a[k] = (uint64_t)(uintptr_t)oid_to_native_l(a[k]); break;
        case 'O': case 'S': if (p) a[k] = (uint64_t)(uintptr_t)&local[k]; break;
        case 'M':
            if (!p) break;
            old[k] = *p;
            local[k] = (uint64_t)(uintptr_t)oid_to_native_l(*p);
            a[k] = (uint64_t)(uintptr_t)&local[k];
            break;
        case 's':
            a[k] = (uint64_t)(uintptr_t)set_to_native_l(a[k], &tmp);
            if (tmp) tmpset[k] = (oset *)(uintptr_t)a[k];
            break;
        case 'T': {
            if (!p) break;
            old[k] = *p;
            oset *ns = set_to_native_l(*p, &tmp);
            if (tmp) {
                /* a set the guest built: a native one made by the library itself, so it can grow and be released */
                static uint32_t (*create)(uint32_t *, oset **), (*add)(uint32_t *, const noid *, oset **);
                if (!create) { create = dlsym(RTLD_DEFAULT, "gss_create_empty_oid_set"); add = dlsym(RTLD_DEFAULT, "gss_add_oid_set_member"); }
                oset *made = NULL;
                uint32_t minor;
                if (create && add && create(&minor, &made) == 0)
                    for (uint64_t e = 0; e < ns->count; e++) add(&minor, &((const noid *)(uintptr_t)ns->elements)[e], &made);
                tmp_free(ns);
                ns = made;
            }
            local[k] = (uint64_t)(uintptr_t)ns;
            a[k] = (uint64_t)(uintptr_t)&local[k];
            break;
        }
        case 'c':
            if (a[k]) {
                const gcb *g = (const gcb *)(uintptr_t)a[k];
                cbs[k] = (ncb){g->ia, g->iaddr, g->aa, g->aaddr, g->appdata};
                a[k] = (uint64_t)(uintptr_t)&cbs[k];
            }
            break;
        case 'f': {
            uint64_t g = guest_fn(a[k]);
            if (g && ni < 2) {
                ictx[ni] = (iter_ctx){g, a[k - 1], false};
                a[k - 1] = (uint64_t)(uintptr_t)&ictx[ni++];
                a[k] = (uint64_t)(uintptr_t)iter_tramp;
            }
            break;
        }
        case 'b': {
            /* a guest block: call gss_iter_creds_f with a trampoline that invokes it */
            uint64_t g = a[k] ? guest_fn(((const uint64_t *)(uintptr_t)a[k])[2]) : 0;
            static void *iter_f;
            if (!iter_f) iter_f = dlsym(RTLD_DEFAULT, "gss_iter_creds_f");
            if (g && iter_f && ni < 2) {
                ictx[ni] = (iter_ctx){g, a[k], true};
                a[k] = (uint64_t)(uintptr_t)&ictx[ni++];
                a[k + 1] = (uint64_t)(uintptr_t)iter_tramp;
                fn = iter_f;
                n++;
            }
            break;
        }
        default: break;
        }
    }
    U();
    uint64_t r = ((fn13)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    L();
    for (int k = 0; kinds[k]; k++) {
        uint64_t *p = (uint64_t *)(uintptr_t)(k < 8 ? c->x[k] : sp[k - 8]);
        switch (kinds[k]) {
        case 'O': if (p) *p = oid_to_guest_l((const noid *)(uintptr_t)local[k]); break;
        case 'M':
            if (!p) break;
            if (!local[k]) {
                uint64_t nat = pm_get(&oid_g2n, old[k]);
                if (nat) { pm_del(&oid_g2n, old[k]); pm_del(&oid_n2g, nat); free((void *)(uintptr_t)old[k]); }
                *p = 0;
            } else *p = oid_to_guest_l((const noid *)(uintptr_t)local[k]);
            break;
        case 'S': if (p) *p = set_to_guest_l((const oset *)(uintptr_t)local[k]); break;
        case 'T':
            if (!p) break;
            if (old[k] && pm_get(&set_g2n, old[k]) != local[k]) set_drop_l(old[k]);
            *p = local[k] ? set_to_guest_l((const oset *)(uintptr_t)local[k]) : 0;
            break;
        case 's': if (tmpset[k]) tmp_free(tmpset[k]); break;
        default: break;
        }
    }
    switch (funcs[idx].ret) {
    case 'u': case 'i': r = (uint32_t)r; break;
    case 'o': r = oid_to_guest_l((const noid *)(uintptr_t)r); break;
    default: break;
    }
    U();
    c->x[0] = r;
}

/* ---- import binding */
static int name_cmp(const void *key, const void *ent) { return strcmp(key, *(const char *const *)ent); }

static hao_name_index funcs_ix = HAO_NAME_INDEX(funcs, NULL);

void hao_gss_interpose_names(hao_name_fn cb, void *ctx) { hao_name_each(&funcs_ix, cb, ctx); }

bool hao_gss_interpose(const char *name, uint64_t *addr)
{
    if (strncasecmp(name, "_gss", 4) && strncmp(name, "___gss", 6) && strncmp(name, "_krb5_gss", 9)) return false;
    long fk = hao_name_find(&funcs_ix, name);
    if (fk >= 0) {
        size_t k = (size_t)fk;
        void *nf = dlsym(RTLD_DEFAULT, name + 1);
        if (!nf) return false;
        native_fn[k] = nf;
        *addr = hao_trap_address(name, trap_gss, k);
        return true;
    }
    const void *v = bsearch(name, vars, sizeof vars / sizeof *vars, sizeof *vars, name_cmp);
    if (!v) return false;
    char kind = ((const typeof(vars[0]) *)v)->kind;
    void *nv = dlsym(RTLD_DEFAULT, name + 1);
    if (!nv) return false;
    L();
    switch (kind) {
    case 'D': *addr = oid_to_guest_l(nv); break;
    case 'P': case 'Q': {
        uint64_t *slot = hao_alloc_check(malloc(8), "GSS-API OID variables");
        *slot = kind == 'P' ? oid_to_guest_l(*(const noid *const *)nv) : set_to_guest_l(*(const oset *const *)nv);
        *addr = (uint64_t)(uintptr_t)slot;
        break;
    }
    case 'A': {
        const noid *ne = nv;
        int cnt = 0;
        while (cnt < 64 && ne[cnt].length) cnt++;
        goid *ga = hao_alloc_check(calloc((size_t)cnt + 1, sizeof *ga), "GSS-API OID variables");
        for (int k = 0; k < cnt; k++) {
            ga[k] = (goid){ne[k].length, 0, ne[k].elements};
            *pm_slot(&oid_g2n, (uint64_t)(uintptr_t)&ga[k], true) = (uint64_t)(uintptr_t)&ne[k];
        }
        *addr = (uint64_t)(uintptr_t)ga;
        break;
    }
    }
    U();
    HAO_LOG(2, "GSS data %s (%c) bound to an arm64-layout copy\n", name, kind);
    return true;
}
