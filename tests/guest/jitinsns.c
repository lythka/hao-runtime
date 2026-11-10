/* Directed checks of instructions the translator emits inline: each one
 * runs over a table of corner values with NZCV set by a preceding compare,
 * and the result, the memory cell and NZCV afterwards are folded into one
 * hash per instruction. The output must equal a native arm64 run's
 * (tests/guest/jitinsns.sh). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NOINL __attribute__((noinline))
static const uint64_t vals[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff,
    0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0xfffffffffffffffeull,
    0xffffffff80000000ull, 0xffffffffffffff80ull, 0xffffffffffff8000ull, 0x0123456789abcdefull, 0xfedcba9876543210ull,
    0xdeadbeefcafef00dull, 0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull, 0x00ff00ff00ff00ffull, 0x8000000080000000ull,
};
#define NV (sizeof vals / sizeof *vals)

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v; h *= 1099511628211ull; return h ^ (h >> 29); }

/* x0 = a, x1 = b on entry; the instruction leaves its result in x2 */
#define REG2(name, insn)                                                                \
    NOINL static uint64_t name(uint64_t a, uint64_t b, uint64_t *fl)                    \
    {                                                                                   \
        register uint64_t x0 __asm__("x0") = a, x1 __asm__("x1") = b;                   \
        register uint64_t x2 __asm__("x2") = 0x1122334455667788ull, x3 __asm__("x3");   \
        __asm__ volatile("cmp x0, x1\n" insn "\nmrs x3, nzcv"                           \
                         : "+r"(x2), "=r"(x3) : "r"(x0), "r"(x1) : "cc");               \
        *fl = x3;                                                                       \
        return x2;                                                                      \
    }
/* x0 = &cell (holding a), x1 = b; x2 = the instruction's register result */
#define MEM2(name, insn)                                                                \
    NOINL static uint64_t name(uint64_t a, uint64_t b, uint64_t *fl)                    \
    {                                                                                   \
        uint64_t cell[2] = {a, 0x0f0f0f0f0f0f0f0full};                                  \
        register uint64_t *x0 __asm__("x0") = cell;                                     \
        register uint64_t x1 __asm__("x1") = b;                                         \
        register uint64_t x2 __asm__("x2") = a ^ (b & 1), x3 __asm__("x3");             \
        __asm__ volatile("cmp x1, x2\n" insn "\nmrs x3, nzcv"                           \
                         : "+r"(x2), "=r"(x3) : "r"(x0), "r"(x1) : "cc", "memory");     \
        *fl = x3;                                                                       \
        return mix(mix(x2, cell[0]), cell[1]);                                          \
    }

REG2(t_udiv, "udiv x2, x0, x1")
REG2(t_udivw, "udiv w2, w0, w1")
REG2(t_sdiv, "sdiv x2, x0, x1")
REG2(t_sdivw, "sdiv w2, w0, w1")
REG2(t_adc, "adc x2, x0, x1")
REG2(t_adcw, "adc w2, w0, w1")
REG2(t_adcs, "adcs x2, x0, x1")
REG2(t_adcsw, "adcs w2, w0, w1")
REG2(t_sbc, "sbc x2, x0, x1")
REG2(t_sbcw, "sbc w2, w0, w1")
REG2(t_sbcs, "sbcs x2, x0, x1")
REG2(t_sbcsw, "sbcs w2, w0, w1")
REG2(t_ngcs, "ngcs x2, x1")
REG2(t_add128, "adds x2, x0, x1\nadcs x2, x2, x0\nadc x2, x2, x1")
REG2(t_rbit, "rbit x2, x0")
REG2(t_rbitw, "rbit w2, w1")
REG2(t_rev16, "rev16 x2, x0")
REG2(t_rev16w, "rev16 w2, w1")
REG2(t_rev32, "rev32 x2, x0")
REG2(t_ctz, "rbit x2, x0\nclz x2, x2")
REG2(t_bfi0, "mov x2, x0\nbfi x2, x1, #0, #1")
REG2(t_bfi1, "mov x2, x0\nbfi x2, x1, #8, #16")
REG2(t_bfi2, "mov x2, x0\nbfi x2, x1, #40, #24")
REG2(t_bfi3, "mov x2, x0\nbfi x2, x1, #63, #1")
REG2(t_bfi4, "mov x2, x0\nbfi w2, w1, #5, #27")
REG2(t_bfi5, "mov x2, x0\nbfi x2, x1, #32, #32")
REG2(t_bfxil0, "mov x2, x0\nbfxil x2, x1, #0, #64")
REG2(t_bfxil1, "mov x2, x0\nbfxil x2, x1, #16, #8")
REG2(t_bfxil2, "mov x2, x0\nbfxil x2, x1, #33, #31")
REG2(t_bfxil3, "mov x2, x0\nbfxil w2, w1, #31, #1")
REG2(t_bfxil4, "mov x2, x0\nbfxil w2, w1, #0, #32")
REG2(t_bfc, "mov x2, x0\nbfc x2, #12, #40")
REG2(t_bfcw, "mov x2, x1\nbfc w2, #0, #7")
REG2(t_movk, "mov x2, #0x1234\nmovk x2, #0x5678, lsl #16\nmovk x2, #0x9abc, lsl #32\nmovk x2, #0xdef0, lsl #48\nadd x2, x2, x0")
REG2(t_movkw, "mov w2, #0xffff\nmovk w2, #0x8000, lsl #16\neor x2, x2, x1")
REG2(t_movk2, "mov x2, x0\nmovk x2, #0xbeef, lsl #16\nmovk x2, #0xdead, lsl #48")
REG2(t_movn, "mov x2, #-0x1235\nmovk x2, #0x1, lsl #32\nsub x2, x2, x1")

#define ATOM(n, mn)                                    \
    MEM2(t_##n##b, mn "b w1, w2, [x0]")                \
    MEM2(t_##n##h, mn "h w1, w2, [x0]")                \
    MEM2(t_##n##w, mn " w1, w2, [x0]")                 \
    MEM2(t_##n##x, mn " x1, x2, [x0]")
ATOM(ldadd, "ldaddal")
ATOM(ldclr, "ldclral")
ATOM(ldeor, "ldeoral")
ATOM(ldset, "ldsetal")
ATOM(ldsmax, "ldsmaxal")
ATOM(ldsmin, "ldsminal")
ATOM(ldumax, "ldumaxal")
ATOM(ldumin, "lduminal")
ATOM(swp, "swpal")
ATOM(ldaddr, "ldadda")
ATOM(swpl, "swpl")
ATOM(cas, "casal")     /* x2 = expected (a or a^1), x1 = new */
MEM2(t_stadd, "staddl x1, [x0]")
MEM2(t_stclrb, "stclrb w1, [x0]")
MEM2(t_casx2, "mov x2, x1\ncasa x2, x1, [x0]")
MEM2(t_ldapr, "ldapr x2, [x0]")
MEM2(t_ldaprb, "ldaprb w2, [x0]")
MEM2(t_ldraa, "ldraa x2, [x0, #8]")
MEM2(t_ldrab, "add x3, x0, #16\nldrab x2, [x3, #-16]!\nsub x3, x3, x0\nadd x2, x2, x3")
MEM2(t_stlr, "stlrh w1, [x0]\nldarb w2, [x0]")

typedef uint64_t (*fn2)(uint64_t, uint64_t, uint64_t *);
static void run(const char *name, fn2 f)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned i = 0; i < NV; i++)
        for (unsigned j = 0; j < NV; j++) {
            uint64_t fl = 0, r = f(vals[i], vals[j], &fl);
            h = mix(mix(h, r), fl);
        }
    printf("%-10s %016llx\n", name, (unsigned long long)h);
}

/* contended atomics: sums and extremes are the same in every interleaving */
#define NT 4
#define ROUNDS 200000
static uint64_t ctr_add, ctr_cas, ctr_max, ctr_or;
static uint32_t ctr_w;
static uint8_t ctr_b;
static void *worker(void *arg)
{
    uint64_t id = (uint64_t)(uintptr_t)arg;
    for (uint64_t k = 0; k < ROUNDS; k++) {
        __atomic_fetch_add(&ctr_add, k + id, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&ctr_w, 3, __ATOMIC_RELAXED);
        __atomic_fetch_add(&ctr_b, 1, __ATOMIC_RELAXED);
        uint64_t old = __atomic_load_n(&ctr_cas, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&ctr_cas, &old, old + 2, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) { }
        uint64_t m = k * NT + id;
        __asm__ volatile("ldumaxal %[m], %[m], [%[p]]" : [m] "+r"(m) : [p] "r"(&ctr_max) : "memory");
        __atomic_fetch_or(&ctr_or, 1ull << ((k + id) & 63), __ATOMIC_SEQ_CST);
    }
    return NULL;
}

#define R(n) run(#n, t_##n)
#define RA(n) R(n##b); R(n##h); R(n##w); R(n##x)
int main(void)
{
    R(udiv); R(udivw); R(sdiv); R(sdivw);
    R(adc); R(adcw); R(adcs); R(adcsw); R(sbc); R(sbcw); R(sbcs); R(sbcsw); R(ngcs); R(add128);
    R(rbit); R(rbitw); R(rev16); R(rev16w); R(rev32); R(ctz);
    R(bfi0); R(bfi1); R(bfi2); R(bfi3); R(bfi4); R(bfi5);
    R(bfxil0); R(bfxil1); R(bfxil2); R(bfxil3); R(bfxil4); R(bfc); R(bfcw);
    R(movk); R(movkw); R(movk2); R(movn);
    RA(ldadd); RA(ldclr); RA(ldeor); RA(ldset); RA(ldsmax); RA(ldsmin); RA(ldumax); RA(ldumin);
    RA(swp); RA(ldaddr); RA(swpl); RA(cas);
    R(stadd); R(stclrb); R(casx2); R(ldapr); R(ldaprb); R(ldraa); R(ldrab); R(stlr);
    pthread_t th[NT];
    for (uintptr_t k = 0; k < NT; k++) pthread_create(&th[k], NULL, worker, (void *)k);
    for (int k = 0; k < NT; k++) pthread_join(th[k], NULL);
    printf("threads    add=%llu w=%u b=%u cas=%llu max=%llu or=%016llx\n", (unsigned long long)ctr_add, ctr_w, ctr_b,
           (unsigned long long)ctr_cas, (unsigned long long)ctr_max, (unsigned long long)ctr_or);
    return 0;
}
