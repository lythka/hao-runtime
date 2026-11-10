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
    hao_opts_parse(&o, none, NULL, "hao_no_block_thunks=1 hao_no_aot=1 hao_nx_stats=1", false, HAO_ENV_SETUGID);
    CHECK(o.no_block_thunks && o.no_aot && o.nx_stats, "boot arguments honoured in every tier");
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
    CHECK(!o.cache[0] && !o.guest_root[0] && o.verbose == 0 && o.no_aot && o.interp, "setugid: only off switches");
    char *e10[] = {"HAO_NO_LIBVAL=1", NULL};
    hao_opts_parse(&o, e10, NULL, NULL, false, HAO_ENV_GUARDED);
    CHECK(!o.no_libval && strstr(o.ignored, "HAO_NO_LIBVAL"), "HAO_NO_LIBVAL ignored in a guarded process");
    hao_opts_parse(&o, e10, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(o.no_libval, "HAO_NO_LIBVAL in an ordinary process");
    hao_opts_parse(&o, none, NULL, "hao_no_libval=1", true, HAO_ENV_GUARDED);
    CHECK(o.no_libval, "boot argument hao_no_libval=1 in PID 1");
    /* the typed boundary (hao/bridge.h): stats and check are diagnostics, no_ifunits turns a feature off,
     * HAO_IF_DIR chooses code (an interface unit decides how native calls are made) */
    char *e11[] = {"HAO_BRIDGE_STATS=1", "HAO_BRIDGE_CHECK=1", "HAO_NO_IFUNITS=1", "HAO_IF_DIR=/u", NULL};
    hao_opts_parse(&o, e11, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(o.bridge_stats && o.bridge_check && o.no_ifunits && !strcmp(o.if_dir, "/u"), "bridge switches, open");
    hao_opts_parse(&o, e11, NULL, NULL, false, HAO_ENV_GUARDED);
    CHECK(o.bridge_stats && o.bridge_check && o.no_ifunits && !o.if_dir[0] && strstr(o.ignored, "HAO_IF_DIR"),
          "bridge switches, guarded: HAO_IF_DIR ignored");
    hao_opts_parse(&o, e11, NULL, NULL, false, HAO_ENV_SETUGID);
    CHECK(!o.bridge_stats && !o.bridge_check && o.no_ifunits && !o.if_dir[0], "bridge switches, setugid: only HAO_NO_IFUNITS");
    /* HAO_DIAG_DIR names a directory hao creates a file in: ordinary processes only */
    char *e12[] = {"HAO_DIAG_DIR=/d", NULL};
    hao_opts_parse(&o, e12, NULL, NULL, false, HAO_ENV_OPEN);
    CHECK(!strcmp(o.diag_dir, "/d"), "HAO_DIAG_DIR, open");
    hao_opts_parse(&o, e12, NULL, NULL, false, HAO_ENV_GUARDED);
    CHECK(!o.diag_dir[0] && strstr(o.ignored, "HAO_DIAG_DIR"), "HAO_DIAG_DIR ignored in a guarded process");
    hao_opts_parse(&o, none, NULL, "hao_bridge_stats=1 hao_no_ifunits=1", false, HAO_ENV_SETUGID);
    CHECK(o.bridge_stats && o.no_ifunits, "boot arguments hao_bridge_stats=1 hao_no_ifunits=1 in every tier");
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
