/*
 * Standalone coverage-guided mutation driver for libFuzzer-style harnesses
 * (LLVMFuzzerTestOneInput). Apple's toolchain ships no libclang_rt.fuzzer, so
 * this provides the loop: targets are built with
 * -fsanitize=address -fsanitize-coverage=trace-pc-guard and this file
 * (compiled without coverage) supplies the guard callbacks. An input that
 * reaches a new edge joins the in-memory corpus (and CORPUS_DIR when one is
 * given). A crash is caught by ASan; its death callback, or the timeout
 * alarm, writes the input to ARTIFACT_PREFIX crash-<hash> / timeout-<hash>.
 *
 *   fuzz_X [-runs=N] [-max_total_time=S] [-seed=N] [-max_len=N] [-timeout=S]
 *          [-artifact_prefix=DIR/] [CORPUS_DIR | FILE]...
 * With only FILE arguments each file is run once (reproduce a crash). The
 * last line reports runs, corpus size, edges and executions per second.
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
/* Optional hooks; a harness's strong definitions replace these. */
static int have_custom = 1;
__attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv) { (void)argc; (void)argv; return 0; }
/* A harness-aware mutation (e.g. rewrite one Mach-O field). */
__attribute__((weak)) size_t hao_fuzz_custom_mutate(uint8_t *data, size_t size, size_t max, uint64_t rnd)
{
    (void)data; (void)max; (void)rnd;
    have_custom = 0;
    return size;
}
void __sanitizer_set_death_callback(void (*cb)(void));

/* ------------------------------------------------------------- coverage */
static uint8_t *seen;           /* guard index -> ever hit */
static uint8_t *hit;            /* guard index -> hit this run */
static uint32_t nguards, nseen;
static uint32_t new_edges;

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    if (start == stop || *start) return;
    for (uint32_t *g = start; g < stop; g++) *g = ++nguards;
    seen = realloc(seen, nguards + 1);
    hit = realloc(hit, nguards + 1);
    memset(seen, 0, nguards + 1);
    memset(hit, 0, nguards + 1);
}

void __sanitizer_cov_trace_pc_guard(uint32_t *g)
{
    uint32_t i = *g;
    if (!i || hit[i]) return;
    hit[i] = 1;
    if (!seen[i]) { seen[i] = 1; nseen++; new_edges++; }
}

/* ------------------------------------------------------------- corpus */
typedef struct { uint8_t *d; size_t n; } unit;
static unit *corpus;
static size_t ncorpus, capcorpus;
static const char *corpus_dir, *artifact_prefix = "./";
static const uint8_t *cur_data;
static size_t cur_size;

static uint64_t fnv(const uint8_t *d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t k = 0; k < n; k++) h = (h ^ d[k]) * 1099511628211ull;
    return h;
}

static void save(const char *dir, const char *prefix, const uint8_t *d, size_t n)
{
    char path[2048];
    snprintf(path, sizeof path, "%s%s%s%016llx", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", prefix,
             (unsigned long long)fnv(d, n));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, d, n);
    (void)w;
    close(fd);
    if (*prefix) fprintf(stderr, "fuzz: wrote %s (%zu bytes)\n", path, n);
}

static void add_unit(const uint8_t *d, size_t n)
{
    if (ncorpus == capcorpus) { capcorpus = capcorpus ? 2 * capcorpus : 64; corpus = realloc(corpus, capcorpus * sizeof *corpus); }
    corpus[ncorpus].d = malloc(n ? n : 1);
    memcpy(corpus[ncorpus].d, d, n);
    corpus[ncorpus].n = n;
    ncorpus++;
}

static void on_death(void) { if (cur_data) save(artifact_prefix, "crash-", cur_data, cur_size); }
static void on_alarm(int sig)
{
    (void)sig;
    if (cur_data) save(artifact_prefix, "timeout-", cur_data, cur_size);
    static const char m[] = "fuzz: timeout\n";
    ssize_t w = write(2, m, sizeof m - 1);
    (void)w;
    _exit(70);
}

static void on_abort(int sig)
{
    if (cur_data) save(artifact_prefix, "crash-", cur_data, cur_size);
    signal(sig, SIG_DFL);
    raise(sig);
}

static unsigned timeout_s = 10;

static int run_one(const uint8_t *d, size_t n)
{
    if (hit) memset(hit, 0, nguards + 1);
    new_edges = 0;
    cur_data = d; cur_size = n;
    alarm(timeout_s);
    LLVMFuzzerTestOneInput(d, n);
    alarm(0);
    cur_data = NULL;
    return (int)new_edges;
}

/* ------------------------------------------------------------- mutation */
static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static size_t rb(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static const int64_t interesting[] = {0, 1, -1, 2, 7, 8, 16, 24, 32, 64, 0x7f, 0x80, 0xff, 0x100, 0x1000, 0x3fff, 0x4000,
                                      0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000ll, 0xffffffffll,
                                      0x100000000ll, 0x7fffffffffffffffll, (int64_t)0x8000000000000000ull};

static size_t mutate(uint8_t *d, size_t n, size_t max)
{
    int rounds = 1 + (int)rb(6);
    for (int r = 0; r < rounds; r++) {
        if (have_custom && rb(3) == 0) { n = hao_fuzz_custom_mutate(d, n, max, rnd()); continue; }
        switch (rb(10)) {
        case 0: if (n) d[rb(n)] ^= (uint8_t)(1u << rb(8)); break;
        case 1: if (n) d[rb(n)] = (uint8_t)rnd(); break;
        case 2: {                                       /* interesting integer, 1/2/4/8 bytes, aligned or not */
            int w = 1 << rb(4);
            if ((size_t)w > n) break;
            size_t o = rb(n - (size_t)w + 1);
            if (rb(2)) o &= ~(size_t)(w - 1);
            int64_t v = interesting[rb(sizeof interesting / sizeof *interesting)];
            if (rb(4) == 0) v += (int64_t)rb(33) - 16;
            memcpy(d + o, &v, (size_t)w);
            break;
        }
        case 3: {                                       /* add a small delta to a 32-bit word */
            if (n < 4) break;
            size_t o = rb(n - 3) & ~(size_t)3;
            uint32_t v; memcpy(&v, d + o, 4); v += (uint32_t)((int)rb(65) - 32); memcpy(d + o, &v, 4);
            break;
        }
        case 4: {                                       /* copy a chunk within the input */
            if (n < 2) break;
            size_t len = 1 + rb(n < 64 ? n - 1 : 64), a = rb(n - len + 1), b = rb(n - len + 1);
            memmove(d + b, d + a, len);
            break;
        }
        case 5: {                                       /* erase a chunk */
            if (n < 2) break;
            size_t len = 1 + rb(n < 32 ? n - 1 : 32), a = rb(n - len + 1);
            memmove(d + a, d + a + len, n - a - len);
            n -= len;
            break;
        }
        case 6: {                                       /* insert random bytes */
            size_t len = 1 + rb(16);
            if (n + len > max) break;
            size_t a = rb(n + 1);
            memmove(d + a + len, d + a, n - a);
            for (size_t k = 0; k < len; k++) d[a + k] = (uint8_t)rnd();
            n += len;
            break;
        }
        case 7: {                                       /* splice from another corpus unit at the same offset */
            const unit *u = &corpus[rb(ncorpus)];
            if (!u->n || !n) break;
            size_t a = rb(n < u->n ? n : u->n), len = 1 + rb(256);
            if (a + len > n) len = n - a;
            if (a + len > u->n) len = u->n - a;
            memcpy(d + a, u->d + a, len);
            break;
        }
        case 8: if (n > 1 && rb(8) == 0) n = 1 + rb(n); break;   /* truncate */
        case 9: {                                       /* insert an interesting integer as decimal text */
            char t[24];
            int len = snprintf(t, sizeof t, "%lld", (long long)(interesting[rb(sizeof interesting / sizeof *interesting)] +
                                                                (rb(2) ? 0 : (int64_t)rb(70000))));
            if (len <= 0 || n + (size_t)len > max) break;
            size_t a = rb(n + 1);
            memmove(d + a + len, d + a, n - a);
            memcpy(d + a, t, (size_t)len);
            n += (size_t)len;
            break;
        }
        }
    }
    return n;
}

/* ------------------------------------------------------------- main */
static void load_path(const char *path, int *nfiles)
{
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "fuzz: %s: not found\n", path); return; }
    if (S_ISDIR(st.st_mode)) {
        if (!corpus_dir) corpus_dir = path;
        DIR *dir = opendir(path);
        struct dirent *e;
        while (dir && (e = readdir(dir))) {
            if (e->d_name[0] == '.') continue;
            char p[2048];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            load_path(p, NULL);
        }
        if (dir) closedir(dir);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *d = malloc((size_t)st.st_size + 1);
    size_t n = fread(d, 1, (size_t)st.st_size, f);
    fclose(f);
    add_unit(d, n);
    free(d);
    if (nfiles) (*nfiles)++;
}

int main(int argc, char **argv)
{
    long runs = -1, max_time = 0, max_len = 1 << 20;
    uint64_t seed = (uint64_t)time(NULL) ^ (uint64_t)getpid();
    LLVMFuzzerInitialize(&argc, &argv);
    int nfiles = 0, ndirs = 0;
    for (int k = 1; k < argc; k++) {
        const char *a = argv[k];
        if (!strncmp(a, "-runs=", 6)) runs = atol(a + 6);
        else if (!strncmp(a, "-max_total_time=", 16)) max_time = atol(a + 16);
        else if (!strncmp(a, "-seed=", 6)) seed = strtoull(a + 6, NULL, 0);
        else if (!strncmp(a, "-max_len=", 9)) max_len = atol(a + 9);
        else if (!strncmp(a, "-timeout=", 9)) timeout_s = (unsigned)atoi(a + 9);
        else if (!strncmp(a, "-artifact_prefix=", 17)) artifact_prefix = a + 17;
        else if (a[0] == '-') { fprintf(stderr, "fuzz: unknown option %s\n", a); return 2; }
        else { struct stat st; if (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) ndirs++; load_path(a, &nfiles); }
    }
    rs = seed + 0x9e3779b97f4a7c15ull;                 /* splitmix64: small seeds give unrelated streams */
    rs = (rs ^ (rs >> 30)) * 0xbf58476d1ce4e5b9ull;
    rs = (rs ^ (rs >> 27)) * 0x94d049bb133111ebull;
    rs ^= rs >> 31;
    if (!rs) rs = 1;
    __sanitizer_set_death_callback(on_death);
    signal(SIGALRM, on_alarm);
    signal(SIGABRT, on_abort);          /* a harness's own abort() (a violated property) */
    if (ndirs == 0 && nfiles > 0) {                     /* reproduce mode */
        for (size_t k = 0; k < ncorpus; k++) run_one(corpus[k].d, corpus[k].n);
        fprintf(stderr, "fuzz: ran %zu inputs\n", ncorpus);
        return 0;
    }
    if (!ncorpus) add_unit((const uint8_t *)"", 0);
    size_t init = ncorpus;
    for (size_t k = 0; k < init; k++) run_one(corpus[k].d, corpus[k].n);
    fprintf(stderr, "fuzz: seed %llu, %zu seed inputs, %u/%u edges\n", (unsigned long long)seed, init, nseen, nguards);
    uint8_t *buf = malloc((size_t)max_len);
    time_t t0 = time(NULL), last = t0;
    long done = 0;
    while (runs < 0 || done < runs) {
        if (max_time && time(NULL) - t0 >= max_time) break;
        const unit *u = &corpus[rb(ncorpus)];
        size_t n = u->n < (size_t)max_len ? u->n : (size_t)max_len;
        memcpy(buf, u->d, n);
        n = mutate(buf, n, (size_t)max_len);
        if (run_one(buf, n)) {
            add_unit(buf, n);
            if (corpus_dir) save(corpus_dir, "", buf, n);
        }
        done++;
        if (time(NULL) - last >= 10) {
            last = time(NULL);
            fprintf(stderr, "fuzz: #%ld corpus %zu edges %u/%u exec/s %ld\n", done, ncorpus, nseen, nguards,
                    (long)(done / (last - t0 ? last - t0 : 1)));
        }
    }
    long el = (long)(time(NULL) - t0);
    fprintf(stderr, "fuzz: done %ld runs in %lds (%ld exec/s), corpus %zu, edges %u/%u, no crashes\n", done, el,
            el ? done / el : done, ncorpus, nseen, nguards);
    return 0;
}
