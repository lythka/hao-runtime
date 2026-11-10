/*
 * hao — growable open-addressed maps (include/hao/gmap.h): tables that start
 * small and grow by replacement into mmap'd memory, with lock-free reads.
 */
#include "hao/gmap.h"
#include <string.h>
#include <sys/mman.h>

static uint64_t total_mapped;
static uint32_t total_grows;

static size_t tab_bytes(const hao_gmap *m, unsigned bits)
{
    return sizeof(hao_gmap_tab) + ((size_t)m->words << bits) * sizeof(uint64_t);
}

static uint32_t tab_limit(const hao_gmap *m, unsigned bits)
{
    uint32_t load = m->load ? m->load : 768;
    if (load > 1008) load = 1008;              /* a free slot always ends a probe */
    return (uint32_t)(((uint64_t)1 << bits) * load / 1024);
}

uint32_t hao_gmap_capacity(const hao_gmap *m) { return tab_limit(m, m->max_bits); }

static hao_gmap_tab *tab_new(hao_gmap *m, unsigned bits)
{
    size_t n = tab_bytes(m, bits);
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    hao_gmap_tab *t = p;
    t->bits = bits;
    t->mask = (uint32_t)(((uint64_t)1 << bits) - 1);
    /* below the largest size, grow at 3/4 at most: probes stay short */
    t->limit = tab_limit(m, bits);
    if (bits < m->max_bits && t->limit > (t->mask + 1) / 4 * 3) t->limit = (t->mask + 1) / 4 * 3;
    __atomic_fetch_add(&m->bytes, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&total_mapped, n, __ATOMIC_RELAXED);
    return t;
}

/* the entry for key, or the free slot that ends its probe */
static uint64_t *tab_find(const hao_gmap *m, hao_gmap_tab *t, const uint64_t *key)
{
    unsigned w = m->words, keys = m->keys;
    for (uint32_t h = hao_gmap_hash(t, key, keys);; h = (h + 1) & t->mask) {
        uint64_t *e = &t->e[(size_t)h * w];
        if (!e[0] || (e[0] == key[0] && (keys < 2 || e[1] == key[1]))) return e;
    }
}

/* under the writer's lock: a larger copy of the current table, published */
static hao_gmap_tab *grow(hao_gmap *m, hao_gmap_tab *t)
{
    unsigned bits = t ? t->bits + 1 : m->min_bits;
    if (bits > m->max_bits) return NULL;
    hao_gmap_tab *n = tab_new(m, bits);
    if (!n) return NULL;
    if (t) {
        unsigned w = m->words;
        for (uint32_t k = 0; k <= t->mask; k++) {
            const uint64_t *e = &t->e[(size_t)k * w];
            if (!e[0]) continue;
            memcpy(tab_find(m, n, e), e, w * sizeof(uint64_t));
            n->used++;
        }
        n->prev = t;
        m->grows++;
        __atomic_fetch_add(&total_grows, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&m->tab, n, __ATOMIC_RELEASE);
    return n;
}

uint64_t *hao_gmap_claim(hao_gmap *m, const uint64_t *key, bool *fresh)
{
    *fresh = false;
    if (!key[0]) return NULL;
    hao_gmap_tab *t = m->tab;
    if (!t && !(t = grow(m, NULL))) return NULL;
    uint64_t *e = tab_find(m, t, key);
    if (e[0]) return e;
    if (t->used >= t->limit) {
        if (!(t = grow(m, t))) return NULL;
        e = tab_find(m, t, key);
    }
    *fresh = true;
    return e;
}

void hao_gmap_publish(hao_gmap *m, uint64_t *slot, const uint64_t *key)
{
    hao_gmap_tab *t = m->tab;
    for (unsigned k = 1; k < m->keys; k++) slot[k] = key[k];
    __atomic_store_n(&slot[0], key[0], __ATOMIC_RELEASE);
    __atomic_store_n(&t->used, t->used + 1, __ATOMIC_RELAXED);
}

void hao_gmap_totals(uint64_t *mapped, uint32_t *grows)
{
    *mapped = __atomic_load_n(&total_mapped, __ATOMIC_RELAXED);
    *grows = __atomic_load_n(&total_grows, __ATOMIC_RELAXED);
}
