/*
 * hao — how far this process's environment is trusted (hao/aot.h).
 *
 * Embedded stubs run with the program's own identity: its setuid/setgid
 * bits, its entitlements, PID 1. The environment of such a process comes
 * from someone less privileged (or, for PID 1 and daemons, from launchd),
 * so HAO_* switches that choose code, paths, checks or bridging are
 * honoured only in an ordinary user's process (never root's), and never in a
 * program Security.framework answers for as an Apple program (its cdhash in
 * stub-identities or native-identities, hao/idlist.h). The tier is computed
 * once; children re-exec and compute their own.
 */
#include "hao/runtime.h"
#include "hao/aot.h"
#include "hao/idlist.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libkern/OSByteOrder.h>
#include <sys/sysctl.h>

int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_ENTITLEMENTS_BLOB 7
#define CS_OPS_DER_ENTITLEMENTS_BLOB 16
#define CS_OPS_CDHASH 5

/* Is this process listed as an Apple program's stand-in (stub-identities, native-identities)?
 * Its cdhash as the kernel holds it. Unknown counts as not listed (the other facts still apply). */
static bool listed(void)
{
    uint8_t h[20];
    return csops(getpid(), CS_OPS_CDHASH, h, sizeof h) == 0 && hao_idlist_find(h) != NULL;
}

/* Entitlements beyond hao's own (allow-jit, disable-library-validation).
 * Anything we cannot read counts as entitled. */
static bool entitled(void)
{
    static uint8_t buf[1 << 16];
    if (csops(getpid(), CS_OPS_ENTITLEMENTS_BLOB, buf, sizeof buf) != 0) return true;
    uint32_t magic = OSReadBigInt32(buf, 0), len = OSReadBigInt32(buf, 4);
    if (magic == 0 && len == 0) {
        /* no XML entitlements: a DER-only blob is unknown content */
        memset(buf, 0, 8);
        if (csops(getpid(), CS_OPS_DER_ENTITLEMENTS_BLOB, buf, sizeof buf) != 0) return errno != EINVAL && errno != ENOTSUP;
        return OSReadBigInt32(buf, 4) > 8;
    }
    if (magic != 0xfade7171 || len < 8 || len > sizeof buf) return true;
    return hao_entitlements_foreign((const char *)buf + 8, len - 8);
}

static int tier = HAO_ENV_SETUGID;
static pthread_once_t tier_once = PTHREAD_ONCE_INIT;

static void tier_init(void)
{
    hao_proc_facts f = {0};
    f.setugid = issetugid() != 0;
    bool (*restricted)(void) = (bool (*)(void))dlsym(RTLD_DEFAULT, "dyld_process_is_restricted");
    f.restricted = restricted && restricted();
    f.entitled = entitled();
    f.identity = listed();
    f.pid = getpid();
    f.ppid = getppid();
    f.uid = getuid(); f.euid = geteuid();
    f.gid = getgid(); f.egid = getegid();
    /* HAO_DISTRUST_ENV=1|2 lowers trust to GUARDED|SETUGID (tests; can
     * never raise it, so reading it unconditionally is safe). */
    const char *d = getenv("HAO_DISTRUST_ENV");
    f.distrust = d ? atoi(d) : 0;
    tier = hao_env_tier_for(&f);
}

int hao_env_tier(void)
{
    pthread_once(&tier_once, tier_init);
    return tier;
}

const char *hao_env_tier_name(int t)
{
    return t == HAO_ENV_OPEN ? "open" : t == HAO_ENV_GUARDED ? "guarded" : "setugid";
}

/*
 * The switches of hao's own command line, as letters (the CLI's stage 1
 * passes them to stage 2 apart from HAO_FLAGS, main.c). Each is honoured
 * where the environment switch of the same effect is: -v and -vv as
 * HAO_VERBOSE, --interp as HAO_INTERP, --stats and --allow-unsigned in an
 * open process only. So a tier that ignores an inherited HAO_FLAGS does not
 * drop the whole command line with it, and no letter does more here than
 * the environment could. An allowed HAO_VERBOSE still decides the verbosity
 * (the environment beats flags, hao/opts.h).
 */
static void cli_flags(const char *letters, int tier)
{
    static const struct { char c; const char *opt, *as; } sw[] = {
        {'v', "-v", "HAO_VERBOSE"}, {'V', "-vv", "HAO_VERBOSE"}, {'i', "--interp", "HAO_INTERP"},
        {'s', "--stats", NULL}, {'u', "--allow-unsigned", NULL}};
    bool env_verbose = getenv("HAO_VERBOSE") != NULL;
    for (size_t k = 0; k < sizeof sw / sizeof sw[0]; k++) {
        if (!strchr(letters, sw[k].c) || (sw[k].c == 'v' && strchr(letters, 'V'))) continue;
        if (!(sw[k].as ? hao_env_switch_allowed(sw[k].as, tier) : tier == HAO_ENV_OPEN)) {
            size_t l = strlen(hao_opt.ignored);
            snprintf(hao_opt.ignored + l, sizeof hao_opt.ignored - l, "%s%s", l ? "," : "", sw[k].opt);
            continue;
        }
        switch (sw[k].c) {
        case 'v': if (!env_verbose && hao_opt.verbose < 1) hao_opt.verbose = 1; break;
        case 'V': if (!env_verbose && hao_opt.verbose < 2) hao_opt.verbose = 2; break;
        case 'i': hao_opt.interp = true; break;
        case 's': hao_opt.stats = true; break;
        case 'u': hao_opt.allow_unsigned = true; break;
        }
    }
}

/* The switch snapshot: once per process, before any guest code or second
 * thread runs. If kern.bootargs cannot be read (a spawn-time sandbox), the
 * boot-argument knobs stay off. A switch this process's tier ignores is
 * named whatever the verbosity: its caller asked for something that will
 * not happen (an unsigned guest refused, a cache not used). */
static void opts_init(const char *flags, const char *argv_flags)
{
    if (hao_opt.ready) return;
    extern char **environ;
    char ba[1024];
    size_t n = sizeof ba - 1;
    const char *bp = NULL;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) == 0) { ba[n] = 0; bp = ba; }
    int t = hao_env_tier();
    hao_opts_parse(&hao_opt, environ, flags, bp, getpid() == 1, t);
    if (argv_flags) cli_flags(argv_flags, t);
    hao_code_alias_mode = hao_opt.code_alias;   /* hao/code.h: before any image loads */
    if (hao_opt.ignored[0]) {
        char list[sizeof hao_opt.ignored];
        snprintf(list, sizeof list, "%s", hao_opt.ignored);
        char *save = NULL;
        for (char *s = strtok_r(list, ",", &save); s; s = strtok_r(NULL, ",", &save))
            fprintf(HAO_DIAG, "hao: %s ignored (%s process)\n", s, hao_env_tier_name(t));
    }
}

void hao_opts_init(const char *flags) { opts_init(flags, NULL); }
void hao_opts_init_cli(const char *flags, const char *argv_flags) { opts_init(flags, argv_flags); }
