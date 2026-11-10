/*
 * Unit test of the interface-unit parser (src/ifunit.c), under ASan/UBSan:
 *   - every seed in tests/fuzz/corpus/ifunit is valid and finds its records;
 *   - one targeted corruption per rule, with the payload hash recomputed so
 *     that the rule itself (not the hash) must reject it, and the reason
 *     must name it;
 *   - every single-byte change of every seed, hash recomputed or not, is
 *     accepted or rejected without reading outside the buffer.
 *   test_ifunit CORPUS_DIR       exit 0 only if every check passes
 */
#include "hao/bridge.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passes, fails;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void rehash(uint8_t *b, size_t n)
{
    uint64_t h = hao_aot_hash64(b + sizeof(hao_if_hdr), n - sizeof(hao_if_hdr), 0);
    memcpy(b + offsetof(hao_if_hdr, payload_hash), &h, 8);
}

/* apply an edit to a copy of the seed, rehash unless told not to, and expect rejection naming `want` */
typedef void (*edit_fn)(uint8_t *b, size_t *n);
static void expect_reject(const char *what, const uint8_t *seed, size_t n, edit_fn e, bool hash, const char *want)
{
    uint8_t *b = malloc(n + 64);
    memcpy(b, seed, n);
    size_t m = n;
    e(b, &m);
    if (hash && m >= sizeof(hao_if_hdr)) rehash(b, m);
    uint8_t *x = malloc(m ? m : 1);   /* exactly sized */
    memcpy(x, b, m);
    char why[256] = "";
    int r = hao_ifunit_check(x, m, NULL, NULL, why, sizeof why);
    CHECK(r != 0 && strstr(why, want), "%s: want rejection \"%s\", got %s \"%s\"", what, want, r ? "rejected" : "accepted", why);
    free(x);
    free(b);
}

static hao_if_rec *rec0(uint8_t *b) { return (hao_if_rec *)(void *)(b + sizeof(hao_if_hdr)); }
static hao_if_hdr *hdr(uint8_t *b) { return (hao_if_hdr *)(void *)b; }

static void e_magic(uint8_t *b, size_t *n) { (void)n; b[0] ^= 1; }
static void e_version(uint8_t *b, size_t *n) { (void)n; hdr(b)->version = 3; }
static void e_recsize(uint8_t *b, size_t *n) { (void)n; hdr(b)->rec_size = 20; }
static void e_filesize(uint8_t *b, size_t *n) { (void)n; hdr(b)->file_size++; }
static void e_truncate(uint8_t *b, size_t *n) { (void)b; *n -= 1; }
static void e_nrecs(uint8_t *b, size_t *n) { (void)n; hdr(b)->nrecs = HAO_IF_MAX_RECS + 1; }
static void e_stroff(uint8_t *b, size_t *n) { (void)n; hdr(b)->strtab_off += 4; }
static void e_hash(uint8_t *b, size_t *n) { b[*n - 2] ^= 0x40; }
static void e_strnul(uint8_t *b, size_t *n) { b[*n - 1] = 'x'; }
static void e_noname(uint8_t *b, size_t *n) { (void)n; rec0(b)->name = 0; }
static void e_order(uint8_t *b, size_t *n) { (void)n; hao_if_rec t = rec0(b)[0]; rec0(b)[0] = rec0(b)[1]; rec0(b)[1] = t; }
static void e_stroob(uint8_t *b, size_t *n) { (void)n; rec0(b)->why = hdr(b)->strtab_size; }
static void e_class(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = 4; }
static void e_flags(uint8_t *b, size_t *n) { (void)n; rec0(b)->flags |= 0x80; }
static void e_resmask(uint8_t *b, size_t *n) { (void)n; rec0(b)->resmask = 4; }
static void e_thunk(uint8_t *b, size_t *n) { (void)n; rec0(b)->thunk = rec0(b)->name; }
static void e_rsv(uint8_t *b, size_t *n) { (void)n; rec0(b)->rsv = 1; }
/* the last record of "all" (_zthunk) is TYPED with a thunk pair; "many" has none */
static hao_if_rec *last(uint8_t *b) { return &rec0(b)[hdr(b)->nrecs - 1]; }
static void e_thunk_noflag(uint8_t *b, size_t *n) { (void)n; if (last(b)->flags & HAO_IFR_THUNK) last(b)->flags &= ~HAO_IFR_THUNK; else rec0(b)->thunk = rec0(b)->name; }
static void e_thunk_kind(uint8_t *b, size_t *n) { (void)n; if (last(b)->flags & HAO_IFR_THUNK) last(b)->kind = 'B'; else { rec0(b)->cls = HAO_BR_TYPED; rec0(b)->kind = 'B'; rec0(b)->flags = HAO_IFR_THUNK; rec0(b)->thunk = rec0(b)->name; } }
static void e_thunk_exact(uint8_t *b, size_t *n) { (void)n; rec0(b)->flags |= HAO_IFR_THUNK; rec0(b)->thunk = last(b)->thunk ? last(b)->thunk : rec0(b)->name; }
static void e_kind(uint8_t *b, size_t *n) { (void)n; rec0(b)->kind = 'q'; }
/* a std::string conversion (src/cxxstr.c) whose roles are missing, or not roles (the record's name) */
static void e_cxxstr_noenc(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_TYPED; rec0(b)->kind = 'Z'; rec0(b)->flags = 0; rec0(b)->enc = 0; }
static void e_cxxstr_badenc(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_TYPED; rec0(b)->kind = 'Z'; rec0(b)->flags = 0; rec0(b)->enc = rec0(b)->name; }
static void e_exact_kind(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_EXACT; rec0(b)->kind = 'B'; }
static void e_typed_bare(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_TYPED; rec0(b)->kind = 0; rec0(b)->flags = 0; }
static void e_sig_noenc(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_TYPED; rec0(b)->kind = 0; rec0(b)->flags = HAO_IFR_SIG; rec0(b)->enc = 0; }
static void e_refused_nowhy(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_REFUSED; rec0(b)->kind = 0; rec0(b)->flags = 0; rec0(b)->why = 0; }
static void e_unknown_mask(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_UNKNOWN; rec0(b)->kind = 0; rec0(b)->flags = HAO_IFR_MASKED; }
/* kind K (tools/gen_stackpack.py): a layout the parser refuses, and one on an EXACT record */
static void e_pack_bad(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_TYPED; rec0(b)->kind = 'K'; rec0(b)->flags = 0; rec0(b)->enc = rec0(b)->name; }
static void e_pack_exact(uint8_t *b, size_t *n) { (void)n; rec0(b)->cls = HAO_BR_EXACT; rec0(b)->kind = 'K'; rec0(b)->flags = 0; rec0(b)->enc = rec0(b)->name; }

/* hao_ifunit_stackpack: what tools/hao_ifunit.py stackpack accepts and refuses */
static void stackpack_cases(void)
{
    static const struct { const char *enc; int n; } c[] = {
        {"0.8,8.1,9.1,10.1,16.8", 5}, {"0.2,2.1,4.4,8.1,16.8,24.1", 6}, {"0.1", 1}, {"120.8", 1},
        {"0.1,1.1,2.1,3.1,4.1,5.1,6.1,7.1,8.1,9.1,10.1,11.1,12.1,13.1,14.1,15.1", 16},
        {"", -1}, {"0.3", -1}, {"1.2", -1}, {"8.1,0.1", -1}, {"0.8,4.4", -1}, {"124.8", -1}, {"128.1", 1}, {"248.8", 1}, {"256.1", -1},
        {"01.1", -1}, {"0.1,", -1}, {"0.1,,1.1", -1}, {"0.18", -1}, {"0", -1}, {"a.1", -1}, {"99999999999.1", -1},
        {"0.1,1.1,2.1,3.1,4.1,5.1,6.1,7.1,8.1,9.1,10.1,11.1,12.1,13.1,14.1,15.1,16.1", 17},
    };
    for (size_t k = 0; k < sizeof c / sizeof c[0]; k++) {
        uint8_t o[HAO_IF_MAX_PACK], w[HAO_IF_MAX_PACK];
        int n = hao_ifunit_stackpack(c[k].enc, o, w);
        CHECK(n == c[k].n, "stack pack \"%s\": %d slots, expected %d", c[k].enc, n, c[k].n);
    }
    uint8_t o[HAO_IF_MAX_PACK], w[HAO_IF_MAX_PACK];
    CHECK(hao_ifunit_stackpack("0.8,8.1,9.1,10.1,16.8", o, w) == 5 && o[3] == 10 && w[3] == 1 && o[4] == 16 && w[4] == 8,
          "stack pack offsets and widths");
    char longpack[256] = "";
    for (unsigned i = 0; i <= HAO_IF_MAX_PACK; i++) {
        char part[16];
        snprintf(part, sizeof part, "%s%u.1", i ? "," : "", i);
        strcat(longpack, part);
        CHECK(hao_ifunit_stackpack(longpack, o, w) == (i < HAO_IF_MAX_PACK ? (int)i + 1 : -1),
              "stack pack slot bound %u", i + 1);
    }
    CHECK(hao_ifunit_stackpack(NULL, o, w) == -1, "stack pack NULL");
}

int main(int argc, char **argv)
{
    stackpack_cases();
    const char *dir = argc > 1 ? argv[1] : "tests/fuzz/corpus/ifunit";
    DIR *d = opendir(dir);
    if (!d) { printf("FAIL no corpus %s\n", dir); return 1; }
    struct dirent *de;
    int seeds = 0;
    uint64_t flips = 0, accepted = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        size_t n;
        uint8_t *seed = load(p, &n);
        if (!seed) continue;
        seeds++;
        hao_if_view v;
        char why[256] = "";
        bool ok = hao_ifunit_check(seed, n, NULL, &v, why, sizeof why) == 0;
        CHECK(ok, "seed %s valid (%s)", de->d_name, why);
        if (ok) {
            uint32_t found = 0;
            for (uint32_t k = 0; k < v.nrecs; k++) found += hao_ifunit_find(&v, v.str + v.recs[k].name) == &v.recs[k];
            CHECK(found == v.nrecs, "seed %s: %u of %u records found by name", de->d_name, found, v.nrecs);
            uint8_t other[16] = {0};
            CHECK(hao_ifunit_check(seed, n, other, NULL, why, sizeof why) != 0 && strstr(why, "UUID"), "seed %s: other UUID rejected",
                  de->d_name);
        }
        if (n < sizeof(hao_if_hdr) + 2 * sizeof(hao_if_rec)) { free(seed); continue; }
        /* the first record of "all" is EXACT and the seed has >= 2 records */
        static const struct { const char *what; edit_fn e; bool hash; const char *want; } c[] = {
            {"magic", e_magic, true, "not a hao interface unit"},
            {"version", e_version, true, "format version"},
            {"record size", e_recsize, true, "record size"},
            {"file size", e_filesize, true, "header says"},
            {"truncated", e_truncate, true, "header says"},
            {"record count", e_nrecs, true, "records"},
            {"string table offset", e_stroff, true, "string table at"},
            {"hash", e_hash, false, "payload hash mismatch"},
            {"string table end", e_strnul, true, "NUL-delimited"},
            {"no name", e_noname, true, "no name"},
            {"order", e_order, true, "not strictly increasing"},
            {"string offset", e_stroob, true, "outside the table"},
            {"class", e_class, true, "class 4"},
            {"flags", e_flags, true, "unknown flags"},
            {"result bits", e_resmask, true, "unknown result bits"},
            {"thunk key without flag", e_thunk, true, "thunk key needs the thunk flag"},
            {"reserved field", e_rsv, true, "reserved field"},
            {"thunk flag without key", e_thunk_noflag, true, "thunk key needs the thunk flag"},
            {"thunk pair with a kind", e_thunk_kind, true, "result kind"},
            {"EXACT with a thunk", e_thunk_exact, true, "EXACT with a conversion"},
            {"kind", e_kind, true, "result kind"},
            {"std::string conversion without roles", e_cxxstr_noenc, true, "kind Z"},
            {"std::string conversion with bad roles", e_cxxstr_badenc, true, "kind Z"},
            {"EXACT with a kind", e_exact_kind, true, "EXACT with a conversion"},
            {"TYPED without a conversion", e_typed_bare, true, "TYPED without a conversion"},
            {"signature without encoding", e_sig_noenc, true, "needs an encoding"},
            {"REFUSED without reason", e_refused_nowhy, true, "REFUSED without a reason"},
            {"UNKNOWN with a mask", e_unknown_mask, true, "UNKNOWN with a type"},
            {"stack pack layout", e_pack_bad, true, "stack pack"},
            {"stack pack on EXACT", e_pack_exact, true, "stack pack"},
        };
        for (size_t k = 0; k < sizeof c / sizeof c[0]; k++) {
            char what[128];
            snprintf(what, sizeof what, "%s: %s", de->d_name, c[k].what);
            expect_reject(what, seed, n, c[k].e, c[k].hash, c[k].want);
        }
        /* every byte, every bit pattern of one flip, with and without the hash fixed */
        uint8_t *x = malloc(n);
        for (size_t i = 0; i < n; i++)
            for (int bit = 0; bit < 8; bit++)
                for (int fix = 0; fix < 2; fix++) {
                    memcpy(x, seed, n);
                    x[i] ^= (uint8_t)(1u << bit);
                    if (fix) rehash(x, n);
                    char w[256];
                    hao_if_view vv;
                    if (hao_ifunit_check(x, n, NULL, &vv, w, sizeof w) == 0) {
                        accepted++;
                        for (uint32_t k = 0; k < vv.nrecs; k++) (void)hao_ifunit_find(&vv, vv.str + vv.recs[k].name);
                    }
                    flips++;
                }
        free(x);
        free(seed);
    }
    closedir(d);
    CHECK(seeds >= 2, "%d seeds", seeds);
    printf("%s ifunit: %d checks passed, %d failed; %llu single-bit edits, %llu accepted\n", fails ? "FAIL" : "PASS", passes, fails,
           (unsigned long long)flips, (unsigned long long)accepted);
    return fails ? 1 : 0;
}
