/*
 * Would hao trust a cache as tools/hao-install.py leaves it in an image?
 *   installed SYSTEM NAME...
 * SYSTEM is a scratch System tree hao-install.py installed a payload into
 * (as this user, so it could not chown to root); NAME a cache file in
 * SYSTEM/usr/lib/hao/aot (HAO_AOT_SYS_RO_DIR, what PID 1 and every daemon
 * search first). For SYSTEM (the image's "/"), each directory down to the
 * cache directory and each file, the real lstat and ACL go through the
 * loader's own rules (src/aot_policy.c: hao_aot_stat_ok, hao_aot_acl_allows,
 * as hao_aot_dir_ok and hao_aot_open_trusted apply them) for the system
 * kind, twice: with the owner the installer gives every path when it runs
 * as root in the image builder (uid 0: os.chown(..., 0, 0)), which must
 * pass, and with the owner this run left (this user), which must fail on
 * the owner alone. Modes, file types and ACLs are the installer's real
 * output. Prints PASS/FAIL per check; exit 1 on any FAIL.
 */
#include "hao/aot.h"
#include <stdio.h>
#include <string.h>
#include <sys/acl.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) { passes++; printf("PASS "); } else { fails++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static void one(const char *path, const char *shown, int role)
{
    static const char *const roles[] = {"ancestor", "cache directory", "cache file"};
    struct stat st;
    char why[256] = "";
    if (lstat(path, &st) != 0) { CHECK(0, "%s: cannot stat", shown); return; }
    acl_t a = acl_get_link_np(path, ACL_TYPE_EXTENDED);
    bool acl_bad = hao_aot_acl_allows(a);
    if (a) acl_free(a);
    CHECK(!acl_bad, "%s (%s): no ACL entry grants write access", shown, roles[role]);
    uid_t me = geteuid();
    struct stat as_root = st;
    as_root.st_uid = 0;
    bool ok = hao_aot_stat_ok(&as_root, HAO_AOT_SYS_RO, role, me, why, sizeof why);
    CHECK(ok, "%s (%s, mode %o): trusted when root-owned%s%s", shown, roles[role], (unsigned)(st.st_mode & 07777),
          ok ? "" : ": ", why);
    if (me != 0) {
        why[0] = 0;
        bool r = hao_aot_stat_ok(&st, HAO_AOT_SYS_RO, role, me, why, sizeof why);
        CHECK(!r && strstr(why, "owned by uid") != NULL, "%s: as left by a non-root install (uid %u): rejected for its owner (%s)",
              shown, (unsigned)st.st_uid, why);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: installed SYSTEM NAME...\n"); return 2; }
    const char *sys = argv[1];
    char p[2048];
    /* SYSTEM is "/", then each element of /usr/lib/hao/aot */
    one(sys, "/", HAO_AOT_ANCESTOR);
    const char *d = HAO_AOT_SYS_RO_DIR;
    for (const char *s = d + 1;; s++) {
        if (*s && *s != '/') continue;
        snprintf(p, sizeof p, "%s%.*s", sys, (int)(s - d), d);
        char shown[256];
        snprintf(shown, sizeof shown, "%.*s", (int)(s - d), d);
        one(p, shown, *s ? HAO_AOT_ANCESTOR : HAO_AOT_DIR);
        if (!*s) break;
    }
    for (int k = 2; k < argc; k++) {
        char shown[256];
        snprintf(p, sizeof p, "%s%s/%s", sys, d, argv[k]);
        snprintf(shown, sizeof shown, "%s/%s", d, argv[k]);
        one(p, shown, HAO_AOT_FILE);
    }
    printf("%s installed: %d checks passed, %d failed\n", fails ? "FAIL" : "PASS", passes, fails);
    return fails ? 1 : 0;
}
