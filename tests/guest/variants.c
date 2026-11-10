// Symbol variants across the native boundary (tools/gen_variants.py, src/variants_gen.inc).
// Darwin exports some APIs under several names ($INODE64, $1050, $DARWIN_EXTSN,
// $NOCANCEL, $LEGACYMAC). An arm64 import must bind to the x86_64 function that is the
// same function: plain stat is stat$INODE64 there, plain select is select$1050, but plain
// getgroups is plain getgroups (the kernel call; getgroups$DARWIN_EXTSN is libsystem_c's
// directory lookup, which made the 27.0 sshd-session's setgroups fail with EINVAL).
//
// Part 1 names every member of those families that the arm64e libraries export (by asm
// label, so no header picks the name) and prints what native code sees: dladdr's library
// and symbol. x86_64's own spelling of arm64's only variant ($INODE64, $1050) is printed
// without the suffix, and a plain x86_64 name whose library also exports name$INODE64 or
// name$1050 is the legacy function and printed as such; $LEGACYMAC is arm64's spelling of
// x86_64's plain Security function. Left out: open, openat, fcntl and syslog (hao binds
// them to variadic traps by name) and the iOS-only plain SecCertificateCopyPublicKey and
// SecCertificateCopySerialNumber (API_UNAVAILABLE(macos)).
// Part 2 checks behaviour that tells variants apart without privileges: select/pselect
// with nfds > FD_SETSIZE (EINVAL only for the 1050 variants), struct stat/dirent/statfs
// and FTSENT layouts, stream counting (plain fopen/fdopen/popen are limited to
// sysconf(_SC_STREAM_MAX), the $DARWIN_EXTSN ones are not), getgroups.
// sshd's own sequence (initgroups as root, then getgroups and setgroups) needs root: for
// an ordinary user both getgroups variants return the same set.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <ftw.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define STRIP(p) ptrauth_strip(p, ptrauth_key_function_pointer)
#else
#define STRIP(p) (p)
#endif

#define SYMS(X) \
    X(_SecCertificateCopyPublicKey$LEGACYMAC) X(_SecCertificateCopySerialNumber$LEGACYMAC) \
    X(___opendir2) X(__readdir_unlocked) X(__seekdir) X(_accept) X(_accept$NOCANCEL) X(_aio_suspend) \
    X(_aio_suspend$NOCANCEL) X(_alphasort) X(_apr_dir_open) X(_apr_dir_read) X(_apr_dir_rewind) \
    X(_apr_file_info_get) X(_apr_file_info_get_locked) X(_apr_stat) X(_close) X(_close$NOCANCEL) X(_connect) \
    X(_connect$NOCANCEL) X(_creat) X(_creat$NOCANCEL) X(_daemon) X(_fdopen) X(_fdopen$DARWIN_EXTSN) \
    X(_fdopendir) X(_fdscandir) X(_fdscandir_b) X(_fopen) X(_fopen$DARWIN_EXTSN) X(_fstat) X(_fstatat) \
    X(_fstatfs) X(_fstatx_np) X(_fsync) X(_fsync$NOCANCEL) X(_fts_children) X(_fts_close) X(_fts_open) \
    X(_fts_open_b) X(_fts_read) X(_fts_set) X(_ftw) X(_getfsstat) X(_getgroups) X(_getgroups$DARWIN_EXTSN) \
    X(_getmntinfo) X(_getmntinfo_r_np) X(_glob) X(_glob_b) X(_lockf) X(_lockf$NOCANCEL) X(_lstat) \
    X(_lstatx_np) X(_msgrcv) X(_msgrcv$NOCANCEL) X(_msgsnd) X(_msgsnd$NOCANCEL) X(_msync) X(_msync$NOCANCEL) \
    X(_nanosleep) X(_nanosleep$NOCANCEL) X(_nftw) X(_opendir) X(_pause) X(_pause$NOCANCEL) X(_poll) \
    X(_poll$NOCANCEL) X(_popen) X(_popen$DARWIN_EXTSN) X(_pread) X(_pread$NOCANCEL) X(_preadv) \
    X(_preadv$NOCANCEL) X(_pselect) X(_pselect$DARWIN_EXTSN) X(_pselect$DARWIN_EXTSN$NOCANCEL) \
    X(_pselect$NOCANCEL) X(_pthread_cond_timedwait) X(_pthread_cond_timedwait$NOCANCEL) X(_pthread_cond_wait) \
    X(_pthread_cond_wait$NOCANCEL) X(_pthread_join) X(_pthread_join$NOCANCEL) X(_pwrite) X(_pwrite$NOCANCEL) \
    X(_pwritev) X(_pwritev$NOCANCEL) X(_read) X(_read$NOCANCEL) X(_readdir) X(_readdir_r) X(_readv) \
    X(_readv$NOCANCEL) X(_reallocarray$DARWIN_EXTSN) X(_reallocarrayf$DARWIN_EXTSN) X(_realpath) \
    X(_realpath$DARWIN_EXTSN) X(_recv) X(_recv$NOCANCEL) X(_recvfrom) X(_recvfrom$NOCANCEL) X(_recvmsg) \
    X(_recvmsg$NOCANCEL) X(_rewinddir) X(_scandir) X(_scandir_b) X(_scandirat) X(_scandirat_b) X(_seekdir) \
    X(_select) X(_select$DARWIN_EXTSN) X(_select$DARWIN_EXTSN$NOCANCEL) X(_select$NOCANCEL) X(_sem_wait) \
    X(_sem_wait$NOCANCEL) X(_send) X(_send$NOCANCEL) X(_sendmsg) X(_sendmsg$NOCANCEL) X(_sendto) \
    X(_sendto$NOCANCEL) X(_sigpause) X(_sigpause$NOCANCEL) X(_sigsuspend) X(_sigsuspend$NOCANCEL) X(_sigwait) \
    X(_sigwait$NOCANCEL) X(_sleep) X(_sleep$NOCANCEL) X(_stat) X(_statfs) X(_statx_np) X(_system) \
    X(_system$NOCANCEL) X(_tcdrain) X(_tcdrain$NOCANCEL) X(_telldir) X(_usleep) X(_usleep$NOCANCEL) X(_wait) \
    X(_wait$NOCANCEL) X(_waitid) X(_waitid$NOCANCEL) X(_waitpid) X(_waitpid$NOCANCEL) X(_write) \
    X(_write$NOCANCEL) X(_writev) X(_writev$NOCANCEL)

#define DECL(s) extern void s(void) __asm(#s);
SYMS(DECL)
#define ENTRY(s) {#s, s},
static const struct { const char *name; void (*fn)(void); } syms[] = {SYMS(ENTRY)};

// the variants under test, called by their exact names
extern int getgroups_plain(int, gid_t *) __asm("_getgroups");
extern int getgroups_ext(int, gid_t *) __asm("_getgroups$DARWIN_EXTSN");
typedef int sel_fn(int, fd_set *, fd_set *, fd_set *, struct timeval *);
typedef int psel_fn(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *);
extern sel_fn select_plain __asm("_select"), select_ext __asm("_select$DARWIN_EXTSN"),
    select_nc __asm("_select$NOCANCEL"), select_ext_nc __asm("_select$DARWIN_EXTSN$NOCANCEL");
extern psel_fn pselect_plain __asm("_pselect"), pselect_ext __asm("_pselect$DARWIN_EXTSN"),
    pselect_nc __asm("_pselect$NOCANCEL"), pselect_ext_nc __asm("_pselect$DARWIN_EXTSN$NOCANCEL");
extern FILE *fdopen_plain(int, const char *) __asm("_fdopen");
extern FILE *fdopen_ext(int, const char *) __asm("_fdopen$DARWIN_EXTSN");
extern FILE *fopen_plain(const char *, const char *) __asm("_fopen");
extern FILE *fopen_ext(const char *, const char *) __asm("_fopen$DARWIN_EXTSN");
extern FILE *popen_plain(const char *, const char *) __asm("_popen");
extern FILE *popen_ext(const char *, const char *) __asm("_popen$DARWIN_EXTSN");
extern char *realpath_plain(const char *, char *) __asm("_realpath");
extern char *realpath_ext(const char *, char *) __asm("_realpath$DARWIN_EXTSN");

static void binding(const char *name, void (*fn)(void))
{
    Dl_info di;
    if (!dladdr(STRIP((void *)fn), &di) || !di.dli_sname || !di.dli_fname) {
        printf("%-44s (no dladdr)\n", name);
        return;
    }
    char s[256];
    snprintf(s, sizeof s, "%s", di.dli_sname);
    const char *legacy = "";
    char *d;
    if ((d = strstr(s, "$INODE64")) || (d = strstr(s, "$1050"))) {
        memmove(d, strchr(d + 1, '$') ? strchr(d + 1, '$') : d + strlen(d), strlen(d) + 1);
    } else if (!strchr(s, '$')) {
        static const char *newer[] = {"$INODE64", "$1050"};
        for (int k = 0; k < 2; k++) {
            char n[300];
            snprintf(n, sizeof n, "%s%s", s, newer[k]);
            void *q = dlsym(RTLD_DEFAULT, n);
            Dl_info qi;
            if (q && dladdr(q, &qi) && qi.dli_fname && !strcmp(qi.dli_fname, di.dli_fname)) legacy = " (legacy)";
        }
    }
    if ((d = strstr(s, "$LEGACYMAC"))) memmove(d, d + 10, strlen(d + 10) + 1);
    const char *lib = strrchr(di.dli_fname, '/');
    printf("%-44s %s %s%s\n", name, lib ? lib + 1 : di.dli_fname, s, legacy);
}

static int bycmp(const FTSENT **a, const FTSENT **b) { return strcmp((*a)->fts_name, (*b)->fts_name); }
static int nftw_n, nftw_files;
static int nftw_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)p;
    nftw_n++;
    if (flag == FTW_F) nftw_files += (int)st->st_size + 100 * f->level;
    return 0;
}
static int ftw_n;
static int ftw_cb(const char *p, const struct stat *st, int flag)
{
    (void)p;
    (void)st;
    ftw_n += flag == FTW_F ? 10 : 1;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (size_t k = 0; k < sizeof syms / sizeof *syms; k++) binding(syms[k].name, syms[k].fn);

    gid_t g[256];
    int n = getgroups_plain(0, NULL), m = getgroups_ext(0, NULL);
    printf("getgroups(0) %d getgroups$DARWIN_EXTSN(0) %d\n", n, m);
    n = getgroups_plain(256, g);
    printf("getgroups %d:", n);
    for (int k = 0; k < n; k++) printf(" %u", g[k]);
    printf("\n");

    sel_fn *sel[] = {select, select_plain, select_ext, select_nc, select_ext_nc};
    psel_fn *psel[] = {pselect, pselect_plain, pselect_ext, pselect_nc, pselect_ext_nc};
    for (int k = 0; k < 5; k++) {
        struct timeval tv = {0, 0};
        struct timespec ts = {0, 0};
        errno = 0;
        int r = sel[k](FD_SETSIZE + 1, NULL, NULL, NULL, &tv);
        printf("select[%d](FD_SETSIZE+1) %d errno %d; ", k, r, r < 0 ? errno : 0);
        errno = 0;
        r = psel[k](FD_SETSIZE + 1, NULL, NULL, NULL, &ts, NULL);
        printf("pselect %d errno %d\n", r, r < 0 ? errno : 0);
    }

    umask(022);
    char dir[] = "/tmp/hao-variants.XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char path[1100];
    static const char *names[] = {"a", "bb", "sub", "sub/ccc"};
    for (int k = 0; k < 4; k++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[k]);
        if (k == 2) { mkdir(path, 0755); continue; }
        FILE *f = fopen(path, "w");
        fprintf(f, "%.*s", 3 + k, "0123456789");
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/a", dir);
    struct stat st, lst, fst, ast;
    int fd = open(path, O_RDONLY);
    int dfd = open(dir, O_RDONLY);
    stat(path, &st);
    lstat(path, &lst);
    fstat(fd, &fst);
    fstatat(dfd, "a", &ast, 0);
    printf("stat size %lld nlink %u mode %o blksize %d ino equal %d%d%d\n", (long long)st.st_size, st.st_nlink,
           st.st_mode, st.st_blksize, st.st_ino == lst.st_ino, st.st_ino == fst.st_ino, st.st_ino == ast.st_ino);
    struct stat xst;
    printf("statx_np %d size %lld; lstatx_np %d; fstatx_np %d\n", statx_np(path, &xst, NULL), (long long)xst.st_size,
           lstatx_np(path, &xst, NULL), fstatx_np(fd, &xst, NULL));

    DIR *d = opendir(dir);
    struct dirent *e;
    long pos = -1;
    char seen[512] = "";
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, "bb")) pos = telldir(d);
        char one[300];
        snprintf(one, sizeof one, " %s/%u/%u/%d", e->d_name, e->d_namlen, e->d_type,
                 !strcmp(e->d_name, "a") ? e->d_ino == st.st_ino : 1);
        strlcat(seen, one, sizeof seen);
    }
    printf("readdir%s\n", seen);  // directory order is the file system's, the same for both runs
    if (pos >= 0) seekdir(d, pos);
    e = readdir(d);
    printf("after seekdir %s; ", e ? e->d_name : "(end)");
    rewinddir(d);
    struct dirent ent, *res;
    int cnt = 0;
    while (readdir_r(d, &ent, &res) == 0 && res) cnt++;
    printf("readdir_r after rewinddir %d entries\n", cnt);
    closedir(d);
    d = fdopendir(open(dir, O_RDONLY));
    cnt = 0;
    while (readdir(d)) cnt++;
    printf("fdopendir %d entries\n", cnt);
    closedir(d);
    struct dirent **list;
    n = scandir(dir, &list, NULL, alphasort);
    printf("scandir %d:", n);
    for (int k = 0; k < n; k++) printf(" %s/%u", list[k]->d_name, list[k]->d_namlen);
    printf("\n");

    char *const roots[] = {dir, NULL};
    FTS *fts = fts_open(roots, FTS_PHYSICAL, bycmp);
    FTSENT *fe;
    printf("fts:");
    while ((fe = fts_read(fts))) {
        if (fe->fts_level == 0 && fe->fts_info == FTS_D) {
            FTSENT *c = fts_children(fts, 0);
            int nc = 0;
            for (; c; c = c->fts_link) nc++;
            printf(" [%d children]", nc);
        }
        printf(" %s/%d/%d/%lld", fe->fts_level ? fe->fts_name : ".", fe->fts_level, fe->fts_info,
               fe->fts_info == FTS_F ? (long long)fe->fts_statp->st_size : -1LL);
        if (!strcmp(fe->fts_name, "sub") && fe->fts_info == FTS_D) fts_set(fts, fe, FTS_SKIP);
    }
    printf("\n");
    fts_close(fts);
    nftw(dir, nftw_cb, 8, FTW_PHYS);
    ftw(dir, ftw_cb, 8);
    printf("nftw %d entries, files %d; ftw %d\n", nftw_n, nftw_files, ftw_n);
    glob_t gl;
    snprintf(path, sizeof path, "%s/*", dir);
    glob(path, 0, NULL, &gl);
    printf("glob %zu:", gl.gl_pathc);
    for (size_t k = 0; k < gl.gl_pathc; k++) printf(" %s", strrchr(gl.gl_pathv[k], '/') + 1);
    printf("\n");
    globfree(&gl);

    struct statfs sf, fsf;
    statfs("/", &sf);
    fstatfs(dfd, &fsf);
    printf("statfs / %s %s rdonly %d; fstatfs(dir) %s\n", sf.f_fstypename, sf.f_mntonname,
           !!(sf.f_flags & MNT_RDONLY), fsf.f_fstypename);
    struct statfs *mnt;
    int nm = getmntinfo(&mnt, MNT_NOWAIT), root = 0;
    for (int k = 0; k < nm; k++) root += !strcmp(mnt[k].f_mntonname, "/");
    int nf = getfsstat(NULL, 0, MNT_NOWAIT);
    printf("getmntinfo >0 %d root %d; getfsstat >0 %d\n", nm > 0, root, nf > 0);

    char rp[1024];
    snprintf(path, sizeof path, "%s/sub/../a", dir);
    char *r1 = realpath_plain(path, rp);
    printf("realpath %s; ", r1 ? strrchr(r1, '/') + 1 : "NULL");
    char *r2 = realpath_ext(path, NULL);
    printf("realpath$DARWIN_EXTSN %s; ", r2 ? strrchr(r2, '/') + 1 : "NULL");
    errno = 0;
    r1 = realpath_ext("/nonexistent-hao/x", NULL);
    printf("nonexistent %s errno %d\n", r1 ? r1 : "NULL", errno);

    // Streams: plain fopen/fdopen/popen count against sysconf(_SC_STREAM_MAX) (RLIMIT_NOFILE),
    // the $DARWIN_EXTSN variants do not. 200 FILEs on one descriptor need no descriptors.
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = 64;
    setrlimit(RLIMIT_NOFILE, &rl);
    int k;
    for (k = 0; k < 200 && fdopen_plain(fd, "r"); k++) {}
    printf("fdopen %s; ", k < 200 ? "limited" : "unlimited");
    errno = 0;
    FILE *f = fopen_plain("/dev/null", "r");
    printf("then fopen %s errno %d; ", f ? "ok" : "NULL", f ? 0 : errno);
    f = fopen_ext("/dev/null", "r");
    printf("fopen$DARWIN_EXTSN %s; ", f ? "ok" : "NULL");
    errno = 0;
    f = popen_plain("true", "r");
    printf("popen %s errno %d; ", f ? "ok" : "NULL", f ? 0 : errno);
    if (f) pclose(f);
    f = popen_ext("true", "r");
    printf("popen$DARWIN_EXTSN %s", f ? "ok" : "NULL");
    if (f) printf(" pclose %d", pclose(f));
    for (k = 0; k < 200 && fdopen_ext(fd, "r"); k++) {}
    printf("; fdopen$DARWIN_EXTSN %s\n", k < 200 ? "limited" : "unlimited");

    for (int j = 3; j >= 0; j--) {
        snprintf(path, sizeof path, "%s/%s", dir, names[j]);
        j == 2 ? rmdir(path) : unlink(path);
    }
    rmdir(dir);
    printf("done\n");
    return 0;
}
