/*
 * hao — growable open-addressed maps for hao's per-process tables (src/gmap.c).
 *
 * hao's lookup tables (thunks, signature thunks, traps, block invokes, ObjC
 * class sets, Swift objects, ...) used to be fixed arrays in libhao's
 * __DATA sized for the worst case. A hashed insert dirties a whole page, so
 * a few thousand entries dirtied most of 13 MB, and a small daemon under hao
 * no longer fit a 15 MB jetsam limit. A hao_gmap starts at one page and grows by replacement, as the
 * bridge plan table does (src/bridge.c):
 *
 *  - Reads take no lock and allocate nothing (hao_gmap_get): signal handlers
 *    on native threads that hold the malloc locks may call them. A reader
 *    loads the current table once and probes it; a table it is still
 *    probing stays mapped after it is replaced (old copies are never
 *    unmapped, at most as many bytes again as the current table).
 *  - Writers serialise on the caller's own lock (inside a hao critical
 *    section, as before). hao_gmap_claim returns the entry for a key, or a
 *    free slot of the current table, growing it first when it is full; the
 *    caller fills the value words and hao_gmap_publish stores the key words,
 *    the first one last with a release store. A writer that fork cuts off
 *    leaves at most an unused slot, or an unpublished larger copy.
 *  - Memory comes from mmap, never malloc. A table that cannot grow (at
 *    max_bits, or mmap failed) makes hao_gmap_claim return NULL: the caller
 *    keeps its old "table full" behaviour.
 *
 * An entry is `words` uint64_t, of which the first `keys` (1 or 2) are the
 * key; key word 0 must be nonzero (0 marks a free slot). A reader sees an
 * entry's value words complete once its key word 0 is visible. Value words
 * may be rewritten in place only where every reader holds the writer's lock.
 */
#ifndef HAO_GMAP_H
#define HAO_GMAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hao_gmap_tab {
    struct hao_gmap_tab *prev;   /* the table this one replaced (still mapped) */
    uint32_t bits, mask, used, limit;
    uint64_t e[];                /* (mask + 1) * words */
} hao_gmap_tab;

typedef struct {
    hao_gmap_tab *tab;           /* current table; atomic */
    uint8_t words, keys;         /* entry size and key words, in uint64_t */
    uint8_t min_bits, max_bits;  /* first and largest table: 2^bits slots */
    uint16_t load;               /* fill limit in 1/1024 of the slots (0: 768) */
    uint64_t bytes;              /* mapped by all tables, current and old */
    uint32_t grows;
} hao_gmap;

/* words, keys, min_bits, max_bits; the largest table holds load/1024 of 2^max_bits */
#define HAO_GMAP_INIT(w, k, lo, hi) {NULL, (w), (k), (lo), (hi), 0, 0, 0}
#define HAO_GMAP_INIT_LOAD(w, k, lo, hi, ld) {NULL, (w), (k), (lo), (hi), (ld), 0, 0}

static inline uint32_t hao_gmap_hash(const hao_gmap_tab *t, const uint64_t *key, unsigned keys)
{
    uint64_t x = key[0] * 0x9E3779B97F4A7C15ull;
    if (keys > 1) x ^= key[1] * 0xC2B2AE3D27D4EB4Full;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    return (uint32_t)(x >> (64 - t->bits)) & t->mask;
}

/* Lock-free lookup: the entry whose key words equal key[0..keys), or NULL. */
static inline const uint64_t *hao_gmap_get(const hao_gmap *m, const uint64_t *key)
{
    const hao_gmap_tab *t = __atomic_load_n(&m->tab, __ATOMIC_ACQUIRE);
    if (!t || !key[0]) return NULL;
    unsigned w = m->words, keys = m->keys;
    for (uint32_t h = hao_gmap_hash(t, key, keys), n = 0; n <= t->mask; n++, h = (h + 1) & t->mask) {
        const uint64_t *e = &t->e[(size_t)h * w];
        uint64_t k0 = __atomic_load_n(&e[0], __ATOMIC_ACQUIRE);
        if (!k0) return NULL;
        if (k0 == key[0] && (keys < 2 || e[1] == key[1])) return e;
    }
    return NULL;
}

static inline const uint64_t *hao_gmap_get1(const hao_gmap *m, uint64_t key) { return hao_gmap_get(m, &key); }

/* Writer, under the caller's lock: the entry for `key` (*fresh = false), or
 * a free slot for it in the current table (*fresh = true), which the caller
 * fills (words keys..words-1) and publishes. NULL: the map is full and
 * cannot grow. */
uint64_t *hao_gmap_claim(hao_gmap *m, const uint64_t *key, bool *fresh);
/* Writer: make a claimed fresh slot visible with its key. */
void hao_gmap_publish(hao_gmap *m, uint64_t *slot, const uint64_t *key);
/* entries in the current table */
static inline uint32_t hao_gmap_count(const hao_gmap *m)
{
    const hao_gmap_tab *t = __atomic_load_n(&m->tab, __ATOMIC_ACQUIRE);
    return t ? __atomic_load_n(&t->used, __ATOMIC_RELAXED) : 0;
}
/* the largest number of entries the map can hold */
uint32_t hao_gmap_capacity(const hao_gmap *m);

/* one-word-key convenience forms */
static inline uint64_t *hao_gmap_claim1(hao_gmap *m, uint64_t key, bool *fresh) { return hao_gmap_claim(m, &key, fresh); }
static inline void hao_gmap_publish1(hao_gmap *m, uint64_t *slot, uint64_t key) { hao_gmap_publish(m, slot, &key); }

/* bytes mapped by every map's tables and the number of growths, for --stats */
void hao_gmap_totals(uint64_t *mapped, uint32_t *grows);

#endif
