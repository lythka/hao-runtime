/* Host test: the bridge plan table (src/bridge.c) against libhao.
 *
 *   capacity  N (default 40000) distinct native entry points get typed
 *             plans and every one reads back with its own kind. The fixed
 *             native signature set this table replaced kept 3072 entries
 *             and dropped the rest to the register bridge.
 *   readers   4 threads look up entry points already published while the
 *             main thread inserts the rest (the table grows under them):
 *             a published entry must never read back missing or wrong.
 *   upgrade   an UNKNOWN entry point becomes TYPED when a source types it
 *             later (a native block's signature); a typed plan is kept.
 *
 *   test_bridge [N]     prints one line per check; exit 0 only if all pass.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static volatile uint32_t published;    /* entry points [0, published) are in the table */
static volatile int stop;
static uint64_t bad_reads, reads;

static uint64_t addr_of(uint32_t k) { return 0x7ff800000000ull + 16ull * k + 16; }
static char kind_of(uint32_t k) { static const char ks[] = "BcCsS"; return ks[k % 5]; }

static void *reader(void *arg)
{
    (void)arg;
    uint64_t n = 0, bad = 0, x = 88172645463325252ull;
    while (!stop) {
        uint32_t p = published;
        if (!p) continue;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t k = (uint32_t)(x % p);
        const hao_plan *pl = hao_bridge_lookup(addr_of(k));
        if (!pl || pl->kind != kind_of(k) || pl->native != addr_of(k)) bad++;
        n++;
    }
    __atomic_fetch_add(&bad_reads, bad, __ATOMIC_RELAXED);
    __atomic_fetch_add(&reads, n, __ATOMIC_RELAXED);
    return NULL;
}

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 40000;
    hao_opts_init(NULL);
    hao_bridge_init();
    int fail = 0;

    pthread_t th[4];
    for (int k = 0; k < 4; k++) pthread_create(&th[k], NULL, reader, NULL);
    for (uint32_t k = 0; k < n; k++) {
        hao_bridge_note(addr_of(k), kind_of(k), NULL, HAO_BRS_BLOCKSIG);
        __atomic_store_n(&published, k + 1, __ATOMIC_RELEASE);
    }
    stop = 1;
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);

    uint32_t found = 0;
    for (uint32_t k = 0; k < n; k++) {
        const hao_plan *pl = hao_bridge_lookup(addr_of(k));
        if (pl && pl->kind == kind_of(k) && pl->cls == HAO_BR_TYPED) found++;
    }
    const hao_plan_table *t = hao_plan_tab;
    printf("capacity   %s  %u of %u typed entry points read back (table %u slots)\n", found == n ? "PASS" : "FAIL", found, n,
           t ? t->mask + 1 : 0);
    if (found != n) fail = 1;
    printf("readers    %s  %llu lookups during growth, %llu wrong or missing\n", bad_reads ? "FAIL" : "PASS",
           (unsigned long long)reads, (unsigned long long)bad_reads);
    if (bad_reads || !reads) fail = 1;

    /* upgrade: bind an unknown import, then type it; then try to retype it */
    uint64_t a = 0x7ff900000010ull;
    int c0 = hao_bridge_bind("_hao_test_no_such_symbol", "_hao_test_no_such_symbol", a);
    const hao_plan *p0 = hao_bridge_lookup(a);
    hao_bridge_note(a, 'B', NULL, HAO_BRS_BLOCKSIG);
    const hao_plan *p1 = hao_bridge_lookup(a);
    hao_bridge_note(a, 's', NULL, HAO_BRS_BLOCKSIG);
    const hao_plan *p2 = hao_bridge_lookup(a);
    bool up = c0 == HAO_BR_UNKNOWN && p0 && p0->cls == HAO_BR_UNKNOWN && p1 && p1->cls == HAO_BR_TYPED && p1->kind == 'B' &&
              p2 == p1;
    printf("upgrade    %s  UNKNOWN -> TYPED 'B', kept against a second note\n", up ? "PASS" : "FAIL");
    if (!up) fail = 1;
    return fail;
}
