/*
 * hao — interposers: native functions whose arm64 and x86_64 ABIs differ
 * in ways a generic register mapping cannot bridge.
 *
 *  - Variadics. Apple arm64 passes every variadic argument on the stack in
 *    8-byte slots. An x86_64 va_list whose register save areas are marked
 *    exhausted (gp_offset=48, fp_offset=176) reads the overflow area in
 *    exactly that layout, so `printf(fmt, ...)` becomes `vprintf(fmt, va)`
 *    with overflow_arg_area = guest sp. Guest `va_list` (a plain pointer)
 *    passed to v* functions is converted the same way.
 *  - setjmp/longjmp: must capture guest registers, not the host's; a
 *    longjmp to an enclosing segment re-enters it (unwind.c).
 *  - _Unwind_Resume: a guest cleanup pad finished; the guest unwind goes on
 *    with the next guest frame (unwind.c).
 *  - Special-ABI helpers (__chkstk_darwin, TLV thunks).
 *  - Native functions the register bridge cannot call exactly, by the
 *    generated tables: narrow (bool/char/short) results, which Apple arm64
 *    has the callee extend to 32 bits (narrowret_gen.inc); arm64 stack
 *    arguments packed at their natural size where SysV uses 8-byte slots
 *    (stackargs_gen.inc); aggregates by value (sigdb_gen.inc). These keep
 *    their native address: they are one source of the bridge's plans
 *    (hao_tables_plan, src/bridge.c), which the generic native-call path
 *    looks up.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include "hao/vafmt.h"
#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef struct {
    unsigned int gp_offset;
    unsigned int fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
} x86_va_list[1];

static void make_va(x86_va_list va, uint64_t guest_ptr)
{
    va->gp_offset = 48;
    va->fp_offset = 176;
    va->overflow_arg_area = (void *)(uintptr_t)guest_ptr;
    va->reg_save_area = NULL;
}

/* ---- variadic -> v-variant */

typedef struct {
    const char *name;     /* imported symbol (with leading underscore) */
    const char *vname;    /* native v-function */
    int nfixed;           /* integer/pointer fixed arguments */
    int nprefix;          /* zero arguments inserted before the fixed ones (e.g. NULL handle) */
} vararg_map;

static const vararg_map varargs[] = {
    {"_printf", "vprintf", 1},       {"_fprintf", "vfprintf", 2},     {"_sprintf", "vsprintf", 2},
    {"_snprintf", "vsnprintf", 3},   {"_asprintf", "vasprintf", 2},   {"_dprintf", "vdprintf", 2},
    {"_scanf", "vscanf", 1},         {"_fscanf", "vfscanf", 2},       {"_sscanf", "vsscanf", 2},
    {"_syslog", "vsyslog", 2},       {"_err", "verr", 2},             {"_errx", "verrx", 2},
    {"_warn", "vwarn", 1},           {"_warnx", "vwarnx", 1},         {"_printf_l", "vprintf_l", 2},
    {"_fprintf_l", "vfprintf_l", 3}, {"_snprintf_l", "vsnprintf_l", 4}, {"_asprintf_l", "vasprintf_l", 3},
    {"_sprintf_l", "vsprintf_l", 3}, {"_wprintf", "vwprintf", 1},     {"_fwprintf", "vfwprintf", 2},
    {"_swprintf", "vswprintf", 3},
    {"___sprintf_chk", "__vsprintf_chk", 4}, {"___snprintf_chk", "__vsnprintf_chk", 5},
    {"_NSLog", "NSLogv", 1},         {"_CFStringCreateWithFormat", "CFStringCreateWithFormatAndArguments", 3},
    /* libxo (FreeBSD-derived tools: wc, ls, df, ps, netstat, ...) */
    {"_xo_emit", "xo_emit_hv", 1, 1}, {"_xo_emit_h", "xo_emit_hv", 2, 0},
    {"_xo_emit_f", "xo_emit_hvf", 2, 1}, {"_xo_emit_hf", "xo_emit_hvf", 3, 0},
    {"_xo_error", "xo_error_hv", 1, 1}, {"_xo_error_h", "xo_error_hv", 2, 0},
    {"_xo_attr", "xo_attr_hv", 2, 1}, {"_xo_attr_h", "xo_attr_hv", 3, 0},
    {"_xo_emit_field", "xo_emit_field_hv", 4, 1}, {"_xo_emit_field_h", "xo_emit_field_hv", 5, 0},
#include "varargs_gen.inc"
};

/* v-functions taking a guest va_list: index of the va_list argument */
typedef struct { const char *name; int va_index; } vlist_map;
static const vlist_map vlists[] = {
    {"_vprintf", 1}, {"_vfprintf", 2}, {"_vsprintf", 2}, {"_vsnprintf", 3}, {"_vasprintf", 2}, {"_vdprintf", 2},
    {"_vscanf", 1}, {"_vfscanf", 2}, {"_vsscanf", 2}, {"_vsyslog", 2}, {"_verr", 2}, {"_verrx", 2}, {"_vwarn", 1},
    {"_vwarnx", 1}, {"_vprintf_l", 2}, {"_vfprintf_l", 3}, {"_vsnprintf_l", 4}, {"_vasprintf_l", 3},
    {"_vsprintf_l", 3}, {"_vwprintf", 1}, {"_vfwprintf", 2}, {"_vswprintf", 3}, {"___vsprintf_chk", 4},
    {"___vsnprintf_chk", 5}, {"_NSLogv", 1}, {"_CFStringCreateWithFormatAndArguments", 3},
    /* libsystem_trace SPI: void os_log_with_args(os_log_t, os_log_type_t, const char *fmt, va_list,
     * void *ret_addr) */
    {"_os_log_with_args", 3},
    /* libsystem_platform private SPI (launchd's early console log) */
    {"__simple_vsnprintf", 3}, {"__simple_vsprintf", 2}, {"__simple_vdprintf", 2}, {"__simple_vesprintf", 3},
#include "vlist_gen.inc"
};

typedef uint64_t (*fn6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* The SDK format attribute of a table entry (fmtattr_gen.inc), when it agrees with the table: the format
 * among the fixed arguments, and the variadic arguments (first >= 0) or a va_list (first == -1) after
 * it. Looked up once per entry. */
static const hao_fmtattr *entry_fmt(const hao_fmtattr **cache, const char *name, int nfixed, bool vlist)
{
    const hao_fmtattr *a = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (a) return a->kind ? a : NULL;
    static const hao_fmtattr none = {0};
    a = hao_fmtattr_func(name);
    if (a && (a->fmt >= nfixed || (vlist ? a->first != -1 : a->first != nfixed))) {
        if (hao_verbose) fprintf(stderr, "hao: %s: format attribute (%d, %d) does not fit %d fixed arguments; format not used\n",
                                 name, a->fmt, a->first, nfixed);
        a = NULL;
    }
    __atomic_store_n(cache, a ? a : &none, __ATOMIC_RELEASE);
    return a;
}

/* fn(prefix zeros..., fixed..., va_list, [registers after the fixed ones when `tail`]);
 * argument `fi` (if >= 0) replaced by `fv` */
static void call_with_va(hao_thread *t, void *fn, int nfixed, int nprefix, uint64_t va_source, bool tail, int fi, uint64_t fv)
{
    hao_cpu *c = &t->cpu;
    if (nfixed + nprefix > 5) {   /* the tables guarantee this; the native call takes 6 registers */
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "va_list bridge with %d fixed arguments is not supported\n", nfixed + nprefix);
        abort();
    }
    x86_va_list va;
    make_va(va, va_source);
    uint64_t a[6] = {0};
    for (int k = 0; k < nfixed; k++) a[k + nprefix] = k == fi ? fv : c->x[k];
    a[nfixed + nprefix] = (uint64_t)(uintptr_t)va;
    if (tail) for (int k = nfixed + 1; k < 6; k++) a[k] = c->x[k];
    c->x[0] = ((fn6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void trap_vararg(hao_thread *t, uint64_t idx)
{
    const vararg_map *m = &varargs[idx];
    static void *cache[sizeof varargs / sizeof *varargs];
    static const hao_fmtattr *fcache[sizeof varargs / sizeof *varargs];
    if (!cache[idx]) cache[idx] = dlsym(RTLD_DEFAULT, m->vname);
    if (!cache[idx]) { fprintf(stderr, "hao: no native %s for %s\n", m->vname, m->name); abort(); }
    const hao_fmtattr *fa = m->nprefix ? NULL : entry_fmt(&fcache[idx], m->name, m->nfixed, false);
    hao_fmtfix fx = {0};
    uint64_t fv = fa ? hao_fmt_fix(fa->kind, t->cpu.x[fa->fmt], &fx) : 0;
    call_with_va(t, cache[idx], m->nfixed, m->nprefix, t->cpu.x[31], false, fa ? fa->fmt : -1, fv);
    hao_fmt_done(&fx);
}

static void trap_vlist(hao_thread *t, uint64_t idx)
{
    const vlist_map *m = &vlists[idx];
    static void *cache[sizeof vlists / sizeof *vlists];
    static const hao_fmtattr *fcache[sizeof vlists / sizeof *vlists];
    if (!cache[idx]) cache[idx] = dlsym(RTLD_DEFAULT, m->name + 1);
    if (!cache[idx]) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "native %s not found\n", m->name + 1); abort(); }
    const hao_fmtattr *fa = entry_fmt(&fcache[idx], m->name, m->va_index, true);
    hao_fmtfix fx = {0};
    uint64_t fv = fa ? hao_fmt_fix(fa->kind, t->cpu.x[fa->fmt], &fx) : 0;
    call_with_va(t, cache[idx], m->va_index, 0, t->cpu.x[m->va_index], true, fa ? fa->fmt : -1, fv);
    hao_fmt_done(&fx);
}

/* variadics whose extra arguments are integers: pass N stack slots */
typedef struct { const char *name; int nfixed; int nextra; } intvar_map;
static const intvar_map intvars[] = {
    {"_open", 2, 1}, {"_openat", 3, 1}, {"_fcntl", 2, 1}, {"_ioctl", 2, 1}, {"_sem_open", 2, 2},
    {"_open$NOCANCEL", 2, 1}, {"_fcntl$NOCANCEL", 2, 1}, {"_openat$NOCANCEL", 3, 1}, {"_shm_open", 2, 1},
    {"_mq_open", 2, 2}, {"_syscall", 1, 8}, {"___open", 2, 1},
};

static void trap_intvar(hao_thread *t, uint64_t idx)
{
    const intvar_map *m = &intvars[idx];
    static void *cache[sizeof intvars / sizeof *intvars];
    if (!cache[idx]) cache[idx] = dlsym(RTLD_DEFAULT, m->name + 1);
    if (!cache[idx]) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "native %s not found\n", m->name + 1); abort(); }
    hao_cpu *c = &t->cpu;
    uint64_t a[9] = {0};
    for (int k = 0; k < m->nfixed; k++) a[k] = c->x[k];
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < m->nextra; k++) a[m->nfixed + k] = sp[k];
    typedef uint64_t (*fn9)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    c->x[0] = ((fn9)cache[idx])(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
}

/* Generic variadics whose extra arguments are integers/pointers: x86_64
 * va_arg reads them in order from the remaining GPRs then the stack, so
 * passing the guest's 8-byte stack slots positionally is exact. */
typedef struct { const char *name; int nfixed; } gvar_map;
static const gvar_map gvars[] = {
    {"__os_log_simple", 7},
    {"_setproctitle", 1},
    /* libsandbox SPI: int sandbox_check(pid_t, const char *operation, int type, ...); the filter
     * argument(s) after `type` are a path, a name or an integer */
    {"_sandbox_check", 3},
#include "intvarargs_gen.inc"
};
#define GVAR_SLOTS 16
#define GVAR_FMT_SLOTS 64
static char gvar_narrow[sizeof gvars / sizeof *gvars];
void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack, uint64_t nstack,
                   uint64_t out[6]);

/* A gvar whose SDK declaration has a format attribute: the format's conversions type the guest's
 * slots, so floating arguments reach the XMM registers SysV va_arg reads them from (W7). */
static bool gvar_typed(hao_thread *t, void *fn, const gvar_map *m, const hao_fmtattr *fa, char narrow)
{
    hao_cpu *c = &t->cpu;
    if (m->nfixed > 6) return false;
    hao_fmtfix fx = {0};
    uint64_t fv = hao_fmt_fix(fa->kind, c->x[fa->fmt], &fx);
    char ty[GVAR_FMT_SLOTS];
    int nt = hao_fmt_types(fa->kind == 'C' ? 'C' : fa->kind == 'w' ? 'w' : 'p', fv, ty, GVAR_FMT_SLOTS);
    if (nt < 0) { hao_fmt_done(&fx); __atomic_fetch_add(&hao_fmt_untyped, 1, __ATOMIC_RELAXED); return false; }
    uint64_t gpr[6] = {0}, stk[2 * GVAR_FMT_SLOTS + 2], out[6] = {0};
    uint8_t xmm[8][16];
    memset(xmm, 0, sizeof xmm);
    int gi = m->nfixed, xi = 0, ns = 0;
    for (int k = 0; k < gi; k++) gpr[k] = k == fa->fmt ? fv : c->x[k];
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < nt; k++) {
        uint64_t s = sp[k];
        if (ty[k] == 'd') {
            if (xi < 8) memcpy(xmm[xi++], &s, 8);
            else stk[ns++] = s;
        } else if (ty[k] == 'L') {     /* the format could not be copied: the x87 value, 16-aligned in memory */
            double d;
            long double ld;
            memcpy(&d, &s, 8);
            ld = d;
            ns += ns & 1;
            memset(&stk[ns], 0, 16);
            memcpy(&stk[ns], &ld, 10);
            ns += 2;
        } else if (gi < 6) gpr[gi++] = s;
        else stk[ns++] = s;
    }
    hao_sysv_call((uint64_t)(uintptr_t)fn, gpr, (const uint8_t (*)[16])xmm, stk, (uint64_t)ns, out);
    hao_fmt_done(&fx);
    c->x[0] = narrow ? hao_narrowret_extend(out[0], narrow) : out[0];
    c->x[1] = out[1];
    return true;
}

static void trap_gvar(hao_thread *t, uint64_t idx)
{
    const gvar_map *m = &gvars[idx];
    static void *cache[sizeof gvars / sizeof *gvars + 1];
    static const hao_fmtattr *fcache[sizeof gvars / sizeof *gvars];
    if (!cache[idx]) {
        const char *sym = m->name + 1;
        cache[idx] = dlsym(RTLD_DEFAULT, sym);
        if (!cache[idx]) { fprintf(stderr, "hao: native %s not found\n", sym); abort(); }
    }
    const hao_fmtattr *fa = entry_fmt(&fcache[idx], m->name, m->nfixed, false);
    if (fa && fa->kind != 'o' && fa->kind != 's' && fa->kind != 'W' && gvar_typed(t, cache[idx], m, fa, gvar_narrow[idx])) return;
    hao_cpu *c = &t->cpu;
    uint64_t a[8 + GVAR_SLOTS] = {0};
    int nf = m->nfixed > 8 ? 8 : m->nfixed;
    for (int k = 0; k < nf; k++) a[k] = c->x[k];
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < GVAR_SLOTS; k++) a[nf + k] = sp[k];
    typedef uint64_t (*fnN)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    c->x[0] = ((fnN)cache[idx])(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12],
                                a[13], a[14], a[15], a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23]);
    if (gvar_narrow[idx]) c->x[0] = hao_narrowret_extend(c->x[0], gvar_narrow[idx]);
}

/* C functions that pass/return aggregates by value: signature-driven call */
typedef struct { const char *name; const char *enc; } sigdb_ent;
#define AUDIT_TOKEN "{?=[8I]}"   /* audit_token_t: 32 bytes, by reference on arm64, on the stack on x86_64 */
static const sigdb_ent sigdb[] = {
    {"_audit_token_to_pid", "i" AUDIT_TOKEN}, {"_audit_token_to_pidversion", "i" AUDIT_TOKEN},
    {"_audit_token_to_asid", "i" AUDIT_TOKEN}, {"_audit_token_to_auid", "I" AUDIT_TOKEN},
    {"_audit_token_to_euid", "I" AUDIT_TOKEN}, {"_audit_token_to_ruid", "I" AUDIT_TOKEN},
    {"_audit_token_to_egid", "I" AUDIT_TOKEN}, {"_audit_token_to_rgid", "I" AUDIT_TOKEN},
    {"_audit_token_to_au32", "v" AUDIT_TOKEN "^I^I^I^I^I^i^i^v"},
    {"_SecTaskCreateWithAuditToken", "^v^v" AUDIT_TOKEN},
    /* Every other native function the 27.0 arm64-only userland imports that passes or returns an
     * aggregate by value whose AAPCS64 and SysV classifications differ -- dominated by audit_token_t
     * (32 bytes: by reference in a GP register on arm64, in the MEMORY class on the x86_64 stack),
     * so the plain register bridge shifts every argument and puts access-control checks on stack
     * garbage. Candidates are selected name-agnostically by a code-evidence class rule and typed
     * from the callees' code; the table is generated. Without it, mds crashes in
     * TCCAccessCheckAuditToken and launchd in sandbox_check_network. */
#include "aggspi_gen.inc"
#include "sigdb_gen.inc"
    {NULL, NULL}
};
/* CoreFoundation SPI: void CFLog(int32_t level, CFStringRef format, ...). No va_list variant is
 * exported, so format with CFStringCreateWithFormatAndArguments and log the result with "%@". */
/* _os_log_internal(dso, log, type, format, ...): libtrace resolves the format by its offset from dso, which
 * logd cannot do for a guest image. libhao_identity encodes the guest's variadic slots as the compiler's os_log
 * buffer and composes the message (src/oslog.c). */
static void trap_os_log_internal(hao_thread *t, uint64_t a)
{
    (void)a;
    static void (*vlog)(void *, void *, uint8_t, const char *, x86_va_list);
    if (!vlog) {
        vlog = (void (*)(void *, void *, uint8_t, const char *, x86_va_list))dlsym(RTLD_DEFAULT, "hao_identity_os_log_internal_v");
        if (!vlog) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "_os_log_internal: libhao_identity not loaded\n"); abort(); }
    }
    hao_cpu *c = &t->cpu;
    x86_va_list va;
    make_va(va, c->x[31]);
    vlog((void *)(uintptr_t)c->x[0], (void *)(uintptr_t)c->x[1], (uint8_t)c->x[2], (const char *)(uintptr_t)c->x[3], va);
}

static void trap_cflog(hao_thread *t, uint64_t a)
{
    (void)a;
    typedef const void *cfref;
    static cfref (*fmt_args)(cfref alloc, cfref opts, cfref fmt, void *va);
    static cfref (*with_cstr)(cfref alloc, const char *s, uint32_t enc);
    static void (*cflog)(int32_t level, cfref fmt, ...);
    static void (*cfrelease)(cfref);
    static cfref percent_at;
    if (!cflog) {
        fmt_args = (cfref (*)(cfref, cfref, cfref, void *))dlsym(RTLD_DEFAULT, "CFStringCreateWithFormatAndArguments");
        with_cstr = (cfref (*)(cfref, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
        cfrelease = (void (*)(cfref))dlsym(RTLD_DEFAULT, "CFRelease");
        cflog = (void (*)(int32_t, cfref, ...))dlsym(RTLD_DEFAULT, "CFLog");
        if (!fmt_args || !with_cstr || !cfrelease || !cflog) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "CFLog: CoreFoundation not loaded\n"); abort(); }
        percent_at = with_cstr(NULL, "%@", 0x08000100 /* kCFStringEncodingUTF8 */);
    }
    hao_cpu *c = &t->cpu;
    x86_va_list va;
    make_va(va, c->x[31]);
    cfref msg = c->x[1] ? fmt_args(NULL, NULL, (cfref)(uintptr_t)c->x[1], (void *)va) : NULL;
    cflog((int32_t)c->x[0], percent_at, msg);
    if (msg) cfrelease(msg);
}

/* execl family: collect NULL-terminated variadic list into argv */
static void trap_execl(hao_thread *t, uint64_t kind)
{
    hao_cpu *c = &t->cpu;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    /* arg0 in x1, the rest on the stack up to a NULL; execle's envp follows the NULL */
    size_t n = 0, k = 0;
    if (c->x[1]) { while (sp[n]) n++; k = n + 1; }
    const char **argv = malloc((n + 2) * sizeof *argv);
    if (!argv) { errno = ENOMEM; c->x[0] = (uint64_t)-1; return; }
    argv[0] = (const char *)(uintptr_t)c->x[1];
    for (size_t j = 0; j < n; j++) argv[j + 1] = (const char *)(uintptr_t)sp[j];
    argv[c->x[1] ? n + 1 : 0] = NULL;
    char *const *envp = kind == 1 ? (char *const *)(uintptr_t)sp[k] : NULL;
    const char *path = (const char *)(uintptr_t)c->x[0];
    int r = kind == 1 ? execve(path, (char *const *)argv, envp) : kind == 2 ? execvp(path, (char *const *)argv) : execv(path, (char *const *)argv);
    int e = errno;
    free(argv);
    errno = e;
    c->x[0] = (uint64_t)(int64_t)r;
}

/* vfork is fork. Native vfork runs no atfork handlers, so hao's locks
 * would stay as the parent's other threads left them, yet the guest child
 * runs translated code (the rest of the caller, the exec path) before it
 * execs. A hao_thread shared with a suspended parent would also hand the
 * child's registers back to it. fork is a valid vfork. */
static void trap_vfork(hao_thread *t, uint64_t a)
{
    (void)a;
    t->cpu.x[0] = (uint64_t)(int64_t)fork();
}

/* xo_warn/xo_err family: text-mode output identical to BSD err(3).
 * kind bits: 1 = exit, 2 = append strerror(errno), 4 = explicit code arg */
static void trap_xo_err(hao_thread *t, uint64_t kind)
{
    hao_cpu *c = &t->cpu;
    int k = 0;
    int eval = 0, code = errno;
    if (kind & 1) eval = (int)c->x[k++];
    if (kind & 4) code = (int)c->x[k++];
    const char *fmt = (const char *)(uintptr_t)c->x[k++];
    x86_va_list va;
    make_va(va, c->x[31]);
    fflush(stdout);
    fprintf(stderr, "%s: ", getprogname());
    if (fmt) vfprintf(stderr, fmt, (void *)va);
    if (kind & 2) fprintf(stderr, "%s%s", fmt ? ": " : "", strerror(code));
    fprintf(stderr, "\n");
    if (kind & 1) {
        void (*fin)(void) = dlsym(RTLD_DEFAULT, "xo_finish");
        if (fin) fin();
        exit(eval);
    }
}

/* ---- setjmp / longjmp on guest state.
 * Guest jmp_buf (arm64: 48 ints) layout used here:
 *   [0..11] x19..x30, [12] sp, [13..20] d8..d15, [21] sigmask, [22] savemask, [23] hao depth */
static void trap_setjmp(hao_thread *t, uint64_t savemask)
{
    hao_cpu *c = &t->cpu;
    uint64_t *jb = (uint64_t *)(uintptr_t)c->x[0];
    for (int k = 0; k < 12; k++) jb[k] = c->x[19 + k];
    jb[12] = c->x[31];
    for (int k = 0; k < 8; k++) jb[13 + k] = c->v[8 + k].d[0];
    bool save = savemask == 1 || (savemask == 2 && c->x[1]);
    jb[22] = save;
    if (save) { sigset_t m; sigprocmask(SIG_BLOCK, NULL, &m); jb[21] = m; }
    jb[23] = (uint64_t)t->depth;
    c->x[0] = 0;
}

static void trap_longjmp(hao_thread *t, uint64_t restore_mask)
{
    hao_cpu *c = &t->cpu;
    uint64_t *jb = (uint64_t *)(uintptr_t)c->x[0];
    uint64_t val = (uint32_t)c->x[1];
    int depth = (int)jb[23];
    const hao_seg *inner = NULL;
    if (depth != t->depth) {
        /* the setjmp is in an enclosing segment: native frames (a callback's
         * caller, a signal frame) lie in between. Start from that segment's
         * guest state, which the innermost segment it entered saved. */
        for (const hao_seg *s = t->seg; s && s->depth > depth; s = s->prev) inner = s;
        if (depth > t->depth || !inner || !inner->prev || inner->prev->depth != depth || !inner->outer) {
            hao_diag_prefix(HAO_DIAG);
            fprintf(HAO_DIAG, "longjmp to a jmp_buf of segment depth %d, which is not live (current %d)\n", depth, t->depth);
            abort();
        }
        *c = *inner->outer;
    }
    for (int k = 0; k < 12; k++) c->x[19 + k] = jb[k];
    c->x[31] = jb[12];
    for (int k = 0; k < 8; k++) { c->v[8 + k].d[0] = jb[13 + k]; c->v[8 + k].d[1] = 0; }
    if (restore_mask && jb[22]) { sigset_t m = (sigset_t)jb[21]; sigprocmask(SIG_SETMASK, &m, NULL); }
    c->x[0] = val ? val : 1;
    c->pc = c->x[30];
    if (inner) hao_longjmp_segment(t, depth);   /* discards the native frames, as a native longjmp does */
}

/* ---- C++ functions returning a non-trivially-copyable class by value
 * (std::exception_ptr): arm64 passes the result address in x8, x86_64 as a
 * hidden first argument in rdi. arg: 0 = current_exception(),
 * 1 = exception_ptr::__from_native_exception_pointer(void *). */
static void trap_cxx_sret(hao_thread *t, uint64_t which)
{
    static void *fn[2];
    static const char *const names[2] = {"_ZSt17current_exceptionv", "_ZNSt13exception_ptr31__from_native_exception_pointerEPv"};
    if (!fn[which]) fn[which] = dlsym(RTLD_DEFAULT, names[which]);
    hao_cpu *c = &t->cpu;
    if (!fn[which]) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "%s: not found\n", names[which]); abort(); }
    ((void (*)(uint64_t, uint64_t))fn[which])(c->x[8], c->x[0]);
}

/* ---- audit_token_t (32 bytes) by value: arm64 passes a pointer to a copy
 * (composite > 16 bytes), x86_64 copies it onto the stack. */
typedef struct { uint32_t v[8]; } hao_atok;
static void trap_sbx_check_token(hao_thread *t, uint64_t a)
{
    /* int sandbox_check_by_audit_token(audit_token_t, const char *op, int type, ...) */
    (void)a;
    static int (*f)(hao_atok, const char *, int, ...);
    if (!f) f = (int (*)(hao_atok, const char *, int, ...))dlsym(RTLD_DEFAULT, "sandbox_check_by_audit_token");
    hao_cpu *c = &t->cpu;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    int r = f(*(const hao_atok *)(uintptr_t)c->x[0], (const char *)(uintptr_t)c->x[1], (int)c->x[2],
              sp[0], sp[1], sp[2], sp[3]);
    c->x[0] = (uint64_t)(int64_t)r;
}

static void trap_sbx_signal_target(hao_thread *t, uint64_t a)
{
    /* int sandbox_check_process_signal_target(audit_token_t, int, audit_token_t, int) */
    (void)a;
    static int (*f)(hao_atok, int, hao_atok, int);
    if (!f) f = (int (*)(hao_atok, int, hao_atok, int))dlsym(RTLD_DEFAULT, "sandbox_check_process_signal_target");
    hao_cpu *c = &t->cpu;
    int r = f(*(const hao_atok *)(uintptr_t)c->x[0], (int)c->x[1], *(const hao_atok *)(uintptr_t)c->x[2], (int)c->x[3]);
    c->x[0] = (uint64_t)(int64_t)r;
}

/* ---- special ABI helpers */

static void trap_nop(hao_thread *t, uint64_t a) { (void)t; (void)a; }  /* __chkstk_darwin: guest stack is committed */

static void trap_exec_header(hao_thread *t, uint64_t a)
{
    (void)a;
    t->cpu.x[0] = hao_main_image ? hao_main_image->base : 0;
}

/* The guest main executable's path as dyld presents it: _NSGetExecutablePath
 * the path it was run by (absolute, symlinks kept), the image list and
 * dladdr its real path. Set by run_guest (main.c). */
char hao_guest_exec_path[1024], hao_guest_image_path[1024];
static void trap_exec_path(hao_thread *t, uint64_t a)
{
    (void)a;
    hao_cpu *c = &t->cpu;
    char *buf = (char *)(uintptr_t)c->x[0];
    uint32_t *size = (uint32_t *)(uintptr_t)c->x[1];
    size_t need = strlen(hao_guest_exec_path) + 1;
    if (*size < need) { *size = (uint32_t)need; c->x[0] = (uint64_t)-1; return; }
    memcpy(buf, hao_guest_exec_path, need);
    c->x[0] = 0;
}

/* dladdr from translated code: guest images are not registered with dyld, so
 * native dladdr fails for their addresses (launchd: dladdr(_NSGetMachExecuteHeader())
 * then getsectiondata(__TEXT,__config) -> "launchd built improperly"). */
static void trap_dladdr(hao_thread *t, uint64_t a)
{
    (void)a;
    hao_cpu *c = &t->cpu;
    Dl_info *info = (Dl_info *)(uintptr_t)c->x[1];
    hao_image *im = hao_image_for_addr(c->x[0]);
    if (!im) { c->x[0] = (uint64_t)dladdr((const void *)(uintptr_t)c->x[0], info); return; }
    info->dli_fname = im == hao_main_image && hao_guest_image_path[0] ? hao_guest_image_path : im->path;
    info->dli_fbase = (void *)(uintptr_t)im->base;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    c->x[0] = 1;
}

/* dyld's main image as translated code sees it: image 0 and the program's
 * header are the guest's own (natively the stub or hao), consistently for
 * every call that names them, and the libmacho lookups that use the main
 * executable implicitly (getsectbyname & co. call _NSGetMachExecuteHeader
 * inside libmacho, where no import trap reaches) read the guest's header.
 * Guest images are not registered with dyld, so the address-to-image
 * queries answer for them here. */
static void trap_dyld_image_header(hao_thread *t, uint64_t a)
{
    (void)a;
    if (t->cpu.x[0] == 0 && hao_main_image) { t->cpu.x[0] = hao_main_image->base; return; }
    t->cpu.x[0] = (uint64_t)(uintptr_t)_dyld_get_image_header((uint32_t)t->cpu.x[0]);
}

extern const struct mach_header *_dyld_get_prog_image_header(void);
extern const char *dyld_image_path_containing_address(const void *addr);
extern const struct mach_header *dyld_image_header_containing_address(const void *addr);
extern intptr_t _dyld_get_image_slide(const struct mach_header *mh);

enum { DY_NAME, DY_VMSLIDE, DY_PROG_HEADER, DY_SLIDE, DY_PATH_AT, DY_HEADER_AT };
static const char *image_path(const hao_image *im)
{
    return im == hao_main_image && hao_guest_image_path[0] ? hao_guest_image_path : im->path;
}
static void trap_dyld_main(hao_thread *t, uint64_t kind)
{
    uint64_t *x = t->cpu.x;
    hao_image *m = hao_main_image, *im;
    switch (kind) {
    case DY_NAME:
        x[0] = x[0] == 0 && m ? (uint64_t)(uintptr_t)image_path(m) : (uint64_t)(uintptr_t)_dyld_get_image_name((uint32_t)x[0]);
        break;
    case DY_VMSLIDE:
        x[0] = x[0] == 0 && m ? m->slide : (uint64_t)_dyld_get_image_vmaddr_slide((uint32_t)x[0]);
        break;
    case DY_PROG_HEADER: x[0] = m ? m->base : (uint64_t)(uintptr_t)_dyld_get_prog_image_header(); break;
    case DY_SLIDE:
        im = hao_image_for_addr(x[0]);
        x[0] = im && im->base == x[0] ? im->slide : (uint64_t)_dyld_get_image_slide((const struct mach_header *)(uintptr_t)x[0]);
        break;
    case DY_PATH_AT:
        im = hao_image_for_addr(x[0]);
        x[0] = im ? (uint64_t)(uintptr_t)image_path(im)
                  : (uint64_t)(uintptr_t)dyld_image_path_containing_address((const void *)(uintptr_t)x[0]);
        break;
    case DY_HEADER_AT:
        im = hao_image_for_addr(x[0]);
        x[0] = im ? im->base : (uint64_t)(uintptr_t)dyld_image_header_containing_address((const void *)(uintptr_t)x[0]);
        break;
    }
}

/* libmacho's implicit-main-executable functions, on the guest's header
 * (same results as libmacho's: unslid addresses, as there). The
 * deprecated *fromheader_64 calls are libmacho's own definitions of them. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
enum { MO_SECTBYNAME, MO_SEGBYNAME, MO_SECTDATA, MO_END, MO_ETEXT, MO_EDATA };
static const struct segment_command_64 *guest_seg(const struct mach_header_64 *mh, const char *name)
{
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strncmp(((const struct segment_command_64 *)lc)->segname, name, 16))
            return (const void *)lc;
    return NULL;
}
static void trap_macho_main(hao_thread *t, uint64_t kind)
{
    uint64_t *x = t->cpu.x;
    const struct mach_header_64 *mh = hao_main_image ? (const void *)(uintptr_t)hao_main_image->base
                                                     : (const void *)_NSGetMachExecuteHeader();
    const char *a0 = (const char *)(uintptr_t)x[0], *a1 = (const char *)(uintptr_t)x[1];
    const struct section_64 *s;
    switch (kind) {
    case MO_SECTBYNAME: x[0] = (uint64_t)(uintptr_t)getsectbynamefromheader_64(mh, a0, a1); break;
    case MO_SEGBYNAME: x[0] = (uint64_t)(uintptr_t)guest_seg(mh, a0); break;
    case MO_SECTDATA:
        x[0] = (uint64_t)(uintptr_t)getsectdatafromheader_64(mh, a0, a1, (uint64_t *)(uintptr_t)x[2]);
        break;
    case MO_END: {
        uint64_t end = 0;
        const struct load_command *lc = (const void *)(mh + 1);
        for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize))
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sg = (const void *)lc;
                if (sg->vmaddr + sg->vmsize > end) end = sg->vmaddr + sg->vmsize;
            }
        x[0] = end;
        break;
    }
    case MO_ETEXT: case MO_EDATA:
        s = getsectbynamefromheader_64(mh, kind == MO_ETEXT ? "__TEXT" : "__DATA", kind == MO_ETEXT ? "__text" : "__data");
        x[0] = s ? s->addr + s->size : 0;
        break;
    }
}
#pragma clang diagnostic pop

/* TLV: descriptors are {thunk, key, offset}. */
typedef struct { uint64_t thunk, key, offset; } tlv_desc;
static void trap_tlv_get(hao_thread *t, uint64_t imgp)
{
    hao_image *img = (hao_image *)(uintptr_t)imgp;
    tlv_desc *d = (tlv_desc *)(uintptr_t)t->cpu.x[0];
    pthread_key_t key = (pthread_key_t)d->key;
    uint8_t *blk = pthread_getspecific(key);
    if (!blk) {
        hao_crit_enter();   /* malloc: a guest signal handler waits until it is done */
        blk = calloc(1, img->tlv_total_size ? img->tlv_total_size : 1);
        memcpy(blk, (void *)(uintptr_t)img->tlv_init_start, img->tlv_init_size);
        pthread_setspecific(key, blk);
        hao_crit_leave();
    }
    t->cpu.x[0] = (uint64_t)(uintptr_t)(blk + d->offset);
}

void hao_tlv_setup(hao_image *img)
{
    if (!img->tlv_desc_count) return;
    pthread_key_t key;
    pthread_key_create(&key, free);
    uint64_t thunk = hao_trap_address("tlv_get_addr", trap_tlv_get, (uint64_t)(uintptr_t)img);
    tlv_desc *d = (tlv_desc *)(uintptr_t)img->tlv_desc;
    for (uint64_t k = 0; k < img->tlv_desc_count; k++) { d[k].thunk = thunk; d[k].key = key; }
}

/* ---- registry */

bool hao_signal_interpose(const char *name, uint64_t *addr);

/* aggregate signature of an imported C function, or NULL */
const char *hao_sigdb_enc(const char *name)
{
    for (size_t k = 0; sigdb[k].name; k++) if (!strcmp(name, sigdb[k].name)) return sigdb[k].enc;
    return NULL;
}

/* A table's fixed-argument count against the native function's own va_start prologue: false when
 * the prologue says otherwise (a fixed double the header scan counted as an integer, a wrong extra
 * entry), so the import falls through to varsniff.c, which binds it from the prologue. */
static bool table_agrees(const char *name, int nfixed)
{
    if (hao_opt.no_varsniff) return true;
    int ngp, nfp;
    if (!hao_varsniff_native((uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, name + 1), &ngp, &nfp)) return true;
    if (ngp == nfixed && nfp == 0) return true;
    if (hao_verbose) fprintf(stderr, "hao: %s: table says %d fixed, native prologue %d (+%d FP)\n", name, nfixed, ngp, nfp);
    return false;
}

/* Lookup order: signals, callback structs, GSS-API, libm (libm.c), Swift task entries (swiftmeta.c), objc_msgSend, then the
 * variadic tables (va_list variants, functions taking a va_list, integer variadics), the fixed
 * traps and the generic integer variadics. The generated tables leave out every name the
 * hand-written tables here handle (tools/gen_varargs.py), so a name is in at most one table. */
/* ABI repairs of libc++'s own functions (std::exception_ptr results through
 * x8): they apply to binds to native libc++ only. With libc++ loaded as a
 * guest image (runtime.c, guest_libcxx) arm64 code calls arm64 code and
 * nothing needs repairing. Every other interposer is a runtime service
 * (setjmp, unwinding, dyld and Mach-O queries, variadics, signals, ObjC) and
 * applies to every bind of a native symbol, guest libc++'s own included. */
static const char *const abi_repair[] = {"__ZSt17current_exceptionv", "__ZNSt13exception_ptr31__from_native_exception_pointerEPv"};

bool hao_interpose_abi_repair(const char *name)
{
    for (size_t k = 0; k < sizeof abi_repair / sizeof *abi_repair; k++)
        if (!strcmp(name, abi_repair[k])) return true;
    return false;
}

static const struct { const char *name; hao_trap_fn fn; uint64_t arg; } fixed[] = {
    {"_setjmp", trap_setjmp, 1}, {"__setjmp", trap_setjmp, 0}, {"_sigsetjmp", trap_setjmp, 2},
    {"_longjmp", trap_longjmp, 1}, {"__longjmp", trap_longjmp, 0}, {"_siglongjmp", trap_longjmp, 1},
    {"__Unwind_Resume", hao_eh_trap_resume, 0},
    {"__ZSt17current_exceptionv", trap_cxx_sret, 0},
    {"__ZNSt13exception_ptr31__from_native_exception_pointerEPv", trap_cxx_sret, 1},
    /* the stack probe, as programs import it (libcompiler_rt's re-export of libsystem_pthread's
     * ____chkstk_darwin, which has thread_chkstk_darwin at the same address: bridge.c answers every name of
     * the address with this trap). arm64 passes the size in x9, x86_64 in rax: the native probe must never
     * see a guest call */
    {"___chkstk_darwin", trap_nop, 0},
    {"__NSGetMachExecuteHeader", trap_exec_header, 0},
    {"__dyld_get_image_header", trap_dyld_image_header, 0}, {"_dladdr", trap_dladdr, 0},
    {"__dyld_get_image_name", trap_dyld_main, DY_NAME}, {"__dyld_get_image_vmaddr_slide", trap_dyld_main, DY_VMSLIDE},
    {"__dyld_get_prog_image_header", trap_dyld_main, DY_PROG_HEADER}, {"__dyld_get_image_slide", trap_dyld_main, DY_SLIDE},
    {"_dyld_image_path_containing_address", trap_dyld_main, DY_PATH_AT},
    {"_dyld_image_header_containing_address", trap_dyld_main, DY_HEADER_AT},
    {"_getsectbyname", trap_macho_main, MO_SECTBYNAME}, {"_getsegbyname", trap_macho_main, MO_SEGBYNAME},
    {"_getsectdata", trap_macho_main, MO_SECTDATA}, {"_get_end", trap_macho_main, MO_END},
    {"_get_etext", trap_macho_main, MO_ETEXT}, {"_get_edata", trap_macho_main, MO_EDATA},
    {"_sandbox_check_by_audit_token", trap_sbx_check_token, 0},
    {"_sandbox_check_process_signal_target", trap_sbx_signal_target, 0},
    {"__NSGetExecutablePath", trap_exec_path, 0},
    {"_xo_err", trap_xo_err, 1 | 2}, {"_xo_errx", trap_xo_err, 1}, {"_xo_errc", trap_xo_err, 1 | 2 | 4},
    {"_xo_warn", trap_xo_err, 2}, {"_xo_warnx", trap_xo_err, 0}, {"_xo_warn_c", trap_xo_err, 2 | 4},
    {"_execl", trap_execl, 0}, {"_execle", trap_execl, 1}, {"_execlp", trap_execl, 2},
    {"_vfork", trap_vfork, 0},
    {"_CFLog", trap_cflog, 0},
    {"__os_log_internal", trap_os_log_internal, 0},
};

/* Every name hao_interpose_lookup answers with a trap; bridge.c resolves them
 * to addresses, so a trap answers its function under every name. */
void hao_interpose_names(hao_name_fn cb, void *ctx)
{
    hao_signal_interpose_names(cb, ctx);
    hao_callbacks_interpose_names(cb, ctx);
    hao_swift_objects_interpose_names(cb, ctx);
    hao_gss_interpose_names(cb, ctx);
    hao_libm_interpose_names(cb, ctx);
    hao_swiftmeta_interpose_names(cb, ctx);
    hao_objc_interpose_names(cb, ctx);
    hao_regvar_interpose_names(cb, ctx);
    for (size_t k = 0; k < sizeof varargs / sizeof *varargs; k++) if (varargs[k].vname) cb(varargs[k].name, ctx);
    for (size_t k = 0; k < sizeof vlists / sizeof *vlists; k++) cb(vlists[k].name, ctx);
    for (size_t k = 0; k < sizeof intvars / sizeof *intvars; k++) cb(intvars[k].name, ctx);
    for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++) cb(fixed[k].name, ctx);
    for (size_t k = 0; k < sizeof gvars / sizeof *gvars; k++) cb(gvars[k].name, ctx);
}

bool hao_interpose_lookup(const char *name, uint64_t *addr)
{
    if (hao_signal_interpose(name, addr)) return true;
    if (hao_callbacks_interpose(name, addr)) return true;
    if (hao_swift_objects_interpose(name, addr)) return true;
    if (hao_gss_interpose(name, addr)) return true;
    if (hao_libm_interpose(name, addr)) return true;
    if (hao_swiftmeta_interpose(name, addr)) return true;
    extern bool hao_objc_interpose(const char *, uint64_t *);
    if (!hao_opt.no_msgsend && hao_objc_interpose(name, addr)) return true;
    extern bool hao_objc_interpose_imp(const char *, uint64_t *);
    if (hao_objc_interpose_imp(name, addr)) return true;
    extern bool hao_regvar_interpose(const char *, uint64_t *);
    if (hao_regvar_interpose(name, addr)) return true;
    for (size_t k = 0; k < sizeof varargs / sizeof *varargs; k++)
        if (varargs[k].vname && !strcmp(name, varargs[k].name)) {
            if (!table_agrees(name, varargs[k].nfixed)) return false;
            *addr = hao_trap_address(name, trap_vararg, k); return true; }
    for (size_t k = 0; k < sizeof vlists / sizeof *vlists; k++)
        if (!strcmp(name, vlists[k].name)) { *addr = hao_trap_address(name, trap_vlist, k); return true; }
    for (size_t k = 0; k < sizeof intvars / sizeof *intvars; k++)
        if (!strcmp(name, intvars[k].name)) { *addr = hao_trap_address(name, trap_intvar, k); return true; }
    for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++)
        if (!strcmp(name, fixed[k].name)) { *addr = hao_trap_address(fixed[k].name, fixed[k].fn, fixed[k].arg); return true; }
    for (size_t k = 0; k < sizeof gvars / sizeof *gvars; k++)
        if (!strcmp(name, gvars[k].name)) {
            if (!table_agrees(name, gvars[k].nfixed)) return false;
            gvar_narrow[k] = hao_narrowret_kind_for_name(name);
            *addr = hao_trap_address(name, trap_gvar, k);
            return true;
        }
    return false;
}

/* ---- narrow integer results */

typedef struct { const char *name; char kind; } narrowret_ent;
static const narrowret_ent narrowrets[] = {
#include "narrowret_gen.inc"
};

char hao_narrowret_kind_for_name(const char *name)
{
    size_t lo = 0, hi = sizeof narrowrets / sizeof *narrowrets;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, narrowrets[mid].name);
        if (!r) return narrowrets[mid].kind;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

char hao_narrowret_kind_for_enc(const char *enc)
{
    if (!enc) return 0;
    while (*enc && strchr("rnNoORVA", *enc)) enc++;   /* method qualifiers (oneway, const, ...) */
    return strchr("BcCsS", *enc) && *enc ? *enc : 0;
}

/* imported C functions whose Apple arm64 stack arguments do not line up with SysV slots */
static const sigdb_ent stackargs[] = {
#include "stackargs_gen.inc"
};

static const char *sorted_enc(const sigdb_ent *t, size_t n, const char *name)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, t[mid].name);
        if (!r) return t[mid].enc;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

const char *hao_stackargs_enc(const char *name)
{
    return sorted_enc(stackargs, sizeof stackargs / sizeof *stackargs, name);
}

void hao_tables_names(hao_name_fn cb, void *ctx)
{
    for (size_t k = 0; k < sizeof stackargs / sizeof *stackargs; k++) cb(stackargs[k].name, ctx);
    for (size_t k = 0; sigdb[k].name; k++) cb(sigdb[k].name, ctx);
    for (size_t k = 0; k < sizeof narrowrets / sizeof *narrowrets; k++) cb(narrowrets[k].name, ctx);
    hao_cxxstr_names(cb, ctx);
}

int hao_tables_plan(const char *name, char *kind, const char **enc)
{
    const char *e = sorted_enc(stackargs, sizeof stackargs / sizeof *stackargs, name);
    if (e) { *kind = 'A'; *enc = e; return HAO_BRS_STACKARGS; }
    for (size_t k = 0; sigdb[k].name; k++)
        if (!strcmp(name, sigdb[k].name)) { *kind = 'A'; *enc = sigdb[k].enc; return HAO_BRS_SIGDB; }   /* the signature path converts the result too */
    char nk = hao_narrowret_kind_for_name(name);
    if (nk) { *kind = nk; *enc = NULL; return HAO_BRS_NARROWRET; }
    return 0;
}

void hao_interpose_init(void) {}
