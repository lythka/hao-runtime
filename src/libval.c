/*
 * hao — library validation, as the guest would get it natively, and the one
 * reader of a guest's embedded code signature.
 *
 * hao's processes carry com.apple.security.cs.disable-library-validation so
 * that dyld maps libhao (ad hoc, not platform) into a stub whose program is a
 * platform or hardened-runtime binary. On its own that entitlement would let
 * the process load any validly signed library, where the program on arm64
 * loads only platform libraries or libraries of its own Team ID. 914 of the
 * 933 arm64-only 27.0 executables are such programs (924 platform, 12 with
 * their own disable-library-validation; tools/libval27.py).
 *
 * So hao restores the native rule once its own libraries are in:
 *  - the policy comes from the main executable's own code signature (its arm
 *    slice's CodeDirectory flags, platform identifier and entitlements), the
 *    rule AMFI applies at exec: platform (CS_FORCED_LV), runtime or library
 *    flag, and no disable-library-validation entitlement. It is read from
 *    the CodeDirectory that integrity was checked with, on the bytes the
 *    image is built from (hao_slice_verify below): never from a second
 *    reading of the file or of the signature. A program that misstates its
 *    signature only changes its own policy;
 *  - native loads (dlopen by the guest or by hao's binder, NSBundle, ...):
 *    the process sets CS_REQUIRE_LV on itself (csops CS_OPS_SET_STATUS,
 *    allowed for any valid process). From then on the kernel refuses every
 *    non-platform library of another Team ID in this process, whatever its
 *    entitlements (measured: an ad hoc dylib is refused with "mapping
 *    process and mapped file (non-platform) have different Team IDs" in a
 *    process signed with disable-library-validation);
 *  - guest images (arm64 dylibs hao maps as data, which the kernel never
 *    validates as code), on the bytes the loader then builds the image
 *    from (check_image below; the slice is taken once, hao/macho.h
 *    hao_slice): every page hash, checked by hao, whatever the policy; then
 *    the loader attaches the slice's signature to the vnode
 *    (F_ADDFILESIGS_INFO, the cdhash must be hao's) and asks the kernel
 *    whether this process may map it (F_CHECK_LV). If the kernel refused
 *    CS_REQUIRE_LV, a guest image must at least be platform code
 *    (F_GETSIGSINFO). Where the kernel cannot hold the signature (an x86_64
 *    kernel accepts only 4 KiB-page CodeDirectories; Apple's arm64 code is
 *    signed with 16 KiB pages, so on Intel this is every Apple library) hao
 *    applies AMFI's rule itself with the signer it establishes
 *    (guest_sig.c): a trusted cdhash list or Security's `anchor apple`
 *    (platform), else the main executable's Team ID with a certificate for
 *    it. HAO_LIBVAL_USER takes that route on any kernel (tests).
 * Images from the arm64e shared cache (dsc.c) come from the OS cache paths
 * and are not checked.
 *
 * One signature, read once. A slice has at most one LC_CODE_SIGNATURE
 * (hao_macho_check_cmds refuses a second), hao_slice_open records where it
 * is, and hao_codesig_parse below is the only code in hao that reads it:
 * structure, the CodeDirectory of highest hash rank, cdhash, sealed blobs.
 * The CLI's policy (guest_sig.c), integrity and library validation all use
 * its result, so no two of them can be shown different signatures.
 * tools/hao_codesig.py is the same reader for the packaging tools;
 * tests/tools/test_codesig.py holds the two to the same answer.
 *
 * An embedded program (tools/hao-embed) is a fat file whose x86_64 slice,
 * the hao stub, is what the kernel validates and runs, with the program's
 * identifier and entitlements, and whose ARM slice is the program. The
 * stub's signature does not cover the ARM slice, so the stub says which one
 * it was made for: hao-embed writes the slice's cdhash into the stub's
 * __TEXT,__hao_slice, which the stub's signature does cover, and the gate
 * below loads a main executable in such a process only if its verified
 * CodeDirectory is that one (hao_libval_bound). Without it, a copy of the
 * file with another ARM slice would run that code under the stub's identity.
 *
 * This file depends only on libc (CommonCrypto is libSystem's): the loader
 * unit tests and fuzzers link it with macho.c.
 */
#include "hao/macho.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crt_externs.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_OPS_SET_STATUS 9
#define CS_REQUIRE_LV 0x2000
#define CS_RUNTIME 0x10000

/* ------------------------------------------------ the signature format */
/* osfmk/kern/cs_blobs.h (xnu-12377); every field is big-endian. */
enum {
    CSMAGIC_REQUIREMENTS = 0xfade0c01, CSMAGIC_CODEDIRECTORY = 0xfade0c02, CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0,
    CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xfade7171, CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xfade7172,
    CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT = 0xfade8181,
    CSSLOT_CODEDIRECTORY = 0, CSSLOT_REQUIREMENTS = 2, CSSLOT_ENTITLEMENTS = 5, CSSLOT_DER_ENTITLEMENTS = 7,
    CSSLOT_LAUNCH_CONSTRAINT_SELF = 8, CSSLOT_LIBRARY_CONSTRAINT = 11,
    CSSLOT_ALTERNATE_CODEDIRECTORIES = 0x1000, CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT = 0x1005,
    CS_SUPPORTSSCATTER = 0x20100, CS_SUPPORTSTEAMID = 0x20200, CS_SUPPORTSCODELIMIT64 = 0x20300,
    CS_HASHTYPE_SHA1 = 1, CS_HASHTYPE_SHA256 = 2, CS_HASHTYPE_SHA256_TRUNCATED = 3, CS_HASHTYPE_SHA384 = 4,
};
#define CS_HASH_MAX 48
#define CS_MAX_BLOB (40u << 20)        /* xnu's CS_MAX_BLOB_SIZE for F_ADDFILESIGS */

static const char DLV[] = "com.apple.security.cs.disable-library-validation";

typedef struct {                       /* the CodeDirectory fields read here */
    uint32_t magic, length, version, flags, hashOffset, identOffset, nSpecialSlots, nCodeSlots, codeLimit;
    uint8_t hashSize, hashType, platform, pageSize;
    uint32_t scatterOffset, teamOffset;
    uint64_t codeLimit64;
} cd_hdr;

static uint32_t be32p(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static uint64_t be64p(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }

/* xnu cs_find_md: effective (slot) size of each hash type; 0 = unsupported */
static unsigned hash_size(uint8_t type)
{
    switch (type) {
    case CS_HASHTYPE_SHA1: case CS_HASHTYPE_SHA256_TRUNCATED: return 20;
    case CS_HASHTYPE_SHA256: return 32;
    case CS_HASHTYPE_SHA384: return 48;
    default: return 0;
    }
}

/* xnu hashPriorities: SHA-1 < SHA-256/20 < SHA-256 < SHA-384 */
static unsigned hash_rank(uint8_t type)
{
    static const uint8_t pri[] = {CS_HASHTYPE_SHA1, CS_HASHTYPE_SHA256_TRUNCATED, CS_HASHTYPE_SHA256, CS_HASHTYPE_SHA384};
    for (unsigned k = 0; k < sizeof pri; k++) if (pri[k] == type) return k + 1;
    return 0;
}

static void md(uint8_t t, const void *p, size_t n, uint8_t out[CS_HASH_MAX])
{
    union { CC_SHA1_CTX s1; CC_SHA256_CTX s256; CC_SHA512_CTX s384; } c;
    if (t == CS_HASHTYPE_SHA1) CC_SHA1_Init(&c.s1);
    else if (t == CS_HASHTYPE_SHA384) CC_SHA384_Init(&c.s384);
    else CC_SHA256_Init(&c.s256);
    while (n) {                                       /* CC_LONG is 32 bits */
        CC_LONG k = n > 0x40000000u ? 0x40000000u : (CC_LONG)n;
        if (t == CS_HASHTYPE_SHA1) CC_SHA1_Update(&c.s1, p, k);
        else if (t == CS_HASHTYPE_SHA384) CC_SHA384_Update(&c.s384, p, k);
        else CC_SHA256_Update(&c.s256, p, k);
        p = (const uint8_t *)p + k;
        n -= k;
    }
    if (t == CS_HASHTYPE_SHA1) CC_SHA1_Final(out, &c.s1);
    else if (t == CS_HASHTYPE_SHA384) CC_SHA384_Final(out, &c.s384);
    else CC_SHA256_Final(out, &c.s256);
}

static bool cd_parse(const uint8_t *p, size_t len, cd_hdr *h)
{
    if (len < 44) return false;
    h->magic = be32p(p); h->length = be32p(p + 4); h->version = be32p(p + 8); h->flags = be32p(p + 12);
    h->hashOffset = be32p(p + 16); h->identOffset = be32p(p + 20); h->nSpecialSlots = be32p(p + 24);
    h->nCodeSlots = be32p(p + 28); h->codeLimit = be32p(p + 32);
    h->hashSize = p[36]; h->hashType = p[37]; h->platform = p[38]; h->pageSize = p[39];
    h->scatterOffset = h->version >= CS_SUPPORTSSCATTER && len >= 48 ? be32p(p + 44) : 0;
    h->teamOffset = h->version >= CS_SUPPORTSTEAMID && len >= 52 ? be32p(p + 48) : 0;
    h->codeLimit64 = h->version >= CS_SUPPORTSCODELIMIT64 && len >= 64 ? be64p(p + 56) : 0;
    return true;
}

/* xnu cs_validate_codedirectory, plus what hao needs to check every page
 * itself: no scatter vector, a 4 KiB or 16 KiB page size. */
static bool cd_valid(const uint8_t *p, size_t len, cd_hdr *h, char *why, size_t n)
{
#define BAD(...) do { snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (!cd_parse(p, len, h) || h->magic != CSMAGIC_CODEDIRECTORY) BAD("malformed CodeDirectory");
    unsigned hs = hash_size(h->hashType);
    if (!hs || h->hashSize != hs) BAD("CodeDirectory hash type %u, size %u", h->hashType, h->hashSize);
    if (h->pageSize != 12 && h->pageSize != 14) BAD("CodeDirectory page size 2^%u", h->pageSize);
    if (len < h->hashOffset || h->hashOffset / hs < h->nSpecialSlots || (len - h->hashOffset) / hs < h->nCodeSlots)
        BAD("CodeDirectory hash slots outside it");
    if (h->scatterOffset) BAD("scattered CodeDirectory");
    if (!h->identOffset || h->identOffset >= len || !memchr(p + h->identOffset, 0, len - h->identOffset))
        BAD("CodeDirectory identifier malformed");
    if (h->teamOffset && (h->teamOffset >= len || !memchr(p + h->teamOffset, 0, len - h->teamOffset)))
        BAD("CodeDirectory team identifier malformed");
    return true;
#undef BAD
}

/* The superblob's blob for slot type with this magic (xnu csblob_find_blob_bytes) */
static const uint8_t *sb_find(const uint8_t *sb, uint32_t type, uint32_t magic, uint32_t *blen)
{
    uint32_t count = be32p(sb + 8);
    for (uint32_t k = 0; k < count; k++) {
        if (be32p(sb + 12 + 8 * k) != type) continue;
        uint32_t off = be32p(sb + 16 + 8 * k);
        const uint8_t *b = sb + off;
        if (be32p(b) != magic) continue;
        *blen = be32p(b + 4);
        return b;
    }
    return NULL;
}

/* <key>KEY</key> followed by <true/> in an XML plist (bounded). */
static bool xml_true(const uint8_t *p, size_t n, const char *key)
{
    char pat[128];
    int pl = snprintf(pat, sizeof pat, "<key>%s</key>", key);
    if (pl <= 0 || (size_t)pl >= sizeof pat) return false;
    for (size_t i = 0; i + (size_t)pl <= n; i++) {
        if (memcmp(p + i, pat, (size_t)pl) != 0) continue;
        size_t j = i + (size_t)pl;
        while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == '\n' || p[j] == '\r')) j++;
        return j + 5 <= n && !memcmp(p + j, "<true", 5);
    }
    return false;
}

/* UTF8String KEY then BOOLEAN TRUE in a DER entitlements dictionary. */
static bool der_true(const uint8_t *p, size_t n, const char *key)
{
    size_t kl = strlen(key);
    if (kl > 127) return false;
    for (size_t i = 0; i + 2 + kl + 3 <= n; i++)
        if (p[i] == 0x0c && p[i + 1] == kl && !memcmp(p + i + 2, key, kl) && p[i + 2 + kl] == 0x01 &&
            p[i + 3 + kl] == 0x01 && p[i + 4 + kl] != 0)
            return true;
    return false;
}

/*
 * xnu cs_validate_csblob over the embedded signature: every index entry in
 * range, CodeDirectories valid and of distinct hash types (the one of
 * highest rank is used; a second CodeDirectory of any hash type already
 * seen is refused, which is stricter than xnu, which refuses only a repeat
 * of the best), at most one entitlements/DER/constraint blob each, with the
 * right magic. Then xnu's csblob_find_special_slot_blob for every blob kind
 * the CodeDirectory seals: a present blob must have its slot and match it,
 * an absent one must have no slot or an all-zero one.
 */
bool hao_codesig_parse(const uint8_t *sb, size_t len, hao_codesig *s, char *why, size_t n)
{
#define BAD(...) do { snprintf(why, n, __VA_ARGS__); return false; } while (0)
    memset(s, 0, sizeof *s);
    if (len < 12 || be32p(sb) != CSMAGIC_EMBEDDED_SIGNATURE || be32p(sb + 4) > len || be32p(sb + 4) < 12)
        BAD("not an embedded signature");
    len = be32p(sb + 4);
    uint32_t count = be32p(sb + 8);
    if ((len - 12) / 8 < count) BAD("signature index past its end");
    unsigned best_rank = 0, ranks = 0;                   /* bit per hash rank seen */
    bool primary = false;
    uint32_t seen = 0;                                   /* bit per sealed slot type 2..11 */
    const uint8_t *cd = NULL;
    cd_hdr h = {0};
    for (uint32_t k = 0; k < count; k++) {
        uint32_t type = be32p(sb + 12 + 8 * k), off = be32p(sb + 16 + 8 * k);
        if (off > len || len - off < 8 || be32p(sb + off + 4) > len - off || be32p(sb + off + 4) < 8)
            BAD("signature blob %u outside the signature", k);
        const uint8_t *b = sb + off;
        uint32_t bl = be32p(b + 4), magic = be32p(b);
        if (type == CSSLOT_CODEDIRECTORY || (type >= CSSLOT_ALTERNATE_CODEDIRECTORIES && type < CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT)) {
            cd_hdr c;
            if (!cd_valid(b, bl, &c, why, n)) return false;
            primary |= type == CSSLOT_CODEDIRECTORY;
            unsigned r = hash_rank(c.hashType);
            if (ranks & (1u << r)) BAD("two CodeDirectories with hash type %u", c.hashType);
            ranks |= 1u << r;
            if (r > best_rank) { best_rank = r; cd = b; h = c; }
        } else if (type == CSSLOT_REQUIREMENTS || type == CSSLOT_ENTITLEMENTS || type == CSSLOT_DER_ENTITLEMENTS ||
                   (type >= CSSLOT_LAUNCH_CONSTRAINT_SELF && type <= CSSLOT_LIBRARY_CONSTRAINT)) {
            uint32_t want = type == CSSLOT_REQUIREMENTS ? CSMAGIC_REQUIREMENTS : type == CSSLOT_ENTITLEMENTS
                ? CSMAGIC_EMBEDDED_ENTITLEMENTS : type == CSSLOT_DER_ENTITLEMENTS ? CSMAGIC_EMBEDDED_DER_ENTITLEMENTS
                : CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT;
            if (magic != want) BAD("signature blob for slot %u has magic %#x", type, magic);
            if (seen & (1u << type)) BAD("two signature blobs for slot %u", type);
            seen |= 1u << type;
        }
    }
    if (!primary || !cd) BAD("no CodeDirectory");
    unsigned hs = h.hashSize;
    static const struct { uint32_t slot, magic; } sealed[] = {
        {CSSLOT_REQUIREMENTS, CSMAGIC_REQUIREMENTS}, {CSSLOT_ENTITLEMENTS, CSMAGIC_EMBEDDED_ENTITLEMENTS},
        {CSSLOT_DER_ENTITLEMENTS, CSMAGIC_EMBEDDED_DER_ENTITLEMENTS}, {8, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT},
        {9, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT}, {10, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT},
        {11, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT}};
    static const uint8_t zero[CS_HASH_MAX];
    for (size_t k = 0; k < sizeof sealed / sizeof sealed[0]; k++) {
        uint32_t bl = 0;
        const uint8_t *b = sb_find(sb, sealed[k].slot, sealed[k].magic, &bl);
        const uint8_t *slot = sealed[k].slot <= h.nSpecialSlots ? cd + h.hashOffset - sealed[k].slot * hs : NULL;
        if (!slot) {
            if (b) BAD("signature blob for slot %u is not sealed by the CodeDirectory", sealed[k].slot);
            continue;
        }
        if (!b) {
            if (memcmp(slot, zero, hs)) BAD("signature blob for slot %u is missing", sealed[k].slot);
            continue;
        }
        uint8_t got[CS_HASH_MAX];
        md(h.hashType, b, bl, got);
        if (memcmp(got, slot, hs)) BAD("signature blob for slot %u does not match its hash", sealed[k].slot);
        if (sealed[k].slot == CSSLOT_ENTITLEMENTS) { s->ents = b; s->ents_len = bl; }
        if (sealed[k].slot == CSSLOT_DER_ENTITLEMENTS) { s->der_ents = b; s->der_ents_len = bl; }
    }
    uint8_t full[CS_HASH_MAX];
    md(h.hashType, cd, h.length, full);
    memcpy(s->cdhash, full, sizeof s->cdhash);
    s->cd = cd;
    s->cd_len = h.length;
    s->flags = h.flags;
    s->platform = h.platform;
    s->hash_type = h.hashType;
    s->hash_size = h.hashSize;
    s->page_shift = h.pageSize;
    s->hash_off = h.hashOffset;
    s->n_code_slots = h.nCodeSlots;
    s->code_limit = h.codeLimit64 ? h.codeLimit64 : h.codeLimit;
    s->ident = (const char *)cd + h.identOffset;
    s->team = h.teamOffset && cd[h.teamOffset] ? (const char *)cd + h.teamOffset : NULL;
    /* the entitlement as the sealed blobs state it: the XML form where there is one, else the DER form */
    s->dlv = s->ents ? xml_true(s->ents + 8, s->ents_len - 8, DLV)
                     : s->der_ents && der_true(s->der_ents + 8, s->der_ents_len - 8, DLV);
    return true;
#undef BAD
}

bool hao_codesig_library_validated(const hao_codesig *cs)
{
    if (!cs->cd || cs->dlv) return false;
    return cs->platform != 0 || (cs->flags & (CS_RUNTIME | CS_REQUIRE_LV)) != 0;
}

/* ------------------------------------------------ integrity of a slice */

typedef struct { uint64_t off, end; } hao_range;   /* slice offsets [off, end) */

/* the file ranges of the executable segments (the pages the kernel validates natively) */
static int exec_ranges(const uint8_t *cmds, uint32_t ncmds, hao_range *r, int max)
{
    int n = 0;
    const uint8_t *p = cmds;
    for (uint32_t k = 0; k < ncmds && n < max; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *sg = (const void *)p;
        if (sg->cmd != LC_SEGMENT_64 || !(sg->initprot & VM_PROT_EXECUTE) || !sg->filesize) continue;
        r[n++] = (hao_range){sg->fileoff, sg->fileoff + sg->filesize};
    }
    return n;
}

/* Every page hash of [0, code_limit) of the slice at bytes, which the
 * caller has checked to hold code_limit bytes: the memory the image is
 * built from, so the pages hashed here are the pages that run. *bad_exec
 * (may be NULL) is set when a page that does not match its hash overlaps
 * one of the nexec ranges (an executable segment). */
static bool pages_valid(const uint8_t *bytes, const hao_codesig *s, const hao_range *exec, int nexec, bool *bad_exec,
                        char *why, size_t n)
{
    uint64_t page = 1ull << s->page_shift;
    uint64_t pages = (s->code_limit + page - 1) / page;
    if (pages != s->n_code_slots) {
        snprintf(why, n, "the CodeDirectory hashes %u pages of a %llu-byte code range", s->n_code_slots,
                 (unsigned long long)s->code_limit);
        return false;
    }
    const uint8_t *slots = s->cd + s->hash_off;
    for (uint64_t at = 0, i = 0; at < s->code_limit; at += page, i++) {
        size_t len = s->code_limit - at < page ? (size_t)(s->code_limit - at) : (size_t)page;
        uint8_t d[CS_HASH_MAX];
        md(s->hash_type, bytes + at, len, d);
        if (memcmp(d, slots + i * s->hash_size, s->hash_size)) {
            snprintf(why, n, "page %llu (offset %#llx) does not match its hash", (unsigned long long)i, (unsigned long long)at);
            for (int k = 0; bad_exec && k < nexec; k++)
                if (at < exec[k].end && at + len > exec[k].off) *bad_exec = true;
            return false;
        }
    }
    return true;
}

/*
 * The slice's signature, as xnu validates one and as far as hao must go
 * because no kernel checks pages it maps as data: the structure
 * (hao_codesig_parse), every page hash at the page size the CodeDirectory
 * declares, and nothing in the slice outside the hashed range but the
 * signature itself (the code limit reaches the signature and nothing
 * follows it), so every byte the loader uses is covered. sl->sig is the
 * result: whoever decides anything about this image reads it there.
 * 0: valid; -1: not (why; *bad_exec, if not NULL, when a page of an
 * executable segment does not match its hash).
 *
 * The slice's bytes do not change (hao_slice), so it is hashed once: the
 * CLI's policy (hao_guest_sig_enforce), the main executable's policy
 * (decide) and a library's integrity and signer (check_image) all ask
 * here, and whoever asks second gets the first answer.
 */
unsigned hao_slice_verify_passes;

int hao_slice_verify(hao_slice *sl, bool *bad_exec, char *why, size_t n)
{
    hao_codesig s;
    if (bad_exec) *bad_exec = false;
    if (sl->sig.cd) return 0;
    if (!sl->sig_size) { snprintf(why, n, "not code signed"); return -1; }
    if ((uint64_t)sl->sig_off + sl->sig_size > sl->size || sl->sig_size > CS_MAX_BLOB) {
        snprintf(why, n, "code signature [%#x, +%#x) outside the slice", sl->sig_off, sl->sig_size);
        return -1;
    }
    if (!hao_codesig_parse(sl->bytes + sl->sig_off, sl->sig_size, &s, why, n)) return -1;
    uint64_t sig_end = (uint64_t)sl->sig_off + sl->sig_size;
    if (s.code_limit < sl->sig_off || s.code_limit > sl->size || sl->size > (s.code_limit > sig_end ? s.code_limit : sig_end)) {
        snprintf(why, n, "it covers [0, %#llx) of a %#llx-byte slice whose signature is at %#x",
                 (unsigned long long)s.code_limit, (unsigned long long)sl->size, sl->sig_off);
        return -1;
    }
    hao_range ex[16];
    int nex = exec_ranges(sl->cmds, sl->ncmds, ex, 16);
    hao_slice_verify_passes++;
    if (!pages_valid(sl->bytes, &s, ex, nex, bad_exec, why, n)) return -1;
    sl->sig = s;
    return 0;
}

/* ------------------------------------------- an embedded program's slice */

int hao_libval_bound(const void *record, size_t size, hao_slice *sl, char *why, size_t n)
{
    hao_slice_record r;
    char w[300];
    if (size != sizeof r) { snprintf(why, n, "its x86_64 slice names its ARM code in a record of %zu bytes, not %zu", size, sizeof r); return -1; }
    memcpy(&r, record, sizeof r);
    if (memcmp(r.magic, HAO_SLICE_MAGIC, sizeof r.magic) || r.version != 1 || r.zero[0] || r.zero[1]) {
        snprintf(why, n, "its x86_64 slice names its ARM code in a record this hao does not read");
        return -1;
    }
    if (!sl->sig_size) { snprintf(why, n, "its ARM code is not code signed; its x86_64 slice was made for signed code"); return -1; }
    if (hao_slice_verify(sl, NULL, w, sizeof w) != 0) { snprintf(why, n, "code signature invalid (%s)", w); return -1; }
    if (r.hash_type != sl->sig.hash_type || memcmp(r.cdhash, sl->sig.cdhash, sizeof r.cdhash)) {
        char got[41], want[41];
        for (int k = 0; k < 20; k++) {
            snprintf(got + 2 * k, 3, "%02x", sl->sig.cdhash[k]);
            snprintf(want + 2 * k, 3, "%02x", r.cdhash[k]);
        }
        snprintf(why, n, "its ARM code (cdhash %s) is not the code its x86_64 slice was made for (cdhash %s)", got, want);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------- library validation */

static int mode = HAO_LIBVAL_UNDECIDED;
static bool expect_main;
static char reason[160];
static hao_imgsig_fn verifier;      /* guest_sig.c's signer check (NULL: the loader's unit tests) */
static bool force_user;             /* HAO_LIBVAL_USER: signers in user space even where the kernel answers */
static bool integrity;              /* the policy came from the main executable's signature: pages are checked */
static bool main_platform;          /* the main executable's CodeDirectory has a platform identifier */
static char main_team[32];          /* its Team ID ("" none, or longer than 31 bytes: compared as none) */
static char route[96];              /* how this image was decided (img->libval_route) */

static void (*report_line)(const char *);

void hao_libval_set_verifier(hao_imgsig_fn fn, bool force, void (*report)(const char *))
{
    verifier = fn;
    force_user = force;
    report_line = report;
}

void hao_libval_expect_main(bool enabled)
{
    expect_main = true;
    if (!enabled && mode == HAO_LIBVAL_UNDECIDED) {
        mode = HAO_LIBVAL_OFF;
        snprintf(reason, sizeof reason, "turned off (hao_no_libval)");
    }
}
int hao_libval_mode(void) { return mode; }
const char *hao_libval_reason(void) { return reason; }

/* The policy, from the main executable's signature. A signed main executable
 * is verified on the bytes it is built from (by hao_guest_sig_enforce
 * already, on this slice, for the CLI; here for an embedded program), and
 * the policy is read from that CodeDirectory: an invalid signature is a
 * refusal, not a program without a policy. */
static int decide(hao_slice *sl, char *why, size_t n)
{
    static const hao_codesig unsigned_code;
    const hao_codesig *cs = &unsigned_code;
    if (sl->sig_size) {
        char w[300];
        if (hao_slice_verify(sl, NULL, w, sizeof w) != 0) {
            snprintf(why, n, "code signature invalid (%s)", w);
            return -1;
        }
        cs = &sl->sig;
    }
    integrity = true;
    main_platform = cs->platform != 0;
    main_team[0] = 0;
    if (cs->team && strlen(cs->team) < sizeof main_team) snprintf(main_team, sizeof main_team, "%s", cs->team);
    if (!hao_codesig_library_validated(cs)) {
        mode = HAO_LIBVAL_OFF;
        snprintf(reason, sizeof reason, "%s", !cs->cd ? "unsigned" : cs->dlv ? "disable-library-validation" :
                 "neither platform nor hardened");
        return 0;
    }
    snprintf(reason, sizeof reason, "%s%s%s", cs->platform ? "platform" : "",
             cs->platform && (cs->flags & (CS_RUNTIME | CS_REQUIRE_LV)) ? ", " : "",
             cs->flags & CS_RUNTIME ? "runtime" : cs->flags & CS_REQUIRE_LV ? "library flag" : "");
    uint32_t f = CS_REQUIRE_LV, now = 0;
    int r = csops(getpid(), CS_OPS_SET_STATUS, &f, sizeof f);
    int e = errno;
    if (r == 0 && csops(getpid(), CS_OPS_STATUS, &now, sizeof now) == 0 && (now & CS_REQUIRE_LV)) {
        mode = HAO_LIBVAL_KERNEL;
    } else {
        mode = HAO_LIBVAL_PLATFORM;
        size_t l = strlen(reason);
        snprintf(reason + l, sizeof reason - l, "; CS_REQUIRE_LV not set (%s): guest images platform only",
                 r ? strerror(e) : "flags unchanged");
    }
    return 0;
}

/* LC_UUID as dyld prints it (<...> in its messages) */
static void uuid_text(const uint8_t *cmds, uint32_t ncmds, char *out, size_t n)
{
    snprintf(out, n, "<no uuid>");
    const uint8_t *p = cmds;
    for (uint32_t k = 0; k < ncmds; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct uuid_command *u = (const void *)p;
        if (u->cmd != LC_UUID || u->cmdsize < sizeof *u) continue;
        const uint8_t *b = u->uuid;
        snprintf(out, n, "<%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X>", b[0], b[1], b[2], b[3],
                 b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return;
    }
}

int terminate_with_payload(int pid, uint32_t reason_namespace, uint64_t reason_code, void *payload, uint32_t payload_size,
                           const char *reason_string, uint64_t reason_flags);
#define OS_REASON_CODESIGNING 9
#define CODESIGNING_EXIT_REASON_INVALID_PAGE 2

/* The library-validation rule as AMFI applies it to a mapped file whose
 * signature is valid: a platform file, else one of the process's own Team
 * ID (the main executable's: under hao the kernel's process is hao's).
 * team: the library's, from its verified CodeDirectory (NULL: none). */
static const char *lv_rule(const char *team, const hao_imgsig *g)
{
    if (g->apple) return NULL;
    if (mode == HAO_LIBVAL_PLATFORM) return "mapped file is not a platform binary";
    if (main_platform) return "mapping process is a platform binary, but mapped file is not";
    if (!team && main_team[0]) return "mapped file has no Team ID and is not a platform binary (signed with custom identity or adhoc?)";
    if (!team || !main_team[0] || strcmp(team, main_team))
        return "mapping process and mapped file (non-platform) have different Team IDs";
    if (!g->team_ok) return "the Team ID is not the signing certificate's";
    return NULL;
}

/*
 * The checks, in dyld's and the kernel's order:
 *  1. integrity (every image, whenever the policy came from the main
 *     executable's signature): hao maps guest pages as data, which no
 *     kernel validates, so hao checks every page hash itself. A page of an
 *     executable segment that does not match kills the process as the
 *     kernel kills it natively when dyld touches that page (SIGKILL,
 *     CODESIGNING "Invalid Page"); any other invalid signature is a load
 *     failure;
 *  2. with library validation on, who may be mapped: the kernel, where it
 *     holds the signature (F_ADDFILESIGS_INFO, then F_CHECK_LV or
 *     F_GETSIGSINFO); else the same rule in user space with the signer
 *     hao establishes (guest_sig.c). An x86_64 kernel holds only 4 KiB-page
 *     CodeDirectories, so on Intel every 16 KiB-page arm64 signature
 *     (all of Apple's) takes the user-space route. So does a same-team
 *     library the kernel refuses because the process's Team ID is hao's.
 * Everything in step 2 is about sl->sig, the CodeDirectory step 1 verified:
 * the pages are hashed once, whichever route decides.
 */
static int check_image(hao_image *img, hao_slice *sl, char *k, size_t kn)
{
    bool lv = mode == HAO_LIBVAL_KERNEL || mode == HAO_LIBVAL_PLATFORM;
    if (!sl->sig_size) {
        if (!lv) return 0;
        snprintf(k, kn, "mapped file is not code signed");
        return -1;
    }
    snprintf(route, sizeof route, "integrity");
    hao_imgsig g = {0};
    char w[400] = "";
    bool bad_exec = false;
    if (hao_slice_verify(sl, &bad_exec, w, sizeof w) != 0) {
        snprintf(k, kn, "code signature invalid (%s)", w);
        if (bad_exec) {
            char msg[1700];
            snprintf(msg, sizeof msg, "%s: %s: a page of an executable segment; killed as the kernel kills it natively", img->path, k);
            if (report_line) report_line(msg);
            else fprintf(stderr, "hao: %s\n", msg);
            terminate_with_payload(getpid(), OS_REASON_CODESIGNING, CODESIGNING_EXIT_REASON_INVALID_PAGE, NULL, 0, msg, 0);
            _exit(137);
        }
        return -1;
    }
    if (!lv) return 0;
    const hao_codesig *cs = &sl->sig;
    snprintf(route, sizeof route, "kernel, %u KiB pages", 1u << (cs->page_shift - 10));
    bool user = force_user;
    if (!user) {
        fsignatures_t fs;
        memset(&fs, 0, sizeof fs);
        fs.fs_file_start = (off_t)img->slice_off;
        fs.fs_blob_start = (void *)(uintptr_t)sl->sig_off;
        fs.fs_blob_size = sl->sig_size;
        fs.fs_fsignatures_size = sizeof fs;
        if (fcntl(img->fd, F_ADDFILESIGS_INFO, &fs) != 0) {
            user = true;   /* the kernel cannot hold it (x86_64: page shift 14 -> EBADEXEC); hao decides */
        } else if (memcmp(fs.fs_cdhash, cs->cdhash, sizeof cs->cdhash) || fs.fs_hash_type != cs->hash_type) {
            snprintf(k, kn, "the kernel holds a different code signature for this file (rewritten in place?)");
            return -1;
        } else if (mode == HAO_LIBVAL_KERNEL) {
            char msg[256] = "";
            fchecklv_t cl = {.lv_file_start = (off_t)img->slice_off, .lv_error_message_size = sizeof msg,
                             .lv_error_message = msg};
            if (fcntl(img->fd, F_CHECK_LV, &cl) == 0) return 0;
            /* the process's Team ID is hao's: the program's own team is decided here */
            if (!(main_team[0] && !main_platform && cs->team && !strcmp(cs->team, main_team))) {
                snprintf(k, kn, "%s", msg[0] ? msg : strerror(errno));
                return -1;
            }
            user = true;
        } else {
            fgetsigsinfo_t gi = {.fg_file_start = (off_t)img->slice_off, .fg_info_request = GETSIGSINFO_PLATFORM_BINARY};
            if (fcntl(img->fd, F_GETSIGSINFO, &gi) == 0 && gi.fg_sig_is_platform) return 0;
            snprintf(k, kn, "mapped file is not a platform binary");
            return -1;
        }
    }
    snprintf(route, sizeof route, "user space%s, %u KiB pages", force_user ? " (HAO_LIBVAL_USER)" : "", 1u << (cs->page_shift - 10));
    if (!verifier) { snprintf(k, kn, "nothing in this process can say who signed it"); return -1; }
    if (verifier(sl, img->path, main_team[0] ? main_team : NULL, &g, w, sizeof w) != 0) {
        snprintf(k, kn, "%s", w);
        return -1;
    }
    size_t rl = strlen(route);
    snprintf(route + rl, sizeof route - rl, ", %s", g.by ? g.by : "");
    const char *bad = lv_rule(cs->team, &g);
    if (!bad) return 0;
    snprintf(k, kn, "%s", bad);
    return -1;
}

int hao_libval_image(hao_image *img, hao_slice *sl, char *why, size_t n)
{
    if (expect_main) {
        expect_main = false;
        /* An embedded program: this process's main executable is its stub, and the stub's record is in pages
         * the kernel validated. Whatever the policy, and whether or not library validation is on, the ARM
         * code that runs under the stub's identity is the code the stub was made for. No record: hao's own
         * launcher, which has checked the file it was given (hao_guest_sig_enforce), or a stub made before
         * hao-embed wrote one, whose ARM slice is verified below but is not tied to it. */
        unsigned long size = 0;
        const void *record = getsectiondata((const struct mach_header_64 *)_NSGetMachExecuteHeader(), "__TEXT",
                                            HAO_SLICE_SECTION, &size);
        if (record && hao_libval_bound(record, size, sl, why, n) != 0) return -1;
        return mode == HAO_LIBVAL_UNDECIDED ? decide(sl, why, n) : 0;
    }
    if (!integrity) return 0;
    char k[500], u[48];
    route[0] = 0;
    int r = check_image(img, sl, k, sizeof k);
    snprintf(img->libval_route, sizeof img->libval_route, "%s", route);
    if (r == 0) return 0;
    uuid_text(sl->cmds, sl->ncmds, u, sizeof u);
    snprintf(why, n, "code signature in %s '%s' not valid for use in process: %s", u, img->path, k);
    return -1;
}
