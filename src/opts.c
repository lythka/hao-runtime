/*
 * hao — parse the switch snapshot (hao/opts.h). Pure: no runtime globals,
 * so tests/aot/opts_test.c links it from libhaocore.a.
 */
#include "hao/opts.h"
#include "hao/aot.h"
#include "hao/cpufeat.h"
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
 *             and the rest are the test switches;
 *   BOOT_SURVEY every process, but from the environment only an open one,
 *             and only in a survey build (make SURVEY=1) or a test build:
 *             hao_swift_value_audit, which a survey image sets so that one
 *             boot lists the Swift values every program, daemons included,
 *             would have refused (it accepts their corruption, swiftvalue.c).
 *             A release build ignores it and says so (swift_value_audit_ignored). */
enum { BOOT_NO, BOOT_DEV, BOOT_YES, BOOT_SURVEY };
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
    {"diag_time", offsetof(hao_opts_t, diag_time), BOOT_YES},
    {"no_ifunits", offsetof(hao_opts_t, no_ifunits), BOOT_DEV},
    {"no_swift_thunks", offsetof(hao_opts_t, no_swift_thunks), BOOT_DEV},
    {"no_swift_values", offsetof(hao_opts_t, no_swift_values), BOOT_DEV},
    {"native_app_libs", offsetof(hao_opts_t, native_app_libs), BOOT_NO},
    {"native_libcxx", offsetof(hao_opts_t, native_libcxx), BOOT_DEV},
    {"no_thunks", offsetof(hao_opts_t, no_thunks), BOOT_DEV},
    {"sigreturn_kernel", offsetof(hao_opts_t, sigreturn_kernel), BOOT_DEV},
    {"sigreturn_user", offsetof(hao_opts_t, sigreturn_user), BOOT_NO},
    {"no_probe", offsetof(hao_opts_t, no_probe), BOOT_DEV},
    {"code_alias", offsetof(hao_opts_t, code_alias), BOOT_NO},
    {"libval_user", offsetof(hao_opts_t, libval_user), BOOT_NO},
    {"swift_value_passthrough", offsetof(hao_opts_t, swift_value_passthrough), BOOT_NO},
    {"swift_value_audit", offsetof(hao_opts_t, swift_value_audit), BOOT_SURVEY},
};
#define NKNOBS (sizeof knobs / sizeof knobs[0])

static bool boot_knob_allowed(int boot)
{
    return boot == BOOT_YES || (boot == BOOT_SURVEY && (hao_survey_build || hao_test_switches)) ||
           (boot == BOOT_DEV && hao_test_switches);
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

/* An on/off environment switch: on unless absent, empty or "0", so
 * HAO_INTERP=0 does not turn the interpreter on. */
static bool env_on(hao_opts_t *o, char **envp, const char *name, int tier)
{
    const char *v = env_get(o, envp, name, tier);
    return v && v[0] && strcmp(v, "0");
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
            else if (!strcmp(key, "hao_crash") && !strcmp(val, "1")) o->crash = true;
            else if (!strncmp(key, "hao_", 4) && !strcmp(val, "1"))
                for (size_t k = 0; k < NKNOBS; k++)
                    if (!strcmp(key + 4, knobs[k].name)) {
                        if (boot_knob_allowed(knobs[k].boot)) *(bool *)((char *)o + knobs[k].off) = true;
                        else if (knobs[k].boot == BOOT_SURVEY) o->swift_value_audit_ignored = true;
                    }
        }
    }
    if (flags) {
        if (hao_env_switch_allowed("HAO_FLAGS", tier)) apply_flags(o, flags, true);
        else if (flags[0]) note_ignored(o, "HAO_FLAGS");
    }
    const char *v;
    if ((v = env_get(o, envp, "HAO_VERBOSE", tier))) o->verbose = atoi(v);
    if (env_on(o, envp, "HAO_INTERP", tier)) o->interp = true;
    if ((v = env_get(o, envp, "HAO_WATCHDOG", tier))) o->watchdog = (unsigned)atoi(v);
    if ((v = env_get(o, envp, "HAO_JIT_CACHE_MB", tier)) && atoi(v) > 0) o->jit_cache_mb = (unsigned)atoi(v);
    if ((v = env_get(o, envp, "HAO_AOT_LIMIT_KB", tier)) && atoi(v) > 0) o->aot_limit_kb = (unsigned)atoi(v);
    if ((v = env_get(o, envp, "HAO_AOT_LIST_DIR", tier))) snprintf(o->aot_list_dir, sizeof o->aot_list_dir, "%s", v);
    if ((v = env_get(o, envp, "HAO_CPU_FEATURES", tier))) {
        uint64_t keep = 0;
        char why[8];
        if (!hao_cpu_profile_parse(v, HAO_CPU_ALL, &keep, why, sizeof why)) keep = 0;
        o->cpu_clear = HAO_CPU_ALL & ~keep;
    }
    if ((v = env_get(o, envp, "HAO_AOT_FREE_MB", tier))) snprintf(o->aot_free_mb, sizeof o->aot_free_mb, "%s", v);
    if (env_on(o, envp, "HAO_CRASH", tier)) o->crash = true;
    if ((v = env_get(o, envp, "HAO_SWIFT_WALK_FACTS", tier))) snprintf(o->swift_walk_facts, sizeof o->swift_walk_facts, "%s", v);
    for (size_t k = 0; k < NKNOBS; k++) {
        char name[40] = "HAO_";
        for (size_t i = 0; knobs[k].name[i] && i < sizeof name - 5; i++)
            name[4 + i] = (char)(knobs[k].name[i] >= 'a' && knobs[k].name[i] <= 'z' ? knobs[k].name[i] - 32 : knobs[k].name[i]);
        if (env_on(o, envp, name, tier)) *(bool *)((char *)o + knobs[k].off) = true;
        else if (knobs[k].boot == BOOT_SURVEY && (v = env_find(envp, name)) && v[0] && strcmp(v, "0"))
            o->swift_value_audit_ignored = true;
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

uint64_t hao_cpu_features(void) { return hao_cpu_host() & ~hao_opt.cpu_clear; }
