/* <fenv.h> under hao: the arm64 FE_* constants, fenv_t layout and default
 * environments against the guest's FPCR/FPSR, mixed with translated FP code
 * and native libm. Each line must match native arm64 (tests/guest/fenv.sh).
 * x86_64 libm's FE_* values differ (FE_DOWNWARD 0x400 vs 0x800000,
 * FE_DIVBYZERO 4 vs 2, ...), and so does its fenv_t. */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#pragma STDC FENV_ACCESS ON

static volatile double one = 1, three = 3, zero = 0, big = DBL_MAX, tiny = DBL_MIN, two5 = 2.5, m25 = -2.5;

static uint64_t fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static uint64_t fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }

static const char *mode_name(int m)
{
    switch (m) {
    case FE_TONEAREST: return "nearest";
    case FE_UPWARD: return "upward";
    case FE_DOWNWARD: return "downward";
    case FE_TOWARDZERO: return "towardzero";
    default: return "?";
    }
}

static void flags(const char *what)
{
    int f = fetestexcept(FE_ALL_EXCEPT);
    printf("%-28s fetestexcept %#04x [%s%s%s%s%s%s] fpsr %#llx\n", what, f, f & FE_INVALID ? " invalid" : "",
           f & FE_DIVBYZERO ? " divbyzero" : "", f & FE_OVERFLOW ? " overflow" : "", f & FE_UNDERFLOW ? " underflow" : "",
           f & FE_INEXACT ? " inexact" : "", f & FE_FLUSHTOZERO ? " flushtozero" : "", (unsigned long long)(fpsr() & 0x9f));
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("initial round %s (%#x) fpcr %#llx\n", mode_name(fegetround()), fegetround(), (unsigned long long)fpcr());

    static const int modes[] = {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO, FE_TONEAREST};
    for (int k = 0; k < 4; k++) {
        int rc = fesetround(modes[k]);
        int got = fegetround();
        /* translated arithmetic, native libm (rint, nearbyint, lrint) and a native long double function */
        printf("fesetround(%s) rc %d fegetround %s fpcr %#llx 1/3 %a -1/3 %a rint(2.5) %g rint(-2.5) %g lrint(2.5) %ld "
               "nearbyint(-2.5) %g rintl(2.5) %g\n",
               mode_name(modes[k]), rc, mode_name(got), (unsigned long long)fpcr(), one / three, -one / three, rint(two5),
               rint(m25), lrint(two5), nearbyint(m25), (double)rintl((long double)two5));
    }
    printf("fesetround(0x400) rc %d -> %s\n", fesetround(0x400) != 0, mode_name(fegetround()));
    printf("fesetround(-1) rc %d -> %s\n", fesetround(-1) != 0, mode_name(fegetround()));

    feclearexcept(FE_ALL_EXCEPT);
    flags("cleared");
    volatile double r = one / three;
    flags("1/3");
    feclearexcept(FE_ALL_EXCEPT);
    r = one / zero;
    flags("1/0");
    feclearexcept(FE_ALL_EXCEPT);
    r = zero / zero;
    flags("0/0");
    feclearexcept(FE_ALL_EXCEPT);
    r = big * 2;
    flags("DBL_MAX*2");
    feclearexcept(FE_ALL_EXCEPT);
    r = tiny / 3;
    flags("DBL_MIN/3");
    feclearexcept(FE_ALL_EXCEPT);
    r = sqrt(-one);
    flags("native sqrt(-1)");
    feclearexcept(FE_ALL_EXCEPT);
    r = log(zero);
    flags("native log(0)");
    feclearexcept(FE_INVALID | FE_DIVBYZERO);
    flags("cleared invalid|divbyzero");
    feclearexcept(FE_ALL_EXCEPT);
    volatile long double ld = rintl((long double)two5 / 2);    /* native x87 code on x86_64 */
    flags("native rintl(1.25)");
    feclearexcept(FE_ALL_EXCEPT);
    flags("cleared after rintl");
    (void)ld;
    (void)r;

    feclearexcept(FE_ALL_EXCEPT);
    printf("feraiseexcept(overflow) rc %d\n", feraiseexcept(FE_OVERFLOW));
    flags("raised overflow");
    printf("feraiseexcept(divbyzero|underflow) rc %d\n", feraiseexcept(FE_DIVBYZERO | FE_UNDERFLOW));
    flags("raised divbyzero|underflow");
    printf("fetestexcept(divbyzero|inexact) %#x\n", fetestexcept(FE_DIVBYZERO | FE_INEXACT));

    fexcept_t saved;
    printf("fegetexceptflag rc %d\n", fegetexceptflag(&saved, FE_DIVBYZERO | FE_OVERFLOW));
    feclearexcept(FE_ALL_EXCEPT);
    flags("cleared again");
    printf("fesetexceptflag rc %d\n", fesetexceptflag(&saved, FE_DIVBYZERO));
    flags("restored divbyzero only");

    fenv_t env;
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INEXACT);
    printf("fegetenv rc %d fpsr %#llx fpcr %#llx\n", fegetenv(&env), (unsigned long long)(env.__fpsr & 0x9f),
           (unsigned long long)env.__fpcr);
    fesetround(FE_TONEAREST);
    feclearexcept(FE_ALL_EXCEPT);
    printf("fesetenv rc %d -> %s\n", fesetenv(&env), mode_name(fegetround()));
    flags("after fesetenv");

    printf("fesetenv(FE_DFL_ENV) rc %d -> %s fpcr %#llx\n", fesetenv(FE_DFL_ENV), mode_name(fegetround()),
           (unsigned long long)fpcr());
    flags("default env");

    /* the documented FPCR idiom: edit a saved environment and install it */
    fegetenv(&env);
    env.__fpcr |= __fpcr_flush_to_zero;
    fesetenv(&env);
    printf("FZ via fenv_t: fpcr %#llx DBL_MIN/4 %a\n", (unsigned long long)fpcr(), tiny / 4);
    fesetenv(FE_DFL_ENV);
    printf("FE_DFL_ENV fpsr %#llx fpcr %#llx\n", (unsigned long long)(FE_DFL_ENV)->__fpsr,
           (unsigned long long)(FE_DFL_ENV)->__fpcr);
    printf("FE_DFL_DISABLE_DENORMS_ENV fpcr %#llx\n", (unsigned long long)(FE_DFL_DISABLE_DENORMS_ENV)->__fpcr);
    fesetenv(FE_DFL_DISABLE_DENORMS_ENV);
    printf("denormals off: fpcr %#llx DBL_MIN/4 %a\n", (unsigned long long)fpcr(), tiny / 4);
    fesetenv(FE_DFL_ENV);
    printf("default again: fpcr %#llx DBL_MIN/4 %a\n", (unsigned long long)fpcr(), tiny / 4);

    fesetround(FE_UPWARD);
    feraiseexcept(FE_OVERFLOW);
    printf("feholdexcept rc %d -> %s\n", feholdexcept(&env), mode_name(fegetround()));
    flags("held");
    feraiseexcept(FE_DIVBYZERO);
    fesetround(FE_TOWARDZERO);
    printf("feupdateenv rc %d -> %s\n", feupdateenv(&env), mode_name(fegetround()));
    flags("updated");
    fesetenv(FE_DFL_ENV);
    printf("end round %s fpcr %#llx\n", mode_name(fegetround()), (unsigned long long)fpcr());
    flags("end");
    return 0;
}
