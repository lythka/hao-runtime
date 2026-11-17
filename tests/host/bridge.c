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
#include "hao/swiftvalue.h"
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
        {"_swift_getFunctionReplacement", true}, {"_swift_getOrigOfReplaceable", true}, {"_swift_retain", false},
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
    /* the builtin bool types of a C++ function's encoding (BOOL on arm64, signed char on x86_64):
     * the name with each of them as a, or NULL for -1 */
    static const struct { const char *sym, *all; } bools[] = {
        {"__Z53TSTCellSetRichTextPayloadClearingIDConvertToPlaintextP7TSTCellP18TSTRichTextPayloadbb",
         "__Z53TSTCellSetRichTextPayloadClearingIDConvertToPlaintextP7TSTCellP18TSTRichTextPayloadaa"},
        {"__Z3bobb", "__Z3boba"},                                               /* b in the identifier */
        {"__Z8cb_storePbb", "__Z8cb_storePaa"},
        {"__ZN4cbns5Table4pickEiib", "__ZN4cbns5Table4pickEiia"},
        {"__ZN4cbns6chooseIiEET_S1_S1_b", "__ZN4cbns6chooseIiEET_S1_S1_a"},   /* template function: result type first */
        {"__ZNSt3__16vectorIbNS_9allocatorIbEEE9push_backERKb", "__ZNSt3__16vectorIaNS_9allocatorIaEEE9push_backERKa"},
        {"__ZlsRNSt3__113basic_ostreamIcNS_11char_traitsIcEEEEb", "__ZlsRNSt3__113basic_ostreamIcNS_11char_traitsIcEEEEa"},
        {"__Z1fPFbbE", "__Z1fPFaaE"},
        {"__Z1fiv", "__Z1fiv"},                                                 /* none */
        {"__Z1fIJibEEvb", "__Z1fIJiaEEva"},                                     /* a template argument pack */
        {"__Z1fbbbbbbb", NULL},                                                 /* more than HAO_CXX_BOOL_MAX */
        {"__ZN3hao1S3setEb.cold.1", NULL}, {"__ZTVN3hao1SE", NULL}, {"__ZN3hao1xE", NULL}, {"__ZZ3foovE1b", NULL},
        {"_bool_fn", NULL},
    };
    int bmis = 0;
    char bfirst[200] = "";
    for (size_t k = 0; k < sizeof bools / sizeof *bools; k++) {
        size_t off[HAO_CXX_BOOL_MAX];
        int n = hao_cxx_bool_types(bools[k].sym, off);
        char all[200];
        snprintf(all, sizeof all, "%s", bools[k].sym);
        for (int i = 0; i < n; i++) all[off[i]] = all[off[i]] == 'b' ? 'a' : '?';
        if ((n < 0) != !bools[k].all || (n >= 0 && strcmp(all, bools[k].all)))
            if (!bmis++) snprintf(bfirst, sizeof bfirst, ": %s (%d)", bools[k].sym, n);
    }
    /* nested packs and parameter declarations are bounded like other nestings: a long name is refused, not recursed */
    {
        size_t deep = 1000000;
        char *nm = malloc(2 * deep + 32), *q = nm;
        q += sprintf(q, "__Z1fI");
        memset(q, 'J', deep); q += deep;
        *q++ = 'i';
        memset(q, 'E', deep); q += deep;
        strcpy(q, "Evb");
        size_t off[HAO_CXX_BOOL_MAX];
        if (hao_cxx_bool_types(nm, off) != -1 && !bmis++) snprintf(bfirst, sizeof bfirst, ": %zu nested packs accepted", deep);
        q = nm + sprintf(nm, "__Z1fI");
        for (size_t k = 0; k < deep / 2; k++) { *q++ = 'T'; *q++ = 'y'; }
        strcpy(q, "bEvb");
        if (hao_cxx_bool_types(nm, off) != 2 && !bmis++) snprintf(bfirst, sizeof bfirst, ": a long Ty chain");
        free(nm);
    }
    printf("cxxbool    %s  bool types in C++ function names (BOOL's other spelling on x86_64)%s\n", bmis ? "FAIL" : "PASS", bfirst);
    if (bmis) fail = 1;
    /* native generic results G? (swiftcc.c): the demangler's spellings, measured on libswift_Concurrency and
     * libswiftCore, and entities that must not qualify */
    static const struct { const char *d, *context; int param; bool throwing; } genres[] = {
        {"Swift.ThrowingTaskGroup.Iterator.next(isolation: isolated Swift.Optional<Swift.Actor>) async throws(B) -> Swift.Optional<A>",
         "Swift.ThrowingTaskGroup.Iterator", 0, true},
        {"Swift.ThrowingTaskGroup.next(isolation: isolated Swift.Optional<Swift.Actor>) async throws -> Swift.Optional<A>",
         "Swift.ThrowingTaskGroup", 0, true},
        {"Swift.AsyncStream.Iterator.next(isolation: isolated Swift.Optional<Swift.Actor>) async -> Swift.Optional<A>",
         "Swift.AsyncStream.Iterator", 0, false},
        {"Swift.Dictionary.Values.Iterator.next() -> Swift.Optional<B>", "Swift.Dictionary.Values.Iterator", 1, false},
        {"Swift.CollectionOfOne.Iterator._elements.getter : A?", "Swift.CollectionOfOne.Iterator", 0, false},
        {"static Swift.Foo.make() -> C?", "Swift.Foo", 2, false},
        {"Swift.Set.Iterator.next() -> Swift.Optional<A.Element>", NULL, 0, false},       /* an associated type */
        {"Swift.Foo.bar() -> Swift.Optional<Swift.Int>", NULL, 0, false},
        {"Swift.Foo.bar() -> () -> Swift.Optional<A>", NULL, 0, false},                   /* a function-type result */
        {"(extension in Swift):Swift.AsyncSequence.first() async -> Swift.Optional<A.Element>", NULL, 0, false},
        {"closure #1 () -> Swift.Optional<A> in Swift.Foo.bar() -> ()", NULL, 0, false},
        {"Swift.withCheckedContinuation<A>(isolation: isolated Swift.Optional<Swift.Actor>, function: Swift.String, "
         "_: (Swift.CheckedContinuation<A, Swift.Never>) -> ()) async -> A", NULL, 0, false},
    };
    int gmis = 0;
    char gfirst[200] = "";
    for (size_t k = 0; k < sizeof genres / sizeof *genres; k++) {
        char ctx[256];
        int param = -1;
        bool thr = false, ok = hao_swift_genres_demangled(genres[k].d, ctx, sizeof ctx, &param, &thr);
        bool want = genres[k].context != NULL;
        if (ok != want || (ok && (strcmp(ctx, genres[k].context) || param != genres[k].param || thr != genres[k].throwing)))
            if (!gmis++) snprintf(gfirst, sizeof gfirst, ": %s", genres[k].d);
    }
    printf("genres     %s  native generic results G? from demangled entities%s\n", gmis ? "FAIL" : "PASS", gfirst);
    if (gmis) fail = 1;
    char sw[512], cx[512];
    int s = hao_swift_demangle_selfcheck(sw, sizeof sw), x = hao_cxx_demangle_selfcheck(cx, sizeof cx);
    printf("demangle   %s  Swift %s%s, C++ %s%s\n", s == 0 && x == 0 ? "PASS" : "FAIL", s < 0 ? "no demangler" : s ? "differs: " : "as expected",
           s > 0 ? sw : "", x < 0 ? "no demangler" : x ? "differs: " : "as expected", x > 0 ? cx : "");
    if (s || x) fail = 1;
    /* a demangled name longer than the rules' stack buffers is read whole: ready() async -> Bool in a
     * 3000-character module keeps its narrow result */
    if (s >= 0) {
        char long_sym[3100];
        int at = snprintf(long_sym, sizeof long_sym, "_$s3000");
        memset(long_sym + at, 'm', 3000);
        strcpy(long_sym + at + 3000, "5readySbyYaF");
        char kind = hao_swift_async_result_kind(long_sym);
        printf("demangle   %s  a long demangled name is read whole (kind %d)\n", kind == 'b' ? "PASS" : "FAIL", kind);
        if (kind != 'b') fail = 1;
    }
    /* the nil ring of callback results (swiftvalue.c): an outer callback's entries survive a nested one that
     * stays within the ring and are restored; past the ring they are counted lost, never taken for others */
    {
        hao_thread *t = calloc(1, sizeof *t);
        uint64_t outer[4], inner, V = (uint64_t)(uintptr_t)outer, W = (uint64_t)(uintptr_t)&inner;
        int rmis = 0;
        for (int pass = 0; pass < 2; pass++) {
            unsigned nested = pass ? HAO_SWIFT_MADE_RING + 4 : 8;   /* the nested callback's notes */
            uint64_t lost0 = hao_swift_witness_lost();
            t->swift_made_n = 0;
            for (unsigned i = 0; i < 4; i++) {   /* the outer callback's nils, still in the guest's bits (1) */
                outer[i] = 1;
                unsigned k = t->swift_made_n++ % HAO_SWIFT_MADE_RING;
                t->swift_made[k].value = V; t->swift_made[k].at = (uint64_t)(uintptr_t)&outer[i];
                t->swift_made[k].native = 2; t->swift_made[k].arm = 1; t->swift_made[k].idx = k;
            }
            unsigned mark = t->swift_made_n;
            for (unsigned i = 0; i < nested; i++) {
                inner = 1;
                unsigned idx = t->swift_made_n++, k = idx % HAO_SWIFT_MADE_RING;
                t->swift_made[k].value = W; t->swift_made[k].at = (uint64_t)(uintptr_t)&inner;
                t->swift_made[k].native = 2; t->swift_made[k].arm = 1; t->swift_made[k].idx = idx;
            }
            hao_swift_witness_result(t, mark, 0);   /* the nested callback returns: no indirect result */
            hao_swift_witness_result(t, 0, V);
            uint64_t lost = hao_swift_witness_lost() - lost0;
            bool ok = pass ? lost == 4 && outer[0] == 1 && outer[3] == 1 : lost == 0 && outer[0] == 2 && outer[3] == 2;
            if (!ok && !rmis++) printf("nilring    FAIL  nested %u: lost %llu, outer bits %llu\n", nested,
                                       (unsigned long long)lost, (unsigned long long)outer[0]);
            if (t->swift_made_n != 0 && !rmis++) printf("nilring    FAIL  not reset to the mark\n");
        }
        if (!rmis) printf("nilring    PASS  nested callbacks keep or count the outer's ring entries\n");
        if (rmis) fail = 1;
        free(t);
    }
    if (bad) fail = 1;
    return fail;
}
