/*
 * hao — library validation, as the guest would get it natively.
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
 *    flag, and no disable-library-validation entitlement. A program that
 *    misstates its signature only changes its own policy; in the CLI form
 *    guest_sig.c has verified that signature, in the embedded form the
 *    file is the program's (docs/security-model.md);
 *  - native loads (dlopen by the guest or by hao's binder, NSBundle, ...):
 *    the process sets CS_REQUIRE_LV on itself (csops CS_OPS_SET_STATUS,
 *    allowed for any valid process). From then on the kernel refuses every
 *    non-platform library of another Team ID in this process, whatever its
 *    entitlements (measured: an ad hoc dylib is refused with "mapping
 *    process and mapped file (non-platform) have different Team IDs" in a
 *    process signed with disable-library-validation);
 *  - guest images (arm64 dylibs hao maps as data, which the kernel never
 *    validates as code), on the descriptor the loader then reads the image
 *    from (check_image below): every page hash, checked by hao (guest_sig.c),
 *    whatever the policy; then the loader attaches the slice's signature to
 *    the vnode (F_ADDFILESIGS_INFO, the cdhash must be hao's) and asks the
 *    kernel whether this process may map it (F_CHECK_LV). If the kernel
 *    refused CS_REQUIRE_LV, a guest image must at least be platform code
 *    (F_GETSIGSINFO). Where the kernel cannot hold the signature (an x86_64
 *    kernel accepts only 4 KiB-page CodeDirectories; Apple's arm64 code is
 *    signed with 16 KiB pages, so on Intel this is every Apple library) hao
 *    applies AMFI's rule itself with the signer it establishes: a trusted
 *    cdhash list or Security's `anchor apple` (platform), else the main
 *    executable's Team ID with a certificate for it. HAO_LIBVAL_USER takes
 *    that route on any kernel (tests).
 * Images from the arm64e shared cache (dsc.c) come from the OS cache paths
 * and are not checked. This file depends only on libc: the loader unit
 * tests and fuzzers link it with macho.c.
 */
#include "hao/macho.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/loader.h>
#include <libkern/OSByteOrder.h>

int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_OPS_SET_STATUS 9
#define CS_REQUIRE_LV 0x2000
#define CS_RUNTIME 0x10000

#define CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0
#define CSMAGIC_CODEDIRECTORY 0xfade0c02
#define CSMAGIC_EMBEDDED_ENTITLEMENTS 0xfade7171
#define CSMAGIC_EMBEDDED_DER_ENTITLEMENTS 0xfade7172
#define CSSLOT_CODEDIRECTORY 0
#define CSSLOT_ENTITLEMENTS 5
#define CSSLOT_DER_ENTITLEMENTS 7

static const char DLV[] = "com.apple.security.cs.disable-library-validation";

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

bool hao_codesig_parse(const uint8_t *b, size_t len, hao_codesig *cs)
{
    memset(cs, 0, sizeof *cs);
    if (len < 12 || OSReadBigInt32(b, 0) != CSMAGIC_EMBEDDED_SIGNATURE) return false;
    uint32_t total = OSReadBigInt32(b, 4), count = OSReadBigInt32(b, 8);
    if (total > len || total < 12 || count > (total - 12) / 8) return false;
    bool xml = false, der = false;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t type = OSReadBigInt32(b, 12 + 8 * k), off = OSReadBigInt32(b, 16 + 8 * k);
        if (off > total || total - off < 8) continue;
        uint32_t magic = OSReadBigInt32(b, off), blen = OSReadBigInt32(b, off + 4);
        if (blen < 8 || blen > total - off) continue;
        const uint8_t *p = b + off;
        if (type == CSSLOT_CODEDIRECTORY && magic == CSMAGIC_CODEDIRECTORY && blen >= 44) {
            uint32_t version = OSReadBigInt32(p, 8);
            cs->sig = true;
            cs->flags = OSReadBigInt32(p, 12);
            cs->platform = p[38];
            if (version >= 0x20200 && blen >= 52) {
                uint32_t t = OSReadBigInt32(p, 48);
                cs->team = t && t < blen && p[t] != 0;
                size_t tl = cs->team ? strnlen((const char *)p + t, blen - t) : 0;
                if (cs->team && tl < sizeof cs->team_id && t + tl < blen) memcpy(cs->team_id, p + t, tl);
            }
        } else if (type == CSSLOT_ENTITLEMENTS && magic == CSMAGIC_EMBEDDED_ENTITLEMENTS) {
            xml = true;
            cs->dlv |= xml_true(p + 8, blen - 8, DLV);
        } else if (type == CSSLOT_DER_ENTITLEMENTS && magic == CSMAGIC_EMBEDDED_DER_ENTITLEMENTS) {
            der = true;
            if (!xml) cs->dlv |= der_true(p + 8, blen - 8, DLV);
        }
    }
    (void)der;
    return cs->sig;
}

bool hao_codesig_library_validated(const hao_codesig *cs)
{
    if (!cs->sig || cs->dlv) return false;
    return cs->platform != 0 || (cs->flags & (CS_RUNTIME | CS_REQUIRE_LV)) != 0;
}

static int mode = HAO_LIBVAL_UNDECIDED;
static bool expect_main;
static char reason[160];
static hao_imgsig_fn verifier;      /* guest_sig.c's checker (NULL: the loader's unit tests) */
static bool force_user;             /* HAO_LIBVAL_USER: signers in user space even where the kernel answers */
static bool integrity;              /* the policy came from the main executable's signature: pages are checked */
static bool main_platform;          /* the main executable's CodeDirectory has a platform identifier */
static char main_team[32];          /* its Team ID ("" none) */
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

/* LC_CODE_SIGNATURE of a validated load-command area: false if absent. */
static bool find_sig(const uint8_t *cmds, uint32_t ncmds, uint64_t slice, uint32_t *off, uint32_t *size)
{
    const uint8_t *p = cmds;
    for (uint32_t k = 0; k < ncmds; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd != LC_CODE_SIGNATURE || lc->cmdsize < sizeof(struct linkedit_data_command)) continue;
        const struct linkedit_data_command *ld = (const void *)p;
        if (ld->dataoff > slice || ld->datasize > slice - ld->dataoff) return false;
        *off = ld->dataoff;
        *size = ld->datasize;
        return ld->datasize > 0;
    }
    return false;
}

static void decide(hao_image *img, const uint8_t *cmds, uint32_t ncmds)
{
    hao_codesig cs = {0};
    uint32_t off = 0, size = 0;
    if (find_sig(cmds, ncmds, img->slice_size, &off, &size) && size <= (64u << 20)) {
        uint8_t *b = malloc(size);
        if (b && pread(img->fd, b, size, (off_t)(img->slice_off + off)) == (ssize_t)size) hao_codesig_parse(b, size, &cs);
        free(b);
    }
    integrity = true;
    main_platform = cs.platform != 0;
    snprintf(main_team, sizeof main_team, "%s", cs.team_id);
    if (!hao_codesig_library_validated(&cs)) {
        mode = HAO_LIBVAL_OFF;
        snprintf(reason, sizeof reason, "%s", !cs.sig ? "unsigned" : cs.dlv ? "disable-library-validation" :
                 "neither platform nor hardened");
        return;
    }
    snprintf(reason, sizeof reason, "%s%s%s", cs.platform ? "platform" : "",
             cs.platform && (cs.flags & (CS_RUNTIME | CS_REQUIRE_LV)) ? ", " : "",
             cs.flags & CS_RUNTIME ? "runtime" : cs.flags & CS_REQUIRE_LV ? "library flag" : "");
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
}

static int kernel_check(hao_image *img, const uint8_t *cmds, uint32_t ncmds, char *why, size_t n)
{
    uint32_t off = 0, size = 0;
    bool sig = find_sig(cmds, ncmds, img->slice_size, &off, &size);
    if (sig) {
        fsignatures_t fs = {.fs_file_start = (off_t)img->slice_off, .fs_blob_start = (void *)(uintptr_t)off,
                            .fs_blob_size = size};
        (void)fcntl(img->fd, F_ADDFILESIGS_RETURN, &fs);   /* a refusal shows in the checks below */
    }
    if (mode == HAO_LIBVAL_KERNEL) {
        char msg[256] = "";
        fchecklv_t lv = {.lv_file_start = (off_t)img->slice_off, .lv_error_message_size = sizeof msg,
                         .lv_error_message = msg};
        if (fcntl(img->fd, F_CHECK_LV, &lv) == 0) return 0;
        snprintf(why, n, "%s", msg[0] ? msg : strerror(errno));
        return -1;
    }
    fgetsigsinfo_t gi = {.fg_file_start = (off_t)img->slice_off, .fg_info_request = GETSIGSINFO_PLATFORM_BINARY};
    if (sig && fcntl(img->fd, F_GETSIGSINFO, &gi) == 0 && gi.fg_sig_is_platform) return 0;
    snprintf(why, n, "mapped file is not a platform binary");
    return -1;
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

int terminate_with_payload(int pid, uint32_t reason_namespace, uint64_t reason_code, void *payload, uint32_t payload_size,
                           const char *reason_string, uint64_t reason_flags);
#define OS_REASON_CODESIGNING 9
#define CODESIGNING_EXIT_REASON_INVALID_PAGE 2

/* The library-validation rule as AMFI applies it to a mapped file whose
 * signature is valid: a platform file, else one of the process's own Team
 * ID (the main executable's: under hao the kernel's process is hao's). */
static const char *lv_rule(const hao_imgsig *g)
{
    if (g->apple) return NULL;
    if (mode == HAO_LIBVAL_PLATFORM) return "mapped file is not a platform binary";
    if (main_platform) return "mapping process is a platform binary, but mapped file is not";
    if (!g->team[0] && main_team[0]) return "mapped file has no Team ID and is not a platform binary (signed with custom identity or adhoc?)";
    if (!g->team[0] || !main_team[0] || strcmp(g->team, main_team))
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
 */
static int check_image(hao_image *img, const uint8_t *cmds, uint32_t ncmds, char *k, size_t kn)
{
    uint32_t off = 0, size = 0;
    bool sig = find_sig(cmds, ncmds, img->slice_size, &off, &size);
    bool lv = mode == HAO_LIBVAL_KERNEL || mode == HAO_LIBVAL_PLATFORM;
    if (!sig) {
        if (!lv) return 0;
        snprintf(k, kn, "mapped file is not code signed");
        return -1;
    }
    hao_range ex[16];
    int nex = exec_ranges(cmds, ncmds, ex, 16);
    snprintf(route, sizeof route, "integrity");
    hao_imgsig g;
    char w[400] = "";
    if (verifier(img->fd, img->path, img->slice_off, img->slice_size, img->cpusubtype, off, size, ex, nex, false, NULL, &g, w,
                 sizeof w) != 0) {
        if (g.bad_exec) {
            char msg[700];
            snprintf(msg, sizeof msg, "%s: %s: a page of an executable segment; killed as the kernel kills it natively", img->path, w);
            if (report_line) report_line(msg);
            else fprintf(stderr, "hao: %s\n", msg);
            terminate_with_payload(getpid(), OS_REASON_CODESIGNING, CODESIGNING_EXIT_REASON_INVALID_PAGE, NULL, 0, msg, 0);
            _exit(137);
        }
        snprintf(k, kn, "%s", w);
        return -1;
    }
    if (!lv) return 0;
    snprintf(route, sizeof route, "kernel, %u KiB pages", 1u << (g.page_shift - 10));
    bool user = force_user;
    if (!user) {
        fsignatures_t fs;
        memset(&fs, 0, sizeof fs);
        fs.fs_file_start = (off_t)img->slice_off;
        fs.fs_blob_start = (void *)(uintptr_t)off;
        fs.fs_blob_size = size;
        fs.fs_fsignatures_size = sizeof fs;
        if (fcntl(img->fd, F_ADDFILESIGS_INFO, &fs) != 0) {
            user = true;   /* the kernel cannot hold it (x86_64: page shift 14 -> EBADEXEC); hao decides */
        } else if (memcmp(fs.fs_cdhash, g.cdhash, sizeof g.cdhash) || fs.fs_hash_type != g.hash_type) {
            snprintf(k, kn, "the kernel holds a different code signature for this file (rewritten in place?)");
            return -1;
        } else if (mode == HAO_LIBVAL_KERNEL) {
            char msg[256] = "";
            fchecklv_t cl = {.lv_file_start = (off_t)img->slice_off, .lv_error_message_size = sizeof msg,
                             .lv_error_message = msg};
            if (fcntl(img->fd, F_CHECK_LV, &cl) == 0) return 0;
            /* the process's Team ID is hao's: the program's own team is decided here */
            if (!(main_team[0] && !main_platform && g.team[0] && !strcmp(g.team, main_team))) {
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
    snprintf(route, sizeof route, "user space%s, %u KiB pages", force_user ? " (HAO_LIBVAL_USER)" : "", 1u << (g.page_shift - 10));
    if (verifier(img->fd, img->path, img->slice_off, img->slice_size, img->cpusubtype, off, size, ex, nex, true,
                 main_team[0] ? main_team : NULL, &g, w, sizeof w) != 0) {
        snprintf(k, kn, "%s", w);
        return -1;
    }
    size_t rl = strlen(route);
    snprintf(route + rl, sizeof route - rl, ", %s", g.by ? g.by : "");
    const char *bad = lv_rule(&g);
    if (!bad) return 0;
    snprintf(k, kn, "%s", bad);
    return -1;
}

int hao_libval_image(hao_image *img, const uint8_t *cmds, uint32_t ncmds, char *why, size_t n)
{
    if (expect_main) {
        expect_main = false;
        if (mode == HAO_LIBVAL_UNDECIDED) decide(img, cmds, ncmds);
        return 0;
    }
    char k[500], u[48];
    int r;
    route[0] = 0;
    if (verifier && integrity) r = check_image(img, cmds, ncmds, k, sizeof k);
    else if (mode == HAO_LIBVAL_KERNEL || mode == HAO_LIBVAL_PLATFORM) r = kernel_check(img, cmds, ncmds, k, sizeof k);
    else return 0;
    snprintf(img->libval_route, sizeof img->libval_route, "%s", route);
    if (r == 0) return 0;
    uuid_text(cmds, ncmds, u, sizeof u);
    snprintf(why, n, "code signature in %s '%s' not valid for use in process: %s", u, img->path, k);
    return -1;
}
