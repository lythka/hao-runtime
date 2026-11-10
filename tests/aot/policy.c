/*
 * Unit test of the AOT trust policy and the cache format checks
 * (src/aot_policy.c, linked from libhaocore.a):
 *   policy TMPDIR
 * TMPDIR must be a directory this user owns, with trusted ancestors (the
 * scratch directory). Prints one PASS/FAIL line per check; exit 1 on any FAIL.
 */
#include "hao/aot.h"
#include "hao/bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------ tiers */
static void test_tiers(void)
{
    hao_proc_facts f = {.pid = 500, .ppid = 400, .uid = 501, .euid = 501, .gid = 20, .egid = 20};
    CHECK(hao_env_tier_for(&f) == HAO_ENV_OPEN, "ordinary user process is OPEN");
    hao_proc_facts g = f; g.setugid = true;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_SETUGID, "issetugid() is SETUGID");
    g = f; g.euid = 0;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_SETUGID, "uid != euid (setuid root) is SETUGID");
    g = f; g.egid = 12;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_SETUGID, "gid != egid (setgid) is SETUGID");
    g = f; g.pid = 1; g.ppid = 0; g.uid = g.euid = 0; g.gid = g.egid = 0;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "PID 1 is GUARDED");
    g = f; g.ppid = 1; g.uid = g.euid = 0;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "root daemon (ppid 1) is GUARDED");
    g = f; g.ppid = 1;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_OPEN, "user agent (ppid 1, uid 501) is OPEN");
    g = f; g.uid = g.euid = 0;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "root shell (ppid != 1) is GUARDED");
    g = f; g.uid = g.euid = 0; g.gid = g.egid = 0; g.ppid = 77;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "root under sudo or cron (any parent) is GUARDED");
    g = f; g.uid = 0; g.euid = 501;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_SETUGID, "root that dropped only its effective uid is SETUGID");
    g = f; g.restricted = true;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "dyld-restricted is GUARDED");
    g = f; g.entitled = true;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "entitled is GUARDED");
    g = f; g.distrust = 1;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_GUARDED, "HAO_DISTRUST_ENV=1 is GUARDED");
    g = f; g.distrust = 7;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_SETUGID, "HAO_DISTRUST_ENV=7 is SETUGID");
    g = f; g.setugid = true; g.distrust = -5;
    CHECK(hao_env_tier_for(&g) == HAO_ENV_SETUGID, "HAO_DISTRUST_ENV cannot raise trust");

    /* code, paths, checks, bridging, signal and memory behaviour: an ordinary user's process only */
    static const char *open_only[] = {"HAO_CACHE", "HAO_GUEST_ROOT", "HAO_FLAGS", "HAO_AOT_LEARN", "HAO_NO_MSGSEND",
                                      "HAO_NO_CLOSURES", "HAO_NO_SWIFTREG", "HAO_NO_BLOCK_THUNKS", "HAO_SOMETHING_NEW",
                                      "HAO_IF_DIR", "HAO_DIAG_DIR", "HAO_TRUST_LIST", "HAO_ASYNC_CONTRACT_LIBRARY",
                                      "HAO_NO_LIBVAL", "HAO_NO_VARSNIFF", "HAO_NO_IFUNITS", "HAO_NO_THUNKS",
                                      "HAO_NATIVE_LIBCXX", "HAO_NO_FIXUP_THUNKS", "HAO_NO_STRUCT_THUNKS",
                                      "HAO_NO_SWIFT_THUNKS", "HAO_NO_SWIFT_VALUES", "HAO_SWIFT_POP", "HAO_BRIDGE_CHECK",
                                      "HAO_SIGRETURN_KERNEL", "HAO_NO_PROBE", "HAO_WATCHDOG", "HAO_JIT_CACHE_MB", NULL};
    /* the same code by another engine, and printing: guarded too, never set-ID */
    static const char *guarded[] = {"HAO_NO_AOT", "HAO_INTERP", "HAO_NO_CHAIN", "HAO_JIT_HELPERS_ONLY", "HAO_VERBOSE",
                                    "HAO_CRASH_TRACE", "HAO_NX_STATS", "HAO_BRIDGE_STATS", NULL};
    static const char *test_only[] = {"HAO_LIBVAL_USER", "HAO_SIGRETURN_USER", "HAO_AOT_LIMIT_KB", "HAO_BRIDGE_ABORT",
                                      "HAO_NO_JIT_MEMORY", "HAO_CODE_ALIAS", NULL};
    for (const char **s = open_only; *s; s++) {
        CHECK(hao_env_switch_allowed(*s, HAO_ENV_OPEN), "%s allowed when OPEN", *s);
        CHECK(!hao_env_switch_allowed(*s, HAO_ENV_GUARDED), "%s ignored when GUARDED", *s);
        CHECK(!hao_env_switch_allowed(*s, HAO_ENV_SETUGID), "%s ignored when SETUGID", *s);
        CHECK(!hao_env_test_switch(*s), "%s is not a test switch", *s);
    }
    for (const char **s = guarded; *s; s++) {
        CHECK(hao_env_switch_allowed(*s, HAO_ENV_OPEN), "%s allowed when OPEN", *s);
        CHECK(hao_env_switch_allowed(*s, HAO_ENV_GUARDED), "%s allowed when GUARDED", *s);
        CHECK(!hao_env_switch_allowed(*s, HAO_ENV_SETUGID), "%s ignored when SETUGID", *s);
    }
    for (const char **s = test_only; *s; s++) {
        CHECK(hao_env_test_switch(*s), "%s is a test switch", *s);
        CHECK(hao_env_switch_allowed(*s, HAO_ENV_OPEN) == hao_test_switches, "%s when OPEN: read by a HAO_TEST_SWITCHES build only (%d)",
              *s, hao_test_switches);
        CHECK(!hao_env_switch_allowed(*s, HAO_ENV_GUARDED), "%s ignored when GUARDED", *s);
        CHECK(!hao_env_switch_allowed(*s, HAO_ENV_SETUGID), "%s ignored when SETUGID", *s);
    }

    /* dependency paths: dyld's rule for a set-ID main executable, nothing refused elsewhere */
    static const struct { const char *path; bool rpath, mainimg, setugid_ok; const char *why; } dp[] = {
        {"/usr/lib/libz.1.dylib", false, true, true, NULL},
        {"/opt/x/lib", true, true, true, NULL},
        {"@rpath/libfoo.dylib", false, true, true, NULL},
        {"@executable_path/libfoo.dylib", false, true, false, "@ path"},
        {"@executable_path/libfoo.dylib", false, false, false, "@ path"},
        {"@executable_path/../lib", true, true, false, "@ path"},
        {"@executable_path", true, false, false, "@ path"},
        {"@loader_path/libfoo.dylib", false, true, true, NULL},
        {"@loader_path/libfoo.dylib", false, false, true, NULL},
        {"@loader_path/../lib", true, false, true, NULL},
        {"@loader_path/../lib", true, true, false, "@ path"},
        {"@loader_path", true, true, false, "@ path"},
        {"libfoo.dylib", false, true, false, "relative path"},
        {"./libfoo.dylib", false, false, false, "relative path"},
        {"../lib/libfoo.dylib", false, true, false, "relative path"},
        {"lib", true, true, false, "relative path"},
        {"../lib", true, false, false, "relative path"},
        {"@executable_pathology/x", false, true, true, NULL},   /* not the variable: expands to nothing anyway */
    };
    for (size_t i = 0; i < sizeof dp / sizeof dp[0]; i++) {
        const char *why = "unset";
        CHECK(hao_dep_path_allowed(dp[i].path, dp[i].rpath, dp[i].mainimg, HAO_ENV_OPEN, &why) && !why,
              "dep %s allowed when OPEN", dp[i].path);
        CHECK(hao_dep_path_allowed(dp[i].path, dp[i].rpath, dp[i].mainimg, HAO_ENV_GUARDED, NULL), "dep %s allowed when GUARDED",
              dp[i].path);
        bool ok = hao_dep_path_allowed(dp[i].path, dp[i].rpath, dp[i].mainimg, HAO_ENV_SETUGID, &why);
        CHECK(ok == dp[i].setugid_ok && (ok ? !why : why && strstr(why, dp[i].why)),
              "dep %s (%s of %s) when SETUGID: %s (%s)", dp[i].path, dp[i].rpath ? "LC_RPATH" : "install name",
              dp[i].mainimg ? "the main executable" : "a library", ok ? "allowed" : "refused", why ? why : "-");
    }

    const char *own = "<plist><dict><key>com.apple.security.cs.allow-jit</key><true/>"
                      "<key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>";
    const char *more = "<plist><dict><key>com.apple.security.cs.allow-jit</key><true/>"
                       "<key>com.apple.private.tcc.allow</key><array/></dict></plist>";
    CHECK(!hao_entitlements_foreign(own, strlen(own)), "hao's own entitlements are not foreign");
    CHECK(hao_entitlements_foreign(more, strlen(more)), "a private entitlement is foreign");
    CHECK(hao_entitlements_foreign("<key>com.apple", 14), "malformed entitlements count as foreign");
    CHECK(!hao_entitlements_foreign("", 0), "no entitlements are not foreign");
}

/* -------------------------------------------------------- locations */
static bool bad_prefix(const char *p)
{
    return !strncmp(p, "/tmp", 4) || !strncmp(p, "/private/tmp", 12) || !strncmp(p, "/Library", 8) || !strncmp(p, "/var/tmp", 8);
}

static void test_dirs(void)
{
    hao_aot_dir d[HAO_AOT_MAX_DIRS];
    int w, k;
    char why[256];
    struct { int tier; uid_t euid; const char *pw, *cache; int n; const char *dirs[3]; const char *wr; const char *what; } c[] = {
        {HAO_ENV_GUARDED, 0, NULL, NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, HAO_AOT_SYS_DIR, "PID 1 / root daemon, no HOME"},
        {HAO_ENV_GUARDED, 0, NULL, "/tmp/evil", 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, HAO_AOT_SYS_DIR, "root daemon ignores HAO_CACHE"},
        {HAO_ENV_SETUGID, 0, "/Users/x", "/Users/x/evil", 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "setuid root: system only, no write"},
        {HAO_ENV_SETUGID, 501, "/Users/x", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "setgid user: no user dir"},
        {HAO_ENV_GUARDED, 501, "/Users/x", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "entitled user process: no user dir"},
        {HAO_ENV_GUARDED, 0, "/var/root", "/var/root/c", 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, HAO_AOT_SYS_DIR,
         "root shell: HAO_CACHE and its home ignored"},
        {HAO_ENV_OPEN, 501, NULL, NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "user without a home: no write"},
        {HAO_ENV_OPEN, 501, "/Users/x", NULL, 3, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR, "/Users/x/Library/Caches/hao"},
         "/Users/x/Library/Caches/hao", "ordinary user"},
        {HAO_ENV_OPEN, 501, "/Users/x/", NULL, 3, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR, "/Users/x/Library/Caches/hao"},
         "/Users/x/Library/Caches/hao", "home with trailing slash"},
        {HAO_ENV_OPEN, 501, "", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "home \"\""},
        {HAO_ENV_OPEN, 501, "/", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "home \"/\""},
        {HAO_ENV_OPEN, 501, "relative", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "relative home"},
        {HAO_ENV_OPEN, 4294967294u, "/var/empty", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "_nobody"},
        {HAO_ENV_OPEN, 200, "/var/db/foo", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "system account uid 200"},
        {HAO_ENV_OPEN, 501, "/var/empty", NULL, 2, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR}, NULL, "home /var/empty"},
        {HAO_ENV_OPEN, 501, "/Users/x", "/Users/x/c", 1, {"/Users/x/c"}, "/Users/x/c", "HAO_CACHE is exclusive"},
        {HAO_ENV_OPEN, 501, "/Users/x", "rel/c", 0, {NULL}, NULL, "relative HAO_CACHE: AOT off"},
        {HAO_ENV_OPEN, 501, "/Users/x", "", 3, {HAO_AOT_SYS_RO_DIR, HAO_AOT_SYS_DIR, "/Users/x/Library/Caches/hao"},
         "/Users/x/Library/Caches/hao", "empty HAO_CACHE is unset"},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        int n = hao_aot_dirs_for(c[i].tier, c[i].euid, c[i].pw, c[i].cache, d, &w, why, sizeof why);
        bool ok = n == c[i].n;
        for (k = 0; ok && k < n; k++) ok = !strcmp(d[k].path, c[i].dirs[k]) && (c[i].cache && c[i].cache[0] ? true : !bad_prefix(d[k].path));
        ok = ok && (c[i].wr ? w >= 0 && !strcmp(d[w].path, c[i].wr) : w < 0);
        CHECK(ok, "dirs: %s (got %d dirs%s%s, write %s)", c[i].what, n, n ? ", first " : "", n ? d[0].path : "", w >= 0 ? d[w].path : "none");
    }
    hao_aot_dirs_for(HAO_ENV_OPEN, 501, "/Users/x", "rel/c", d, &w, why, sizeof why);
    CHECK(strstr(why, "not absolute") != NULL, "relative HAO_CACHE says why (%s)", why);

    /* interface units (hao/bridge.h): HAO_IF_DIR replaces the system directory, OPEN only */
    static const struct { int tier; const char *dir; int n, kind; const char *path, *what; } u[] = {
        {HAO_ENV_OPEN, NULL, 1, HAO_AOT_SYS_RO, HAO_IF_SYS_DIR, "units: system directory"},
        {HAO_ENV_OPEN, "", 1, HAO_AOT_SYS_RO, HAO_IF_SYS_DIR, "units: empty HAO_IF_DIR is unset"},
        {HAO_ENV_OPEN, "/Users/x/if", 1, HAO_AOT_EXPLICIT, "/Users/x/if", "units: HAO_IF_DIR is exclusive"},
        {HAO_ENV_OPEN, "rel/if", 0, 0, NULL, "units: relative HAO_IF_DIR: no units"},
        {HAO_ENV_GUARDED, "/Users/x/if", 1, HAO_AOT_SYS_RO, HAO_IF_SYS_DIR, "units: HAO_IF_DIR ignored when GUARDED"},
        {HAO_ENV_SETUGID, "/Users/x/if", 1, HAO_AOT_SYS_RO, HAO_IF_SYS_DIR, "units: HAO_IF_DIR ignored when SETUGID"},
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) {
        int n = hao_if_dirs_for(u[i].tier, u[i].dir, d, why, sizeof why);
        CHECK(n == u[i].n && (!n || (d[0].kind == u[i].kind && !strcmp(d[0].path, u[i].path))), "%s (got %d%s%s)", u[i].what, n,
              n ? ", " : "", n ? d[0].path : "");
    }
    hao_if_dirs_for(HAO_ENV_OPEN, "rel/if", d, why, sizeof why);
    CHECK(strstr(why, "not absolute") != NULL, "relative HAO_IF_DIR says why (%s)", why);
}

/* -------------------------------------------------- owner/mode rules */
static struct stat mk(mode_t type, mode_t perm, uid_t uid)
{
    struct stat st;
    memset(&st, 0, sizeof st);
    st.st_mode = type | perm;
    st.st_uid = uid;
    return st;
}

static void test_stat_rules(const char *tmp)
{
    char why[256];
    uid_t me = geteuid();
    struct stat st = mk(S_IFREG, 0644, 0);
    CHECK(hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_FILE, me, why, sizeof why), "root 0644 file ok in SYS");
    st = mk(S_IFREG, 0644, me);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_FILE, me, why, sizeof why), "user-owned file rejected in SYS");
    CHECK(hao_aot_stat_ok(&st, HAO_AOT_USER, HAO_AOT_FILE, me, why, sizeof why), "user-owned file ok in USER");
    st = mk(S_IFREG, 0644, me + 1);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_EXPLICIT, HAO_AOT_FILE, me, why, sizeof why), "other user's file rejected");
    st = mk(S_IFREG, 0664, 0);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_FILE, me, why, sizeof why), "group-writable file rejected");
    st = mk(S_IFREG, 0646, 0);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_FILE, me, why, sizeof why), "world-writable file rejected");
    st = mk(S_IFLNK, 0755, 0);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_FILE, me, why, sizeof why), "symlink file rejected");
    st = mk(S_IFIFO, 0644, 0);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_FILE, me, why, sizeof why), "FIFO rejected");
    st = mk(S_IFDIR, 01777, 0);
    CHECK(hao_aot_stat_ok(&st, HAO_AOT_USER, HAO_AOT_ANCESTOR, me, why, sizeof why), "sticky 1777 ancestor ok for USER");
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_SYS, HAO_AOT_ANCESTOR, me, why, sizeof why), "sticky ancestor rejected for SYS");
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_USER, HAO_AOT_DIR, me, why, sizeof why), "sticky 1777 cache dir rejected");
    st = mk(S_IFDIR, 0775, 0);
    CHECK(!hao_aot_stat_ok(&st, HAO_AOT_USER, HAO_AOT_ANCESTOR, me, why, sizeof why), "0775 ancestor rejected");

    /* real directories */
    char p[1024], q[1024], r[1024];
    snprintf(p, sizeof p, "%s/d755", tmp); mkdir(p, 0755); chmod(p, 0755);
    CHECK(hao_aot_dir_ok(p, HAO_AOT_EXPLICIT, me, why, sizeof why), "0755 dir ok (%s)", why);
    CHECK(!hao_aot_dir_ok(p, HAO_AOT_SYS, me, why, sizeof why), "user-owned dir rejected for SYS");
    snprintf(p, sizeof p, "%s/d775", tmp); mkdir(p, 0775); chmod(p, 0775);
    CHECK(!hao_aot_dir_ok(p, HAO_AOT_EXPLICIT, me, why, sizeof why), "0775 dir rejected");
    snprintf(p, sizeof p, "%s/d1777", tmp); mkdir(p, 01777); chmod(p, 01777);
    CHECK(!hao_aot_dir_ok(p, HAO_AOT_EXPLICIT, me, why, sizeof why), "1777 cache dir rejected");
    snprintf(q, sizeof q, "%s/d1777/c", tmp); mkdir(q, 0700); chmod(q, 0700);
    CHECK(hao_aot_dir_ok(q, HAO_AOT_EXPLICIT, me, why, sizeof why), "0700 dir under a sticky 1777 dir ok (%s)", why);
    snprintf(p, sizeof p, "%s/d775/c", tmp); mkdir(p, 0700); chmod(p, 0700);
    CHECK(!hao_aot_dir_ok(p, HAO_AOT_EXPLICIT, me, why, sizeof why), "0700 dir under a 0775 dir rejected");
    snprintf(p, sizeof p, "%s/d755", tmp);
    snprintf(r, sizeof r, "%s/link", tmp); unlink(r); symlink(p, r);
    CHECK(!hao_aot_dir_ok(r, HAO_AOT_EXPLICIT, me, why, sizeof why), "symlinked cache dir rejected");
    CHECK(!hao_aot_literal_ok(r, why, sizeof why), "user-owned symlink in the configured path rejected");
    CHECK(hao_aot_literal_ok("/tmp/nonexistent-hao-test/c", why, sizeof why), "root-owned /tmp symlink in the configured path ok");
    CHECK(!hao_aot_dir_ok("/private/tmp", HAO_AOT_EXPLICIT, me, why, sizeof why), "/private/tmp itself rejected");
    CHECK(!hao_aot_dir_ok("/Library/Caches", HAO_AOT_EXPLICIT, me, why, sizeof why), "/Library/Caches rejected");
    CHECK(!hao_aot_dir_ok("relative", HAO_AOT_EXPLICIT, me, why, sizeof why), "relative path rejected");
}

/* ------------------------------------------------------ format checks */
static uint8_t gen[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static uint8_t uuid[16] = {0xaa, 0xbb, 0xcc};
static uint64_t rlo[1] = {0x4000}, rhi[1] = {0x8000};

/* A valid file: 3 blocks at code 16/32/48, one link from block 0 to block 2. */
static size_t build(uint8_t *f, hao_aot_expect *x)
{
    hao_aot_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, HAO_AOT_MAGIC, 8);
    h.version = HAO_AOT_VERSION;
    h.hdr_size = sizeof h;
    memcpy(h.uuid, uuid, 16);
    memcpy(h.gen, gen, 16);
    h.vmsize = 0x10000;
    h.guest_id = 0x1234;
    h.nblocks = 3;
    h.nlinks = 1;
    h.code_size = 64;
    hao_aot_ent e[3] = {{0x4000, 16}, {0x4010, 32}, {0x4100, 48}};
    hao_aot_link l = {20, 2};
    uint8_t code[64];
    memset(code, 0x90, sizeof code);
    int32_t rel = 48 - (20 + 4);
    memcpy(code + 20, &rel, 4);
    h.file_size = sizeof h + sizeof e + sizeof l + sizeof code;
    uint8_t *q = f + sizeof h;
    memcpy(q, e, sizeof e); q += sizeof e;
    memcpy(q, &l, sizeof l); q += sizeof l;
    memcpy(q, code, sizeof code);
    h.payload_hash = hao_aot_hash64(f + sizeof h, (size_t)(h.file_size - sizeof h), 0);
    memcpy(f, &h, sizeof h);
    *x = (hao_aot_expect){uuid, gen, 0x10000, 0x1234, rlo, rhi, 1};
    return (size_t)h.file_size;
}

static void rehash(uint8_t *f, size_t n)
{
    hao_aot_hdr *h = (hao_aot_hdr *)f;
    h->payload_hash = hao_aot_hash64(f + sizeof *h, n - sizeof *h, 0);
}

/* After an accepted check, verify independently that the loader would only
 * touch memory inside the file and the image. */
static bool view_in_bounds(const uint8_t *f, size_t n, const hao_aot_view *v, const hao_aot_expect *x)
{
    const hao_aot_hdr *h = v->hdr;
    if ((const uint8_t *)v->code < f || (const uint8_t *)v->code + h->code_size != f + n) return false;
    for (uint32_t k = 0; k < h->nblocks; k++)
        if (v->ents[k].code_off >= h->code_size || v->ents[k].guest_off >= x->vmsize || (v->ents[k].code_off & 15)) return false;
    return true;
}

static void test_format(void)
{
    static uint64_t store[1024];
    uint8_t *f = (uint8_t *)store;
    hao_aot_expect x;
    hao_aot_view v;
    char why[256];
    size_t n = build(f, &x);
    CHECK(hao_aot_check(f, n, &x, &v, why, sizeof why) == 0, "valid cache accepted (%s)", why);
    hao_aot_hdr *h = (hao_aot_hdr *)f;
    hao_aot_ent *e = (hao_aot_ent *)(f + sizeof *h);
    hao_aot_link *l = (hao_aot_link *)(f + sizeof *h + 3 * sizeof *e);
#define MUT(what, stmt, hash, needle) do { n = build(f, &x); stmt; if (hash) rehash(f, n); \
        int r_ = hao_aot_check(f, n, &x, &v, why, sizeof why); \
        CHECK(r_ != 0 && strstr(why, needle), "%s rejected (%s)", what, r_ ? why : "accepted"); } while (0)
    MUT("bad magic", f[0] ^= 1, 0, "not a hao");
    MUT("version 4", h->version = 4, 0, "format version");
    MUT("header size", h->hdr_size = 64, 0, "header size");
    MUT("other guest uuid", h->uuid[3] ^= 1, 0, "UUID");
    MUT("header from another hao build", h->gen[15] ^= 1, 0, "another hao build");
    MUT("vmsize", h->vmsize += 0x1000, 0, "vmsize");
    MUT("guest load commands", h->guest_id ^= 1, 0, "load commands");
    MUT("file_size field", h->file_size += 8, 0, "header says");
    MUT("truncated file", n -= 8, 0, "header says");
    MUT("truncated to the header", n = 40, 0, "truncated");
    MUT("code_size + 65536 wrap", h->code_size = 0xFFFFFFFFFFFF0000ull, 0, "do not add up");
    MUT("code_size larger", h->code_size += 16, 0, "do not add up");
    MUT("nblocks huge", h->nblocks = 0xffffffffu, 0, "blocks for");
    MUT("nblocks + 1", h->nblocks += 1, 0, "do not add up");
    MUT("nlinks huge", h->nlinks = 0x40000000u, 0, "do not add up");
    MUT("payload byte flipped, hash not fixed", f[n - 1] ^= 1, 0, "hash");
    MUT("entry byte flipped, hash not fixed", e[0].code_off ^= 0x100, 0, "hash");
    MUT("code_off 0x7ffffff0", e[1].code_off = 0x7ffffff0, 1, "outside the code area");
    MUT("code_off = code_size", e[2].code_off = 64, 1, "outside the code area");
    MUT("code_off in the base slot", e[0].code_off = 0, 1, "outside the code area");
    MUT("code_off misaligned", e[1].code_off = 33, 1, "not 16-aligned");
    MUT("code_off not increasing", e[2].code_off = 32, 1, "not increasing");
    MUT("guest_off 0xfffffff0", e[1].guest_off = 0xfffffff0u, 1, "outside the image");
    MUT("guest_off outside code ranges", e[1].guest_off = 0x100, 1, "outside the image");
    MUT("guest_off at the end of the code range", e[1].guest_off = 0x8000, 1, "outside the image");
    MUT("guest_off misaligned", e[1].guest_off = 0x4012, 1, "not 4-aligned");
    MUT("link target out of range", l->target = 3, 1, "target block");
    MUT("link site past the end", l->site = 62, 1, "outside the code area");
    MUT("link site in the base slot", l->site = 4, 1, "outside the code area");
    MUT("link rel32 changed", f[n - 64 + 20] ^= 4, 1, "does not reach");
    n = build(f, &x);
    CHECK(hao_aot_check(f + 4, n - 4, &x, &v, why, sizeof why) != 0, "unaligned buffer rejected");

    /* random mutations, rehashed, so they reach every check: never accept a
     * view that would leave the file or the image */
    srandom(12345);
    int accepted = 0, rejected = 0, bad = 0;
    for (int it = 0; it < 200000; it++) {
        n = build(f, &x);
        int nm = 1 + (int)(random() % 3);
        for (int m = 0; m < nm; m++) {
            size_t at = (size_t)random() % n;
            if (random() & 1) f[at] ^= (uint8_t)(1u << (random() % 8));
            else f[at] = (uint8_t)random();
        }
        if (random() % 8 == 0) n = (size_t)random() % (n + 1);
        if (n >= sizeof(hao_aot_hdr) && random() % 2) rehash(f, n);
        if (hao_aot_check(f, n, &x, &v, why, sizeof why) == 0) { accepted++; if (!view_in_bounds(f, n, &v, &x)) bad++; }
        else rejected++;
    }
    CHECK(bad == 0, "fuzz: %d accepted, %d rejected, %d accepted out of bounds", accepted, rejected, bad);
    printf("fuzz: 200000 mutations, %d accepted, %d rejected, %d out of bounds\n", accepted, rejected, bad);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: policy TMPDIR\n"); return 2; }
    test_tiers();
    test_dirs();
    test_stat_rules(argv[1]);
    test_format();
    printf("%s policy: %d checks passed, %d failed\n", fails ? "FAIL" : "PASS", passes, fails);
    return fails ? 1 : 0;
}
