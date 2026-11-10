/*
 * hao — code-signature policy for guests started through the CLI form
 * (build/hao PROGRAM, and every exec HaoExec routes to /usr/libexec/hao).
 *
 * Such a process carries hao's code identity, not the guest's: the kernel
 * validated /usr/libexec/hao, and the guest is just a file hao opens. So
 * hao checks the guest's arm64 slice itself before running it:
 *   - a signature that does not validate (tampered page, truncated or
 *     malformed signature, CMS that does not sign this CodeDirectory) is
 *     refused, always;
 *   - an unsigned guest is refused, as arm64 hardware refuses unsigned
 *     arm64 code, unless the user ran `hao --allow-unsigned` in an ordinary
 *     (open-tier) process;
 *   - a guest that is not Apple-signed and claims entitlements beyond the
 *     ones hao holds (tools/hao.entitlements, plus get-task-allow) is
 *     refused: it would run without them, under hao's identity;
 *   - Apple-signed guests run; their entitlements are not carried (embed
 *     such programs with tools/hao-embed to keep their identity);
 *   - a set-user-ID or set-group-ID guest whose privilege this process
 *     lacks is refused rather than run unprivileged.
 * Entitlements cannot be dropped from a running process, so the ones hao
 * holds are part of what any guest gets: keep them minimal (see
 * docs/security-model.md for the kernel side).
 *
 * The verdict needs no daemon wherever it can be had without one, because
 * launchd starts arm64-only programs long before trustd exists:
 *   1. Integrity, always, here: the signature's structure (as xnu's
 *      cs_validate_csblob), the best CodeDirectory (xnu's hash ranking) and
 *      its cdhash, every page hash of the slice up to codeLimit, and the
 *      special slots of the blobs the superblob carries (requirements,
 *      entitlements, launch constraints). Nothing in the slice may lie
 *      outside the hashed range except the signature itself, so every
 *      byte the loader maps or reads is covered. This is required because
 *      no kernel checks those pages for hao: they are mapped
 *      non-executable, and xnu enforces page hashes only on executable
 *      mappings.
 *   2. Who signed that CodeDirectory, first answer wins:
 *      - ad hoc (CS_ADHOC): nobody; integrity is the whole check;
 *      - its cdhash is in a trusted cdhash list (/usr/lib/hao/trusted-
 *        cdhashes, root-owned, written at image build from the release's
 *        Apple-signed arm64-only programs; HAO_TRUST_LIST adds one in an
 *        open-tier process): Apple;
 *      - the kernel: the signature registered on this descriptor
 *        (F_ADDFILESIGS_INFO; AMFI validates the CMS with CoreTrust) and
 *        returned this cdhash; Apple when F_GETSIGSINFO says platform. An
 *        x86_64 kernel registers only 4 KiB-page CodeDirectories (xnu
 *        cs_validate_codedirectory: pageSize must be 12 or PAGE_SHIFT), and
 *        arm64 slices are signed with 16 KiB pages, so this answers on
 *        Apple-silicon hosts and for 4 KiB-page signatures;
 *      - otherwise Security.framework (SecStaticCodeCheckValidity, basic:
 *        the CMS over the CodeDirectory; that CodeDirectory must be this
 *        cdhash): the only step that needs trustd, reached only by
 *        CMS-signed guests that are neither listed nor registrable.
 *
 * The check runs in the process that runs the guest (the CLI's stage 2), on
 * the descriptor the loader then maps the image from: nothing a caller puts
 * in the environment or in argv stands in for it, and no path is opened
 * twice. Everything but the Security.framework step needs only libc and
 * CommonCrypto. That step runs in a fork of this process, which exits with
 * the answer (sec_ask): the frameworks it loads (with dlopen; libhao does
 * not link them) are not loaded into the guest's process for this check,
 * and the answer is for the cdhash this process computed on its own
 * descriptor. Library validation's user-space route (hao_guest_sig_image,
 * below) is different: it asks Security in the running process, where a
 * fork could not safely do it.
 */
#include "hao/runtime.h"
#include "hao/aot.h"
#include "hao/macho.h"
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HAO_TRUST_SYS_DIR "/usr/lib/hao"
#define HAO_TRUST_NAME "trusted-cdhashes"

/* ---------------------------------------------------- signature format */
/* osfmk/kern/cs_blobs.h (xnu-12377); every field is big-endian. */
enum {
    CSMAGIC_REQUIREMENTS_ = 0xfade0c01, CSMAGIC_CODEDIRECTORY_ = 0xfade0c02, CSMAGIC_EMBEDDED_SIGNATURE_ = 0xfade0cc0,
    CSMAGIC_EMBEDDED_ENTITLEMENTS_ = 0xfade7171, CSMAGIC_EMBEDDED_DER_ENTITLEMENTS_ = 0xfade7172,
    CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT_ = 0xfade8181,
    CSSLOT_CODEDIRECTORY_ = 0, CSSLOT_REQUIREMENTS_ = 2, CSSLOT_ENTITLEMENTS_ = 5, CSSLOT_DER_ENTITLEMENTS_ = 7,
    CSSLOT_LAUNCH_CONSTRAINT_SELF_ = 8, CSSLOT_LIBRARY_CONSTRAINT_ = 11,
    CSSLOT_ALTERNATE_CODEDIRECTORIES_ = 0x1000, CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT_ = 0x1005,
    CS_SUPPORTSSCATTER_ = 0x20100, CS_SUPPORTSTEAMID_ = 0x20200, CS_SUPPORTSCODELIMIT64_ = 0x20300,
    CS_HASHTYPE_SHA1_ = 1, CS_HASHTYPE_SHA256_ = 2, CS_HASHTYPE_SHA256_TRUNCATED_ = 3, CS_HASHTYPE_SHA384_ = 4,
};
#define CS_CDHASH_LEN_ 20
#define CS_HASH_MAX_ 48
#define CS_ADHOC_ 0x2u
#define CS_MAX_BLOB_ (40u << 20)       /* xnu's CS_MAX_BLOB_SIZE for F_ADDFILESIGS */

typedef struct {                       /* the CodeDirectory fields read here */
    uint32_t magic, length, version, flags, hashOffset, identOffset, nSpecialSlots, nCodeSlots, codeLimit;
    uint8_t hashSize, hashType, platform, pageSize;
    uint32_t spare2, scatterOffset, teamOffset, spare3;
    uint64_t codeLimit64;
} cd_hdr;

static uint32_t be32p(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static uint64_t be64p(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }

/* xnu cs_find_md: effective (slot) size of each hash type; 0 = unsupported */
static unsigned hash_size(uint8_t type)
{
    switch (type) {
    case CS_HASHTYPE_SHA1_: case CS_HASHTYPE_SHA256_TRUNCATED_: return 20;
    case CS_HASHTYPE_SHA256_: return 32;
    case CS_HASHTYPE_SHA384_: return 48;
    default: return 0;
    }
}

/* xnu hashPriorities: SHA-1 < SHA-256/20 < SHA-256 < SHA-384 */
static unsigned hash_rank(uint8_t type)
{
    static const uint8_t pri[] = {CS_HASHTYPE_SHA1_, CS_HASHTYPE_SHA256_TRUNCATED_, CS_HASHTYPE_SHA256_, CS_HASHTYPE_SHA384_};
    for (unsigned k = 0; k < sizeof pri; k++) if (pri[k] == type) return k + 1;
    return 0;
}

typedef union { CC_SHA1_CTX s1; CC_SHA256_CTX s256; CC_SHA512_CTX s384; } md_ctx;
static void md_init(uint8_t t, md_ctx *c)
{
    if (t == CS_HASHTYPE_SHA1_) CC_SHA1_Init(&c->s1);
    else if (t == CS_HASHTYPE_SHA384_) CC_SHA384_Init(&c->s384);
    else CC_SHA256_Init(&c->s256);
}
static void md_update(uint8_t t, md_ctx *c, const void *p, size_t n)
{
    while (n) {                                       /* CC_LONG is 32 bits */
        CC_LONG k = n > 0x40000000u ? 0x40000000u : (CC_LONG)n;
        if (t == CS_HASHTYPE_SHA1_) CC_SHA1_Update(&c->s1, p, k);
        else if (t == CS_HASHTYPE_SHA384_) CC_SHA384_Update(&c->s384, p, k);
        else CC_SHA256_Update(&c->s256, p, k);
        p = (const uint8_t *)p + k;
        n -= k;
    }
}
static void md_final(uint8_t t, md_ctx *c, uint8_t out[CS_HASH_MAX_])
{
    if (t == CS_HASHTYPE_SHA1_) CC_SHA1_Final(out, &c->s1);
    else if (t == CS_HASHTYPE_SHA384_) CC_SHA384_Final(out, &c->s384);
    else CC_SHA256_Final(out, &c->s256);
}
static void md(uint8_t t, const void *p, size_t n, uint8_t out[CS_HASH_MAX_])
{
    md_ctx c;
    md_init(t, &c);
    md_update(t, &c, p, n);
    md_final(t, &c, out);
}

typedef struct {
    const uint8_t *cd;                 /* the best CodeDirectory, inside blob */
    cd_hdr h;
    uint64_t code_limit;
    uint8_t cdhash[CS_CDHASH_LEN_];
    const char *ident, *team;          /* NUL-terminated inside the CodeDirectory (team may be NULL) */
    const uint8_t *ents;               /* XML entitlements blob (header included), slot-verified */
    uint32_t ents_len;
    bool der_ents;                     /* a DER entitlements blob is present */
} sig_info;

static bool cd_parse(const uint8_t *p, size_t len, cd_hdr *h)
{
    if (len < 44) return false;
    h->magic = be32p(p); h->length = be32p(p + 4); h->version = be32p(p + 8); h->flags = be32p(p + 12);
    h->hashOffset = be32p(p + 16); h->identOffset = be32p(p + 20); h->nSpecialSlots = be32p(p + 24);
    h->nCodeSlots = be32p(p + 28); h->codeLimit = be32p(p + 32);
    h->hashSize = p[36]; h->hashType = p[37]; h->platform = p[38]; h->pageSize = p[39];
    h->scatterOffset = h->version >= CS_SUPPORTSSCATTER_ && len >= 48 ? be32p(p + 44) : 0;
    h->teamOffset = h->version >= CS_SUPPORTSTEAMID_ && len >= 52 ? be32p(p + 48) : 0;
    h->codeLimit64 = h->version >= CS_SUPPORTSCODELIMIT64_ && len >= 64 ? be64p(p + 56) : 0;
    return true;
}

/* xnu cs_validate_codedirectory, plus what hao needs to check every page
 * itself: no scatter vector, a 4 KiB or 16 KiB page size. */
static bool cd_valid(const uint8_t *p, size_t len, cd_hdr *h, char *why, size_t n)
{
#define BAD(...) do { snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (!cd_parse(p, len, h) || h->magic != CSMAGIC_CODEDIRECTORY_) BAD("malformed CodeDirectory");
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
static const uint8_t *sb_find(const uint8_t *sb, size_t len, uint32_t type, uint32_t magic, uint32_t *blen)
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

/* xnu cs_validate_csblob over the embedded signature: every index entry in
 * range, CodeDirectories valid and of distinct hash types (the best one is
 * used), at most one entitlements/DER/constraint blob each, with the right
 * magic. Then xnu's csblob_find_special_slot_blob for every blob kind the
 * CodeDirectory seals: a present blob must have its slot and match it, an
 * absent one must have no slot or an all-zero one. */
static bool sig_parse(const uint8_t *sb, size_t len, sig_info *s, char *why, size_t n)
{
#define BAD(...) do { snprintf(why, n, __VA_ARGS__); return false; } while (0)
    memset(s, 0, sizeof *s);
    if (len < 12 || be32p(sb) != CSMAGIC_EMBEDDED_SIGNATURE_ || be32p(sb + 4) > len || be32p(sb + 4) < 12)
        BAD("not an embedded signature");
    len = be32p(sb + 4);
    uint32_t count = be32p(sb + 8);
    if ((len - 12) / 8 < count) BAD("signature index past its end");
    unsigned best_rank = 0;
    bool primary = false;
    uint32_t seen = 0;                                   /* bit per sealed slot type 2..11 */
    for (uint32_t k = 0; k < count; k++) {
        uint32_t type = be32p(sb + 12 + 8 * k), off = be32p(sb + 16 + 8 * k);
        if (off > len || len - off < 8 || be32p(sb + off + 4) > len - off || be32p(sb + off + 4) < 8)
            BAD("signature blob %u outside the signature", k);
        const uint8_t *b = sb + off;
        uint32_t bl = be32p(b + 4), magic = be32p(b);
        if (type == CSSLOT_CODEDIRECTORY_ || (type >= CSSLOT_ALTERNATE_CODEDIRECTORIES_ && type < CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT_)) {
            cd_hdr h;
            if (!cd_valid(b, bl, &h, why, n)) return false;
            primary |= type == CSSLOT_CODEDIRECTORY_;
            unsigned r = hash_rank(h.hashType);
            if (r == best_rank) BAD("two CodeDirectories with hash type %u", h.hashType);
            if (r > best_rank) { best_rank = r; s->cd = b; s->h = h; }
        } else if (type == CSSLOT_REQUIREMENTS_ || type == CSSLOT_ENTITLEMENTS_ || type == CSSLOT_DER_ENTITLEMENTS_ ||
                   (type >= CSSLOT_LAUNCH_CONSTRAINT_SELF_ && type <= CSSLOT_LIBRARY_CONSTRAINT_)) {
            uint32_t want = type == CSSLOT_REQUIREMENTS_ ? CSMAGIC_REQUIREMENTS_ : type == CSSLOT_ENTITLEMENTS_
                ? CSMAGIC_EMBEDDED_ENTITLEMENTS_ : type == CSSLOT_DER_ENTITLEMENTS_ ? CSMAGIC_EMBEDDED_DER_ENTITLEMENTS_
                : CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT_;
            if (magic != want) BAD("signature blob for slot %u has magic %#x", type, magic);
            if (seen & (1u << type)) BAD("two signature blobs for slot %u", type);
            seen |= 1u << type;
        }
    }
    if (!primary || !s->cd) BAD("no CodeDirectory");
    const cd_hdr *h = &s->h;
    unsigned hs = h->hashSize;
    static const struct { uint32_t slot, magic; } sealed[] = {
        {CSSLOT_REQUIREMENTS_, CSMAGIC_REQUIREMENTS_}, {CSSLOT_ENTITLEMENTS_, CSMAGIC_EMBEDDED_ENTITLEMENTS_},
        {CSSLOT_DER_ENTITLEMENTS_, CSMAGIC_EMBEDDED_DER_ENTITLEMENTS_}, {8, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT_},
        {9, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT_}, {10, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT_},
        {11, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT_}};
    static const uint8_t zero[CS_HASH_MAX_];
    for (size_t k = 0; k < sizeof sealed / sizeof sealed[0]; k++) {
        uint32_t bl = 0;
        const uint8_t *b = sb_find(sb, len, sealed[k].slot, sealed[k].magic, &bl);
        const uint8_t *slot = sealed[k].slot <= h->nSpecialSlots ? s->cd + h->hashOffset - sealed[k].slot * hs : NULL;
        if (!slot) {
            if (b) BAD("signature blob for slot %u is not sealed by the CodeDirectory", sealed[k].slot);
            continue;
        }
        if (!b) {
            if (memcmp(slot, zero, hs)) BAD("signature blob for slot %u is missing", sealed[k].slot);
            continue;
        }
        uint8_t got[CS_HASH_MAX_];
        md(h->hashType, b, bl, got);
        if (memcmp(got, slot, hs)) BAD("signature blob for slot %u does not match its hash", sealed[k].slot);
        if (sealed[k].slot == CSSLOT_ENTITLEMENTS_) { s->ents = b; s->ents_len = bl; }
        if (sealed[k].slot == CSSLOT_DER_ENTITLEMENTS_) s->der_ents = true;
    }
    uint8_t full[CS_HASH_MAX_];
    md(h->hashType, s->cd, h->length, full);
    memcpy(s->cdhash, full, CS_CDHASH_LEN_);
    s->code_limit = h->codeLimit64 ? h->codeLimit64 : h->codeLimit;
    s->ident = (const char *)s->cd + h->identOffset;
    s->team = h->teamOffset ? (const char *)s->cd + h->teamOffset : NULL;
    return true;
#undef BAD
}

/* Every page hash of [0, code_limit) of the slice. exec (nexec ranges of
 * slice offsets, may be NULL): *bad_exec is set when a page that does not
 * match its hash overlaps one of them (an executable segment). */
static bool pages_valid(int fd, uint64_t slice_off, const sig_info *s, const hao_range *exec, int nexec, bool *bad_exec,
                        char *why, size_t n)
{
    const cd_hdr *h = &s->h;
    uint64_t page = 1ull << h->pageSize;
    uint64_t pages = (s->code_limit + page - 1) / page;
    if (pages != h->nCodeSlots) {
        snprintf(why, n, "the CodeDirectory hashes %u pages of a %llu-byte code range", h->nCodeSlots,
                 (unsigned long long)s->code_limit);
        return false;
    }
    enum { CHUNK = 1 << 20 };                        /* a multiple of both page sizes */
    /* mapped, not malloc'd: this runs in the guest's process, and a freed
     * 1 MiB heap block would stay in its footprint for the process's life */
    uint8_t *buf = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (buf == MAP_FAILED) buf = NULL;
    if (!buf) { snprintf(why, n, "out of memory"); return false; }
    const uint8_t *slots = s->cd + h->hashOffset;
    bool ok = true;
    for (uint64_t at = 0; at < s->code_limit && ok;) {
        size_t want = (size_t)(s->code_limit - at < CHUNK ? s->code_limit - at : CHUNK), got = 0;
        while (got < want) {
            ssize_t r = pread(fd, buf + got, want - got, (off_t)(slice_off + at + got));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { snprintf(why, n, "cannot read the code at %#llx", (unsigned long long)(at + got)); ok = false; break; }
            got += (size_t)r;
        }
        for (size_t p = 0; ok && p < want; p += page) {
            uint64_t i = (at + p) / page;
            size_t len = want - p < page ? want - p : (size_t)page;
            uint8_t d[CS_HASH_MAX_];
            md(h->hashType, buf + p, len, d);
            if (memcmp(d, slots + i * h->hashSize, h->hashSize)) {
                snprintf(why, n, "page %llu (offset %#llx) does not match its hash", (unsigned long long)i,
                         (unsigned long long)(at + p));
                ok = false;
                for (int k = 0; bad_exec && k < nexec; k++)
                    if (at + p < exec[k].end && at + p + len > exec[k].off) *bad_exec = true;
            }
        }
        at += want;
    }
    munmap(buf, CHUNK);
    return ok;
}

/* -------------------------------------------- signer: trusted cdhash list */

/* "# comment" lines and one lowercase or uppercase 40-hex cdhash per line. */
static bool list_has(const char *data, size_t len, const uint8_t cdhash[CS_CDHASH_LEN_])
{
    char want[2 * CS_CDHASH_LEN_ + 1];
    for (int k = 0; k < CS_CDHASH_LEN_; k++) snprintf(want + 2 * k, 3, "%02x", cdhash[k]);
    for (const char *p = data, *e = data + len; p < e;) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        size_t l = (size_t)((nl ? nl : e) - p);
        if (l >= 2 * CS_CDHASH_LEN_ && p[0] != '#' && (l == 2 * CS_CDHASH_LEN_ || p[2 * CS_CDHASH_LEN_] == ' ' || p[2 * CS_CDHASH_LEN_] == '\t')) {
            bool eq = true;
            for (int k = 0; k < 2 * CS_CDHASH_LEN_ && eq; k++) eq = (p[k] | 0x20) == want[k];
            if (eq) return true;
        }
        p += l + 1;
    }
    return false;
}

/* 1: listed, 0: not (or no list), -1: a list exists but is not trusted (why set) */
static int trusted_list_has(const hao_aot_dir *d, const char *name, const uint8_t cdhash[CS_CDHASH_LEN_], char *why, size_t n)
{
    struct stat st;
    int fd = hao_aot_open_trusted(d, name, &st, why, n);
    if (fd < 0) return why[0] ? -1 : 0;
    int r = 0;
    if (st.st_size > 16 << 20) { snprintf(why, n, "larger than 16 MiB"); r = -1; }
    char *buf = r ? NULL : malloc(st.st_size ? (size_t)st.st_size : 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t k = pread(fd, buf + got, (size_t)st.st_size - got, (off_t)got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    if (buf) r = list_has(buf, got, cdhash);
    free(buf);
    close(fd);
    return r;
}

/* 1: the cdhash is in the system's trusted cdhash list or HAO_TRUST_LIST's
 * (an open-tier process only), else 0; a list that exists but cannot be
 * used is reported. */
static int trusted_cdhash(const uint8_t cdhash[CS_CDHASH_LEN_], char *w2, size_t w2n)
{
    FILE *f = HAO_DIAG;
    w2[0] = 0;
    hao_aot_dir sys = {HAO_TRUST_SYS_DIR, HAO_AOT_SYS_RO}, ex = {"", HAO_AOT_EXPLICIT};
    int in = trusted_list_has(&sys, HAO_TRUST_NAME, cdhash, w2, w2n);
    if (in < 0 && hao_opt.verbose) {
        hao_diag_prefix(f);
        fprintf(f, "%s/%s: not used: %s\n", HAO_TRUST_SYS_DIR, HAO_TRUST_NAME, w2);
    }
    if (in <= 0 && hao_opt.trust_list[0]) {
        char dir[1024];
        const char *sl = strrchr(hao_opt.trust_list, '/');
        if (hao_opt.trust_list[0] == '/' && sl && sl[1]) {
            snprintf(dir, sizeof dir, "%.*s", sl == hao_opt.trust_list ? 1 : (int)(sl - hao_opt.trust_list), hao_opt.trust_list);
            snprintf(ex.path, sizeof ex.path, "%s", dir);
            w2[0] = 0;
            in = trusted_list_has(&ex, sl + 1, cdhash, w2, w2n);
        } else {
            snprintf(w2, w2n, "not an absolute file path");
            in = -1;
        }
        if (in < 0) {
            hao_diag_prefix(f);
            fprintf(f, "HAO_TRUST_LIST=%s: not used: %s\n", hao_opt.trust_list, w2[0] ? w2 : "no such file");
        }
    }
    w2[0] = 0;
    return in > 0;
}

/* ------------------------------------------------- signer: the kernel */

/* 0: the kernel accepted the signature and holds this cdhash (*platform
 * from F_GETSIGSINFO); 1: refused (why: errno); -1: the kernel holds a
 * different signature for these bytes. */
static int kernel_verdict(int fd, uint64_t slice_off, uint32_t sig_off, uint32_t sig_size, const sig_info *s, bool *platform,
                          char *why, size_t n)
{
    fsignatures_t fs;
    memset(&fs, 0, sizeof fs);
    fs.fs_file_start = (off_t)slice_off;
    fs.fs_blob_start = (void *)(uintptr_t)sig_off;
    fs.fs_blob_size = sig_size;
    fs.fs_fsignatures_size = sizeof fs;
    if (fcntl(fd, F_ADDFILESIGS_INFO, &fs) != 0) { snprintf(why, n, "%s", strerror(errno)); return 1; }
    if (memcmp(fs.fs_cdhash, s->cdhash, CS_CDHASH_LEN_) || fs.fs_hash_type != s->h.hashType) {
        snprintf(why, n, "the kernel holds a different code signature for this file (rewritten in place?)");
        return -1;
    }
    fgetsigsinfo_t gi = {(off_t)slice_off, GETSIGSINFO_PLATFORM_BINARY, 0};
    if (fcntl(fd, F_GETSIGSINFO, &gi) != 0) { snprintf(why, n, "F_GETSIGSINFO: %s", strerror(errno)); return 1; }
    *platform = gi.fg_sig_is_platform != 0;
    return 0;
}

/* ---------------------------------------- signer: Security.framework */

typedef const struct __SecCode *SecStaticCodeRef_;
typedef const struct __SecRequirement *SecRequirementRef_;
typedef uint32_t SecCSFlags_;

static struct {
    OSStatus (*create)(CFURLRef, SecCSFlags_, CFDictionaryRef, SecStaticCodeRef_ *);
    OSStatus (*check)(SecStaticCodeRef_, SecCSFlags_, SecRequirementRef_);
    OSStatus (*req)(CFStringRef, SecCSFlags_, SecRequirementRef_ *);
    OSStatus (*info)(SecStaticCodeRef_, SecCSFlags_, CFDictionaryRef *);
    CFStringRef *k_arch, *k_subarch, *k_unique, *k_cdhashes;
} sec;

static struct {
    CFURLRef (*url)(CFAllocatorRef, const UInt8 *, CFIndex, Boolean);
    CFNumberRef (*num)(CFAllocatorRef, CFNumberType, const void *);
    CFDictionaryRef (*dict)(CFAllocatorRef, const void **, const void **, CFIndex, const CFDictionaryKeyCallBacks *,
                            const CFDictionaryValueCallBacks *);
    const void *(*dict_get)(CFDictionaryRef, const void *);
    CFStringRef (*str)(CFAllocatorRef, const char *, CFStringEncoding);
    CFIndex (*data_len)(CFDataRef);
    const UInt8 *(*data_ptr)(CFDataRef);
    CFIndex (*arr_count)(CFArrayRef);
    const void *(*arr_get)(CFArrayRef, CFIndex);
    void (*release)(CFTypeRef);
    const CFDictionaryKeyCallBacks *keycb;
    const CFDictionaryValueCallBacks *valcb;
} cf;

static bool sec_load(void)
{
    static int state;                       /* 0 not tried, 1 ok, -1 failed */
    if (state) return state > 0;
    state = -1;
    void *c = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
    void *h = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY | RTLD_LOCAL);
    if (!h || !c) return false;
#define CF(f, n) if (!(*(void **)&cf.f = dlsym(c, n))) return false
    CF(url, "CFURLCreateFromFileSystemRepresentation");
    CF(num, "CFNumberCreate");
    CF(dict, "CFDictionaryCreate");
    CF(dict_get, "CFDictionaryGetValue");
    CF(str, "CFStringCreateWithCString");
    CF(data_len, "CFDataGetLength");
    CF(data_ptr, "CFDataGetBytePtr");
    CF(arr_count, "CFArrayGetCount");
    CF(arr_get, "CFArrayGetValueAtIndex");
    CF(release, "CFRelease");
    CF(keycb, "kCFTypeDictionaryKeyCallBacks");
    CF(valcb, "kCFTypeDictionaryValueCallBacks");
#undef CF
#define SYM(f, n) if (!(*(void **)&sec.f = dlsym(h, n))) return false
    SYM(create, "SecStaticCodeCreateWithPathAndAttributes");
    SYM(check, "SecStaticCodeCheckValidity");
    SYM(req, "SecRequirementCreateWithString");
    SYM(info, "SecCodeCopySigningInformation");
    SYM(k_arch, "kSecCodeAttributeArchitecture");
    SYM(k_subarch, "kSecCodeAttributeSubarchitecture");
    SYM(k_unique, "kSecCodeInfoUnique");
#undef SYM
    *(void **)&sec.k_cdhashes = dlsym(h, "kSecCodeInfoCdHashes");   /* every CodeDirectory's cdhash, if known */
    state = 1;
    return true;
}

/* SecStaticCode.h, SecCode.h. hao has checked the pages and the structure:
 * Security only authenticates the CodeDirectory (kSecCSBasicValidateOnly:
 * bundle resources are not code, and exec does not check them either). */
#define kSecCSBasic_ ((1u << 1) | (1u << 2))
#define kSecCSSigningInformation_ (1u << 1)

static bool cfdata_is(CFDataRef d, const uint8_t cdhash[CS_CDHASH_LEN_])
{
    return d && cf.data_len(d) >= CS_CDHASH_LEN_ && !memcmp(cf.data_ptr(d), cdhash, CS_CDHASH_LEN_);
}

/* 0: Security authenticated the slice's CodeDirectory (*apple: it satisfies
 * "anchor apple") and that CodeDirectory is s->cdhash, the one whose pages
 * hao checked through its descriptor. Security opens the path, not the
 * descriptor, so that it finds a bundle's Info.plist (the CodeDirectory
 * seals it); the cdhash binds what it validated to what hao checked. */
static bool sec_satisfies(SecStaticCodeRef_ code, const char *req_text)
{
    SecRequirementRef_ req = NULL;
    CFStringRef str = cf.str(NULL, req_text, kCFStringEncodingUTF8);
    bool ok = str && sec.req(str, 0, &req) == 0 && sec.check(code, kSecCSBasic_, req) == 0;
    if (req) cf.release(req);
    if (str) cf.release(str);
    return ok;
}

/* team (may be NULL): *team_ok when the CMS's leaf certificate is issued for
 * that Team ID by Apple's developer anchor (a library's Team ID is the
 * CodeDirectory's, which only the certificate binds). */
static int sec_verdict(const char *path, uint32_t subtype, const sig_info *s, bool *apple, const char *team, bool *team_ok,
                       char *why, size_t n)
{
    if (!sec_load()) { snprintf(why, n, "cannot load Security.framework to check the CMS signature"); return -1; }
    CFURLRef url = cf.url(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    int cpu = CPU_TYPE_ARM64, sub = (int)(subtype & ~CPU_SUBTYPE_MASK);
    CFNumberRef c = cf.num(NULL, kCFNumberIntType, &cpu), sn = cf.num(NULL, kCFNumberIntType, &sub);
    const void *keys[] = {*sec.k_arch, *sec.k_subarch}, *vals[] = {c, sn};
    CFDictionaryRef attrs = cf.dict(NULL, keys, vals, 2, cf.keycb, cf.valcb);
    SecStaticCodeRef_ code = NULL;
    OSStatus st = sec.create(url, 0, attrs, &code);
    cf.release(attrs); cf.release(c); cf.release(sn); cf.release(url);
    if (st || !code) { snprintf(why, n, "Security cannot open the arm64 code (OSStatus %d)", (int)st); return -1; }
    int rc = 0;
    OSStatus valid = sec.check(code, kSecCSBasic_, NULL);
    if (valid) { snprintf(why, n, "code signature invalid (OSStatus %d)", (int)valid); rc = -1; goto out; }
    CFDictionaryRef info = NULL;
    bool bound = false;
    if (sec.info(code, kSecCSSigningInformation_, &info) == 0 && info) {
        CFArrayRef all = sec.k_cdhashes ? cf.dict_get(info, *sec.k_cdhashes) : NULL;
        if (all)
            for (CFIndex k = 0; k < cf.arr_count(all) && !bound; k++) bound = cfdata_is(cf.arr_get(all, k), s->cdhash);
        if (!bound) bound = cfdata_is(cf.dict_get(info, *sec.k_unique), s->cdhash);
        cf.release(info);
    }
    if (!bound) { snprintf(why, n, "Security validated a different CodeDirectory (file changed during the check?)"); rc = -1; goto out; }
    *apple = sec_satisfies(code, "anchor apple");
    if (team && team_ok) {
        char r[160];
        snprintf(r, sizeof r, "anchor apple generic and certificate leaf[subject.OU] = \"%s\"", team);
        *team_ok = sec_satisfies(code, r);
    }
out:
    cf.release(code);
    return rc;
}

/*
 * sec_verdict for the main executable, asked in a child. Security.framework
 * and what it loads must not stay in the process that runs the guest, so the
 * question goes to a fork of this process, which exits with the answer. A
 * fork, not an exec of hao's path: the child is this code by construction,
 * where a path could name another program by the time it is run. The child
 * answers for s->cdhash, which this process computed on its own descriptor,
 * and only one of the three statuses below counts: a child that is killed,
 * or that leaves any other way, is a refusal. The pipe carries the reason,
 * for the diagnostic only.
 */
enum { SEC_CHILD_VALID = 90, SEC_CHILD_APPLE = 91, SEC_CHILD_REFUSED = 92 };

static int sec_ask(const char *path, uint32_t subtype, const sig_info *s, bool *apple, char *why, size_t n)
{
    int p[2];
    if (pipe(p) != 0) { snprintf(why, n, "cannot ask Security.framework (pipe: %s)", strerror(errno)); return -1; }
    /* waitpid gets the status whatever SIGCHLD disposition this process inherited: an ignored SIGCHLD
     * does not reap children here, and SA_NOCLDWAIT does not survive the exec that started this process */
    int rc = -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        bool a = false;
        char w[400] = "";
        int r = sec_verdict(path, subtype, s, &a, NULL, NULL, w, sizeof w);
        if (r != 0) (void)!write(p[1], w, strlen(w));
        _exit(r != 0 ? SEC_CHILD_REFUSED : a ? SEC_CHILD_APPLE : SEC_CHILD_VALID);
    }
    close(p[1]);
    if (pid < 0) {
        snprintf(why, n, "cannot ask Security.framework (fork: %s)", strerror(errno));
    } else {
        char w[400];
        size_t got = 0;
        for (;;) {
            ssize_t k = read(p[0], w + got, sizeof w - 1 - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        w[got] = 0;
        int st = 0;
        pid_t r;
        while ((r = waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
        if (r == pid && WIFEXITED(st) && (WEXITSTATUS(st) == SEC_CHILD_VALID || WEXITSTATUS(st) == SEC_CHILD_APPLE)) {
            *apple = WEXITSTATUS(st) == SEC_CHILD_APPLE;
            rc = 0;
        } else if (r == pid && WIFEXITED(st) && WEXITSTATUS(st) == SEC_CHILD_REFUSED && got) {
            snprintf(why, n, "%s", w);
        } else if (r == pid && WIFSIGNALED(st)) {
            snprintf(why, n, "the Security.framework check was killed by signal %d", WTERMSIG(st));
        } else {
            snprintf(why, n, "the Security.framework check gave no answer (wait status %#x)", r == pid ? (unsigned)st : ~0u);
        }
    }
    close(p[0]);
    return rc;
}

/* ------------------------------------------------------------- policy */

/* The slice's LC_CODE_SIGNATURE (*sig_size 0: none), after checking that
 * nothing the loader maps lies past code_limit except the signature: called
 * twice, first without a code limit (to find the signature). */
static bool slice_layout(int fd, uint64_t slice_off, uint64_t slice_size, uint32_t *sig_off, uint32_t *sig_size,
                         char *why, size_t n)
{
    struct mach_header_64 mh;
    *sig_off = *sig_size = 0;
    if (slice_size < sizeof mh || pread(fd, &mh, sizeof mh, (off_t)slice_off) != sizeof mh || mh.magic != MH_MAGIC_64 ||
        mh.cputype != CPU_TYPE_ARM64) {
        snprintf(why, n, "bad arm64 mach header");
        return false;
    }
    if (mh.sizeofcmds > slice_size - sizeof mh) { snprintf(why, n, "load commands overrun the slice"); return false; }
    uint8_t *cmds = malloc(mh.sizeofcmds ? mh.sizeofcmds : 1);
    if (!cmds || pread(fd, cmds, mh.sizeofcmds, (off_t)(slice_off + sizeof mh)) != (ssize_t)mh.sizeofcmds ||
        hao_macho_check_cmds(cmds, mh.sizeofcmds, mh.ncmds, NULL, why, n) != 0) {
        if (cmds && !why[0]) snprintf(why, n, "short load commands");
        free(cmds);
        return false;
    }
    const uint8_t *p = cmds;
    for (uint32_t k = 0; k < mh.ncmds; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd != LC_CODE_SIGNATURE) continue;
        const struct linkedit_data_command *cs = (const void *)p;
        *sig_off = cs->dataoff;
        *sig_size = cs->datasize;
    }
    free(cmds);
    if (*sig_size && ((uint64_t)*sig_off + *sig_size > slice_size || *sig_size > CS_MAX_BLOB_)) {
        snprintf(why, n, "code signature [%#x, +%#x) outside the slice", *sig_off, *sig_size);
        return false;
    }
    return true;
}

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/* The policy for the file open on fd, which the caller then hands to the
 * loader (hao_load_main_image): every byte read here and every byte mapped
 * there comes through this one descriptor. path names the file in
 * diagnostics and is what Security.framework opens (sec_verdict).
 * 0: run it; -1: refused (diagnostic printed). */
int hao_guest_sig_enforce(int fd, const char *path)
{
    FILE *f = HAO_DIAG;
    double t0 = now_us();
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        hao_diag_prefix(f);
        fprintf(f, "%s: not a regular file\n", path);
        return -1;
    }
    /* setuid/setgid cannot be honoured here: the kernel exec'd hao (and
     * through HaoExec's #! route it clears the bits). Running a privileged
     * helper without its privileges would fail in ways it does not expect. */
    if (((st.st_mode & S_ISUID) && geteuid() != st.st_uid) || ((st.st_mode & S_ISGID) && getegid() != st.st_gid)) {
        hao_diag_prefix(f);
        fprintf(f, "%s: set-%s-ID program, which hao cannot run with its privileges here; refusing to run "
                   "(embed it with tools/hao-embed)\n", path, st.st_mode & S_ISUID ? "user" : "group");
        return -1;
    }
    int rc = 0;
    char why[600] = "", w2[400] = "";
    const char *kind = "valid", *by = "";         /* the verdict and who gave it */
    uint8_t *blob = NULL;
    sig_info s;
    memset(&s, 0, sizeof s);
    bool apple = false, foreign = false;
    uint64_t slice_off = 0, slice_size = 0;
    uint32_t subtype = 0, sig_off = 0, sig_size = 0;
    if (!hao_macho_choose_slice(fd, (uint64_t)st.st_size, &slice_off, &slice_size, &subtype, w2, sizeof w2)) {
        snprintf(why, sizeof why, "no arm64 code to check (%s)", w2);
        rc = -1;
        goto out;
    }
    if (!slice_layout(fd, slice_off, slice_size, &sig_off, &sig_size, w2, sizeof w2)) {
        snprintf(why, sizeof why, "code signature invalid (%s)", w2);
        rc = -1;
        goto out;
    }
    if (!sig_size) {
        if (!hao_opt.allow_unsigned) {
            snprintf(why, sizeof why, "not code signed (arm64 code must be signed; `hao --allow-unsigned` runs it anyway)");
            rc = -1;
        } else if (hao_opt.verbose) {
            hao_diag_prefix(f);
            fprintf(f, "%s: not code signed; running it (--allow-unsigned)\n", path);
        }
        goto out;
    }
    blob = malloc(sig_size);
    if (!blob || pread(fd, blob, sig_size, (off_t)(slice_off + sig_off)) != (ssize_t)sig_size) {
        snprintf(why, sizeof why, "cannot read the code signature");
        rc = -1;
        goto out;
    }
    if (!sig_parse(blob, sig_size, &s, w2, sizeof w2)) {
        snprintf(why, sizeof why, "code signature invalid (%s)", w2);
        rc = -1;
        goto out;
    }
    /* Every byte of the slice is page-hashed or is the signature: the code
     * limit reaches the signature, and nothing follows the signature. */
    if (s.code_limit < sig_off || s.code_limit > slice_size ||
        slice_size > (s.code_limit > (uint64_t)sig_off + sig_size ? s.code_limit : (uint64_t)sig_off + sig_size)) {
        snprintf(why, sizeof why, "code signature invalid (it covers [0, %#llx) of a %#llx-byte slice whose signature is at %#x)",
                 (unsigned long long)s.code_limit, (unsigned long long)slice_size, sig_off);
        rc = -1;
        goto out;
    }
    if (!pages_valid(fd, slice_off, &s, NULL, 0, NULL, w2, sizeof w2)) {
        snprintf(why, sizeof why, "code signature invalid (%s)", w2);
        rc = -1;
        goto out;
    }
    /* who vouches for this CodeDirectory */
    if (s.h.flags & CS_ADHOC_) {
        kind = "ad hoc";
        by = "CodeDirectory and page hashes";
    } else {
        int in = trusted_cdhash(s.cdhash, w2, sizeof w2);
        if (in > 0) {
            kind = "Apple";
            by = "trusted cdhash list";
            apple = true;
        } else {
            /* HAO_LIBVAL_USER (tests): the signer in user space, as on a kernel that cannot hold the signature */
            bool platform = false;
            int k = 1;
            if (hao_opt.libval_user) snprintf(w2, sizeof w2, "not asked: HAO_LIBVAL_USER");
            else k = kernel_verdict(fd, slice_off, sig_off, sig_size, &s, &platform, w2, sizeof w2);
            if (k < 0) {
                snprintf(why, sizeof why, "code signature invalid (%s)", w2);
                rc = -1;
                goto out;
            }
            if (k == 0) {
                kind = platform ? "Apple" : "valid";
                by = platform ? "kernel: platform binary" : "kernel";
                apple = platform;
            } else {
                char kw[200];
                snprintf(kw, sizeof kw, "%s", w2);
                if (hao_opt.verbose) {
                    hao_diag_prefix(f);
                    fprintf(f, "%s: the kernel did not register the signature (%s); asking Security.framework\n", path, kw);
                }
                if (sec_ask(path, subtype, &s, &apple, w2, sizeof w2) != 0) {
                    snprintf(why, sizeof why, "%s; the cdhash is in no trusted cdhash list, and the kernel did not "
                             "register the signature (%s)", w2, kw);
                    rc = -1;
                    goto out;
                }
                kind = apple ? "Apple" : "valid";
                by = "Security.framework";
            }
        }
    }
    if (s.ents && s.ents_len > 8) foreign = hao_entitlements_foreign((const char *)s.ents + 8, s.ents_len - 8);
    else if (s.der_ents) foreign = true;           /* DER only: claims entitlements hao cannot read */
    if (foreign && !apple) {
        snprintf(why, sizeof why, "%s signature (%s) claims entitlements hao cannot grant; embed it with tools/hao-embed",
                 s.h.flags & CS_ADHOC_ ? "ad hoc" : "non-Apple", s.ident);
        rc = -1;
    } else if (hao_opt.verbose) {
        hao_diag_prefix(f);
        fprintf(f, "%s: %s signature (%s), identifier %s%s%s, cdhash ", path, kind, by, s.ident, s.team ? ", team " : "",
                s.team ? s.team : "");
        for (int k = 0; k < CS_CDHASH_LEN_; k++) fprintf(f, "%02x", s.cdhash[k]);
        fprintf(f, "; checked in %.0f us%s\n", now_us() - t0,
                foreign ? "; its entitlements are not carried (embed it to keep them)" : "");
    }
out:
    free(blob);
    if (rc) {
        hao_diag_prefix(f);
        fprintf(f, "%s: %s; refusing to run\n", path, why);
    }
    return rc;
}

/* ----------------------------------------- guest libraries (libval.c) */

/*
 * A guest dylib's signature, checked here as for the main executable: the
 * structure, the best CodeDirectory and its cdhash, every page hash of the
 * slice at the page size the CodeDirectory declares (4 or 16 KiB), and
 * nothing the loader maps outside the hashed range but the signature.
 * With signer, also who signed it: ad hoc (nobody), a trusted cdhash list
 * (Apple), else Security.framework (the CMS over this CodeDirectory, then
 * `anchor apple`, and the Team ID's certificate when team is given). The
 * kernel's own answer is libval.c's: this is the route for signatures the
 * kernel cannot hold (an x86_64 kernel registers only 4 KiB pages).
 * 0: valid (out filled); -1: not (why; out->bad_exec when a page of an
 * executable segment does not match its hash).
 */
int hao_guest_sig_image(int fd, const char *path, uint64_t slice_off, uint64_t slice_size, uint32_t subtype,
                        uint32_t sig_off, uint32_t sig_size, const hao_range *exec, int nexec, bool signer,
                        const char *team, hao_imgsig *out, char *why, size_t n)
{
    memset(out, 0, sizeof *out);
    char w2[400] = "";
    int rc = -1;
    sig_info s;
    uint8_t *blob = NULL;
    if (!sig_size) { snprintf(why, n, "not code signed"); return -1; }
    if ((uint64_t)sig_off + sig_size > slice_size || sig_size > CS_MAX_BLOB_) {
        snprintf(why, n, "code signature [%#x, +%#x) outside the slice", sig_off, sig_size);
        return -1;
    }
    blob = malloc(sig_size);
    if (!blob || pread(fd, blob, sig_size, (off_t)(slice_off + sig_off)) != (ssize_t)sig_size) {
        snprintf(why, n, "cannot read the code signature");
        goto out;
    }
    if (!sig_parse(blob, sig_size, &s, w2, sizeof w2)) { snprintf(why, n, "code signature invalid (%s)", w2); goto out; }
    if (s.code_limit < sig_off || s.code_limit > slice_size ||
        slice_size > (s.code_limit > (uint64_t)sig_off + sig_size ? s.code_limit : (uint64_t)sig_off + sig_size)) {
        snprintf(why, n, "code signature invalid (it covers [0, %#llx) of a %#llx-byte slice whose signature is at %#x)",
                 (unsigned long long)s.code_limit, (unsigned long long)slice_size, sig_off);
        goto out;
    }
    if (!pages_valid(fd, slice_off, &s, exec, nexec, &out->bad_exec, w2, sizeof w2)) {
        snprintf(why, n, "code signature invalid (%s)", w2);
        goto out;
    }
    memcpy(out->cdhash, s.cdhash, sizeof out->cdhash);
    out->hash_type = s.h.hashType;
    out->page_shift = s.h.pageSize;
    out->adhoc = (s.h.flags & CS_ADHOC_) != 0;
    if (s.team) snprintf(out->team, sizeof out->team, "%s", s.team);
    rc = 0;
    if (!signer) goto out;
    if (out->adhoc) { out->by = "ad hoc"; goto out; }
    if (trusted_cdhash(s.cdhash, w2, sizeof w2)) { out->apple = true; out->by = "trusted cdhash list"; goto out; }
    /* a team requirement only for a well-formed Team ID (it is quoted into the requirement text) */
    bool team_ok = false, tsafe = team && team[0] && strlen(team) < 32;
    for (const char *t = team; tsafe && *t; t++)
        tsafe = (*t >= 'A' && *t <= 'Z') || (*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z');
    if (sec_verdict(path, subtype, &s, &out->apple, tsafe ? team : NULL, &team_ok, w2, sizeof w2) != 0) {
        snprintf(why, n, "%s", w2);
        rc = -1;
        goto out;
    }
    out->team_ok = team_ok;
    out->by = "Security.framework";
out:
    free(blob);
    return rc;
}

/* The cdhash of a signature held in memory (the AOT loader's identity of a
 * signed image, jit.c): sig_parse's structure checks and nothing else. */
bool hao_guest_sig_cdhash(const uint8_t *blob, size_t len, uint8_t cdhash[CS_CDHASH_LEN_], uint8_t *hash_type, char *why, size_t n)
{
    sig_info s;
    if (len > CS_MAX_BLOB_) { snprintf(why, n, "a %zu-byte signature", len); return false; }
    if (!sig_parse(blob, len, &s, why, n)) return false;
    memcpy(cdhash, s.cdhash, CS_CDHASH_LEN_);
    *hash_type = s.h.hashType;
    return true;
}
