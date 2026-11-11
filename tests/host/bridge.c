/* Host test: the bridge plan table (src/bridge.c) against libhao.
 *
 *   capacity  N (default 40000) distinct native entry points get typed
 *             plans and every one reads back with its own kind. The fixed
 *             native signature set this table replaced kept 3072 entries
 *             and dropped the rest to the register bridge.
 *   readers   4 threads look up entry points already published while the
 *             main thread inserts the rest (the table grows under them):
 *             a published entry must never read back missing or wrong.
 *   upgrade   an UNKNOWN entry point becomes TYPED when a source types it
 *             later (a native block's signature); a typed plan is kept.
 *   tables    the interposer tables searched by bisection are sorted, every
 *             name index (interpose.c) finds what a linear scan does, and
 *             every name hao_interpose_names lists is in an index that the
 *             answering interposer searches.
 *   stamp     the generated tables' source build (hao/gen_stamp.h) and this
 *             system's are read; a different build is reported, not failed.
 *   mangled   C++ this-returning constructors and destructors and Swift
 *             fixed-ABI entry points are recognised from fixed mangled names.
 *   demangle  the Swift and C++ demanglers spell the fixed symbols the way the
 *             rules that still read demangled text expect (libswiftCore is
 *             loaded for it).
 *
 *   test_bridge [N]     prints one line per check; exit 0 only if all pass.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include "hao/gen_stamp.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static volatile uint32_t published;    /* entry points [0, published) are in the table */
static volatile int stop;
static uint64_t bad_reads, reads;

static uint64_t addr_of(uint32_t k) { return 0x7ff800000000ull + 16ull * k + 16; }
static char kind_of(uint32_t k) { static const char ks[] = "BcCsS"; return ks[k % 5]; }

static void *reader(void *arg)
{
    (void)arg;
    uint64_t n = 0, bad = 0, x = 88172645463325252ull;
    while (!stop) {
        uint32_t p = published;
        if (!p) continue;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t k = (uint32_t)(x % p);
        const hao_plan *pl = hao_bridge_lookup(addr_of(k));
        if (!pl || pl->kind != kind_of(k) || pl->native != addr_of(k)) bad++;
        n++;
    }
    __atomic_fetch_add(&bad_reads, bad, __ATOMIC_RELAXED);
    __atomic_fetch_add(&reads, n, __ATOMIC_RELAXED);
    return NULL;
}

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 40000;
    hao_opts_init(NULL);
    hao_bridge_init();
    int fail = 0;

    pthread_t th[4];
    for (int k = 0; k < 4; k++) pthread_create(&th[k], NULL, reader, NULL);
    for (uint32_t k = 0; k < n; k++) {
        hao_bridge_note(addr_of(k), kind_of(k), NULL, HAO_BRS_BLOCKSIG);
        __atomic_store_n(&published, k + 1, __ATOMIC_RELEASE);
    }
    stop = 1;
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);

    uint32_t found = 0;
    for (uint32_t k = 0; k < n; k++) {
        const hao_plan *pl = hao_bridge_lookup(addr_of(k));
        if (pl && pl->kind == kind_of(k) && pl->cls == HAO_BR_TYPED) found++;
    }
    const hao_plan_table *t = hao_plan_tab;
    printf("capacity   %s  %u of %u typed entry points read back (table %u slots)\n", found == n ? "PASS" : "FAIL", found, n,
           t ? t->mask + 1 : 0);
    if (found != n) fail = 1;
    printf("readers    %s  %llu lookups during growth, %llu wrong or missing\n", bad_reads ? "FAIL" : "PASS",
           (unsigned long long)reads, (unsigned long long)bad_reads);
    if (bad_reads || !reads) fail = 1;

    /* upgrade: bind an unknown import, then type it; then try to retype it */
    uint64_t a = 0x7ff900000010ull;
    int c0 = hao_bridge_bind("_hao_test_no_such_symbol", "_hao_test_no_such_symbol", a);
    const hao_plan *p0 = hao_bridge_lookup(a);
    hao_bridge_note(a, 'B', NULL, HAO_BRS_BLOCKSIG);
    const hao_plan *p1 = hao_bridge_lookup(a);
    hao_bridge_note(a, 's', NULL, HAO_BRS_BLOCKSIG);
    const hao_plan *p2 = hao_bridge_lookup(a);
    bool up = c0 == HAO_BR_UNKNOWN && p0 && p0->cls == HAO_BR_UNKNOWN && p1 && p1->cls == HAO_BR_TYPED && p1->kind == 'B' &&
              p2 == p1;
    printf("upgrade    %s  UNKNOWN -> TYPED 'B', kept against a second note\n", up ? "PASS" : "FAIL");
    if (!up) fail = 1;
    char why[256];
    int bad = hao_interpose_selfcheck(why, sizeof why);
    printf("tables     %s  bisected tables sorted, name indexes agree with a scan and hold every listed name%s%s\n", bad ? "FAIL" : "PASS", bad ? ": " : "", why);
    char build[32];
    bool same = hao_gen_stamp_check(build, sizeof build);
    printf("stamp      %s  tables from macOS %s (%s), format %d; this system %s%s\n", build[0] ? "PASS" : "FAIL",
           HAO_GEN_OS_VERSION, HAO_GEN_OS_BUILD, HAO_GEN_FORMAT, build[0] ? build : "unknown", same ? " (same)" : "");
    if (!build[0]) fail = 1;

    static const struct { const char *sym; bool yes; } cxx[] = {
        {"__ZN3hao1SC1Ev", true}, {"__ZN3hao1SC2ERKS0_", true}, {"__ZN3hao1SD1Ev", true}, {"__ZN3hao1SD2Ev", true},
        {"__ZN3hao1SCI24BaseEi", true},                                         /* inheriting constructor */
        {"__ZN7xgboost4data13SimpleDMatrixC1INS0_10CSCAdapterEEEPT_fi", true},  /* constructor template */
        {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1B8ne200100IDnEEPKc", true},   /* ABI tag */
        {"__ZN3hao1SD0Ev", false}, {"__ZN3hao1SC3Ev", false}, {"__ZN3hao1S3getEv", false},
        {"__ZN3hao4C1E_1fEv", false},                                           /* hao::C1E_::f() */
        {"__ZThn8_N3hao1SD1Ev", false},
    };
    static const struct { const char *sym; bool yes; } swift[] = {
        {"_$s3hao1SVMa", true}, {"_$s3hao1SVwxx", true}, {"_$s3hao1SVwcp", true},
        {"_$s3hao5readySbyYaF", false}, {"_$s3hao1SV1aSivg", false},
    };
    dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_LAZY);   /* swiftcc.c finds the demangler once it is loaded */
    int mis = 0;
    char first[200] = "";
    for (size_t k = 0; k < sizeof cxx / sizeof *cxx; k++)
        if (hao_cxx_this_return(cxx[k].sym) != cxx[k].yes && !mis++) snprintf(first, sizeof first, ": %s", cxx[k].sym);
    for (size_t k = 0; k < sizeof swift / sizeof *swift; k++)
        if (hao_swift_fixed_abi(swift[k].sym) != swift[k].yes && !mis++) snprintf(first, sizeof first, ": %s", swift[k].sym);
    printf("mangled    %s  C++ this-return and Swift fixed-ABI names from the mangling%s\n", mis ? "FAIL" : "PASS", first);
    if (mis) fail = 1;
    char sw[512], cx[512];
    int s = hao_swift_demangle_selfcheck(sw, sizeof sw), x = hao_cxx_demangle_selfcheck(cx, sizeof cx);
    printf("demangle   %s  Swift %s%s, C++ %s%s\n", s == 0 && x == 0 ? "PASS" : "FAIL", s < 0 ? "no demangler" : s ? "differs: " : "as expected",
           s > 0 ? sw : "", x < 0 ? "no demangler" : x ? "differs: " : "as expected", x > 0 ? cx : "");
    if (s || x) fail = 1;
    if (bad) fail = 1;
    return fail;
}
