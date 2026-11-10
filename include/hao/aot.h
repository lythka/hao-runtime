/*
 * hao — AOT cache format and trust policy.
 *
 * An AOT cache (<LC_UUID>.aot) is x86_64 code that hao copies into its
 * executable code cache. Two things make that safe, and both are needed:
 *
 *  - who could write it: the file and every directory above it are owned by
 *    root (or, for an ordinary user process, by that user), not writable by
 *    anyone else, and not symlinks;
 *  - what it was translated from: the header carries a cryptographic
 *    identity of the guest code (HAO_AOT_ID_*), and a cache is used only for
 *    an image whose own code gives the same identity. The LC_UUID, vmsize
 *    and load-command hash only name a guest; a file with another program's
 *    header and its own instructions has all three.
 *
 * The code identity of a signed guest is the cdhash of the slice hao runs
 * (the hash of its best CodeDirectory, which holds a hash of every page of
 * the slice: header, load commands and code). `hao --aot` records it only
 * after checking every page against it; the loader recomputes it from the
 * signature in the image it mapped. The identity of an unsigned guest is a
 * SHA-256 over its mach header, load commands and executable segments, which
 * the loader has to compute over the mapped image each time it finds a
 * candidate cache; such caches are never written to, or loaded from, a
 * system directory.
 *
 * The other format checks below catch corruption and caches from another
 * hao build; they cannot make an untrusted file safe.
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

/* ------------------------------------------------------------ format v6
 * file = hdr | ents[nblocks] | links[nlinks] | code[code_size]
 * code[0..15] is the base slot (the image base, patched at load).
 * payload_hash = hao_aot_hash64(ents..end of code). It is an integrity
 * check only; trust comes from ownership and from the code identity. */
#define HAO_AOT_MAGIC "HAOAOT2"
#define HAO_AOT_VERSION 6
#define HAO_AOT_MAX_FILE (1024ull << 20)

/* What code_id holds. */
enum {
    HAO_AOT_ID_CDHASH = 1,  /* code_id[0..19]: cdhash of the slice's best CodeDirectory, rest zero;
                             * id_hash: that CodeDirectory's hash type (cs_blobs.h CS_HASHTYPE_*) */
    HAO_AOT_ID_CODE = 2,    /* code_id: hao_aot_code_id() of an unsigned slice; id_hash 0 */
};
#define HAO_AOT_ID_LEN 32
#define HAO_AOT_CDHASH_LEN 20

typedef struct {
    char magic[8];                               /*   0 */
    uint32_t version, hdr_size;                  /*   8, 12 */
    uint8_t uuid[16];                            /*  16: guest LC_UUID */
    uint8_t gen[16];                             /*  32: LC_UUID of the libhao that generated the code */
    uint64_t vmsize;                             /*  48: guest image vmsize */
    uint64_t guest_id;                           /*  56: hao_aot_guest_id() of the guest slice */
    uint32_t nblocks, nlinks;                    /*  64, 68 */
    uint64_t code_size, file_size, payload_hash; /*  72, 80, 88 */
    uint32_t id_kind, id_hash;                   /*  96, 100: HAO_AOT_ID_* and its hash type */
    uint8_t code_id[HAO_AOT_ID_LEN];             /* 104: the guest code's identity */
} hao_aot_hdr;                                   /* 136 bytes */

typedef struct { uint32_t guest_off, code_off; } hao_aot_ent;      /* strictly increasing code_off */
typedef struct { uint32_t site, target; } hao_aot_link;           /* rel32 at code+site jumps to ents[target] */

/* What the loading image expects. Code ranges are image-relative [lo, hi).
 * id_kind says whether the image is signed; code_id (with id_hash) is its
 * code identity, or NULL while it has not been computed (see hao_aot_check). */
typedef struct {
    const uint8_t *uuid, *gen;
    uint64_t vmsize, guest_id;
    const uint64_t *range_lo, *range_hi;
    int nranges;
    uint32_t id_kind, id_hash;
    const uint8_t *code_id;
} hao_aot_expect;

typedef struct {
    const hao_aot_hdr *hdr;
    const hao_aot_ent *ents;
    const hao_aot_link *links;
    const uint8_t *code;
} hao_aot_view;

/* Validate a whole cache file held in memory. 0 = valid (v filled in);
 * -1 = rejected, with the reason in why. Never reads outside buf[0..size).
 * With x->code_id NULL it never returns 0: it returns HAO_AOT_NEED_ID when
 * the header names this guest, this hao build and this kind of identity, so
 * the caller computes the identity (an unsigned guest's costs a hash of its
 * code) only for a file that can still match, and checks again. */
#define HAO_AOT_NEED_ID 1
int hao_aot_check(const void *buf, uint64_t size, const hao_aot_expect *x, hao_aot_view *v, char *why, size_t n);

uint64_t hao_aot_hash64(const void *p, size_t n, uint64_t seed);
/* A cheap name for a guest slice beyond its UUID: slice size, mach header
 * and load commands (segment layout, code-signature location). Not
 * cryptographic and not a statement about the code: see code_id. */
uint64_t hao_aot_guest_id(const void *mach_header, uint64_t avail, uint64_t slice_size);

/* The code identity of an unsigned slice: SHA-256 over a domain tag, the
 * mach header and load commands (cmds_len bytes at mach_header), and each
 * executable range as mapped (image-relative [lo, hi) and its hi - lo bytes
 * at code[k]). Lengths and offsets are hashed as little-endian 64-bit. */
void hao_aot_code_id(const void *mach_header, uint64_t cmds_len, const uint64_t *lo, const uint64_t *hi,
                     const uint8_t *const *code, int nranges, uint8_t out[HAO_AOT_ID_LEN]);

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

/* Is this cache directory one every process trusts: a system kind, or a
 * canonical path that is, or lies inside, one of the system directories
 * (HAO_CACHE may name them)? Such a directory holds caches for signed code
 * only: `hao --aot` writes no HAO_AOT_ID_CODE cache there and the loader
 * takes none from there. */
bool hao_aot_dir_is_system(int kind, const char *canon);

typedef struct { char path[1024]; int kind; } hao_aot_dir;

/* Directories to search, in order; returns how many. *write_idx is the
 * entry `hao --aot` writes to, or -1. hao_cache is HAO_CACHE (ignored
 * unless tier is OPEN, so never read by root; must be absolute, else nothing is used and why says
 * so). pw_dir is the euid's home from the user database (never $HOME);
 * a user directory exists only for an OPEN process with 501 <= euid <
 * 0x80000000. */
int hao_aot_dirs_for(int tier, uid_t euid, const char *pw_dir, const char *hao_cache, hao_aot_dir *out,
                     int *write_idx, char *why, size_t n);

/* The directory `hao --aot --cache DIR` writes to. The command line is the
 * invoking user's own choice, unlike the environment, which a guarded
 * process inherits from whoever started it: DIR is taken at every tier but
 * SETUGID, where the caller is not the user the process runs as. It is an
 * EXPLICIT directory like HAO_CACHE's: absolute, and held to the same
 * ownership and system-directory rules by the caller. false: why says. */
bool hao_aot_dir_explicit(int tier, const char *dir, hao_aot_dir *out, char *why, size_t n);

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
