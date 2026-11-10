/*
 * hao — AOT cache format and trust policy.
 *
 * An AOT cache (<LC_UUID>.aot) is x86_64 code that hao copies into its
 * executable code cache, so it is trusted exactly as far as the file and
 * every directory above it are: owned by root (or, for an ordinary user
 * process, by that user), not writable by anyone else, and not symlinks.
 * The format checks below catch corruption and caches from another guest
 * or another hao build; they cannot make an untrusted file safe.
 *
 * The pure parts (format validation, the directory list, the owner/mode
 * rules, the environment tiers) live in aot_policy.c, which is in
 * libhaocore.a so tests/aot/policy.c can link them without the runtime.
 */
#ifndef HAO_AOT_H
#define HAO_AOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ------------------------------------------------------------ format v5
 * file = hdr | ents[nblocks] | links[nlinks] | code[code_size]
 * code[0..15] is the base slot (the image base, patched at load).
 * payload_hash = hao_aot_hash64(ents..end of code). It is an integrity
 * check only; trust comes from ownership. */
#define HAO_AOT_MAGIC "HAOAOT2"
#define HAO_AOT_VERSION 5
#define HAO_AOT_MAX_FILE (1024ull << 20)

typedef struct {
    char magic[8];
    uint32_t version, hdr_size;
    uint8_t uuid[16];       /* guest LC_UUID */
    uint8_t gen[16];        /* LC_UUID of the libhao that generated the code */
    uint64_t vmsize;        /* guest image vmsize */
    uint64_t guest_id;      /* hao_aot_guest_id() of the guest slice */
    uint32_t nblocks, nlinks;
    uint64_t code_size, file_size, payload_hash;
} hao_aot_hdr;

typedef struct { uint32_t guest_off, code_off; } hao_aot_ent;      /* strictly increasing code_off */
typedef struct { uint32_t site, target; } hao_aot_link;           /* rel32 at code+site jumps to ents[target] */

/* What the loading image expects. Code ranges are image-relative [lo, hi). */
typedef struct {
    const uint8_t *uuid, *gen;
    uint64_t vmsize, guest_id;
    const uint64_t *range_lo, *range_hi;
    int nranges;
} hao_aot_expect;

typedef struct {
    const hao_aot_hdr *hdr;
    const hao_aot_ent *ents;
    const hao_aot_link *links;
    const uint8_t *code;
} hao_aot_view;

/* Validate a whole cache file held in memory. 0 = valid (v filled in);
 * -1 = rejected, with the reason in why. Never reads outside buf[0..size). */
int hao_aot_check(const void *buf, uint64_t size, const hao_aot_expect *x, hao_aot_view *v, char *why, size_t n);

uint64_t hao_aot_hash64(const void *p, size_t n, uint64_t seed);
/* Identity of a guest slice beyond its UUID: slice size, mach header and
 * load commands (segment layout, code-signature location). */
uint64_t hao_aot_guest_id(const void *mach_header, uint64_t avail, uint64_t slice_size);

/* ------------------------------------------------- environment trust
 * OPEN:    an ordinary user's process. Every HAO_* switch is honoured
 *          (the test switches only in a HAO_TEST_SWITCHES build).
 * GUARDED: PID 1, any process with effective uid 0, a process dyld treats
 *          as restricted, or one holding entitlements beyond hao's own. Its
 *          environment is not trusted to choose code, paths, checks or how
 *          calls are bridged: only the engine switches (HAO_NO_AOT,
 *          HAO_INTERP, HAO_NO_CHAIN, HAO_JIT_HELPERS_ONLY) and the printing
 *          ones (HAO_VERBOSE, HAO_CRASH_TRACE, HAO_NX_STATS,
 *          HAO_BRIDGE_STATS) are honoured.
 * SETUGID: credentials changed at exec (issetugid(), or uid != euid, or
 *          gid != egid). No switch is honoured, and dependency paths follow
 *          dyld's rule for such a process (hao_dep_path_allowed).
 * Boot arguments (hao=, hao_verbose=, and the hao_<knob>=1 that hao/opts.h
 * lists) are the trusted channel for PID 1 and daemons. */
enum { HAO_ENV_OPEN = 0, HAO_ENV_GUARDED = 1, HAO_ENV_SETUGID = 2 };

typedef struct {
    bool setugid;           /* issetugid() */
    bool restricted;        /* dyld_process_is_restricted() */
    bool entitled;          /* entitlements beyond allow-jit/disable-library-validation */
    long pid, ppid;
    uid_t uid, euid;
    gid_t gid, egid;
    int distrust;           /* HAO_DISTRUST_ENV: can only lower trust */
} hao_proc_facts;

int hao_env_tier_for(const hao_proc_facts *f);
/* May switch `name` be read from the environment at this tier? */
bool hao_env_switch_allowed(const char *name, int tier);
/* Was this hao built with HAO_TEST_SWITCHES (a test or bring-up build,
 * never a release)? */
extern const bool hao_test_switches;
/* Is `name` one of the test switches (never read by a build without
 * HAO_TEST_SWITCHES)? */
bool hao_env_test_switch(const char *name);
/* May a dependency path be expanded and tried at this tier? path is an
 * install name (from_rpath false) or an LC_RPATH entry of the image that is
 * (main_image) or is not the main executable. false: *why (if not NULL) is
 * dyld's wording for the refusal. */
bool hao_dep_path_allowed(const char *path, bool from_rpath, bool main_image, int tier, const char **why);
/* Does an XML entitlements plist name any key outside hao's own? */
bool hao_entitlements_foreign(const char *xml, size_t n);

/* ---------------------------------------------------- cache locations */
enum { HAO_AOT_SYS_RO, HAO_AOT_SYS, HAO_AOT_USER, HAO_AOT_EXPLICIT };
#define HAO_AOT_SYS_RO_DIR "/usr/lib/hao/aot"
#define HAO_AOT_SYS_DIR "/private/var/db/hao"
#define HAO_AOT_MAX_DIRS 3

typedef struct { char path[1024]; int kind; } hao_aot_dir;

/* Directories to search, in order; returns how many. *write_idx is the
 * entry `hao --aot` writes to, or -1. hao_cache is HAO_CACHE (ignored
 * unless tier is OPEN, so never read by root; must be absolute, else nothing is used and why says
 * so). pw_dir is the euid's home from the user database (never $HOME);
 * a user directory exists only for an OPEN process with 501 <= euid <
 * 0x80000000. */
int hao_aot_dirs_for(int tier, uid_t euid, const char *pw_dir, const char *hao_cache, hao_aot_dir *out,
                     int *write_idx, char *why, size_t n);

/* Owner and mode rules for one path element. role: 0 = ancestor directory,
 * 1 = the cache directory, 2 = the cache file. Owners: root for SYS kinds;
 * root or euid for USER/EXPLICIT. No group/other write bit, except that an
 * ancestor of a USER/EXPLICIT directory may be sticky (/private/tmp), since
 * the next element must itself be trusted-owned. */
enum { HAO_AOT_ANCESTOR, HAO_AOT_DIR, HAO_AOT_FILE };
bool hao_aot_stat_ok(const struct stat *st, int kind, int role, uid_t euid, char *why, size_t n);
/* lstat-based check of an absolute canonical directory and all its
 * ancestors (an ACL allow entry with a write-type right rejects). */
bool hao_aot_dir_ok(const char *canon, int kind, uid_t euid, char *why, size_t n);
/* Symlinks in the configured path must be root-owned (/tmp, /var). */
bool hao_aot_literal_ok(const char *path, char *why, size_t n);
/* Does the extended ACL (from acl_get_*_np) grant anyone write, delete,
 * attribute, security or owner rights? */
bool hao_aot_acl_allows(void *acl);
/* Open d->path/name for reading if the file, d->path and every directory
 * above them pass the rules above for d->kind (src/jit.c). -1 with why
 * empty: no such file; with why set: the file exists but is rejected. */
int hao_aot_open_trusted(const hao_aot_dir *d, const char *name, struct stat *st, char *why, size_t n);

#endif
