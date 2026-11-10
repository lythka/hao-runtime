/* Host test: hao's growable maps (src/gmap.c, include/hao/gmap.h), which
 * replaced libhao's fixed worst-case tables to keep per-process memory
 * within jetsam limits.
 *
 *   capacity  a one-word-key map fills to its capacity (max_bits, load) and
 *             every key reads back its value; one more key is refused
 *   pairs     two-word keys (guest, sig): pairs that share a word are
 *             distinct entries
 *   readers   4 threads look up keys already published while the main
 *             thread inserts the rest under a lock (the map grows 12 times
 *             under them): a published key must never read back missing or
 *             with another value
 *   scale     a map of N entries maps O(N) bytes (old tables included), and
 *             its first table is one page
 *   pages     keys one 4 KB page apart (functions of many images) dirty
 *             O(N) pages, not one page each
 *   fork      a child forked while another thread inserts sees every key
 *             published before the fork, and can insert and grow
 *
 *   test_gmap     prints one line per check; exit 0 only if all pass.
 */
#include "hao/gmap.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static void check(const char *name, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void check(const char *name, int ok, const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    printf("%-9s %s  %s\n", name, ok ? "PASS" : "FAIL", buf);
    if (!ok) fails++;
}

static bool put1(hao_gmap *m, uint64_t k, uint64_t v)
{
    bool fresh;
    uint64_t *e = hao_gmap_claim1(m, k, &fresh);
    if (!e) return false;
    if (fresh) { e[1] = v; hao_gmap_publish1(m, e, k); }
    return true;
}

static uint64_t key_of(uint32_t k) { return 0x100000000ull + 4096ull * k; }
static uint64_t val_of(uint32_t k) { return 0xdead0000ull ^ (uint64_t)k * 0x9E3779B97F4A7C15ull; }

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static hao_gmap rmap = HAO_GMAP_INIT(2, 1, 8, 22);
static volatile uint32_t published;
static volatile int stop;
static uint64_t bad_reads, reads;

static void *reader(void *arg)
{
    (void)arg;
    uint64_t n = 0, bad = 0, x = 88172645463325252ull;
    while (!stop) {
        uint32_t p = published;
        if (!p) continue;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t k = (uint32_t)(x % p);
        const uint64_t *e = hao_gmap_get1(&rmap, key_of(k));
        if (!e || e[1] != val_of(k)) bad++;
        n++;
    }
    __atomic_fetch_add(&reads, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&bad_reads, bad, __ATOMIC_RELAXED);
    return NULL;
}

/* resident pages of the map's current and old tables */
static size_t resident_pages(const hao_gmap *m)
{
    size_t n = 0;
    for (const hao_gmap_tab *t = m->tab; t; t = t->prev) {
        size_t bytes = sizeof *t + ((size_t)m->words << t->bits) * 8, pages = (bytes + 4095) / 4096;
        char *vec = malloc(pages + 1);
        uintptr_t a = (uintptr_t)t & ~(uintptr_t)4095;
        if (vec && mincore((void *)a, pages * 4096, vec) == 0)
            for (size_t k = 0; k < pages; k++) n += vec[k] & 1;
        free(vec);
    }
    return n;
}

static void *forker_inserts(void *arg)
{
    hao_gmap *m = arg;
    for (uint32_t k = 100000; k < 400000 && !stop; k++) {
        pthread_mutex_lock(&lock);
        put1(m, key_of(k), val_of(k));
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(void)
{
    /* capacity */
    {
        hao_gmap m = HAO_GMAP_INIT(2, 1, 4, 12);
        uint32_t cap = hao_gmap_capacity(&m), n = 0, bad = 0;
        while (put1(&m, key_of(n), val_of(n))) n++;
        for (uint32_t k = 0; k < n; k++) {
            const uint64_t *e = hao_gmap_get1(&m, key_of(k));
            if (!e || e[1] != val_of(k)) bad++;
        }
        bool fresh;
        int again = hao_gmap_claim1(&m, key_of(0), &fresh) != NULL && !fresh;   /* an existing key is still found */
        check("capacity", n == cap && cap == 3072 && !bad && again && !hao_gmap_get1(&m, key_of(n)),
              "%u of capacity %u inserted, %u wrong, full map still finds old keys: %d", n, cap, bad, again);
        hao_gmap z = HAO_GMAP_INIT(2, 1, 4, 12);
        check("zero-key", !hao_gmap_claim1(&z, 0, &fresh) && !hao_gmap_get1(&z, 0), "key 0 is refused");
    }
    /* pairs */
    {
        hao_gmap m = HAO_GMAP_INIT(3, 2, 4, 16);
        uint32_t bad = 0;
        for (uint64_t g = 1; g <= 200; g++)
            for (uint64_t s = 1; s <= 20; s++) {
                uint64_t key[2] = {g << 12, s << 4};
                bool fresh;
                uint64_t *e = hao_gmap_claim(&m, key, &fresh);
                if (!e || !fresh) { bad++; continue; }
                e[2] = g * 1000 + s;
                hao_gmap_publish(&m, e, key);
            }
        for (uint64_t g = 1; g <= 200; g++)
            for (uint64_t s = 1; s <= 20; s++) {
                uint64_t key[2] = {g << 12, s << 4};
                const uint64_t *e = hao_gmap_get(&m, key);
                if (!e || e[2] != g * 1000 + s) bad++;
            }
        uint64_t miss[2] = {1 << 12, 21 << 4};
        check("pairs", !bad && hao_gmap_count(&m) == 4000 && !hao_gmap_get(&m, miss), "4000 (guest, sig) pairs, %u wrong", bad);
    }
    /* readers */
    {
        enum { N = 1u << 20 };
        pthread_t th[4];
        for (int k = 0; k < 4; k++) pthread_create(&th[k], NULL, reader, NULL);
        for (uint32_t k = 0; k < N; k++) {
            pthread_mutex_lock(&lock);
            put1(&rmap, key_of(k), val_of(k));
            pthread_mutex_unlock(&lock);
            __atomic_store_n(&published, k + 1, __ATOMIC_RELEASE);
        }
        stop = 1;
        for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);
        stop = 0;
        check("readers", !bad_reads && reads > 0 && rmap.grows >= 12, "%llu lookups during %u growths, %llu wrong or missing",
              (unsigned long long)reads, rmap.grows, (unsigned long long)bad_reads);
    }
    /* scale and pages */
    {
        hao_gmap small = HAO_GMAP_INIT(2, 1, 8, 19);
        put1(&small, key_of(1), 1);
        uint64_t first = small.bytes;
        hao_gmap m = HAO_GMAP_INIT(2, 1, 8, 19);
        for (uint32_t k = 0; k < 4000; k++) put1(&m, key_of(k), val_of(k));
        /* 4000 entries: current table 8192 slots x 16 B = 128 KB, the old ones as much again */
        check("scale", first <= 4096 + 64 && m.bytes <= 2 * (8192 * 16 + 64) + 8 * 64,
              "first table %llu bytes; 4000 entries map %llu KB (a fixed table for 2^18 thunks: 4096 KB)",
              (unsigned long long)first, (unsigned long long)(m.bytes >> 10));
        size_t pages = resident_pages(&m);
        check("pages", pages <= 2 * (8192 * 16 / 4096) + 16, "4000 keys a page apart: %zu resident pages", pages);
    }
    /* fork */
    {
        hao_gmap *m = mmap(NULL, sizeof *m, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        *m = (hao_gmap)HAO_GMAP_INIT(2, 1, 8, 22);
        for (uint32_t k = 0; k < 50000; k++) put1(m, key_of(k), val_of(k));
        pthread_t t;
        pthread_create(&t, NULL, forker_inserts, m);
        int children = 0, bad = 0;
        for (int f = 0; f < 50; f++) {
            usleep(1000);
            pid_t p = fork();
            if (p == 0) {
                lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;   /* as the *_fork_child hooks do */
                for (uint32_t k = 0; k < 50000; k++) {
                    const uint64_t *e = hao_gmap_get1(m, key_of(k));
                    if (!e || e[1] != val_of(k)) _exit(1);
                }
                for (uint32_t k = 1000000; k < 1100000; k++) {
                    pthread_mutex_lock(&lock);
                    bool ok = put1(m, key_of(k), val_of(k));
                    pthread_mutex_unlock(&lock);
                    if (!ok) _exit(2);
                }
                const uint64_t *e = hao_gmap_get1(m, key_of(1099999));
                _exit(e && e[1] == val_of(1099999) ? 0 : 3);
            }
            if (p < 0) { bad++; continue; }
            int st;
            waitpid(p, &st, 0);
            children++;
            if (!WIFEXITED(st) || WEXITSTATUS(st)) bad++;
        }
        stop = 1;
        pthread_join(t, NULL);
        check("fork", children == 50 && !bad, "%d children, %d saw a missing key or could not insert", children, bad);
    }
    return fails ? 1 : 0;
}
