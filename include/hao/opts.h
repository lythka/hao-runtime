/*
 * hao — the switch snapshot. Every HAO_* switch and hao_* boot argument hao
 * honours is read once, at startup, before any guest code or second thread runs, into
 * hao_opt; readers load its fields. A guest's own setenv cannot change hao's
 * behaviour, and no hot path calls getenv.
 */
#ifndef HAO_OPTS_H
#define HAO_OPTS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int verbose;                /* -v/-vv, HAO_VERBOSE=N, PID 1: hao=v|V, hao_verbose=N */
    bool interp, stats;         /* --interp/HAO_INTERP, --stats (PID 1: hao=i|s) */
    bool allow_unsigned;        /* --allow-unsigned (HAO_FLAGS u, open tier): run an unsigned CLI guest */
    unsigned watchdog;          /* HAO_WATCHDOG=N seconds (open tier: it takes SIGALRM and exits 124) */
    unsigned jit_cache_mb;      /* HAO_JIT_CACHE_MB=N: translated-code space (0 = default) */
    unsigned aot_limit_kb;      /* HAO_AOT_LIMIT_KB=N: AOT loading's memory limit instead of the kernel's (tests; jit.c aot_fits) */
    /* boolean knobs: environment HAO_<NAME>; boot argument hao_<name>=1 for those listed below */
    bool nx_stats, crash_trace, jit_helpers_only, no_aot, no_chain, no_msgsend, no_closures, no_swiftreg,
         no_block_thunks, no_fixup_thunks, no_struct_thunks, aot_learn, no_jit_memory, bridge_check, bridge_abort,
         no_varsniff, no_libval, bridge_stats, no_ifunits, no_swift_thunks, no_thunks, no_swift_values,
         diag_time,              /* HAO_DIAG_TIME: diagnostics lines stamped with the mach clock, and startup phase
                                    lines (runtime.c hao_phase) */
         native_libcxx,          /* HAO_NATIVE_LIBCXX: libc++ stays native (no guest libc++ from the arm64e cache) */
         sigreturn_kernel,       /* HAO_SIGRETURN_KERNEL: hao's signal handlers return through __sigreturn (signals.c) */
         sigreturn_user,         /* HAO_SIGRETURN_USER: resume in user space under Rosetta too (tests; signals.c) */
         no_probe,               /* HAO_NO_PROBE: no fault-tolerant reads; blocks.c reads candidates through Mach (signals.c) */
         code_alias,             /* HAO_CODE_ALIAS: hao reads guest code from an alias, the guest's text pages poisoned (code.c) */
         libval_user,            /* HAO_LIBVAL_USER: library validation's signer checks in user space, as on an x86_64 kernel (tests; libval.c) */
         swift_value_strict;     /* HAO_SWIFT_VALUE_STRICT: a refused Swift value ends the process instead of passing
                                    through unpatched (tests; swiftvalue.c) */
    char cache[1024];           /* HAO_CACHE ("" = unset) */
    char guest_root[1024];      /* HAO_GUEST_ROOT ("" = unset) */
    char if_dir[1024];          /* HAO_IF_DIR: interface units (hao/bridge.h) instead of /usr/lib/hao/if ("" = unset) */
    char async_contract_library[1024]; /* explicitly selected own-code async provider; open tier only */
    char trust_list[1024];      /* HAO_TRUST_LIST: extra cdhash list for CLI guests (guest_sig.c; "" = unset) */
    char diag_dir[1024];        /* HAO_DIAG_DIR: diagnostics log directory (runtime.c; "" = unset: hao_crash=1's) */
    char swift_pop[16];         /* HAO_SWIFT_POP: "signature" or "frame" ("" = both; runtime.c callee_pop) */
    char ignored[512];          /* comma-separated switches the tier ignored */
    bool ready;
} hao_opts_t;

extern hao_opts_t hao_opt;

/* Pure (src/opts.c, in libhaocore.a): fill o from an environment block, the
 * HAO_FLAGS letters (v, V, i, s, u; NULL = none), the kernel boot arguments
 * (NULL = none) and the environment tier (hao/aot.h). Precedence, lowest to
 * highest: defaults, boot arguments, flags, environment.
 *
 * Boot arguments are set by root and apply in every tier, so only these are
 * read: in every process hao_nx_stats=1, hao_crash_trace=1,
 * hao_bridge_stats=1, hao_diag_time=1, hao_no_aot=1, hao_no_chain=1, hao_jit_helpers_only=1;
 * in PID 1 also hao=FLAGS (v, V, i, s) and hao_verbose=N. No boot argument
 * turns a check off (there is no hao_no_libval and no hao=u). A build with
 * HAO_TEST_SWITCHES defined also reads the bring-up knobs that change
 * bridging or signal return (src/opts.c: BOOT_DEV).
 *
 * flags and the environment are filtered by hao_env_switch_allowed(). The
 * test switches (HAO_LIBVAL_USER, HAO_SIGRETURN_USER, HAO_AOT_LIMIT_KB,
 * HAO_BRIDGE_ABORT, HAO_NO_JIT_MEMORY, HAO_CODE_ALIAS) are read only by a
 * HAO_TEST_SWITCHES build, from an open-tier environment. */
void hao_opts_parse(hao_opts_t *o, char **envp, const char *flags, const char *bootargs, bool pid1, int tier);
/* Runtime (src/trust.c): once per process, from environ and kern.bootargs. */
void hao_opts_init(const char *flags);

#endif
