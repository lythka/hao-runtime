/* Host test: the code cache, its locks and its fixed areas.
 *
 *   test_jitlock aot BLOCKS GUEST    hao_aot_load of 40 copies of GUEST
 *                                    (HAO_CACHE holds its cache) while 4
 *                                    threads translate: the AOT regions must
 *                                    not overlap each other or their blocks,
 *                                    and every block must still run
 *   test_jitlock sigs                signature thunks keep their signature
 *                                    up to 65536, are made once per (guest,
 *                                    sig), and past the area are refused (0;
 *                                    the script checks the one-time warning)
 *   test_jitlock closure N           every signature thunk maps back to its
 *                                    guest (hao_guest_of_entry), and a miss
 *                                    stays cheap
 *   test_jitlock full N              run with HAO_JIT_CACHE_MB=8: more code
 *                                    than fits; blocks past the limit run in
 *                                    the interpreter, with NZCV crossing
 *                                    between JIT and interpreted blocks
 * Guest signal handlers against hao's locks are tested end to end by
 * tests/guest/signals.sh (signals.c defers them inside hao_crit sections).
 * Each prints one summary line ending in PASS or FAIL; exit 0 on PASS. */
#include "hao/runtime.h"
#include "hao/aot.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct hsig hsig;
hsig *hao_sig(const char *enc);                     /* abi.c */
uint64_t hao_sig_thunk(uint64_t guest, void *sig);  /* jit.c */

/* ------------------------------------------------ guest code */

/* BI-instruction function: x0 = sum of (k & 15), then ret */
enum { BI = 96 };
static uint64_t add_want;
static uint32_t *make_add_fns(size_t n)
{
    uint32_t *g = malloc(n * BI * 4);
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < BI - 1; k++) g[i * BI + k] = 0x91000000u | (uint32_t)((k & 15) << 10);   /* add x0, x0, #k&15 */
        g[i * BI + BI - 1] = 0xd65f03c0;                                                           /* ret */
    }
    add_want = 0;
    for (int k = 0; k < BI - 1; k++) add_want += (uint64_t)(k & 15);
    return g;
}

static hao_thread *new_thread(void)
{
    size_t n = (sizeof(hao_thread) + 63) & ~(size_t)63;   /* all of it: the JIT uses the thread's jump cache */
    hao_thread *t = aligned_alloc(64, n);
    memset(t, 0, n);
    return t;
}

/* run the function at fn to its ret; false if it did not return in `limit` blocks */
static bool run_fn(hao_thread *t, const uint32_t *fn, int limit)
{
    memset(&t->cpu, 0, sizeof t->cpu);
    t->cpu.pc = (uint64_t)(uintptr_t)fn;
    t->cpu.x[30] = HAO_RETURN_MAGIC;
    for (int n = 0; n < limit && t->cpu.pc != HAO_RETURN_MAGIC; n++) hao_jit_run_block(t);
    return t->cpu.pc == HAO_RETURN_MAGIC;
}

/* ------------------------------------------------ aot */

/* HAO_CACHE holds a valid cache for GUEST (made by `hao --aot GUEST`).
 * The test loads NIMG copies of GUEST (each at its own address) and loads
 * the cache into each while NT threads translate. */
enum { NT = 4, NIMG = 40 };
static int aot_nb;
static uint32_t *aot_fns;
static volatile int aot_go;

static void *aot_translator(void *a)
{
    int id = (int)(intptr_t)a, bad = 0;
    hao_thread *t = new_thread();
    while (!aot_go) ;
    for (int i = id; i < aot_nb; i += NT) bad += !run_fn(t, aot_fns + (size_t)i * BI, 4) || t->cpu.x[0] != add_want;
    return (void *)(intptr_t)bad;
}

/* the code of the one cache file in dir (format: include/hao/aot.h) */
static uint8_t *aot_file_code(const char *dir, uint64_t *size)
{
    char cmd[1200], p[1200];
    snprintf(cmd, sizeof cmd, "ls '%s'/*.aot 2>/dev/null | head -1", dir);
    FILE *ls = popen(cmd, "r");
    if (!ls || !fgets(p, sizeof p, ls)) { if (ls) pclose(ls); return NULL; }
    pclose(ls);
    p[strcspn(p, "\n")] = 0;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static uint8_t buf[256 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    hao_aot_hdr h;
    if (n < sizeof h) return NULL;
    memcpy(&h, buf, sizeof h);
    uint64_t off = h.hdr_size + 8ull * h.nblocks + 8ull * h.nlinks;
    if (off + h.code_size > n) return NULL;
    *size = h.code_size;
    return buf + off;
}

static int t_aot(int nb, const char *guest)
{
    aot_nb = nb;
    const char *dir = getenv("HAO_CACHE");
    uint64_t cs = 0;
    uint8_t *want = dir ? aot_file_code(dir, &cs) : NULL;
    if (!want) { fprintf(stderr, "aot: no cache in HAO_CACHE=%s\n", dir ? dir : "(unset)"); return 2; }
    hao_jit_init();
    aot_fns = make_add_fns((size_t)nb);
    hao_image *im[NIMG];
    char err[512];
    for (int j = 0; j < NIMG; j++)
        if (!(im[j] = hao_load_image(guest, hao_resolve_import, err, sizeof err))) { fprintf(stderr, "aot: %s: %s\n", guest, err); return 2; }
    pthread_t th[NT];
    for (int k = 0; k < NT; k++) pthread_create(&th[k], NULL, aot_translator, (void *)(intptr_t)k);
    aot_go = 1;
    usleep(2000);
    int loaded = 0;
    for (int j = 0; j < NIMG; j++) { loaded += hao_aot_load(im[j]); usleep(200); }
    int bad = 0;
    for (int k = 0; k < NT; k++) { void *r; pthread_join(th[k], &r); bad += (int)(intptr_t)r; }
    int clobbered = 0, overlap = 0;
    for (int j = 0; j < NIMG; j++) {
        uint8_t *c = im[j]->aot;
        if (c && memcmp(c + 16, want + 16, cs - 16)) clobbered++;
        for (int i = 0; c && i < j; i++)
            if (im[i]->aot && (uint8_t *)im[i]->aot < c + cs && c < (uint8_t *)im[i]->aot + cs) overlap++;
    }
    hao_thread *t = new_thread();
    int bad2 = 0;
    for (int i = 0; i < nb; i++) bad2 += !run_fn(t, aot_fns + (size_t)i * BI, 4) || t->cpu.x[0] != add_want;
    bool ok = loaded == NIMG && !clobbered && !overlap && !bad && !bad2;
    printf("aot: loaded=%d/%d code=%lluKB aot_regions_clobbered=%d overlapping=%d wrong_first=%d wrong_rerun=%d -> %s\n", loaded, NIMG,
           (unsigned long long)(cs >> 10), clobbered, overlap, bad, bad2, ok ? "PASS" : "FAIL");
    return !ok;
}

/* ------------------------------------------------ signature thunks */

static bool sig_imm(uint64_t e, uint64_t g, uint64_t s)
{
    uint64_t gg, ss;
    memcpy(&gg, (void *)(uintptr_t)(e + 2), 8);
    memcpy(&ss, (void *)(uintptr_t)(e + 12), 8);
    return gg == g && ss == s;
}

static int t_sigs(void)
{
    hao_jit_init();
    const int n1 = 30000, n3 = 40000;   /* 70000 in all: past the area's 65536 */
    int lost = 0, first_lost = -1;
    uint64_t e0 = 0;
    for (int k = 0; k < n1; k++) {
        uint64_t g = 0x7100000000ull + 16ull * (unsigned)k, s = 0x500000000000ull + (unsigned)k;
        uint64_t e = hao_sig_thunk(g, (void *)(uintptr_t)s);
        if (!k) e0 = e;
        if (!sig_imm(e, g, s)) { if (first_lost < 0) first_lost = k; lost++; }
    }
    int dup = 0;   /* the same pair again: the same thunk */
    for (int k = 0; k < 100000; k++) dup += hao_sig_thunk(0x7100000000ull, (void *)(uintptr_t)0x500000000000ull) != e0;
    int sig_ok = 0, plain = 0, other = 0;
    for (int k = n1; k < n1 + n3; k++) {
        uint64_t g = 0x7100000000ull + 16ull * (unsigned)k, s = 0x500000000000ull + (unsigned)k;
        uint64_t e = hao_sig_thunk(g, (void *)(uintptr_t)s);
        if (!e) plain++;   /* area full: none (abi.c then notes the signature) */
        else if (sig_imm(e, g, s) && hao_guest_of_entry(e) == g) sig_ok++;
        else other++;
    }
    /* 65536 signature thunks fit: n1 + (65536 - n1) more, the rest fall back */
    int want_sig = 65536 - n1;
    bool ok = !lost && !dup && sig_ok == want_sig && plain == n1 + n3 - 65536 && !other;
    printf("sigs: first=%d lost_signature=%d (first #%d) repeat_new_address=%d; next=%d: signature=%d none_past_area=%d wrong=%d -> %s\n",
           n1, lost, first_lost, dup, n3, sig_ok, plain, other, ok ? "PASS" : "FAIL");
    return !ok;
}

static int t_closure(int n)
{
    const char *enc = "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16";
    int nosig = 0, unmapped = 0;
    for (int k = 0; k < n; k++) {
        uint64_t g = 0x7100000000ull + 16ull * (unsigned)k, e = hao_guest_entry_for_sig(g, enc);
        if (hao_thunk_target(e)) nosig++;              /* fell back to a plain thunk */
        if (hao_guest_of_entry(e) != g) unmapped++;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t s = 0;
    for (int k = 0; k < 100000; k++) s += hao_guest_of_entry(0x7fff00000000ull + 16ull * (unsigned)k);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / 1e5;
    bool ok = !nosig && !unmapped && !s && ns < 1000;
    printf("closure: n=%d plain_fallback=%d unmapped=%d miss_ns=%.0f -> %s\n", n, nosig, unmapped, ns, ok ? "PASS" : "FAIL");
    return !ok;
}

/* ------------------------------------------------ cache full */

/* FW-instruction function with a flag dependency across a block boundary:
 *   A: add x0, x0, #k&15 (x FA) ; cmp x0, #want ; b B
 *      brk #0
 *   B: cset x1, eq ; b.ne D
 *   C: ret
 *   D: mov x0, #0xdead ; ret
 * x0 == want and x1 == 1 only if NZCV survives the A -> B exit. */
enum { FA = 92, FW = 100, OFF_B = FA + 3, OFF_D = FA + 6 };
static uint64_t full_want;
static uint32_t *make_flag_fns(size_t n)
{
    uint32_t *g = malloc(n * FW * 4);
    full_want = 0;
    for (int k = 0; k < FA; k++) full_want += (uint64_t)(k & 15);
    for (size_t i = 0; i < n; i++) {
        uint32_t *f = g + i * FW;
        for (int k = 0; k < FA; k++) f[k] = 0x91000000u | (uint32_t)((k & 15) << 10);
        f[FA] = 0xF100001Fu | (uint32_t)(full_want << 10);   /* cmp x0, #want */
        f[FA + 1] = 0x14000002;                               /* b +8 (B) */
        f[FA + 2] = 0xD4200000;                               /* brk #0 */
        f[OFF_B] = 0x9A9F17E1;                                /* cset x1, eq */
        f[OFF_B + 1] = 0x54000041;                            /* b.ne +8 (D) */
        f[OFF_B + 2] = 0xd65f03c0;                            /* ret */
        f[OFF_D] = 0xD29BD5A0;                                /* mov x0, #0xdead */
        f[OFF_D + 1] = 0xd65f03c0;                            /* ret */
        for (int k = OFF_D + 2; k < FW; k++) f[k] = 0xD4200000;
    }
    return g;
}

static int t_full(int nb)
{
    hao_jit_init();
    uint32_t *g = make_flag_fns((size_t)nb);
    hao_thread *t = new_thread();
    /* Translate B alone for every third function and A alone for the next,
     * so that once the cache fills, functions run A interpreted -> B JIT and
     * A JIT -> B interpreted. */
    for (int i = 0; i < nb; i++) {
        if (i % 3 == 0) continue;
        memset(&t->cpu, 0, sizeof t->cpu);
        t->cpu.x[30] = HAO_RETURN_MAGIC;
        t->cpu.pc = (uint64_t)(uintptr_t)(g + (size_t)i * FW + (i % 3 == 1 ? OFF_B : 0));
        hao_jit_run_block(t);
    }
    int bad = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nb; i++)
            bad += !run_fn(t, g + (size_t)i * FW, 400) || t->cpu.x[0] != full_want || t->cpu.x[1] != 1;
    uint64_t th = hao_thunk_for(0x7300000000ull);
    bool thunk_ok = hao_thunk_target(th) == 0x7300000000ull;
    bool ok = !bad && thunk_ok;
    printf("full: functions=%d wrong=%d thunk_after_full=%s -> %s\n", nb, bad, thunk_ok ? "ok" : "MISSING", ok ? "PASS" : "FAIL");
    return !ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "aot") && argc > 3) return t_aot(atoi(argv[2]), argv[3]);
    if (!strcmp(m, "sigs")) return t_sigs();
    if (!strcmp(m, "closure")) return t_closure(argc > 2 ? atoi(argv[2]) : 20000);
    if (!strcmp(m, "full")) return t_full(argc > 2 ? atoi(argv[2]) : 20000);
    fprintf(stderr, "usage: test_jitlock aot N GUEST | sigs | closure [N] | full [N]\n");
    return 2;
}
