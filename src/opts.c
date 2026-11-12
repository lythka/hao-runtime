/*
 * hao — parse the switch snapshot (hao/opts.h). Pure: no runtime globals,
 * so tests/aot/opts_test.c links it from libhaocore.a.
 */
#include "hao/opts.h"
#include "hao/aot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hao_opts_t hao_opt;

/* A boot argument hao_<knob>=1 reaches every process of the boot, set-ID
 * programs and PID 1 included, and stays set across restarts. So each knob
 * says whether it may come from there:
 *   BOOT_YES  what a guarded process may also take from its environment
 *             (hao/aot.h): the engine and printing switches;
 *   BOOT_DEV  bring-up switches that change how calls are bridged or signals
 *             return: only in a HAO_TEST_SWITCHES build;
 *   BOOT_NO   never: hao_no_libval would turn library validation off for
 *             the whole system, aot_learn writes files from every process,
 *             and the rest are the test switches. */
enum { BOOT_NO, BOOT_DEV, BOOT_YES };
static const struct { const char *name; size_t off; int boot; } knobs[] = {
    {"nx_stats", offsetof(hao_opts_t, nx_stats), BOOT_YES},
    {"crash_trace", offsetof(hao_opts_t, crash_trace), BOOT_YES},
    {"jit_helpers_only", offsetof(hao_opts_t, jit_helpers_only), BOOT_YES},
    {"no_aot", offsetof(hao_opts_t, no_aot), BOOT_YES},
    {"no_chain", offsetof(hao_opts_t, no_chain), BOOT_YES},
    {"no_msgsend", offsetof(hao_opts_t, no_msgsend), BOOT_DEV},
    {"no_closures", offsetof(hao_opts_t, no_closures), BOOT_DEV},
    {"no_swiftreg", offsetof(hao_opts_t, no_swiftreg), BOOT_DEV},
    {"no_block_thunks", offsetof(hao_opts_t, no_block_thunks), BOOT_DEV},
    {"no_fixup_thunks", offsetof(hao_opts_t, no_fixup_thunks), BOOT_DEV},
    {"no_struct_thunks", offsetof(hao_opts_t, no_struct_thunks), BOOT_DEV},
    {"aot_learn", offsetof(hao_opts_t, aot_learn), BOOT_NO},
    {"no_jit_memory", offsetof(hao_opts_t, no_jit_memory), BOOT_NO},
    {"bridge_check", offsetof(hao_opts_t, bridge_check), BOOT_DEV},
    {"bridge_abort", offsetof(hao_opts_t, bridge_abort), BOOT_NO},
    {"no_varsniff", offsetof(hao_opts_t, no_varsniff), BOOT_DEV},
    {"no_libval", offsetof(hao_opts_t, no_libval), BOOT_NO},
    {"bridge_stats", offsetof(hao_opts_t, bridge_stats), BOOT_YES},
    {"no_ifunits", offsetof(hao_opts_t, no_ifunits), BOOT_DEV},
    {"no_swift_thunks", offsetof(hao_opts_t, no_swift_thunks), BOOT_DEV},
    {"no_swift_values", offsetof(hao_opts_t, no_swift_values), BOOT_DEV},
    {"native_libcxx", offsetof(hao_opts_t, native_libcxx), BOOT_DEV},
    {"no_thunks", offsetof(hao_opts_t, no_thunks), BOOT_DEV},
    {"sigreturn_kernel", offsetof(hao_opts_t, sigreturn_kernel), BOOT_DEV},
    {"sigreturn_user", offsetof(hao_opts_t, sigreturn_user), BOOT_NO},
    {"no_probe", offsetof(hao_opts_t, no_probe), BOOT_DEV},
    {"code_alias", offsetof(hao_opts_t, code_alias), BOOT_NO},
    {"libval_user", offsetof(hao_opts_t, libval_user), BOOT_NO},
    {"swift_value_survey", offsetof(hao_opts_t, swift_value_survey), BOOT_YES},
};
#define NKNOBS (sizeof knobs / sizeof knobs[0])

static bool boot_knob_allowed(int boot)
{
    return boot == BOOT_YES || (boot == BOOT_DEV && hao_test_switches);
}

/* HAO_FLAGS / hao=FLAGS letters: v verbose, V more verbose (either alone
 * works), i interpreter, s statistics, and from HAO_FLAGS only (cli) u,
 * allow an unsigned guest: the boot argument must not turn the signature
 * check off. */
static void apply_flags(hao_opts_t *o, const char *f, bool cli)
{
    if (strchr(f, 'V')) o->verbose = 2;
    else if (strchr(f, 'v')) o->verbose = 1;
    if (strchr(f, 'i')) o->interp = true;
    if (strchr(f, 's')) o->stats = true;
    if (cli && strchr(f, 'u')) o->allow_unsigned = true;
}

static void note_ignored(hao_opts_t *o, const char *name)
{
    size_t l = strlen(o->ignored);
    snprintf(o->ignored + l, sizeof o->ignored - l, "%s%s", l ? "," : "", name);
}

/* The value of NAME in envp (NAME= present with an empty value counts). */
static const char *env_find(char **envp, const char *name)
{
    size_t n = strlen(name);
    for (char **e = envp; e && *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

/* An environment switch, if present and allowed at this tier. */
static const char *env_get(hao_opts_t *o, char **envp, const char *name, int tier)
{
    const char *v = env_find(envp, name);
    if (!v) return NULL;
    if (hao_env_switch_allowed(name, tier)) return v;
    note_ignored(o, name);
    return NULL;
}

void hao_opts_parse(hao_opts_t *o, char **envp, const char *flags, const char *bootargs, bool pid1, int tier)
{
    memset(o, 0, sizeof *o);
    if (bootargs) {
        char ba[1024];
        snprintf(ba, sizeof ba, "%s", bootargs);
        char *save = NULL;
        for (char *tok = strtok_r(ba, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            const char *key = tok, *val = eq + 1;
            if (pid1 && !strcmp(key, "hao")) apply_flags(o, val, false);
            else if (pid1 && !strcmp(key, "hao_verbose")) o->verbose = atoi(val);
            else if (!strncmp(key, "hao_", 4) && !strcmp(val, "1"))
                for (size_t k = 0; k < NKNOBS; k++)
                    if (!strcmp(key + 4, knobs[k].name) && boot_knob_allowed(knobs[k].boot))
                        *(bool *)((char *)o + knobs[k].off) = true;
        }
    }
    if (flags) {
        if (hao_env_switch_allowed("HAO_FLAGS", tier)) apply_flags(o, flags, true);
        else if (flags[0]) note_ignored(o, "HAO_FLAGS");
    }
    const char *v;
    if ((v = env_get(o, envp, "HAO_VERBOSE", tier))) o->verbose = atoi(v);
    if (env_get(o, envp, "HAO_INTERP", tier)) o->interp = true;
    if ((v = env_get(o, envp, "HAO_WATCHDOG", tier))) o->watchdog = (unsigned)atoi(v);
    if ((v = env_get(o, envp, "HAO_JIT_CACHE_MB", tier)) && atoi(v) > 0) o->jit_cache_mb = (unsigned)atoi(v);
    if ((v = env_get(o, envp, "HAO_AOT_LIMIT_KB", tier)) && atoi(v) > 0) o->aot_limit_kb = (unsigned)atoi(v);
    for (size_t k = 0; k < NKNOBS; k++) {
        char name[40] = "HAO_";
        for (size_t i = 0; knobs[k].name[i] && i < sizeof name - 5; i++)
            name[4 + i] = (char)(knobs[k].name[i] >= 'a' && knobs[k].name[i] <= 'z' ? knobs[k].name[i] - 32 : knobs[k].name[i]);
        if (env_get(o, envp, name, tier)) *(bool *)((char *)o + knobs[k].off) = true;
    }
    if ((v = env_get(o, envp, "HAO_CACHE", tier))) snprintf(o->cache, sizeof o->cache, "%s", v);
    if ((v = env_get(o, envp, "HAO_GUEST_ROOT", tier))) snprintf(o->guest_root, sizeof o->guest_root, "%s", v);
    if ((v = env_get(o, envp, "HAO_IF_DIR", tier))) snprintf(o->if_dir, sizeof o->if_dir, "%s", v);
    if ((v = env_get(o, envp, "HAO_ASYNC_CONTRACT_LIBRARY", tier))) snprintf(o->async_contract_library, sizeof o->async_contract_library, "%s", v);
    if ((v = env_get(o, envp, "HAO_TRUST_LIST", tier))) snprintf(o->trust_list, sizeof o->trust_list, "%s", v);
    if ((v = env_get(o, envp, "HAO_DIAG_DIR", tier))) snprintf(o->diag_dir, sizeof o->diag_dir, "%s", v);
    if ((v = env_get(o, envp, "HAO_SWIFT_POP", tier))) snprintf(o->swift_pop, sizeof o->swift_pop, "%s", v);
    o->ready = true;
}
