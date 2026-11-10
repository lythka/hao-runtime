/* Host test: block chaining rewrites the rel32 of a block's exit jmp while
 * other threads may be executing it, so that field must be 4-aligned (one
 * store that never crosses an 8-byte or cache-line boundary).
 *
 *   test_chain sites [N]     N guest functions whose exits land at varied
 *                            x86 offsets; every chain site (the rel32 the
 *                            exit stub reports in host_scratch[0]) must be
 *                            4-aligned, and every one must get chained
 *   test_chain race [N] [T]  T threads run the same N fresh functions at
 *                            once, so first exits are chained while other
 *                            threads execute them; every result must be right
 * Each prints one summary line ending in PASS or FAIL (exit 0 on PASS) and
 * then the hao-jit statistics line on stderr. */
#include "hao/runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FW = 16 };   /* words per function */

/* Function i:
 *   k fillers of four kinds (k = i % 10), of different x86 lengths
 *   cmp x0, #adds ; b.eq +8 ; brk #0
 *   add x0, x0, #100 ; b +8 ; brk #0
 *   ret
 * returns adds + 100, where adds counts the "add x0, x0, #1" fillers. */
static uint32_t *make_fns(int n, uint64_t *want)
{
    uint32_t *g = aligned_alloc(16384, ((size_t)n * FW * 4 + 16383) & ~(size_t)16383);
    for (int i = 0; i < n; i++) {
        uint32_t *f = g + (size_t)i * FW;
        int k = i % 10, adds = 0, w = 0;
        for (int j = 0; j < k; j++) {
            switch ((i / 10 + j) % 4) {
            case 0: f[w++] = 0x91000400; adds++; break;   /* add x0, x0, #1 */
            case 1: f[w++] = 0xD28000A1; break;           /* movz x1, #5 */
            case 2: f[w++] = 0xCA010042; break;           /* eor x2, x2, x1 */
            case 3: f[w++] = 0x11000463; break;           /* add w3, w3, #1 */
            }
        }
        f[w++] = 0xF100001Fu | (uint32_t)adds << 10;      /* cmp x0, #adds */
        f[w++] = 0x54000040;                              /* b.eq +8 */
        f[w++] = 0xD4200000;                              /* brk #0 */
        f[w++] = 0x91019000;                              /* add x0, x0, #100 */
        f[w++] = 0x14000002;                              /* b +8 */
        f[w++] = 0xD4200000;                              /* brk #0 */
        f[w++] = 0xD65F03C0;                              /* ret */
        while (w < FW) f[w++] = 0xD4200000;
        want[i] = (uint64_t)adds + 100;
    }
    return g;
}

static hao_thread *new_thread(void)
{
    hao_thread *t = aligned_alloc(64, 4096);
    memset(t, 0, 4096);
    return t;
}

typedef struct { int sites, unaligned, crossing; } site_stats;

/* run fn to its ret; count the unchained exits that reported a site */
static bool run_fn(hao_thread *t, const uint32_t *fn, site_stats *s)
{
    memset(&t->cpu, 0, sizeof t->cpu);
    t->cpu.pc = (uint64_t)(uintptr_t)fn;
    t->cpu.x[30] = HAO_RETURN_MAGIC;
    for (int n = 0; n < 64 && t->cpu.pc != HAO_RETURN_MAGIC; n++) {
        int r = hao_jit_run_block(t);
        uint64_t site = t->cpu.host_scratch[0];
        if (r == HAO_EXIT_BRANCH && site && s) {
            s->sites++;
            if (site & 3) s->unaligned++;
            if ((site & 7) > 4 || (site & 63) > 60) s->crossing++;
        }
        if (r != HAO_EXIT_NONE && r != HAO_EXIT_BRANCH) return false;
    }
    return t->cpu.pc == HAO_RETURN_MAGIC;
}

static int t_sites(int n)
{
    hao_jit_init();
    uint64_t *want = malloc((size_t)n * 8);
    uint32_t *g = make_fns(n, want);
    hao_thread *t = new_thread();
    site_stats s1 = {0}, s2 = {0}, s3 = {0};
    int wrong = 0;
    /* pass 1 translates every block; pass 2 chains each exit to its now
     * translated target; pass 3 must not leave the translated code */
    site_stats *ss[3] = {&s1, &s2, &s3};
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < n; i++)
            wrong += !run_fn(t, g + (size_t)i * FW, ss[pass]) || t->cpu.x[0] != want[i];
    int unaligned = s1.unaligned + s2.unaligned, crossing = s1.crossing + s2.crossing;
    bool ok = !wrong && s1.sites > 0 && !unaligned && !s3.sites;
    printf("sites: functions=%d sites=%d unaligned=%d crossing_8_or_64=%d unchained_after_link=%d wrong=%d -> %s\n", n,
           s1.sites + s2.sites, unaligned, crossing, s3.sites, wrong, ok ? "PASS" : "FAIL");
    hao_jit_stats();
    return !ok;
}

static int race_n;
static uint32_t *race_g;
static uint64_t *race_want;
static volatile int race_go;

static void *race_thread(void *a)
{
    (void)a;
    hao_thread *t = new_thread();
    int wrong = 0;
    while (!race_go) ;
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < race_n; i++) wrong += !run_fn(t, race_g + (size_t)i * FW, NULL) || t->cpu.x[0] != race_want[i];
    return (void *)(intptr_t)wrong;
}

static int t_race(int n, int nt)
{
    hao_jit_init();
    race_n = n;
    race_want = malloc((size_t)n * 8);
    race_g = make_fns(n, race_want);
    pthread_t th[64];
    if (nt > 64) nt = 64;
    for (int k = 0; k < nt; k++) pthread_create(&th[k], NULL, race_thread, NULL);
    race_go = 1;
    int wrong = 0;
    for (int k = 0; k < nt; k++) { void *r; pthread_join(th[k], &r); wrong += (int)(intptr_t)r; }
    bool ok = !wrong;
    printf("race: functions=%d threads=%d wrong=%d -> %s\n", n, nt, wrong, ok ? "PASS" : "FAIL");
    hao_jit_stats();
    return !ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "sites")) return t_sites(argc > 2 ? atoi(argv[2]) : 2000);
    if (!strcmp(m, "race")) return t_race(argc > 2 ? atoi(argv[2]) : 2000, argc > 3 ? atoi(argv[3]) : 8);
    fprintf(stderr, "usage: test_chain sites [N] | race [N] [THREADS]\n");
    return 2;
}
