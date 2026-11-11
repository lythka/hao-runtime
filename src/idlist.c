/*
 * hao — the stub-identities and native-identities lists (include/hao/idlist.h).
 * No libhao dependency: also built into libhao_codeid.a for Security.framework.
 */
#include "../include/hao/idlist.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYS_DIR "/usr/lib/hao"
static const char *const names[] = {"stub-identities", "native-identities"};

static char dir[1024] = SYS_DIR;
static uid_t owner = 0;                   /* root; the test hook allows the current user */
static hao_idrow *rows;
static size_t nrows;
static pthread_once_t once = PTHREAD_ONCE_INIT;

#ifdef HAO_TEST_SWITCHES
void hao_idlist_use_dir_for_test(const char *d)
{
    snprintf(dir, sizeof dir, "%s", d);
    owner = getuid();
}
#endif

/* st is owned by `owner` (or root), not writable by group or others, and path has no extended ACL. */
static bool safe(const char *path, const struct stat *st)
{
    if ((st->st_uid != owner && st->st_uid != 0) || (st->st_mode & (S_IWGRP | S_IWOTH))) return false;
    acl_t a = acl_get_link_np(path, ACL_TYPE_EXTENDED);   /* NULL: none */
    if (a) {
        acl_free(a);
        return false;
    }
    return true;
}

/* Every directory from / to dir (the test hook: dir only): a real directory, safe. */
static bool dir_safe(void)
{
    char p[sizeof dir];
    snprintf(p, sizeof p, "%s", dir);
    if (p[0] != '/') return false;
    for (char *s = p + strlen(p);; ) {
        struct stat st;
        if (lstat(*p ? p : "/", &st) != 0 || !S_ISDIR(st.st_mode) || !safe(*p ? p : "/", &st)) return false;
        if (!*p || owner != 0) return true;
        while (s > p && *s != '/') s--;
        *s = 0;
    }
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static const char *hex20(const char *p, const char *e, uint8_t out[20])
{
    if (e - p < 40) return NULL;
    for (int k = 0; k < 20; k++) {
        int hi = hexv(p[2 * k]), lo = hexv(p[2 * k + 1]);
        if (hi < 0 || lo < 0) return NULL;
        out[k] = (uint8_t)(hi << 4 | lo);
    }
    return p + 40;
}
static const char *spaces(const char *p, const char *e)
{
    const char *s = p;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    return p > s ? p : NULL;
}

/* One line; false if it is not a row (skipped). */
static bool parse_row(const char *p, const char *e, hao_idrow *r)
{
    while (e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
    if (!(p = hex20(p, e, r->observed)) || !(p = spaces(p, e)) || !(p = hex20(p, e, r->reference)) || !(p = spaces(p, e)))
        return false;
    const char *a = p;
    while (p < e && *p != ' ' && *p != '\t') p++;
    if (p == a || (size_t)(p - a) >= sizeof r->arch) return false;
    memcpy(r->arch, a, (size_t)(p - a));
    r->arch[p - a] = 0;
    if (!(p = spaces(p, e)) || p == e || *p == '/' || (size_t)(e - p) + 2 > sizeof r->path) return false;
    r->path[0] = '/';
    memcpy(r->path + 1, p, (size_t)(e - p));
    r->path[e - p + 1] = 0;
    return !strstr(r->path, "/../") && memcmp(r->observed, r->reference, 20) != 0;
}

static void read_list(const char *name, hao_idrow **v, size_t *n, size_t *cap)
{
    char path[sizeof dir + 64];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && safe(path, &st) && st.st_size <= (16 << 20) &&
        (buf = malloc((size_t)st.st_size + 1))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t k = pread(fd, buf + got, (size_t)st.st_size - got, (off_t)got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        for (const char *p = buf, *e = buf + got; p < e;) {
            const char *nl = memchr(p, '\n', (size_t)(e - p));
            const char *le = nl ? nl : e;
            if (*p != '#') {
                if (*n == *cap) {
                    size_t c = *cap ? 2 * *cap : 256;
                    hao_idrow *m = realloc(*v, c * sizeof **v);
                    if (!m) break;
                    *v = m;
                    *cap = c;
                }
                if (parse_row(p, le, &(*v)[*n])) (*n)++;
            }
            p = le + 1;
        }
    }
    free(buf);
    close(fd);
}

static int cmp(const void *a, const void *b) { return memcmp(a, b, 20); }

static void load(void)
{
    if (!dir_safe()) return;
    hao_idrow *v = NULL;
    size_t n = 0, cap = 0;
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) read_list(names[k], &v, &n, &cap);
    if (!n) { free(v); return; }
    qsort(v, n, sizeof *v, cmp);
    /* no chains: drop a row whose reference is some row's observed cdhash; and duplicate observed rows */
    size_t w = 0;
    for (size_t k = 0; k < n; k++) {
        if (bsearch(v[k].reference, v, n, sizeof *v, cmp)) continue;
        if ((k > 0 && !memcmp(v[k].observed, v[k - 1].observed, 20)) || (k + 1 < n && !memcmp(v[k].observed, v[k + 1].observed, 20)))
            continue;                                     /* one observed cdhash, two answers: neither */
        v[w++] = v[k];
    }
    rows = v;
    nrows = w;
}

const hao_idrow *hao_idlist_find(const uint8_t cdhash[20])
{
    pthread_once(&once, load);
    return nrows ? bsearch(cdhash, rows, nrows, sizeof *rows, cmp) : NULL;
}
