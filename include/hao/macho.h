/*
 * hao — arm64/arm64e Mach-O image loader for the hybrid model.
 *
 * Guest images are mapped into the host (x86_64) process. Their code pages
 * are mapped readable but NOT executable: translated code reads them as
 * data, and a native call into guest code faults, which the runtime turns
 * into a guest callback. Imports are bound through a resolver supplied by
 * the runtime, normally to native x86_64 library symbols.
 */
#ifndef HAO_MACHO_H
#define HAO_MACHO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define HAO_MAX_DEPS 256
#define HAO_MAX_RANGES 16
#define HAO_MAX_RPATHS 32
#define HAO_MAX_LAZY 32

typedef struct hao_image hao_image;

/* Resolve an import. ordinal: 1..n = dependency index+1, 0 = self,
 * -1 = main executable, -2 = flat, -3 = weak lookup.
 * Return the address to store; set *found=false if unresolved. name is
 * valid for the call only (it lies in the image's fixups, which the loader
 * gives back when the load ends): a resolver that keeps it copies it. */
typedef uint64_t (*hao_resolver)(hao_image *img, int ordinal, const char *name, bool weak, bool *found);

struct hao_image {
    char path[1024];
    int fd;                     /* open only while loading (-1 after) */
    uint64_t dev, ino;          /* the file loaded (st_dev, st_ino) */
    uint64_t slice_off, slice_size;
    uint32_t cpusubtype;
    uint64_t base;              /* runtime address of the mach header */
    uint64_t pref_base;         /* __TEXT vmaddr */
    uint64_t slide;
    uint64_t vmsize;
    /* Exact mapped segments for native address-to-image queries. Immutable once
     * listed, retained with the descriptor after unload; excludes reserved holes
     * and the shared cache's ambiguous common __LINKEDIT. */
    uint64_t (*identity_ranges)[2];
    uint32_t n_identity_ranges;
    /* a shared-cache image (dsc.c): its segments lie apart in the cache, so
     * [base, base+vmsize) is only __TEXT and every segment is listed here */
    bool dsc;
    uint64_t seg_lo[HAO_MAX_RANGES], seg_hi[HAO_MAX_RANGES];
    int nseg;
    uint64_t entry;             /* LC_MAIN entry (runtime) or 0 */
    uint64_t stack_size;
    bool is_dylib, is_pie;
    uint64_t code_lo[HAO_MAX_RANGES], code_hi[HAO_MAX_RANGES];
    int ncode;
    /* where hao reads the code ranges (hao/code.h): guest address + code_delta (0: the guest
     * mapping itself); code_alias/code_alias_size: an alias this image owns (on-disk images) */
    int64_t code_delta;
    uint64_t code_alias, code_alias_size;
    int ndeps;
    char *deps[HAO_MAX_DEPS];
    bool dep_weak[HAO_MAX_DEPS];      /* LC_LOAD_WEAK_DYLIB */
    bool dep_reexport[HAO_MAX_DEPS];  /* LC_REEXPORT_DYLIB */
    bool dep_upward[HAO_MAX_DEPS];    /* LC_LOAD_UPWARD_DYLIB */
    bool dep_cxx[HAO_MAX_DEPS];       /* an import from it has an Itanium C++ name (__Z...) */
    void *dep_handle[HAO_MAX_DEPS];   /* runtime.c: native handle or a marker */
    struct hao_image *dep_image[HAO_MAX_DEPS];  /* runtime.c: the guest image of a guest dependency */
    char *rpaths[HAO_MAX_RPATHS];     /* LC_RPATH, unexpanded */
    int nrpaths;
    /* LC_LAZY_LOAD_DYLIB_INFO blobs (one per lazily loaded dylib) */
    uint8_t *lazy_info[HAO_MAX_LAZY];
    uint32_t lazy_info_size[HAO_MAX_LAZY];
    int nlazy;
    struct hao_image *loader;   /* runtime.c: the image whose dependency loaded this one (@rpath search chain) */
    int init_state;             /* runtime.c: 0 loaded, 1 initialising, 2 initialised */
    uint32_t load_seq;          /* load order (main executable first) */
    /* initializers */
    uint64_t init_ptrs, init_ptrs_count;     /* S_MOD_INIT_FUNC_POINTERS */
    uint64_t init_offs, init_offs_count;     /* S_INIT_FUNC_OFFSETS (u32 offsets) */
    /* thread-local variables */
    uint64_t tlv_desc, tlv_desc_count;       /* S_THREAD_LOCAL_VARIABLES: {thunk,key,offset} */
    uint64_t tlv_init_start, tlv_init_size;  /* template: REGULAR then ZEROFILL */
    uint64_t tlv_total_size;
    /* linkedit */
    const uint8_t *exports_trie;
    uint32_t exports_trie_size;
    /* all sections (for ObjC/Swift metadata) */
    struct { char seg[17], sect[17]; uint64_t addr, size; uint32_t flags; } sects[128];
    int nsects;
    /* fixup statistics */
    uint64_t n_rebase, n_bind, n_auth, n_unresolved;
    uint8_t uuid[16];
    bool has_uuid;
    char libval_route[96];      /* libval.c: how its code signature was decided ("" not checked) */
    uint64_t func_starts_off;   /* LC_FUNCTION_STARTS (file offset in slice) */
    uint32_t func_starts_size;
    uint64_t *fstarts;          /* sorted absolute function start addresses */
    size_t nfstarts;
    uint64_t *fnslots;          /* chained-fixup rebase slots whose target is a guest function start */
    size_t nfnslots, capfnslots;
    void *jit_slot;             /* code-cache word holding `base` for PIC code */
    void *aot;                  /* loaded AOT cache (aot.c) */
    /* executable memory for the image's Swift metadata island entries (swiftmeta.c), mapped right after the
     * image when it was reserved (hao_island_reserve): [isl_lo, isl_hi), bump-allocated from isl_next */
    uint64_t isl_lo, isl_hi, isl_next;
    bool released;              /* taken off hao_images (read atomically by the lookups) */
    struct hao_image *next;     /* hao_images: see hao_image_register */
};

/* Island entries (swiftmeta.c) are rel32 targets: they must lie within +-2 GB of the fields that name them.
 * A process can fill that range of its image with later mappings before the image's metadata is walked
 * (WindowManager, Finder, sharingd and searchpartyd on MacBookPro9,2 do), so when hao_island_reserve is set the
 * loader maps an image with Swift metadata together with executable room after it, while the room is free:
 * one 32-byte entry per possible function start plus HAO_ISLAND_DYN_SLOTS for entries made at run time. */
#define HAO_ISLAND_SLOT 32
#define HAO_ISLAND_DYN_SLOTS 1024
extern bool hao_island_reserve;
/* the bytes of island room for an image with nfstarts_bound function starts at most */
uint64_t hao_island_room(uint64_t nfstarts_bound);

/*
 * The slice of a guest file that an image is built from, as bytes that do
 * not change while hao uses them. hao checks a guest's code signature itself
 * (libval.c, guest_sig.c) and builds the image afterwards. If the two read
 * the file separately, whoever may write the file can change it in between:
 * the kernel validates a native image page by page as it pages it in, and
 * nothing does that for pages hao maps as data. So the slice is taken once,
 * and the mach header, the load commands, the signature, every hashed page,
 * the linkedit blobs and the segments all come from bytes:
 *  - copied: a file that somebody other than root may change. The slice was
 *    read once into anonymous memory, and the image's segments are made of
 *    that memory (whole pages are handed over, not copied again). The image
 *    then shares no page with the file or with another process: it costs
 *    the slice's size for as long as it is loaded, in memory that can be
 *    compressed but not dropped (measured: 7.2 MB more peak footprint for an
 *    8.4 MB library, 66 MB for a 67 MB one; load time unchanged);
 *  - not copied: a file only root can change (root owns it, no group or
 *    other write bit, no ACL, on a local volume: the system's own files).
 *    bytes is the file, mapped, and the segments are mapped from the file
 *    as before, shared with every process that maps it.
 * fd stays the file for what the kernel is asked about it (F_ADDFILESIGS_INFO,
 * F_CHECK_LV, F_GETSIGSINFO); the slice does not own it.
 */
/* An embedded code signature as hao_codesig_parse (libval.c) read it: the
 * one reader, for the CLI's policy, integrity and library validation alike.
 * Pointers lie in the signature that was parsed (for a slice: in its bytes). */
typedef struct {
    const uint8_t *cd;          /* the CodeDirectory of highest hash rank (NULL: nothing parsed) */
    uint32_t cd_len;
    uint32_t flags;             /* its flags (adhoc 0x2, library 0x2000, runtime 0x10000) */
    uint8_t platform;           /* platform identifier (0: not a platform binary) */
    uint8_t hash_type, hash_size, page_shift;
    uint32_t hash_off, n_code_slots;
    uint64_t code_limit;
    uint8_t cdhash[20];
    const char *ident, *team;   /* NUL-terminated in cd; team NULL: no Team ID */
    const uint8_t *ents;        /* XML entitlements blob, header included, sealed by cd (NULL: none) */
    uint32_t ents_len;
    const uint8_t *der_ents;    /* DER entitlements blob, likewise */
    uint32_t der_ents_len;
    bool dlv;                   /* com.apple.security.cs.disable-library-validation */
} hao_codesig;

typedef struct hao_slice {
    int fd;
    uint64_t dev, ino;          /* the file (st_dev, st_ino) */
    uint64_t off, size;         /* the slice in the file */
    const uint8_t *bytes;       /* the slice, [0, size) */
    bool copied;
    uint32_t cpusubtype;        /* the mach header's */
    uint32_t filetype, flags;   /* the mach header's */
    const uint8_t *cmds;        /* the load commands, in bytes, validated (hao_macho_check_cmds) */
    uint32_t ncmds, sizeofcmds;
    int nsegs;                  /* LC_SEGMENT_64 commands */
    uint32_t sig_off, sig_size; /* its one LC_CODE_SIGNATURE (sig_size 0: not signed); not yet checked to lie in the slice */
    hao_codesig sig;            /* the signature, once hao_slice_verify accepted it (sig.cd NULL: not verified) */
} hao_slice;
/* The slice the loader runs (arm64e, else arm64) of the regular file open on
 * fd; NULL with why filled if there is none or its header or load commands
 * are malformed. */
hao_slice *hao_slice_open(int fd, char *why, size_t n);
void hao_slice_close(hao_slice *sl);
/* Leave sl for the loader: the next hao_load_image_fd_cb of sl->fd builds the
 * image from it instead of opening the slice again, and closes it. For a
 * caller that has checked these bytes and must load what it checked
 * (hao_guest_sig_enforce). */
void hao_slice_hand(hao_slice *sl);
/* The loader's unit tests: 1 maps every file, 2 copies every file (0: by
 * what the file's owner, mode, ACL and volume say). */
extern int hao_slice_test_path;

/* Load path (fat or thin), choose arm64e (else arm64) slice, map, apply
 * fixups. The file is untrusted: every offset, size and count is validated
 * against the slice, its linkedit blobs and the reserved region. Returns
 * NULL and fills err (naming the offending field) on failure. */
hao_image *hao_load_image(const char *path, hao_resolver resolve, char *err, size_t errlen);
/* The same, calling mapped(img, ctx) once the image is mapped and listed in
 * hao_images, before any of its imports are resolved (lets a loader record
 * the image first, so that a cyclic dependency finds it). */
typedef void (*hao_mapped_fn)(hao_image *img, void *ctx);
hao_image *hao_load_image_cb(const char *path, hao_resolver resolve, hao_mapped_fn mapped, void *ctx, char *err,
                             size_t errlen);
/* The same from fd, an open descriptor of the file, instead of opening path
 * (which then only names the image): for a caller that has checked that
 * descriptor and must load what it checked (the slice it left with
 * hao_slice_hand, if any). The image owns fd from here on; it is closed
 * whether the load succeeds or fails. */
hao_image *hao_load_image_fd_cb(const char *path, int fd, hao_resolver resolve, hao_mapped_fn mapped, void *ctx,
                                char *err, size_t errlen);
/* Native bundle fallback: recheck ARM-only MH_BUNDLE on the descriptor used
 * for signature validation and mapping, not only the preflight pathname. */
bool hao_is_arm_only_bundle(const char *path);
hao_image *hao_load_bundle_cb(const char *path, hao_resolver resolve, hao_mapped_fn mapped, void *ctx,
                              char *err, size_t errlen);
/* Add an image built elsewhere (dsc.c) to hao_images, in load order. */
void hao_image_register(hao_image *img);
/* Map only (no fixups, no binding): for offline translation. */
hao_image *hao_map_image_only(const char *path, char *err, size_t errlen);

/* Take an image off hao_images and unmap it (failed loads, fuzzing). A
 * listed image's struct and the arrays a lookup reaches stay allocated:
 * another thread may hold it from a lookup that raced the release. */
void hao_unload_image(hao_image *img);
/* Validate a load-command area (cmdsize, per-command sizes, nsects, string
 * offsets, at most one LC_CODE_SIGNATURE); 0 if every command is well
 * formed, else -1 with why filled. *nsegs gets the LC_SEGMENT_64 count.
 * Used for files and cache images. */
int hao_macho_check_cmds(const uint8_t *cmds, uint32_t sizeofcmds, uint32_t ncmds, int *nsegs, char *why, size_t n);
/* The file's arm64 dylib slice has no ObjC or Swift metadata (a C++
 * library that may run as guest code next to its native slice); false with
 * the reason in why. */
bool hao_macho_guest_cxx_eligible(const char *path, char *why, size_t whylen);
/* The slice the loader runs from a file of fsize bytes: arm64e (plain
 * subtype first), else arm64. Thin files give off 0 and size fsize. */
bool hao_macho_choose_slice(int fd, uint64_t fsize, uint64_t *off, uint64_t *size, uint32_t *sub, char *why, size_t whylen);

/* The code signature of a slice and library validation as the guest gets it
 * natively (src/libval.c). */
/* Parse an embedded signature (SuperBlob) as xnu validates one; false with
 * why filled if it is malformed or has no CodeDirectory. */
bool hao_codesig_parse(const uint8_t *blob, size_t len, hao_codesig *cs, char *why, size_t n);
/* Would the program load only platform or same-team libraries natively? */
bool hao_codesig_library_validated(const hao_codesig *cs);
/* Check the slice's signature on its bytes: structure, every page hash, and
 * that nothing in the slice but the signature lies outside the hashed range.
 * 0: valid, and sl->sig is the signature; -1: not (why; *bad_exec, if not
 * NULL, when a page of an executable segment does not match its hash). */
int hao_slice_verify(hao_slice *sl, bool *bad_exec, char *why, size_t n);
/* A guest image's signature as hao checks it in user space (guest_sig.c
 * hao_guest_sig_image, through hao_libval_set_verifier). */
typedef struct {
    bool apple;          /* signer asked: a trusted cdhash list or `anchor apple` (Security) */
    bool team_ok;        /* signer asked with a team: its certificate is that Team ID's */
    bool bad_exec;       /* invalid: a page of an executable segment does not match its hash */
    const char *by;      /* who vouched (signer asked) */
} hao_imgsig;
typedef int (*hao_imgsig_fn)(hao_slice *sl, const char *path, bool signer, const char *team, hao_imgsig *out,
                             char *why, size_t n);
int hao_guest_sig_image(hao_slice *sl, const char *path, bool signer, const char *team, hao_imgsig *out,
                        char *why, size_t n);
/* The runtime hands libval.c hao's signature checker (guest_sig.c), and
 * whether to check signers in user space even where the kernel could
 * (HAO_LIBVAL_USER, tests). Without a checker (the loader's unit tests and
 * fuzzers) only the kernel is asked. report writes a diagnostic line (the
 * diagnostics log) before an invalid page ends the process. */
void hao_libval_set_verifier(hao_imgsig_fn fn, bool force_user, void (*report)(const char *line));
enum { HAO_LIBVAL_UNDECIDED, HAO_LIBVAL_OFF, HAO_LIBVAL_KERNEL, HAO_LIBVAL_PLATFORM };
/* The next image loaded is the main executable: its signature sets the
 * policy (enabled = false: off, HAO_NO_LIBVAL=1 in an ordinary process;
 * no boot argument turns it off). */
void hao_libval_expect_main(bool enabled);
/* The loader's gate, after an image's load commands validate and before it
 * is mapped: sl is the slice the image is then built from. img->fd and the
 * slice fields are set. 0 = may load, else -1 with why filled. */
int hao_libval_image(hao_image *img, hao_slice *sl, char *why, size_t n);
int hao_libval_mode(void);
const char *hao_libval_reason(void);

/* Look up an exported symbol in a guest image's export trie. */
bool hao_image_export(hao_image *img, const char *name, uint64_t *addr);
/* The full trie entry: HAO_EXPORT_ADDR sets *addr (absolute symbols as
 * stored, others relative to the image); HAO_EXPORT_REEXPORT sets *ordinal
 * (the dependency it comes from) and *import_name (NULL: the same name). */
enum { HAO_EXPORT_NONE, HAO_EXPORT_ADDR, HAO_EXPORT_REEXPORT };
int hao_image_export_info(hao_image *img, const char *name, uint64_t *addr, int *ordinal, const char **import_name);

/* Find a section by name; returns its address and size (0 if absent). */
uint64_t hao_image_section(hao_image *img, const char *seg, const char *sect, uint64_t *size);

/* Is `a` exactly the entry point of a guest function? */
bool hao_is_guest_function(uint64_t a);

/* Find the guest image containing a code address (NULL if none). Lock-free:
 * any thread, also in a signal handler, while another thread loads. */
hao_image *hao_image_for_code(uint64_t addr);
hao_image *hao_image_for_addr(uint64_t addr);
/* The end of the mapped range of `im` that holds `a` (a segment of a
 * shared-cache image, else the whole image), or 0 if `a` is outside it. */
uint64_t hao_image_extent(const hao_image *im, uint64_t a);
/* The image list, newest first. Only the loader (serialised by runtime.c's
 * load lock) changes it; walk it from any thread with these. */
extern hao_image *hao_images;
static inline hao_image *hao_images_first(void) { return __atomic_load_n(&hao_images, __ATOMIC_ACQUIRE); }
static inline hao_image *hao_images_next(const hao_image *im) { return __atomic_load_n(&im->next, __ATOMIC_ACQUIRE); }

#endif
