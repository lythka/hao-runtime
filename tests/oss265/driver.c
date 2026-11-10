/* Exercises 26.5 OSS Libc sources (compiled under oss_* names) with
 * deterministic inputs. Built for arm64e (run under hao) and x86_64
 * (native); outputs must be identical. */
#include "rename.h"
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vis.h>
#include <limits.h>

static uint64_t rng = 0x26F71ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static uint64_t fnv = 1469598103934665603ull;
static void mix(const void *p, size_t n) { const unsigned char *b = p; while (n--) { fnv ^= *b++; fnv *= 1099511628211ull; } }

static int icmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int scmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int rcmp(void *th, const void *a, const void *b) { (*(int *)th)++; return icmp(b, a); }
typedef struct { double key; int id; char tag[12]; } rec;
static int reccmp(const void *a, const void *b) { const rec *x = a, *y = b; return x->key < y->key ? -1 : x->key > y->key ? 1 : x->id - y->id; }
static int nodes;
static void walker(const void *n, VISIT v, int depth) { if (v == postorder || v == leaf) { nodes++; mix(*(int *const *)n, sizeof(int)); mix(&depth, sizeof depth); } }

int main(void)
{
    enum { N = 20000 };
    int *a = malloc(N * sizeof *a), *b = malloc(N * sizeof *b);
    for (int i = 0; i < N; i++) a[i] = (int)(rnd() % 100000) - 50000;
    memcpy(b, a, N * sizeof *a); oss_qsort(b, N, sizeof *b, icmp); mix(b, N * sizeof *b);
    printf("qsort first=%d last=%d\n", b[0], b[N - 1]);
    int calls = 0; memcpy(b, a, N * sizeof *a); oss_qsort_r(b, N, sizeof *b, &calls, rcmp); mix(b, N * sizeof *b);
    printf("qsort_r desc first=%d calls>0=%d\n", b[0], calls > 0);
    memcpy(b, a, N * sizeof *a); oss_heapsort(b, N, sizeof *b, icmp); mix(b, N * sizeof *b);
    memcpy(b, a, N * sizeof *a); printf("mergesort rc=%d\n", oss_mergesort(b, N, sizeof *b, icmp)); mix(b, N * sizeof *b);
    int key = b[N / 3]; int *hit = oss_bsearch(&key, b, N, sizeof *b, icmp);
    printf("bsearch found=%d\n", hit && *hit == key);

    rec *r = calloc(5000, sizeof *r);
    for (int i = 0; i < 5000; i++) { r[i].key = (rnd() % 1000) / 7.0 - 50.25; r[i].id = i; snprintf(r[i].tag, sizeof r[i].tag, "t%05d", (int)(rnd() % 99999)); }
    oss_mergesort(r, 5000, sizeof *r, reccmp); mix(r, 5000 * sizeof *r);
    printf("records key0=%.4f id0=%d keyN=%.4f\n", r[0].key, r[0].id, r[4999].key);

    const unsigned char **strs = malloc(3000 * sizeof *strs);
    char *pool = malloc(3000 * 16);
    for (int i = 0; i < 3000; i++) { char *s = pool + 16 * i; int len = 1 + rnd() % 14; for (int k = 0; k < len; k++) s[k] = 'a' + rnd() % 26; s[len] = 0; strs[i] = (const unsigned char *)s; }
    const unsigned char **s2 = malloc(3000 * sizeof *s2);
    memcpy(s2, strs, 3000 * sizeof *s2);
    printf("radixsort rc=%d first=%s last=%s\n", oss_radixsort(s2, 3000, NULL, 0), s2[0], s2[2999]);
    for (int i = 0; i < 3000; i++) mix(s2[i], strlen((const char *)s2[i]));
    memcpy(s2, strs, 3000 * sizeof *s2); oss_qsort(s2, 3000, sizeof *s2, scmp);
    printf("qsort strings first=%s\n", s2[0]);

    const char *hay = "the quick brown fox jumps over the lazy dog; the end";
    printf("memmem=%td strnstr=%s\n", (const char *)oss_memmem(hay, strlen(hay), "lazy", 4) - hay, oss_strnstr(hay, "fox", 20));
    char line[] = "alpha,beta,,gamma,delta"; char *lp = line, *tok; int nt = 0;
    while ((tok = oss_strsep(&lp, ",")) != NULL) { mix(tok, strlen(tok)); nt++; }
    printf("strsep tokens=%d\n", nt);

    char mode[16]; mode_t modes[] = {S_IFREG | 0644, S_IFDIR | 0755, S_IFLNK | 0777, S_IFREG | 04755, S_IFDIR | 01777, S_IFCHR | 0620, S_IFIFO | 0600, S_IFSOCK | 0700};
    for (unsigned k = 0; k < sizeof modes / sizeof *modes; k++) { oss_strmode(modes[k], mode); printf("strmode %06o %s\n", (unsigned)modes[k], mode); }

    void *root = NULL; int *keys = malloc(2000 * sizeof *keys);
    for (int i = 0; i < 2000; i++) { keys[i] = (int)(rnd() % 5000); oss_tsearch(&keys[i], &root, icmp); }
    oss_twalk(root, walker); printf("tsearch nodes=%d\n", nodes);
    int del = 0; for (int i = 0; i < 2000; i += 3) if (oss_tdelete(&keys[i], &root, icmp)) del++;
    int probe = keys[1]; printf("tdelete %d tfind=%d\n", del, oss_tfind(&probe, &root, icmp) != NULL);

    int table[64]; size_t nel = 0;
    for (int i = 0; i < 200; i++) { int v = (int)(rnd() % 50); if (nel < 64) oss_lsearch(&v, table, &nel, sizeof v, icmp); }
    printf("lsearch distinct=%zu\n", nel); mix(table, nel * sizeof *table);

    char raw[256], enc[1100];
    for (int i = 0; i < 255; i++) raw[i] = (char)(i + 1); raw[255] = 0;
    oss_strvis(enc, raw, VIS_WHITE | VIS_OCTAL | VIS_CSTYLE); mix(enc, strlen(enc));
    printf("strvis len=%zu head=%.24s\n", strlen(enc), enc);

    const char *nums[] = {"0", "42", "-17", "9223372036854775807", "99999999999999999999", "12abc", "", "-9223372036854775808"};
    for (unsigned k = 0; k < sizeof nums / sizeof *nums; k++) {
        const char *err = NULL; long long v = oss_strtonum(nums[k], LLONG_MIN, LLONG_MAX, &err);
        printf("strtonum('%s') = %lld err=%s\n", nums[k], v, err ? err : "-");
    }
    /* compiler-vectorised arithmetic over the sorted data */
    double acc = 0; for (int i = 0; i < N; i++) acc += b[i] * 0.5 - (b[i] & 7) * 1.25;
    long long iacc = 0; for (int i = 0; i < N; i++) iacc += (long long)b[i] * b[N - 1 - i];
    printf("acc=%.3f iacc=%lld\n", acc, iacc);
    printf("fnv=%016llx\n", (unsigned long long)fnv);
    return 0;
}
