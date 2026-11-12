/*
 * Unit test of hao_opts_parse (src/opts.c, linked from libhaocore.a): the
 * switch snapshot's precedence, boot-argument matching and environment tiers.
 */
#include "hao/opts.h"
#include "hao/aot.h"
#include <stdio.h>
#include <string.h>

static int fails, n;
#define CHECK(c, ...) do { n++; if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    hao_opts_t o;
    char *none[] = {NULL};
    const char *ba = "hao=vi hao_verbose=2 hao_no_chain=1 hao_crash_trace=1";
    hao_opts_parse(&o, none, NULL, ba, true, HAO_ENV_GUARDED);
    CHECK(o.verbose == 2 && o.interp && o.no_chain && o.crash_trace, "PID 1 boot args: verbose %d interp %d", o.verbose, o.interp);
    hao_opts_parse(&o, none, NULL, ba, false, HAO_ENV_OPEN);
    CHECK(o.verbose == 0 && !o.interp && o.no_chain && o.crash_trace, "hao=/hao_verbose= are PID 1 only; knobs everywhere");
    char *e3[] = {"HAO_NO_CLOSURES=1", "HAO_VERBOSE=1", "HAO_CACHE=/x", "HAO_GUEST_ROOT=/g", NULL};
    hao_opts_parse(&o, e3, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(o.no_closures && o.verbose == 1 && !strcmp(o.cache, "/x") && !strcmp(o.guest_root, "/g"), "environment, open");
    hao_opts_parse(&o, none, NULL, "hao_no_chainx=1 xhao_no_aot=1 hao_no_aot=10 hao_no_swiftreg hao_no_msgsend=0", false, HAO_ENV_OPEN);
    CHECK(!o.no_chain && !o.no_aot && !o.no_swiftreg && !o.no_msgsend, "boot arguments match exactly (key and =1)");
    hao_opts_parse(&o, none, NULL, "hao_no_chain=1 hao_no_aot=1 hao_nx_stats=1", false, HAO_ENV_SETUGID);
    CHECK(o.no_chain && o.no_aot && o.nx_stats, "listed boot arguments honoured in every tier");
    char *e6[] = {"HAO_NO_CHAIN=", NULL};
    hao_opts_parse(&o, e6, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(o.no_chain, "an empty value counts as present");
    char *e7[] = {"HAO_VERBOSE=3", NULL};
    hao_opts_parse(&o, e7, "vs", NULL, false, HAO_ENV_OPEN);
    CHECK(o.verbose == 3 && o.stats, "environment beats HAO_FLAGS");
    char *e8[] = {"HAO_CACHE=/evil", "HAO_GUEST_ROOT=/evil", "HAO_AOT_LEARN=1", "HAO_NO_CLOSURES=1", "HAO_VERBOSE=2",
                  "HAO_NO_AOT=1", "HAO_INTERP=1", NULL};
    hao_opts_parse(&o, e8, "s", NULL, false, HAO_ENV_GUARDED);
    CHECK(!o.cache[0] && !o.guest_root[0] && !o.aot_learn && !o.no_closures && !o.stats && o.verbose == 2 && o.no_aot && o.interp,
          "guarded: code/path/bridging/flags ignored, diagnostics and off switches kept");
    CHECK(strstr(o.ignored, "HAO_CACHE") && strstr(o.ignored, "HAO_FLAGS") && strstr(o.ignored, "HAO_AOT_LEARN"), "ignored list: %s", o.ignored);
    hao_opts_parse(&o, e8, "v", NULL, false, HAO_ENV_SETUGID);
    CHECK(!o.cache[0] && !o.guest_root[0] && o.verbose == 0 && !o.no_aot && !o.interp && !o.stats, "setugid: no switch at all");
    CHECK(strstr(o.ignored, "HAO_NO_AOT") && strstr(o.ignored, "HAO_INTERP") && strstr(o.ignored, "HAO_VERBOSE"),
          "setugid ignored list: %s", o.ignored);
    char *e10[] = {"HAO_NO_LIBVAL=1", NULL};
    hao_opts_parse(&o, e10, NULL, NULL, false, HAO_ENV_GUARDED);
    CHECK(!o.no_libval && strstr(o.ignored, "HAO_NO_LIBVAL"), "HAO_NO_LIBVAL ignored in a guarded process");
    hao_opts_parse(&o, e10, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(o.no_libval, "HAO_NO_LIBVAL in an ordinary process");
    hao_opts_parse(&o, none, NULL, "hao_no_libval=1", true, HAO_ENV_GUARDED);
    CHECK(!o.no_libval, "no boot argument turns library validation off (PID 1)");
    hao_opts_parse(&o, none, NULL, "hao_no_libval=1", false, HAO_ENV_OPEN);
    CHECK(!o.no_libval, "no boot argument turns library validation off (ordinary process)");
    hao_opts_parse(&o, none, NULL, "hao=u hao=vu", true, HAO_ENV_GUARDED);
    CHECK(!o.allow_unsigned && o.verbose == 1, "boot argument hao=u does not allow unsigned guests");
    /* the boot-argument list (hao/opts.h): every knob by name, in PID 1, a daemon and a set-ID process */
    {
        static const char *yes[] = {"nx_stats", "crash_trace", "bridge_stats", "no_aot", "no_chain", "jit_helpers_only", NULL};
        static const char *dev[] = {"no_msgsend", "no_closures", "no_swiftreg", "no_block_thunks", "no_fixup_thunks",
                                    "no_struct_thunks", "bridge_check", "no_varsniff", "no_ifunits", "no_swift_thunks",
                                    "no_swift_values", "native_libcxx", "no_thunks", "sigreturn_kernel", "no_probe", NULL};
        static const char *never[] = {"no_libval", "aot_learn", "no_jit_memory", "bridge_abort", "sigreturn_user", "code_alias",
                                      "libval_user", "swift_value_strict", "interp", "stats", "allow_unsigned", "watchdog", "cache", "guest_root",
                                      "if_dir", "trust_list", "diag_dir", "async_contract_library", "aot_limit_kb",
                                      "jit_cache_mb", "swift_pop", NULL};
        static const int tiers[] = {HAO_ENV_OPEN, HAO_ENV_GUARDED, HAO_ENV_SETUGID};
        hao_opts_t zero;
        hao_opts_parse(&zero, none, NULL, NULL, false, HAO_ENV_OPEN);
        int nknobs = 0;
        for (int pass = 0; pass < 3; pass++) {
            const char **l = pass == 0 ? yes : pass == 1 ? dev : never;
            bool want = pass == 0 || (pass == 1 && hao_test_switches);
            for (; *l; l++, nknobs += pass < 2) {
                char arg[96], up[64] = "HAO_";
                snprintf(arg, sizeof arg, "hao_%s=1 hao_%s=/x", *l, *l);
                for (int t = 0; t < 3; t++) {
                    hao_opts_parse(&o, none, NULL, arg, t == 1, tiers[t]);
                    o.ready = zero.ready;
                    bool set = memcmp(&o, &zero, sizeof o) != 0;
                    CHECK(set == want, "boot argument hao_%s=1 %s at tier %d", *l, want ? "read" : "not read", tiers[t]);
                }
                /* what a release build reads from the boot arguments, a guarded process may read from its environment */
                for (size_t i = 0; (*l)[i]; i++) up[4 + i] = (char)((*l)[i] >= 'a' && (*l)[i] <= 'z' ? (*l)[i] - 32 : (*l)[i]);
                if (pass == 0) CHECK(hao_env_switch_allowed(up, HAO_ENV_GUARDED), "%s: boot-listed, so guarded-allowed", up);
                if (pass == 1) CHECK(!hao_env_switch_allowed(up, HAO_ENV_GUARDED) && !hao_env_test_switch(up), "%s: bring-up knob", up);
            }
        }
        CHECK(nknobs == 21, "boot-argument lists name 21 knobs (%d)", nknobs);
    }
    /* the test switches: read from an ordinary environment by a HAO_TEST_SWITCHES build only */
    {
        char *et[] = {"HAO_LIBVAL_USER=1", "HAO_SIGRETURN_USER=1", "HAO_AOT_LIMIT_KB=64", "HAO_BRIDGE_ABORT=1",
                      "HAO_NO_JIT_MEMORY=1", "HAO_CODE_ALIAS=1", NULL};
        hao_opts_parse(&o, et, NULL, NULL, false, HAO_ENV_OPEN);
        bool all = o.libval_user && o.sigreturn_user && o.aot_limit_kb == 64 && o.bridge_abort && o.no_jit_memory && o.code_alias;
        bool any = o.libval_user || o.sigreturn_user || o.aot_limit_kb || o.bridge_abort || o.no_jit_memory || o.code_alias;
        if (hao_test_switches) CHECK(all, "test switches read in a test build (open)");
        else CHECK(!any && strstr(o.ignored, "HAO_CODE_ALIAS") && strstr(o.ignored, "HAO_LIBVAL_USER"),
                   "test switches not read by a build without HAO_TEST_SWITCHES (ignored: %s)", o.ignored);
        hao_opts_parse(&o, et, NULL, NULL, false, HAO_ENV_GUARDED);
        any = o.libval_user || o.sigreturn_user || o.aot_limit_kb || o.bridge_abort || o.no_jit_memory || o.code_alias;
        CHECK(!any, "test switches never read in a guarded process");
    }
    /* switches that make a correct program incorrect or take a signal are not "off switches": open tier only */
    {
        char *eb[] = {"HAO_NO_VARSNIFF=1", "HAO_NO_THUNKS=1", "HAO_NATIVE_LIBCXX=1", "HAO_SIGRETURN_KERNEL=1", "HAO_NO_PROBE=1",
                      "HAO_WATCHDOG=5", "HAO_NO_IFUNITS=1", "HAO_BRIDGE_CHECK=1", NULL};
        hao_opts_parse(&o, eb, NULL, NULL, false, HAO_ENV_OPEN);
        CHECK(o.no_varsniff && o.no_thunks && o.native_libcxx && o.sigreturn_kernel && o.no_probe && o.watchdog == 5 &&
              o.no_ifunits && o.bridge_check, "bridging, signal and watchdog switches, open");
        for (int t = HAO_ENV_GUARDED; t <= HAO_ENV_SETUGID; t++) {
            hao_opts_parse(&o, eb, NULL, NULL, false, t);
            CHECK(!o.no_varsniff && !o.no_thunks && !o.native_libcxx && !o.sigreturn_kernel && !o.no_probe && !o.watchdog &&
                  !o.no_ifunits && !o.bridge_check && strstr(o.ignored, "HAO_WATCHDOG") && strstr(o.ignored, "HAO_NO_VARSNIFF"),
                  "bridging, signal and watchdog switches ignored at tier %d (%s)", t, o.ignored);
        }
    }
    /* the typed boundary (hao/bridge.h): stats prints; check binds more, no_ifunits changes how calls are made and
     * HAO_IF_DIR chooses code (an interface unit decides how native calls are made) */
    char *e11[] = {"HAO_BRIDGE_STATS=1", "HAO_BRIDGE_CHECK=1", "HAO_NO_IFUNITS=1", "HAO_IF_DIR=/u", NULL};
    hao_opts_parse(&o, e11, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(o.bridge_stats && o.bridge_check && o.no_ifunits && !strcmp(o.if_dir, "/u"), "bridge switches, open");
    hao_opts_parse(&o, e11, NULL, NULL, false, HAO_ENV_GUARDED);
    CHECK(o.bridge_stats && !o.bridge_check && !o.no_ifunits && !o.if_dir[0] && strstr(o.ignored, "HAO_IF_DIR"),
          "bridge switches, guarded: only HAO_BRIDGE_STATS");
    hao_opts_parse(&o, e11, NULL, NULL, false, HAO_ENV_SETUGID);
    CHECK(!o.bridge_stats && !o.bridge_check && !o.no_ifunits && !o.if_dir[0], "bridge switches, setugid: none");
    /* HAO_DIAG_DIR names a directory hao creates a file in: ordinary processes only */
    char *e12[] = {"HAO_DIAG_DIR=/d", NULL};
    hao_opts_parse(&o, e12, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(!strcmp(o.diag_dir, "/d"), "HAO_DIAG_DIR, open");
    hao_opts_parse(&o, e12, NULL, NULL, false, HAO_ENV_GUARDED);
    CHECK(!o.diag_dir[0] && strstr(o.ignored, "HAO_DIAG_DIR"), "HAO_DIAG_DIR ignored in a guarded process");
    hao_opts_parse(&o, none, NULL, "hao_bridge_stats=1 hao_jit_helpers_only=1", false, HAO_ENV_SETUGID);
    CHECK(o.bridge_stats && o.jit_helpers_only, "boot arguments hao_bridge_stats=1 hao_jit_helpers_only=1 in every tier");
    char *e9[] = {"HAO_NO_CHAINX=1", "XHAO_NO_AOT=1", "HAO_NO_AO=1", NULL};
    hao_opts_parse(&o, e9, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(!o.no_chain && !o.no_aot, "environment names match exactly");
    hao_opts_parse(&o, none, "V", NULL, false, HAO_ENV_OPEN);
    CHECK(o.verbose == 2, "HAO_FLAGS V alone is -vv (verbose %d)", o.verbose);
    hao_opts_parse(&o, none, NULL, "hao=V", true, HAO_ENV_GUARDED);
    CHECK(o.verbose == 2, "boot argument hao=V alone is -vv (verbose %d)", o.verbose);
    hao_opts_parse(&o, none, "u", NULL, false, HAO_ENV_OPEN);
    CHECK(o.allow_unsigned, "HAO_FLAGS u allows an unsigned guest (open)");
    hao_opts_parse(&o, none, "u", NULL, false, HAO_ENV_GUARDED);
    CHECK(!o.allow_unsigned, "HAO_FLAGS u ignored in a guarded process");
    char provider[]="HAO_ASYNC_CONTRACT_LIBRARY=/contract.dylib";
    char *ep[]={provider,NULL};
    hao_opts_parse(&o,ep,NULL,NULL,false,HAO_ENV_OPEN);
    CHECK(!strcmp(o.async_contract_library,"/contract.dylib"),"async provider, open");
    provider[sizeof provider-2]='x';
    CHECK(!strcmp(o.async_contract_library,"/contract.dylib"),"async provider is a startup snapshot");
    hao_opts_parse(&o,ep,NULL,NULL,false,HAO_ENV_GUARDED);
    CHECK(!o.async_contract_library[0] && strstr(o.ignored,"HAO_ASYNC_CONTRACT_LIBRARY"),"async provider ignored, guarded");
    hao_opts_parse(&o,ep,NULL,NULL,false,HAO_ENV_SETUGID);
    CHECK(!o.async_contract_library[0],"async provider ignored, setugid");
    printf("%s opts: %d/%d ok\n", fails ? "FAIL" : "PASS", n - fails, n);
    return fails != 0;
}
