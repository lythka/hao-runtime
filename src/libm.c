/*
 * libm for translated code: where the arm64 and x86_64 definitions of the
 * C math interface differ in more than the calling convention.
 *
 * 1. <fenv.h>. The C floating-point environment API is defined per architecture, and the
 * two definitions disagree on every value that crosses the call (SDK
 * fenv.h): arm64 FE_DOWNWARD is 0x800000 (FPCR.RMode), x86_64's 0x400
 * (x87/MXCSR RC); arm64 FE_DIVBYZERO is 2 (FPSR.DZC), x86_64's 4; arm64
 * fenv_t is {FPSR, FPCR}, x86_64's {x87 CW, x87 SW, MXCSR, reserved}; the
 * arm64 FE_DFL_DISABLE_DENORMS_ENV does not exist on x86_64. Bound to x86_64
 * libm, a guest's fesetround(FE_DOWNWARD) fails, fetestexcept(FE_DIVBYZERO)
 * tests the x86 overflow bit, and code that edits a saved fenv_t (the
 * documented `env.__fpcr |= __fpcr_flush_to_zero` idiom) writes the MXCSR.
 *
 * So the whole interface binds to traps that do what the arm64 libm does
 * (its arm64e code, read in the host's libsystem_m: each function is a few
 * MRS/MSR of FPSR and FPCR) on the guest's FPCR and FPSR, through the accessors in
 * cpu.h. Their home is the host MXCSR (and the x87 control and status words
 * for native long double code), so translated code, the interpreter and
 * native libm all see the result. The default environments are arm64
 * objects in libhao.
 *
 * 2. long double. On arm64 long double is double, and libsystem_m exports
 * each long double function as an alias of its double twin (sqrtl is sqrt,
 * nexttowardl is nextafter, lrintl is llrint; checked by address in the
 * arm64e libsystem_m, tests/guest/bridgefix.sh). x86_64 libm computes them
 * in 80-bit x87 precision with the long double on the stack: marshalled,
 * the result is rounded twice and differs where the precision shows
 * (nexttowardl(1, 2) is 1 + 2^-63, which rounds back to 1), and modfl writes
 * a 16-byte long double through a pointer to an 8-byte one. So each of these
 * imports binds to the native double function, which is what arm64 runs.
 * strtold, wcstold and their _l forms are separate code on arm64, but with
 * a 53-bit long double they are the correctly rounded conversion that
 * strtod is (x86_64's 64-bit rounding plus hao's rounding to 53 bits gets
 * inputs just above a halfway point wrong). nexttoward and nexttowardf (a
 * long double only as the direction) stay marshalled: exact either way.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <string.h>

/* The arm64 values (SDK fenv.h, __arm64__). */
#define A_FE_ALL_EXCEPT 0x9fu        /* FPSR IOC DZC OFC UFC IXC, IDC (FE_FLUSHTOZERO) */
#define A_FE_INVALID    0x01u
#define A_FE_DIVBYZERO  0x02u
#define A_FE_OVERFLOW   0x04u
#define A_FE_UNDERFLOW  0x08u
#define A_FE_INEXACT    0x10u
#define A_FE_FLUSHTOZERO 0x80u
#define A_FE_RMODE      0x00c00000u  /* FPCR.RMode */
#define A_FPCR_FZ       0x01000000u
#define A_FPCR_TRAPS    0x00009f00u  /* IOE DZE OFE UFE IXE IDE */

typedef struct { uint64_t fpsr, fpcr; } a64_fenv_t;

/* _FE_DFL_ENV and _FE_DFL_DISABLE_DENORMS_ENV as the arm64 libm defines them
 * (read on the host: {0, 0} and {0, FPCR.FZ}). */
static const a64_fenv_t dfl_env = {0, 0};
static const a64_fenv_t dfl_nodenorm_env = {0, A_FPCR_FZ};

static void *gptr(uint64_t a) { return (void *)(uintptr_t)hao_tbi(a); }

static void set_env(hao_cpu *c, const a64_fenv_t *e)
{
    a64_fpsr_write(c, (uint32_t)e->fpsr);
    a64_fpcr_write(c, (uint32_t)e->fpcr);
}

/* feraiseexcept raises each exception with an arithmetic operation, so
 * overflow and underflow also raise inexact (underflow not when FPCR.FZ
 * flushes the result) and FE_FLUSHTOZERO sets IDC; no trap is enabled on
 * Apple silicon. */
static void raise_flags(hao_cpu *c, uint32_t e)
{
    uint32_t f = e & (A_FE_INVALID | A_FE_DIVBYZERO | A_FE_INEXACT | A_FE_FLUSHTOZERO);
    if (e & A_FE_OVERFLOW) f |= A_FE_OVERFLOW | A_FE_INEXACT;
    if (e & A_FE_UNDERFLOW) f |= A_FE_UNDERFLOW | ((a64_fpcr_read(c) & A_FPCR_FZ) ? 0 : A_FE_INEXACT);
    if (f) a64_fpsr_write(c, a64_fpsr_read(c) | f);
}

enum { F_GETROUND, F_SETROUND, F_CLEAR, F_GETFLAG, F_SETFLAG, F_TEST, F_RAISE, F_GETENV, F_SETENV, F_HOLD, F_UPDATE };

static void trap_fenv(hao_thread *t, uint64_t which)
{
    hao_cpu *c = &t->cpu;
    uint32_t e = (uint32_t)c->x[0];
    uint64_t r = 0;
    switch (which) {
    case F_GETROUND: r = a64_fpcr_read(c) & A_FE_RMODE; break;
    case F_SETROUND:                                  /* an invalid mode is returned unchanged */
        if (e & ~A_FE_RMODE) { r = e; break; }
        a64_fpcr_write(c, (a64_fpcr_read(c) & ~A_FE_RMODE) | e);
        break;
    case F_CLEAR: a64_fpsr_write(c, a64_fpsr_read(c) & ~(e & A_FE_ALL_EXCEPT)); break;
    case F_TEST: r = e & a64_fpsr_read(c) & A_FE_ALL_EXCEPT; break;
    case F_RAISE: raise_flags(c, e); break;
    case F_GETFLAG: {                                 /* (fexcept_t *, int) */
        uint16_t v = (uint16_t)((uint32_t)c->x[1] & a64_fpsr_read(c) & A_FE_ALL_EXCEPT);
        memcpy(gptr(c->x[0]), &v, 2);
        break;
    }
    case F_SETFLAG: {                                 /* (const fexcept_t *, int) */
        uint32_t m = (uint32_t)c->x[1] & A_FE_ALL_EXCEPT;
        uint16_t v;
        memcpy(&v, gptr(c->x[0]), 2);
        a64_fpsr_write(c, (a64_fpsr_read(c) & ~m) | (m & v));
        break;
    }
    case F_GETENV: case F_HOLD: {
        a64_fenv_t env = {a64_fpsr_read(c), a64_fpcr_read(c)};
        memcpy(gptr(c->x[0]), &env, sizeof env);
        if (which == F_HOLD) {                        /* flags cleared, traps off */
            env.fpsr &= ~(uint64_t)A_FE_ALL_EXCEPT;
            env.fpcr &= ~(uint64_t)A_FPCR_TRAPS;
            set_env(c, &env);
        }
        break;
    }
    case F_SETENV: case F_UPDATE: {
        uint32_t raised = a64_fpsr_read(c) & A_FE_ALL_EXCEPT;
        a64_fenv_t env;
        memcpy(&env, gptr(c->x[0]), sizeof env);
        set_env(c, &env);
        if (which == F_UPDATE) raise_flags(c, raised);
        break;
    }
    }
    c->x[0] = r;
}

/* arm64 long double function -> the double function it is an alias of */
static const struct { const char *name, *twin; } ld_twins[] = {
    {"_acoshl", "acosh"}, {"_acosl", "acos"}, {"_asinhl", "asinh"}, {"_asinl", "asin"},
    {"_atan2l", "atan2"}, {"_atanhl", "atanh"}, {"_atanl", "atan"}, {"_cbrtl", "cbrt"},
    {"_ceill", "ceil"}, {"_copysignl", "copysign"}, {"_coshl", "cosh"}, {"_cosl", "cos"},
    {"_erfcl", "erfc"}, {"_erfl", "erf"}, {"_exp2l", "exp2"}, {"_expl", "exp"},
    {"_expm1l", "expm1"}, {"_fabsl", "fabs"}, {"_fdiml", "fdim"}, {"_floorl", "floor"},
    {"_fmal", "fma"}, {"_fmaxl", "fmax"}, {"_fminl", "fmin"}, {"_fmodl", "fmod"},
    {"_frexpl", "frexp"}, {"_hypotl", "hypot"}, {"_ilogbl", "ilogb"}, {"_ldexpl", "ldexp"},
    {"_lgammal", "lgamma"}, {"_lgammal_r", "lgamma_r"}, {"_log10l", "log10"}, {"_log1pl", "log1p"},
    {"_log2l", "log2"}, {"_logbl", "logb"}, {"_logl", "log"}, {"_modfl", "modf"},
    {"_nanl", "nan"}, {"_nearbyintl", "nearbyint"}, {"_nextafterl", "nextafter"},
    {"_nexttowardl", "nextafter"}, {"_powl", "pow"}, {"_remainderl", "remainder"},
    {"_remquol", "remquo"}, {"_rintl", "rint"}, {"_roundl", "round"}, {"_scalblnl", "scalbln"},
    {"_scalbnl", "ldexp"}, {"_sinhl", "sinh"}, {"_sinl", "sin"}, {"_sqrtl", "sqrt"},
    {"_tanhl", "tanh"}, {"_tanl", "tan"}, {"_tgammal", "tgamma"}, {"_truncl", "trunc"},
    {"_lrintl", "llrint"}, {"_llrintl", "llrint"}, {"_lroundl", "llround"}, {"_llroundl", "llround"},
    {"___fpclassifyl", "__fpclassifyd"}, {"___isfinitel", "__isfinited"}, {"___isinfl", "__isinfd"},
    {"___isnanl", "__isnand"}, {"___isnormall", "__isnormald"}, {"___signbitl", "__signbitd"},
    {"_cabsl", "cabs"}, {"_cargl", "carg"}, {"_cimagl", "cimag"}, {"_creall", "creal"},
    {"_cacosl", "cacos"}, {"_cacoshl", "cacosh"}, {"_casinl", "casin"}, {"_casinhl", "casinh"},
    {"_catanl", "catan"}, {"_catanhl", "catanh"}, {"_ccosl", "ccos"}, {"_ccoshl", "ccosh"},
    {"_cexpl", "cexp"}, {"_clogl", "clog"}, {"_conjl", "conj"}, {"_cpowl", "cpow"},
    {"_cprojl", "cproj"}, {"_csinl", "csin"}, {"_csinhl", "csinh"}, {"_csqrtl", "csqrt"},
    {"_ctanl", "ctan"}, {"_ctanhl", "ctanh"},
    /* separate code on arm64, the same conversion (see above) */
    {"_strtold", "strtod"}, {"_strtold_l", "strtod_l"}, {"_wcstold", "wcstod"}, {"_wcstold_l", "wcstod_l"},
};

static hao_name_index ld_twins_ix = HAO_NAME_INDEX(ld_twins, NULL);

static const char *ld_twin(const char *name)
{
    long k = hao_name_find(&ld_twins_ix, name);
    return k < 0 ? NULL : ld_twins[k].twin;
}

static const struct { const char *name; int which; } fenv_fns[] = {
    {"_fegetround", F_GETROUND}, {"_fesetround", F_SETROUND}, {"_feclearexcept", F_CLEAR},
    {"_fegetexceptflag", F_GETFLAG}, {"_fesetexceptflag", F_SETFLAG}, {"_fetestexcept", F_TEST},
    {"_feraiseexcept", F_RAISE}, {"_fegetenv", F_GETENV}, {"_fesetenv", F_SETENV},
    {"_feholdexcept", F_HOLD}, {"_feupdateenv", F_UPDATE},
};

static hao_name_index fenv_ix = HAO_NAME_INDEX(fenv_fns, NULL);

void hao_libm_interpose_names(hao_name_fn cb, void *ctx) { hao_name_each(&fenv_ix, cb, ctx); }

bool hao_libm_interpose(const char *name, uint64_t *addr)
{
    if (name[0] != '_') return false;
    const char *twin = ld_twin(name);
    if (twin) {
        void *p = dlsym(RTLD_DEFAULT, twin);
        if (!p) return false;
        *addr = (uint64_t)(uintptr_t)p;
        return true;
    }
    if (!strcmp(name, "__FE_DFL_ENV")) { *addr = (uint64_t)(uintptr_t)&dfl_env; return true; }
    if (!strcmp(name, "__FE_DFL_DISABLE_DENORMS_ENV")) { *addr = (uint64_t)(uintptr_t)&dfl_nodenorm_env; return true; }
    long k = hao_name_find(&fenv_ix, name);
    if (k < 0) return false;
    *addr = hao_trap_address(fenv_fns[k].name, trap_fenv, (uint64_t)fenv_fns[k].which);
    return true;
}
