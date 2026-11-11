/* hao — arm64/arm64e Mach-O loader. See include/hao/macho.h.
 *
 * The guest file is untrusted input (hao runs it as root when launchd is
 * PID 1, and the kext routes any arm64-only file here). This is a
 * validating parser: every offset, size and count read from the file is
 * checked against the chosen slice, the load-command area, the linkedit
 * blob it indexes or the reserved image region, with 64-bit arithmetic that
 * cannot wrap, before anything is read, mapped or written. Any violation
 * rejects the whole image with a message naming the field. */
#include "hao/macho.h"
#include "hao/identity.h"
#include <errno.h>
#include "hao/code.h"
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/fat.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

hao_image *hao_images;
static bool map_only;

#define ERR(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

#ifndef CPU_SUBTYPE_ARM64E
#define CPU_SUBTYPE_ARM64E 2
#endif

#define PAGE 0x1000ull                  /* host (x86_64) page size; mmap granularity */
#define MAX_FAT_ARCHS 64
#define MAX_SEGS 255                    /* dyld's limit */
#define MAX_VMSIZE (1ull << 40)
#define MAX_FSTARTS (1u << 24)
/* Sizes a guest file may claim. A Mach-O's linkedit offsets are 32 bits and its signature (at most xnu's
 * 40 MiB) ends it, so no slice whose linkedit data hao can read is longer than MAX_SLICE. The others bound
 * what one image makes hao walk or keep (the exports trie and each lazy-load record are copied and stay
 * with the image). They are far above what linkers write: over the 10143 arm64 slices of a macOS 27.2
 * system and its applications, the largest load-command area is 33 KiB, exports trie 4.0 MiB, chained
 * fixups 4.9 MiB, legacy stream 0.7 MiB, function starts 0.8 MiB and slice 240 MiB. */
#define MAX_SLICE ((1ull << 32) + (40ull << 20))
#define MAX_CMDS (4u << 20)             /* sizeofcmds */
#define MAX_BLOB (256u << 20)           /* exports trie, chained fixups, a legacy dyld stream, function starts */
#define MAX_LAZY_BLOB (4u << 20)        /* one LC_LAZY_LOAD_DYLIB_INFO record; an image has at most HAO_MAX_LAZY */

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }

/* [off, off+len) lies within [0, size), without wrapping */
static bool in_range(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size - off; }

/* Pick the arm64e slice (plain subtype 2, any ABI-version bits), else arm64. */
bool hao_macho_choose_slice(int fd, uint64_t fsize, uint64_t *off, uint64_t *size, uint32_t *sub, char *why, size_t whylen)
{
    uint32_t magic;
    if (pread(fd, &magic, 4, 0) != 4) { snprintf(why, whylen, "file too short"); return false; }
    if (magic == MH_MAGIC_64) {
        struct mach_header_64 h;
        if (pread(fd, &h, sizeof h, 0) != sizeof h) { snprintf(why, whylen, "short mach header"); return false; }
        if (h.cputype != CPU_TYPE_ARM64) { snprintf(why, whylen, "no arm64/arm64e slice"); return false; }
        *off = 0; *size = fsize; *sub = h.cpusubtype;
        return true;
    }
    if (magic != FAT_CIGAM && magic != FAT_CIGAM_64) { snprintf(why, whylen, "not a 64-bit Mach-O or fat file"); return false; }
    struct fat_header fh;
    if (pread(fd, &fh, sizeof fh, 0) != sizeof fh) { snprintf(why, whylen, "short fat header"); return false; }
    uint32_t n = be32(fh.nfat_arch);
    if (n == 0 || n > MAX_FAT_ARCHS) { snprintf(why, whylen, "fat header: %u architectures", n); return false; }
    uint64_t esz = magic == FAT_CIGAM_64 ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
    if (!in_range(sizeof fh, (uint64_t)n * esz, fsize)) { snprintf(why, whylen, "fat arch table past end of file"); return false; }
    int best = -1;
    uint64_t boff = 0, bsize = 0;
    uint32_t bsub = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t cpu, s;
        uint64_t o, z;
        if (magic == FAT_CIGAM_64) {
            struct fat_arch_64 a;
            if (pread(fd, &a, sizeof a, (off_t)(sizeof fh + k * esz)) != sizeof a) { snprintf(why, whylen, "short fat arch"); return false; }
            cpu = be32((uint32_t)a.cputype); s = be32((uint32_t)a.cpusubtype); o = be64(a.offset); z = be64(a.size);
        } else {
            struct fat_arch a;
            if (pread(fd, &a, sizeof a, (off_t)(sizeof fh + k * esz)) != sizeof a) { snprintf(why, whylen, "short fat arch"); return false; }
            cpu = be32((uint32_t)a.cputype); s = be32((uint32_t)a.cpusubtype); o = be32(a.offset); z = be32(a.size);
        }
        if (cpu != CPU_TYPE_ARM64) continue;
        /* Rank: plain arm64e (no x1-style extension) > arm64e.* > arm64. */
        uint32_t base_sub = s & ~CPU_SUBTYPE_MASK;
        int rank = base_sub == CPU_SUBTYPE_ARM64E ? ((s & CPU_SUBTYPE_MASK) == 0x80000000u || (s & CPU_SUBTYPE_MASK) == 0 ? 3 : 2) : 1;
        if (rank > best) { best = rank; boff = o; bsize = z; bsub = s; }
    }
    if (best < 0) { snprintf(why, whylen, "no arm64/arm64e slice"); return false; }
    if (!in_range(boff, bsize, fsize) || boff < sizeof fh) {
        snprintf(why, whylen, "arm64 slice [%#llx, +%#llx) outside the file (%#llx bytes)", (unsigned long long)boff,
                 (unsigned long long)bsize, (unsigned long long)fsize);
        return false;
    }
    *off = boff; *size = bsize; *sub = bsub;
    return true;
}

/* The arm64 slice of the dylib at path can run as guest code although the
 * file also has a native slice (runtime.c: a library guest code calls with
 * C++ names). Its metadata must not be what a native runtime would have to
 * share with a native copy: no Objective-C (any __objc_* section, as in
 * the eligibility check) and no Swift (__swift5_*). */
bool hao_macho_guest_cxx_eligible(const char *path, char *why, size_t whylen)
{
    bool ok = false;
    uint8_t *cmds = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    uint64_t off, size;
    uint32_t sub;
    struct mach_header_64 mh;
    if (fd < 0 || fstat(fd, &st) != 0) { snprintf(why, whylen, "%s: cannot open", path); goto out; }
    if (!hao_macho_choose_slice(fd, (uint64_t)st.st_size, &off, &size, &sub, why, whylen)) goto out;
    if (size < sizeof mh || pread(fd, &mh, sizeof mh, (off_t)off) != sizeof mh || mh.magic != MH_MAGIC_64 ||
        mh.cputype != CPU_TYPE_ARM64 || mh.filetype != MH_DYLIB || !in_range(sizeof mh, mh.sizeofcmds, size)) {
        snprintf(why, whylen, "%s: no arm64 dylib slice", path);
        goto out;
    }
    if (!(cmds = malloc(mh.sizeofcmds ? mh.sizeofcmds : 1)) ||
        pread(fd, cmds, mh.sizeofcmds, (off_t)(off + sizeof mh)) != (ssize_t)mh.sizeofcmds) {
        snprintf(why, whylen, "%s: short load commands", path);
        goto out;
    }
    char w[200];
    if (hao_macho_check_cmds(cmds, mh.sizeofcmds, mh.ncmds, NULL, w, sizeof w) != 0) { snprintf(why, whylen, "%s: %s", path, w); goto out; }
    const uint8_t *p = cmds;
    for (uint32_t k = 0; k < mh.ncmds; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)p;
        const struct section_64 *sc = (const void *)(sg + 1);
        for (uint32_t j = 0; j < sg->nsects; j++)
            if (!strncmp(sc[j].sectname, "__objc_", 7) || !strncmp(sc[j].sectname, "__swift5_", 9)) {
                snprintf(why, whylen, "%s: has %.16s,%.16s", path, sc[j].segname, sc[j].sectname);
                goto out;
            }
    }
    ok = true;
out:
    free(cmds);
    if (fd >= 0) close(fd);
    return ok;
}

/* ULEB128 in [*p, end). Sets *bad on truncation or more than 64 bits. */
static uint64_t read_uleb(const uint8_t **p, const uint8_t *end, bool *bad)
{
    uint64_t r = 0;
    int sh = 0;
    for (;;) {
        if (*p >= end) { *bad = true; return 0; }
        uint8_t b = *(*p)++;
        if (sh >= 64 || (sh == 63 && (b & 0x7e))) { *bad = true; return 0; }
        r |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) return r;
        sh += 7;
    }
}

/* Export-trie lookup. Every step consumes at least one byte of `name` (an
 * empty edge is malformed), so the walk ends after at most strlen(name)+1
 * nodes whatever the trie's shape, cycles included. */
int hao_image_export_info(hao_image *img, const char *name, uint64_t *addr, int *ordinal, const char **import_name)
{
    const uint8_t *start = img->exports_trie;
    if (!start || !img->exports_trie_size) return HAO_EXPORT_NONE;
    const uint8_t *end = start + img->exports_trie_size, *p = start;
    const char *s = name;
    bool bad = false;
    for (;;) {
        const uint8_t *q = p;
        uint64_t term = read_uleb(&q, end, &bad);
        if (bad || term > (uint64_t)(end - q)) return HAO_EXPORT_NONE;
        const uint8_t *children = q + term;
        if (*s == 0) {
            if (!term) return HAO_EXPORT_NONE;
            uint64_t flags = read_uleb(&q, children, &bad);
            if (bad) return HAO_EXPORT_NONE;
            if (flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
                uint64_t ord = read_uleb(&q, children, &bad);
                if (bad || ord > (uint64_t)img->ndeps || q >= children) return HAO_EXPORT_NONE;
                const char *imp = (const char *)q;
                size_t len = strnlen(imp, (size_t)(children - q));
                if (len == (size_t)(children - q)) return HAO_EXPORT_NONE;   /* unterminated */
                if (ordinal) *ordinal = (int)ord;
                if (import_name) *import_name = len ? imp : NULL;
                return HAO_EXPORT_REEXPORT;
            }
            uint64_t off = read_uleb(&q, children, &bad);   /* stub-and-resolver: use the stub */
            if (bad) return HAO_EXPORT_NONE;
            if ((flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE) { *addr = off; return HAO_EXPORT_ADDR; }
            if (off >= img->vmsize && !hao_image_extent(img, img->base + off)) return HAO_EXPORT_NONE;
            *addr = img->base + off;
            return HAO_EXPORT_ADDR;
        }
        q = children;
        if (q >= end) return HAO_EXPORT_NONE;
        uint8_t nchild = *q++;
        const uint8_t *next = NULL;
        for (uint8_t k = 0; k < nchild; k++) {
            const char *edge = (const char *)q;
            size_t len = strnlen(edge, (size_t)(end - q));
            if (len == 0 || len == (size_t)(end - q)) return HAO_EXPORT_NONE;   /* empty or unterminated edge */
            q += len + 1;
            uint64_t child = read_uleb(&q, end, &bad);
            if (bad || child >= img->exports_trie_size) return HAO_EXPORT_NONE;
            if (!strncmp(s, edge, len)) { s += len; next = start + child; break; }
        }
        if (!next) return HAO_EXPORT_NONE;
        p = next;
    }
}

bool hao_image_export(hao_image *img, const char *name, uint64_t *addr)
{
    return hao_image_export_info(img, name, addr, NULL, NULL) == HAO_EXPORT_ADDR;
}

uint64_t hao_image_section(hao_image *img, const char *seg, const char *sect, uint64_t *size)
{
    for (int k = 0; k < img->nsects; k++)
        if ((!seg || !strncmp(img->sects[k].seg, seg, 16)) && !strncmp(img->sects[k].sect, sect, 16)) {
            if (size) *size = img->sects[k].size;
            return img->sects[k].addr;
        }
    if (size) *size = 0;
    return 0;
}

bool hao_is_guest_function(uint64_t a)
{
    hao_image *im = hao_image_for_code(a);
    if (!im || !im->nfstarts) return false;
    size_t lo = 0, hi = im->nfstarts;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (im->fstarts[mid] < a) lo = mid + 1; else hi = mid;
    }
    return lo < im->nfstarts && im->fstarts[lo] == a;
}

uint64_t hao_image_extent(const hao_image *im, uint64_t a)
{
    if (a >= im->base && a - im->base < im->vmsize) return im->base + im->vmsize;
    for (int k = 0; k < im->nseg; k++)
        if (a >= im->seg_lo[k] && a < im->seg_hi[k]) return im->seg_hi[k];
    return 0;
}

/* Every address range a guest image was ever registered with (hao_image_extent's ranges and the code
 * ranges), sorted and merged: a value outside all of them is in no image, which the two lookups below
 * answer with a binary search instead of a walk of every image's ranges (they run for each argument value of
 * every native call, src/blocks.c). A snapshot is immutable and never freed (a reader may hold it); the
 * next one is built from it and the new image and replaces it by compare-and-swap, before the image is
 * listed, so a walker that sees an image (acquire on the list head) sees a snapshot that covers it. Only
 * grows: a released image's ranges stay, which costs a walk at most. */
typedef struct { size_t n; uint64_t r[][2]; } range_snap;
#define RANGES_OFF ((range_snap *)(uintptr_t)1)   /* out of memory once: no filtering */
static range_snap *ranges_snap;

static int range_cmp(const void *x, const void *y)
{
    uint64_t a = ((const uint64_t *)x)[0], b = ((const uint64_t *)y)[0];
    return a < b ? -1 : a > b;
}

static void ranges_add(const hao_image *img)
{
    for (;;) {
        range_snap *old = __atomic_load_n(&ranges_snap, __ATOMIC_ACQUIRE);
        if (old == RANGES_OFF) return;
        size_t on = old ? old->n : 0, n = on;
        range_snap *s = malloc(sizeof *s + (on + 1 + 2 * HAO_MAX_RANGES) * sizeof s->r[0]);
        if (!s) { __atomic_store_n(&ranges_snap, RANGES_OFF, __ATOMIC_RELEASE); return; }
        if (on) memcpy(s->r, old->r, on * sizeof s->r[0]);
        if (img->vmsize) { s->r[n][0] = img->base; s->r[n][1] = img->base + img->vmsize; n++; }
        for (int k = 0; k < img->nseg && k < HAO_MAX_RANGES; k++)
            if (img->seg_hi[k] > img->seg_lo[k]) { s->r[n][0] = img->seg_lo[k]; s->r[n][1] = img->seg_hi[k]; n++; }
        for (int k = 0; k < img->ncode && k < HAO_MAX_RANGES; k++)
            if (img->code_hi[k] > img->code_lo[k]) { s->r[n][0] = img->code_lo[k]; s->r[n][1] = img->code_hi[k]; n++; }
        qsort(s->r, n, sizeof s->r[0], range_cmp);
        size_t m = 0;
        for (size_t k = 0; k < n; k++) {
            if (m && s->r[k][0] <= s->r[m - 1][1]) { if (s->r[k][1] > s->r[m - 1][1]) s->r[m - 1][1] = s->r[k][1]; }
            else { s->r[m][0] = s->r[k][0]; s->r[m][1] = s->r[k][1]; m++; }
        }
        s->n = m;
        if (__atomic_compare_exchange_n(&ranges_snap, &old, s, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
        free(s);   /* never published */
    }
}

/* The immutable snapshot only grows. Its outer envelope cheaply rejects
 * native/heap/scalar addresses before the exact range search. Keep the
 * successful code-hint path ahead of this check so positives pay no new loads.
 * Load the image-list head before the snapshot so every visible image is
 * covered by the snapshot's publication-before-list ordering. */
static bool ranges_envelope_maybe(const range_snap *s, uint64_t a)
{
    return !s || s == RANGES_OFF || !s->n ||
           (a >= s->r[0][0] && a < s->r[s->n - 1][1]);
}

/* false: a is in no range a guest image was registered with */
static bool ranges_maybe(const range_snap *s, uint64_t a)
{
    if (!s || s == RANGES_OFF) return true;
    size_t lo = 0, hi = s->n;   /* the first range with r[0] > a */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s->r[mid][0] <= a) lo = mid + 1; else hi = mid;
    }
    return lo && a < s->r[lo - 1][1];
}

bool hao_guest_range_maybe(uint64_t a)
{
    const range_snap *ranges = __atomic_load_n(&ranges_snap, __ATOMIC_ACQUIRE);
    return ranges_envelope_maybe(ranges, a) && ranges_maybe(ranges, a);
}

/* Any address inside a guest image's mapped range (header, data, code). */
hao_image *hao_image_for_addr(uint64_t a)
{
    hao_image *first = hao_images_first();
    const range_snap *ranges = __atomic_load_n(&ranges_snap, __ATOMIC_ACQUIRE);
    if (!ranges_envelope_maybe(ranges, a) || !ranges_maybe(ranges, a)) return NULL;
    for (hao_image *im = first; im; im = hao_images_next(im))
        if (hao_image_extent(im, a) && !__atomic_load_n(&im->released, __ATOMIC_ACQUIRE)) return im;
    return NULL;
}

/* Native frameworks make these queries on guest data without crossing a guest
 * import trap (Mail's EmailFoundation localized-string lookup). The existing
 * image registry is lock-free; its descriptor and ranges outlive release. */
bool hao_identity_guest_image(const void *address, hao_identity_info *info)
{
    uint64_t a = (uint64_t)(uintptr_t)address;
    /* Do not use hao_image_for_addr's execution-range shortcut here: cache
     * images may have more segments than that fixed-size execution index. */
    for (hao_image *im = hao_images_first(); im; im = hao_images_next(im)) {
        for (uint32_t k = 0; k < im->n_identity_ranges; k++) {
            if (a < im->identity_ranges[k][0] || a >= im->identity_ranges[k][1]) continue;
            if (__atomic_load_n(&im->released, __ATOMIC_ACQUIRE)) break;
            *info = (hao_identity_info){im->path, (void *)(uintptr_t)im->base, (intptr_t)im->slide};
            return true;
        }
    }
    return false;
}

/* The image the last lookup found: one process-wide hint, never a
 * thread-local variable. This runs in signal handlers on native threads
 * (signals.c), and dyld allocates a thread's TLV block on its first access,
 * which deadlocks or traps when the signal interrupted malloc. An image is
 * never freed once it was on the list (release_image), so a stale hint
 * fails the range check at worst. */
static hao_image *last_code_img;

hao_image *hao_image_for_code(uint64_t a)
{
    hao_image *last = __atomic_load_n(&last_code_img, __ATOMIC_ACQUIRE);
    if (last) {
        int n = __atomic_load_n(&last->ncode, __ATOMIC_ACQUIRE);
        for (int k = 0; k < n; k++) if (a >= last->code_lo[k] && a < last->code_hi[k]) return last;
    }
    hao_image *first = hao_images_first();
    const range_snap *ranges = __atomic_load_n(&ranges_snap, __ATOMIC_ACQUIRE);
    if (!ranges_envelope_maybe(ranges, a) || !ranges_maybe(ranges, a)) return NULL;
    for (hao_image *im = first; im; im = hao_images_next(im)) {
        int n = __atomic_load_n(&im->ncode, __ATOMIC_ACQUIRE);
        for (int k = 0; k < n; k++)
            if (a >= im->code_lo[k] && a < im->code_hi[k]) {
                __atomic_store_n(&last_code_img, im, __ATOMIC_RELEASE);
                return im;
            }
    }
    return NULL;
}

/* ------------------------------------------------------- chained fixups */

/* One LC_SEGMENT_64, as validated by hao_load_image (file vmaddr; index is the
 * load-command order that chained-fixup segment indices use). */
typedef struct {
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t initprot;
    bool pagezero;
} seginfo;

typedef struct {
    int ordinal;
    bool weak;
    const char *name;
    int64_t addend;
    uint64_t value;
} import_t;

/* Library ordinal from an import entry: 1..n dependencies, 0 self, and the
 * special negatives -1 (main executable), -2 (flat), -3 (weak lookup). */
static bool import_ordinal(uint32_t v, int bits, int ndeps, int *out)
{
    uint32_t m = (1u << bits) - 1;
    v &= m;
    int o = v > m - 16 ? (int)v - (int)(m + 1) : (int)v;
    if (o < BIND_SPECIAL_DYLIB_WEAK_LOOKUP || o > ndeps) return false;
    *out = o;
    return true;
}

/* A rebased pointer to a guest function start: a candidate for a native
 * entry point (fnslots.c rewrites it after ObjC and block registration). */
static void note_fnslot(hao_image *img, uint64_t loc)
{
    if (img->nfnslots == img->capfnslots) {
        size_t cap = img->capfnslots ? img->capfnslots * 2 : 256;
        uint64_t *n = realloc(img->fnslots, cap * sizeof *n);
        if (!n) return;
        img->fnslots = n;
        img->capfnslots = cap;
    }
    img->fnslots[img->nfnslots++] = loc;
}

static int apply_chained(hao_image *img, const uint8_t *lc_data, uint32_t lc_size, hao_resolver resolve,
                         const seginfo *segs, int nseg, char *err, size_t errlen)
{
    const struct dyld_chained_fixups_header *h = (const void *)lc_data;
    import_t *imp = NULL;
#define CERR(...) do { snprintf(err, errlen, "chained fixups: " __VA_ARGS__); free(imp); return -1; } while (0)
    if (lc_size < sizeof *h) CERR("blob of %u bytes is smaller than its header", lc_size);
    if (h->fixups_version != 0) CERR("version %u", h->fixups_version);
    if (h->symbols_format != 0) CERR("compressed symbol table (format %u) unsupported", h->symbols_format);
    uint64_t esz;
    switch (h->imports_format) {
    case DYLD_CHAINED_IMPORT: esz = sizeof(struct dyld_chained_import); break;
    case DYLD_CHAINED_IMPORT_ADDEND: esz = sizeof(struct dyld_chained_import_addend); break;
    case DYLD_CHAINED_IMPORT_ADDEND64: esz = sizeof(struct dyld_chained_import_addend64); break;
    default: CERR("imports format %u", h->imports_format);
    }
    uint32_t count = h->imports_count;
    if (!in_range(h->imports_offset, (uint64_t)count * esz, lc_size)) CERR("%u imports at %#x overrun the blob", count, h->imports_offset);
    if (h->symbols_offset > lc_size) CERR("symbols offset %#x past the blob", h->symbols_offset);
    if (h->starts_offset > lc_size || lc_size - h->starts_offset < sizeof(uint32_t)) CERR("starts offset %#x past the blob", h->starts_offset);
    const char *syms = (const char *)lc_data + h->symbols_offset;
    uint64_t syms_size = lc_size - h->symbols_offset;
    imp = calloc(count ? count : 1, sizeof *imp);
    if (!imp) CERR("out of memory for %u imports", count);
    const uint8_t *ip = lc_data + h->imports_offset;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t lib, name_off;
        bool weak;
        int bits = 8;
        const uint8_t *e = ip + esz * k;
        if (h->imports_format == DYLD_CHAINED_IMPORT) {
            struct dyld_chained_import x; memcpy(&x, e, sizeof x);
            lib = x.lib_ordinal; weak = x.weak_import; name_off = x.name_offset;
        } else if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
            struct dyld_chained_import_addend x; memcpy(&x, e, sizeof x);
            lib = x.lib_ordinal; weak = x.weak_import; name_off = x.name_offset; imp[k].addend = x.addend;
        } else {
            struct dyld_chained_import_addend64 x; memcpy(&x, e, sizeof x);
            lib = x.lib_ordinal; weak = x.weak_import; name_off = x.name_offset; imp[k].addend = (int64_t)x.addend;
            bits = 16;
        }
        if (name_off >= syms_size || strnlen(syms + name_off, (size_t)(syms_size - name_off)) == syms_size - name_off)
            CERR("import %u: name offset %#x outside the symbol table", k, name_off);
        if (!import_ordinal(lib, bits, img->ndeps, &imp[k].ordinal))
            CERR("import %u (%s): library ordinal %u out of range (%d dependencies)", k, syms + name_off, lib, img->ndeps);
        imp[k].weak = weak;
        imp[k].name = syms + name_off;
        /* a library this image calls with C++ names (runtime.c: such a library may run as guest code) */
        if (imp[k].ordinal > 0 && !strncmp(imp[k].name, "__Z", 3)) img->dep_cxx[imp[k].ordinal - 1] = true;
    }
    for (uint32_t k = 0; k < count; k++) {
        bool found = false;
        imp[k].value = resolve(img, imp[k].ordinal, imp[k].name, imp[k].weak, &found);
        if (!found && !imp[k].weak) img->n_unresolved++;
        imp[k].value += (uint64_t)imp[k].addend;
    }

    const uint8_t *sib = lc_data + h->starts_offset;
    uint64_t si_size = lc_size - h->starts_offset;
    uint32_t seg_count;
    memcpy(&seg_count, sib, 4);
    if (!in_range(4, (uint64_t)seg_count * 4, si_size)) CERR("%u segment starts overrun the blob", seg_count);
    if (seg_count > (uint32_t)nseg) CERR("starts for %u segments, image has %d", seg_count, nseg);
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t so;
        memcpy(&so, sib + 4 + 4 * s, 4);
        if (!so) continue;
        const size_t hdr = offsetof(struct dyld_chained_starts_in_segment, page_start);
        if (!in_range(so, hdr, si_size)) CERR("segment %u: starts at %#x past the blob", s, so);
        struct dyld_chained_starts_in_segment ss;
        memcpy(&ss, sib + so, hdr);
        if (!in_range(so + hdr, (uint64_t)ss.page_count * 2, si_size)) CERR("segment %u: %u page starts overrun the blob", s, ss.page_count);
        const seginfo *sg = &segs[s];
        if (sg->pagezero || !sg->vmsize) CERR("segment %u has fixups but is not mapped", s);
        if (ss.segment_offset != sg->vmaddr - img->pref_base)   /* offset from the mach header */
            CERR("segment %u: segment_offset %#llx does not match its vmaddr", s, (unsigned long long)ss.segment_offset);
        if (!(sg->initprot & VM_PROT_WRITE)) CERR("segment %u has fixups but is not writable", s);
        if (ss.page_size != 0x1000 && ss.page_size != 0x4000) CERR("segment %u: page size %#x", s, ss.page_size);
        if ((uint64_t)ss.page_count * ss.page_size > sg->vmsize + ss.page_size - 1)
            CERR("segment %u: %u pages of %#x exceed its vmsize %#llx", s, ss.page_count, ss.page_size, (unsigned long long)sg->vmsize);
        uint16_t fmt = ss.pointer_format;
        int stride;
        switch (fmt) {
        case DYLD_CHAINED_PTR_ARM64E: case DYLD_CHAINED_PTR_ARM64E_USERLAND: case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            stride = 8; break;
        case DYLD_CHAINED_PTR_64: case DYLD_CHAINED_PTR_64_OFFSET:
            stride = 4; break;
        default:
            CERR("segment %u: pointer format %u unsupported", s, fmt);
        }
        /* every fixup location is an 8-byte slot inside this segment */
        uint64_t seg_lo = sg->vmaddr + img->slide, seg_len = sg->vmsize;
        for (uint32_t pg = 0; pg < ss.page_count; pg++) {
            uint16_t start;
            memcpy(&start, sib + so + hdr + 2 * pg, 2);
            if (start == DYLD_CHAINED_PTR_START_NONE) continue;
            if (start & DYLD_CHAINED_PTR_START_MULTI) CERR("multi-start pages unsupported");
            if (start >= ss.page_size) CERR("segment %u page %u: start %#x past the page", s, pg, start);
            uint64_t off = (uint64_t)pg * ss.page_size + start;
            for (;;) {
                if (!in_range(off, 8, seg_len)) CERR("segment %u: fixup at +%#llx outside the segment", s, (unsigned long long)off);
                uint64_t loc = seg_lo + off;
                uint64_t raw;
                memcpy(&raw, (void *)(uintptr_t)loc, 8);
                uint64_t val, next;
                bool is_rebase;
                if (stride == 8) {
                    bool auth = raw >> 63, bind = (raw >> 62) & 1;
                    next = (raw >> 51) & 0x7ff;
                    is_rebase = !bind;
                    if (bind) {
                        uint32_t ord = fmt == DYLD_CHAINED_PTR_ARM64E_USERLAND24 ? (uint32_t)(raw & 0xffffff) : (uint32_t)(raw & 0xffff);
                        if (ord >= count) CERR("bind ordinal %u out of range (%u imports)", ord, count);
                        val = imp[ord].value;
                        if (!auth) {
                            int64_t add = ((int64_t)(raw << 13)) >> 45;  /* bits 32..50, signed 19 */
                            val += (uint64_t)add;
                        } else img->n_auth++;
                        img->n_bind++;
                    } else if (auth) {
                        val = img->base + (raw & 0xffffffffull);
                        img->n_auth++;
                        img->n_rebase++;
                    } else {
                        uint64_t target = raw & 0x7ffffffffffull, high8 = (raw >> 43) & 0xff;
                        val = (fmt == DYLD_CHAINED_PTR_ARM64E) ? target + img->slide : img->base + target;
                        val |= high8 << 56;
                        img->n_rebase++;
                    }
                } else {
                    bool bind = raw >> 63;
                    next = (raw >> 51) & 0xfff;
                    is_rebase = !bind;
                    if (bind) {
                        uint32_t ord = (uint32_t)(raw & 0xffffff);
                        if (ord >= count) CERR("bind ordinal %u out of range (%u imports)", ord, count);
                        val = imp[ord].value + ((raw >> 24) & 0xff);
                        img->n_bind++;
                    } else {
                        uint64_t target = raw & 0xfffffffffull, high8 = (raw >> 36) & 0xff;
                        val = (fmt == DYLD_CHAINED_PTR_64) ? target + img->slide : img->base + target;
                        val |= high8 << 56;
                        img->n_rebase++;
                    }
                }
                memcpy((void *)(uintptr_t)loc, &val, 8);
                if (is_rebase && hao_is_guest_function(val)) note_fnslot(img, loc);
                if (!next) break;
                off += next * (uint64_t)stride;    /* next > 0: strictly forward, so no cycles */
            }
        }
    }
#undef CERR
    free(imp);
    return 0;
}

/* Legacy dyld pointer streams (mach-o/loader.h). Every stream is first
 * interpreted without writes or symbol resolution. Lazy pointer bindings are
 * resolved eagerly; no guest call is routed through dyld_stub_binder. */
enum { LEGACY_REBASE, LEGACY_BIND, LEGACY_LAZY, LEGACY_WEAK };
#define MAX_LEGACY_FIXUPS (1u << 26)

static int64_t legacy_sleb(const uint8_t **p, const uint8_t *end, bool *bad)
{
    uint64_t v = 0;
    unsigned shift = 0;
    for (;;) {
        if (*p == end) { *bad = true; return 0; }
        uint8_t b = *(*p)++;
        if (shift == 63 && (b & 0x7f) != 0 && (b & 0x7f) != 0x7f) { *bad = true; return 0; }
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) v |= UINT64_MAX << shift;
            return (int64_t)v;
        }
        if (shift >= 64) { *bad = true; return 0; }
    }
}

static int legacy_stream(hao_image *img, const uint8_t *data, uint32_t size, int mode,
                         const seginfo *segs, int nseg, hao_resolver resolve, bool apply,
                         char *err, size_t errlen)
{
#define DERR(...) do { snprintf(err, errlen, "legacy dyld: " __VA_ARGS__); return -1; } while (0)
    const uint8_t *p = data, *end = data + size;
    uint64_t off = 0, actions = 0;
    int seg = -1, type = mode == LEGACY_LAZY ? BIND_TYPE_POINTER : 0;
    int ordinal = mode == LEGACY_WEAK ? BIND_SPECIAL_DYLIB_WEAK_LOOKUP : 0;
    bool have_ordinal = mode == LEGACY_WEAK, weak = false, bad = false, done = false;
    int64_t addend = 0;
    const char *name = NULL;
    while (p < end) {
        uint8_t byte = *p++, op = byte & 0xf0, imm = byte & 0x0f;
        uint64_t count = 0, skip = 0;
        if (!op) {
            if (imm) DERR("invalid DONE opcode");
            done = true;
            if (mode != LEGACY_LAZY) {
                while (p < end) if (*p++) DERR("nonzero bytes after DONE");
                break;
            }
            /* Each lazy record has an independent state machine. */
            off = 0; seg = -1; type = BIND_TYPE_POINTER; ordinal = 0;
            have_ordinal = false; weak = false; name = NULL; addend = 0;
            continue;
        }
        done = false;
        if (mode == LEGACY_REBASE) {
            switch (op) {
            case REBASE_OPCODE_SET_TYPE_IMM:
                if (imm != REBASE_TYPE_POINTER) DERR("unsupported rebase type %u", imm);
                type = imm; break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = imm; off = read_uleb(&p, end, &bad); break;
            case REBASE_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end, &bad); break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: off += (uint64_t)imm * 8; break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES: count = imm; break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: count = read_uleb(&p, end, &bad); break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: count = 1; skip = read_uleb(&p, end, &bad); break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb(&p, end, &bad); skip = read_uleb(&p, end, &bad); break;
            default: DERR("unknown rebase opcode %#x", byte);
            }
        } else {
            switch (op) {
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                if (mode == LEGACY_WEAK) DERR("library ordinal in weak stream");
                ordinal = imm; have_ordinal = true; break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
                uint64_t value = read_uleb(&p, end, &bad);
                if (mode == LEGACY_WEAK || value > (uint64_t)img->ndeps) DERR("library ordinal out of range");
                ordinal = (int)value; have_ordinal = true; break;
            }
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                if (mode == LEGACY_WEAK) DERR("library ordinal in weak stream");
                ordinal = imm ? (int)imm - 16 : 0; have_ordinal = true; break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
                if (imm & ~(BIND_SYMBOL_FLAGS_WEAK_IMPORT | BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION)) DERR("unsupported symbol flags %#x", imm);
                if (imm & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) DERR("strong-definition coalescing unsupported");
                const uint8_t *nul = memchr(p, 0, (size_t)(end - p));
                if (!nul || nul == p) DERR("unterminated or empty symbol name");
                name = (const char *)p; p = nul + 1;
                weak = (imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
                break;
            }
            case BIND_OPCODE_SET_TYPE_IMM:
                if (imm != BIND_TYPE_POINTER) DERR("unsupported bind type %u", imm);
                type = imm; break;
            case BIND_OPCODE_SET_ADDEND_SLEB: addend = legacy_sleb(&p, end, &bad); break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = imm; off = read_uleb(&p, end, &bad); break;
            /* Old linkers encode backward moves as unsigned wrapping deltas.
             * Address arithmetic is modulo 2^64; every actual write is bounded. */
            case BIND_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end, &bad); break;
            case BIND_OPCODE_DO_BIND: count = 1; break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1; skip = read_uleb(&p, end, &bad); break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1; skip = (uint64_t)imm * 8; break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb(&p, end, &bad); skip = read_uleb(&p, end, &bad); break;
            case BIND_OPCODE_THREADED: DERR("threaded bind opcodes unsupported");
            default: DERR("unknown bind opcode %#x", byte);
            }
            if (have_ordinal && (ordinal < BIND_SPECIAL_DYLIB_WEAK_LOOKUP || ordinal > img->ndeps)) DERR("library ordinal out of range");
        }
        if (bad) DERR("malformed LEB128 operand");
        if (!count) continue;
        if (count > MAX_LEGACY_FIXUPS - actions) DERR("fixup count exceeds limit");
        actions += count;
        if (type != BIND_TYPE_POINTER) DERR("fixup type not set");
        if (seg < 0 || seg >= nseg || segs[seg].pagezero || !segs[seg].vmsize) DERR("fixup segment not mapped");
        const seginfo *sg = &segs[seg];
        if (!(sg->initprot & VM_PROT_WRITE)) DERR("fixup segment is not writable");
        if (mode != LEGACY_REBASE && (!name || !have_ordinal)) DERR("bind symbol or library ordinal not set");
        /* Mark only actual imports, after all stream state has been set.
         * The validation pass visits every stream before a resolver runs. */
        if (mode != LEGACY_REBASE && ordinal > 0 && !strncmp(name, "__Z", 3)) img->dep_cxx[ordinal - 1] = true;
        for (uint64_t k = 0; k < count; k++) {
            if (!in_range(off, 8, sg->vmsize)) DERR("fixup outside its segment");
            if (apply) {
                uint64_t loc = sg->vmaddr + img->slide + off, value;
                if (mode == LEGACY_REBASE) {
                    memcpy(&value, (void *)(uintptr_t)loc, 8);
                    value += img->slide;
                    img->n_rebase++;
                } else {
                    bool found = false;
                    value = resolve(img, ordinal, name, weak || mode == LEGACY_WEAK, &found);
                    if (!found && mode == LEGACY_WEAK) DERR("weak definition %s not found", name);
                    if (!found && !weak) img->n_unresolved++;
                    value += (uint64_t)addend;
                    img->n_bind++;
                }
                memcpy((void *)(uintptr_t)loc, &value, 8);
                if (mode == LEGACY_REBASE && hao_is_guest_function(value)) note_fnslot(img, loc);
            }
            off += 8 + skip;
        }
    }
    if (size && !done) DERR("missing DONE opcode");
    return 0;
#undef DERR
}

static uint64_t null_resolver(hao_image *img, int o, const char *n, bool w, bool *found)
{
    (void)img; (void)o; (void)n; (void)w;
    *found = true;
    return 0;
}

hao_image *hao_map_image_only(const char *path, char *err, size_t errlen)
{
    map_only = true;
    hao_image *img = hao_load_image(path, null_resolver, err, errlen);
    map_only = false;
    return img;
}

static uint32_t load_seq;
uint64_t hao_ncache_gen = 1;   /* hao/macho.h */

/*
 * hao_images is read without a lock by every thread (hao_image_for_code
 * and hao_image_for_addr run on each translation, native call, NX fault
 * and signal delivery) while the loader changes it; the loader is
 * serialised by runtime.c's load lock (lazy loads, dependencies found at
 * run time, failed loads). So:
 *  - an image is published whole: its fields are written, then it becomes
 *    the head with a release store, and walkers load each link with
 *    acquire (hao_images_first/next);
 *  - an image is unlinked with one store into its predecessor and keeps its
 *    own next, so a walker standing on it goes on to the rest of the list;
 *  - an image that was ever listed is never freed, nor is anything a lookup
 *    reaches through it (function starts, export trie, dependency names,
 *    rpaths, lazy-load records, function-pointer slots): a thread may hold
 *    it from a lookup that raced its release. Only failed loads and
 *    hao_unload_image release listed images, so this keeps little.
 */
void hao_image_register(hao_image *img)
{
    img->load_seq = load_seq++;
    ranges_add(img);   /* before the image is listed (ranges_snap) */
    img->next = hao_images;
    __atomic_store_n(&hao_images, img, __ATOMIC_RELEASE);
    hao_ncache_invalidate();   /* its code is guest code now, not a native target */
}

/* Take img off the list and drop every reference other images hold to it
 * (dependencies it loaded while binding keep living, with its loader as
 * theirs). With keep_range the region stays reserved, inaccessible: other
 * images may already hold pointers into it, which must fault rather than
 * reach whatever would be mapped there next. */
static void release_image(hao_image *img, bool keep_range)
{
    bool listed = false;
    /* a lookup that still holds img (the hint, or its walk of the list)
     * finds no code ranges and skips it; the struct stays */
    __atomic_store_n(&img->released, true, __ATOMIC_RELEASE);
    __atomic_store_n(&img->ncode, 0, __ATOMIC_RELEASE);
    for (hao_image **pp = &hao_images; *pp; pp = &(*pp)->next)
        if (*pp == img) { __atomic_store_n(pp, img->next, __ATOMIC_RELEASE); listed = true; break; }
    for (hao_image *im = hao_images; im; im = im->next) {
        if (im->loader == img) im->loader = img->loader;
        for (int k = 0; k < im->ndeps; k++) if (im->dep_image[k] == img) im->dep_image[k] = NULL;
    }
    hao_image *hint = img;
    __atomic_compare_exchange_n(&last_code_img, &hint, NULL, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    hao_ncache_invalidate();
    hao_code_release(img, keep_range);
    if (img->base && img->vmsize) {
        if (keep_range) mprotect((void *)(uintptr_t)img->base, img->vmsize, PROT_NONE);
        else munmap((void *)(uintptr_t)img->base, img->vmsize);
    }
    /* island room no entry was taken from (entries, once made, may be named by any field: kept) */
    if (img->isl_lo && __atomic_load_n(&img->isl_next, __ATOMIC_ACQUIRE) == img->isl_lo) {
        munmap((void *)(uintptr_t)img->isl_lo, img->isl_hi - img->isl_lo);
        img->isl_lo = img->isl_hi = img->isl_next = 0;
    }
    if (img->fd >= 0) close(img->fd);
    img->fd = -1;
    if (listed) return;   /* kept with its arrays: see hao_image_register */
    for (int k = 0; k < img->ndeps; k++) free(img->deps[k]);
    for (int k = 0; k < img->nrpaths; k++) free(img->rpaths[k]);
    for (int k = 0; k < img->nlazy; k++) free(img->lazy_info[k]);
    free((void *)img->exports_trie);
    free(img->fstarts);
    free(img->fnslots);
    free(img->identity_ranges);
    free(img);
}

void hao_unload_image(hao_image *img)
{
    if (img) release_image(img, false);
}

int hao_macho_check_cmds(const uint8_t *cmds, uint32_t sizeofcmds, uint32_t ncmds, int *nsegs, char *why, size_t n)
{
#define CMDERR(...) do { snprintf(why, n, __VA_ARGS__); return -1; } while (0)
    if (ncmds > sizeofcmds / sizeof(struct load_command)) CMDERR("%u load commands cannot fit in %u bytes", ncmds, sizeofcmds);
    const uint8_t *p = cmds;
    uint32_t left = sizeofcmds;
    int nseg = 0, nsig = 0;
    for (uint32_t k = 0; k < ncmds; k++) {
        if (left < sizeof(struct load_command)) CMDERR("load command %u past sizeofcmds", k);
        struct load_command lc;
        memcpy(&lc, p, sizeof lc);
        if (lc.cmdsize < sizeof lc || lc.cmdsize > left || lc.cmdsize % 8)
            CMDERR("load command %u (%#x): bad cmdsize %u", k, lc.cmd, lc.cmdsize);
        uint32_t need = 0, str = 0;
        switch (lc.cmd) {
        case LC_SEGMENT_64: {
            need = sizeof(struct segment_command_64);
            if (lc.cmdsize < need) break;
            const struct segment_command_64 *sg = (const void *)p;
            if (sg->nsects > (lc.cmdsize - need) / sizeof(struct section_64))
                CMDERR("segment %.16s: %u sections do not fit in cmdsize %u", sg->segname, sg->nsects, lc.cmdsize);
            if (++nseg > MAX_SEGS) CMDERR("more than %d segments", MAX_SEGS);
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB:
        case LC_ID_DYLIB:
            need = sizeof(struct dylib_command);
            if (lc.cmdsize >= need) { str = ((const struct dylib_command *)p)->dylib.name.offset; if (!str) str = UINT32_MAX; }
            break;
        case LC_RPATH:
            need = sizeof(struct rpath_command);
            if (lc.cmdsize >= need) { str = ((const struct rpath_command *)p)->path.offset; if (!str) str = UINT32_MAX; }
            break;
        case LC_UUID: need = sizeof(struct uuid_command); break;
        case LC_MAIN: need = sizeof(struct entry_point_command); break;
        case LC_CODE_SIGNATURE:
            /* one signature: every reader (hao_slice_open for libval.c and guest_sig.c, tools/hao_codesig.py)
             * must mean the same one, and a second could put one in front of the check and another behind it */
            if (++nsig > 1) CMDERR("more than one LC_CODE_SIGNATURE");
            need = sizeof(struct linkedit_data_command); break;
        case LC_FUNCTION_STARTS: case LC_DYLD_CHAINED_FIXUPS: case LC_DYLD_EXPORTS_TRIE: case LC_LAZY_LOAD_DYLIB_INFO:
            need = sizeof(struct linkedit_data_command); break;
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: need = sizeof(struct dyld_info_command); break;
        default: break;
        }
        if (lc.cmdsize < need) CMDERR("load command %u (%#x): cmdsize %u < %u", k, lc.cmd, lc.cmdsize, need);
        /* a string must start after the fixed part and end inside the command */
        if (str && (str < need || str >= lc.cmdsize || !memchr(p + str, 0, lc.cmdsize - str)))
            CMDERR("load command %u (%#x): %s offset %u outside the command", k, lc.cmd,
                   lc.cmd == LC_RPATH ? "path" : "dylib name", str == UINT32_MAX ? 0 : str);
        p += lc.cmdsize;
        left -= lc.cmdsize;
    }
    if (nsegs) *nsegs = nseg;
    return 0;
#undef CMDERR
}

static bool is_pagezero(const struct segment_command_64 *sg)
{
    return sg->vmaddr == 0 && sg->initprot == 0 && sg->maxprot == 0 && sg->filesize == 0 &&
           !strncmp(sg->segname, "__PAGEZERO", 16);
}

bool hao_island_reserve;

uint64_t hao_island_room(uint64_t n)
{
    if (n > (64u << 20) / HAO_ISLAND_SLOT) n = (64u << 20) / HAO_ISLAND_SLOT;   /* a corrupt count */
    return ((n + HAO_ISLAND_DYN_SLOTS) * HAO_ISLAND_SLOT + PAGE - 1) & ~(PAGE - 1);
}

/* [a, a+size) within rel32 reach of every byte of [lo, hi) */
static bool island_reach(uint64_t a, uint64_t size, uint64_t lo, uint64_t hi)
{
    const uint64_t reach = 0x7fff0000ull;
    return (a >= hi || hi - a < reach) && (a + size <= lo || a + size - lo < reach);
}

/* The island room of an image mapped at [base, base+vmsize): executable memory (MAP_JIT: x86_64 macOS gives
 * no MAP_FIXED with it, so the room is asked for by address, at the free range at its end that the caller
 * reserved or that follows a fixed image, and checked). Leaves img->isl_* zero when it gets none. */
static void map_island_room(hao_image *img, uint64_t room, bool reserved)
{
    uint64_t at = img->base + img->vmsize;
    if (reserved) munmap((void *)(uintptr_t)at, room);
    void *p = mmap((void *)(uintptr_t)at, room, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return;
    if (!island_reach((uint64_t)(uintptr_t)p, room, img->base, img->base + img->vmsize)) { munmap(p, room); return; }
    img->isl_lo = img->isl_next = (uint64_t)(uintptr_t)p;
    img->isl_hi = img->isl_lo + room;
}

/* Reserve [addr, addr+size) for a non-PIE image without replacing anything
 * already mapped there (MAP_FIXED would silently clobber hao's own memory). */
static void *reserve_fixed(uint64_t addr, uint64_t size)
{
    mach_vm_address_t a = addr;
    if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS || a != addr) return MAP_FAILED;
    if (mprotect((void *)(uintptr_t)a, size, PROT_NONE) != 0) { mach_vm_deallocate(mach_task_self(), a, size); return MAP_FAILED; }
    return (void *)(uintptr_t)a;
}

/* A linkedit blob the image keeps after its slice is closed. */
static uint8_t *blob_copy(const uint8_t *p, uint32_t size)
{
    uint8_t *b = malloc(size ? size : 1);
    if (b) memcpy(b, p, size);
    return b;
}

/* ------------------------------------------------------------- slices */

int hao_slice_test_path;

/* Can nobody but root change the file open on fd? Root owns it, neither its
 * group nor others may write it, it has no ACL (any entry counts, and so
 * does an ACL that cannot be read: the kernel says ENOENT when there is
 * none), and it is on a local volume, where those bits are the kernel's to
 * enforce. This is about the open file, so the directories above its name
 * do not matter: they decide which file a path reaches, and fd already is
 * one. A sealed system volume's files all qualify. (The owner and mode
 * rule is aot_policy.c's hao_aot_stat_ok for a system file; the loader's
 * unit tests link this file without it.) */
static bool file_fixed(int fd, const struct stat *st)
{
    if (hao_slice_test_path) return hao_slice_test_path == 1;
    if (st->st_uid != 0 || (st->st_mode & (S_IWGRP | S_IWOTH))) return false;
    struct statfs fs;
    if (fstatfs(fd, &fs) != 0 || !(fs.f_flags & MNT_LOCAL)) return false;
    errno = 0;
    acl_t a = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    int e = errno;
    if (a) acl_free(a);
    return a == NULL && e == ENOENT;
}

static hao_slice *slice_pending;       /* hao_slice_hand */

void hao_slice_hand(hao_slice *sl)
{
    if (slice_pending && slice_pending != sl) hao_slice_close(slice_pending);
    slice_pending = sl;
}

void hao_slice_close(hao_slice *sl)
{
    if (!sl) return;
    if (slice_pending == sl) slice_pending = NULL;
    if (sl->bytes) munmap((void *)(uintptr_t)sl->bytes, (size_t)((sl->size + PAGE - 1) & ~(PAGE - 1)));
    free(sl);
}

hao_slice *hao_slice_open(int fd, char *why, size_t n)
{
#define SERR(...) do { snprintf(why, n, __VA_ARGS__); goto fail; } while (0)
    hao_slice *sl = calloc(1, sizeof *sl);
    struct stat st;
    if (!sl) { snprintf(why, n, "out of memory"); return NULL; }
    sl->fd = fd;
    if (fstat(fd, &st) != 0) SERR("fstat: %s", strerror(errno));
    if (!S_ISREG(st.st_mode)) SERR("not a regular file");
    sl->dev = (uint64_t)st.st_dev;
    sl->ino = (uint64_t)st.st_ino;
    uint32_t fat_sub = 0;
    if (!hao_macho_choose_slice(fd, (uint64_t)st.st_size, &sl->off, &sl->size, &fat_sub, why, n)) goto fail;
    if (sl->off & (PAGE - 1)) SERR("arm64 slice offset %#llx is not page aligned", (unsigned long long)sl->off);
    struct mach_header_64 mh;
    if (sl->size < sizeof mh) SERR("bad mach header");
    if (sl->size > MAX_SLICE) SERR("arm64 slice of %#llx bytes", (unsigned long long)sl->size);
    size_t len = (size_t)((sl->size + PAGE - 1) & ~(PAGE - 1));
    if (file_fixed(fd, &st)) {
        void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, (off_t)sl->off);
        if (m == MAP_FAILED) SERR("map the slice: %s", strerror(errno));
        sl->bytes = m;
    } else {
        /* read once; what is read is what is checked and what runs */
        uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) SERR("no memory for a copy of the slice (%#llx bytes)", (unsigned long long)sl->size);
        sl->bytes = m;
        sl->copied = true;
        for (uint64_t got = 0; got < sl->size;) {
            uint64_t want = sl->size - got;
            ssize_t r = pread(fd, m + got, want > (1u << 30) ? (1u << 30) : (size_t)want, (off_t)(sl->off + got));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) SERR("short read of the slice at %#llx", (unsigned long long)got);
            got += (uint64_t)r;
        }
        mprotect(m, len, PROT_READ);
    }
    memcpy(&mh, sl->bytes, sizeof mh);
    if (mh.magic != MH_MAGIC_64) SERR("bad mach header");
    if (mh.cputype != CPU_TYPE_ARM64) SERR("arm64 slice holds cputype %#x", mh.cputype);
    /* the fat header is outside every signature: what it says of the slice must be what the slice says */
    if ((fat_sub ^ (uint32_t)mh.cpusubtype) & ~CPU_SUBTYPE_MASK)
        SERR("fat header names cpusubtype %#x for a slice whose header says %#x", fat_sub, (uint32_t)mh.cpusubtype);
    sl->cpusubtype = (uint32_t)mh.cpusubtype;
    sl->filetype = mh.filetype;
    sl->flags = mh.flags;
    if (!in_range(sizeof mh, mh.sizeofcmds, sl->size)) SERR("load commands (%u bytes) overrun the slice", mh.sizeofcmds);
    if (mh.sizeofcmds > MAX_CMDS) SERR("%u bytes of load commands", mh.sizeofcmds);
    sl->cmds = sl->bytes + sizeof mh;
    sl->ncmds = mh.ncmds;
    sl->sizeofcmds = mh.sizeofcmds;
    if (hao_macho_check_cmds(sl->cmds, sl->sizeofcmds, sl->ncmds, &sl->nsegs, why, n) != 0) goto fail;
    const uint8_t *p = sl->cmds;
    for (uint32_t k = 0; k < sl->ncmds; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct linkedit_data_command *ld = (const void *)p;
        if (ld->cmd != LC_CODE_SIGNATURE) continue;       /* at most one, of full size (hao_macho_check_cmds) */
        sl->sig_off = ld->dataoff;
        sl->sig_size = ld->datasize;
    }
    return sl;
fail:
    hao_slice_close(sl);
    return NULL;
#undef SERR
}

static bool arm_only_bundle_fd(int fd)
{
    struct stat st;
    uint32_t magic = 0;
    bool ok = false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || pread(fd, &magic, 4, 0) != 4) goto out;
    if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) {
        struct fat_header head;
        if (pread(fd, &head, sizeof head, 0) != sizeof head) goto out;
        unsigned count = __builtin_bswap32(head.nfat_arch);
        size_t stride = magic == FAT_CIGAM ? sizeof(struct fat_arch) : sizeof(struct fat_arch_64);
        if (!count || count > 64 || sizeof head + count * stride > (uint64_t)st.st_size) goto out;
        for (unsigned k = 0; k < count; k++) {
            uint32_t cpu;
            if (pread(fd, &cpu, 4, sizeof head + k * stride) != 4 ||
                __builtin_bswap32(cpu) == CPU_TYPE_X86_64) goto out;
        }
    }
    uint64_t offset, length;
    uint32_t subtype;
    char why[128];
    struct mach_header_64 mh;
    if (!hao_macho_choose_slice(fd, st.st_size, &offset, &length, &subtype, why, sizeof why) ||
        length < sizeof mh || pread(fd, &mh, sizeof mh, offset) != sizeof mh) goto out;
    ok = mh.magic == MH_MAGIC_64 && mh.cputype == CPU_TYPE_ARM64 && mh.filetype == MH_BUNDLE;
out:
    return ok;
}
bool hao_is_arm_only_bundle(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = arm_only_bundle_fd(fd);
    close(fd);
    return ok;
}

static hao_image *load_image(const char *, int, hao_resolver, hao_mapped_fn, void *, bool, char *, size_t);
hao_image *hao_load_bundle_cb(const char *path, hao_resolver resolve, hao_mapped_fn mapped, void *ctx,
                              char *err, size_t errlen)
{
    return load_image(path, -1, resolve, mapped, ctx, true, err, errlen);
}

hao_image *hao_load_image_fd_cb(const char *path, int fd, hao_resolver resolve, hao_mapped_fn mapped, void *ctx,
                                char *err, size_t errlen)
{
    if (fd < 0) { snprintf(err, errlen, "%s: no open descriptor to load from", path); return NULL; }
    return load_image(path, fd, resolve, mapped, ctx, false, err, errlen);
}

hao_image *hao_load_image(const char *path, hao_resolver resolve, char *err, size_t errlen)
{
    return hao_load_image_cb(path, resolve, NULL, NULL, err, errlen);
}

hao_image *hao_load_image_cb(const char *path, hao_resolver resolve, hao_mapped_fn mapped, void *ctx, char *err,
                             size_t errlen)
{
    return load_image(path, -1, resolve, mapped, ctx, false, err, errlen);
}
/* fd >= 0: the file, already open (the image owns the descriptor from here
 * on, also when the load fails); path then only names it. Otherwise path is
 * opened. */
static hao_image *load_image(const char *path, int fd, hao_resolver resolve, hao_mapped_fn mapped, void *ctx,
                             bool bundle_only, char *err, size_t errlen)
{
    hao_image *img = calloc(1, sizeof *img);
    hao_slice *sl = NULL;
    const uint8_t *chained = NULL, *legacy[4] = {0};
    uint32_t legacy_size[4] = {0};
    bool legacy_seen = false;
    seginfo *segs = NULL;
    bool linked = false, visible = false;
    if (!img) {
        if (fd >= 0) close(fd);
        snprintf(err, errlen, "%s: out of memory", path);
        return NULL;
    }
    snprintf(img->path, sizeof img->path, "%s", path);
    img->fd = fd >= 0 ? fd : open(path, O_RDONLY | O_CLOEXEC);
    if (img->fd < 0) ERR("open %s: %s", path, strerror(errno));
    if (bundle_only && !arm_only_bundle_fd(img->fd)) ERR("%s: native fallback requires an ARM-only MH_BUNDLE", path);
    char why[320];
    /* The slice, taken once (hao/macho.h): the one the caller checked on this descriptor and left for
     * this load (hao_slice_hand), else opened here. From here on nothing is read from the file. */
    struct stat st;
    if (fd >= 0 && slice_pending && slice_pending->fd == fd && fstat(fd, &st) == 0 &&
        slice_pending->dev == (uint64_t)st.st_dev && slice_pending->ino == (uint64_t)st.st_ino) {
        sl = slice_pending;
        slice_pending = NULL;
    } else if (!(sl = hao_slice_open(img->fd, why, sizeof why))) ERR("%s: %s", path, why);
    img->dev = sl->dev;
    img->ino = sl->ino;
    img->slice_off = sl->off;
    img->slice_size = sl->size;
    img->cpusubtype = sl->cpusubtype;
    const uint64_t slice = sl->size;
    const uint8_t *const cmds = sl->cmds;

    if (bundle_only && sl->filetype != MH_BUNDLE) ERR("%s: native fallback requires an ARM-only MH_BUNDLE", path);
    if (sl->filetype != MH_EXECUTE && sl->filetype != MH_DYLIB && sl->filetype != MH_BUNDLE &&
        !(map_only && sl->filetype == MH_DYLINKER))
        ERR("%s: filetype %u not loadable", path, sl->filetype);
    img->is_dylib = sl->filetype != MH_EXECUTE;
    img->is_pie = (sl->flags & MH_PIE) || img->is_dylib;

    /* pass 0 (hao_slice_open): every command's size and in-command offsets are valid; the passes below
     * index commands without further checks. */
    int nseg = sl->nsegs;
    /* library validation (libval.c): on the bytes the image is then built from */
    if (!map_only && hao_libval_image(img, sl, why, sizeof why) != 0) ERR("%s: %s", path, why);
    if (sl->sig.cd) {
        img->cdhash_type = sl->sig.hash_type;
        memcpy(img->cdhash, sl->sig.cdhash, sizeof img->cdhash);
    }
#define FOR_EACH_CMD(p) for (const uint8_t *p = cmds; p; p = NULL) \
        for (uint32_t _k = 0; _k < sl->ncmds; _k++, p += ((const struct load_command *)p)->cmdsize)

    /* pass 1: segment table, extent, sanity of every segment */
    segs = calloc(nseg ? (size_t)nseg : 1, sizeof *segs);
    if (!segs) ERR("%s: out of memory", path);
    uint64_t lo = UINT64_MAX, hi = 0, fstarts_bound = 0;
    int si = 0, hdr_seg = -1;
    bool swift = false;
    FOR_EACH_CMD(p) {
        /* every function start takes at least one byte of LC_FUNCTION_STARTS */
        if (((const struct load_command *)p)->cmd == LC_FUNCTION_STARTS)
            fstarts_bound = ((const struct linkedit_data_command *)p)->datasize;
    }
    FOR_EACH_CMD(p) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)p;
        seginfo *s = &segs[si++];
        s->vmaddr = sg->vmaddr; s->vmsize = sg->vmsize; s->fileoff = sg->fileoff; s->filesize = sg->filesize;
        s->initprot = (uint32_t)sg->initprot;
        if (is_pagezero(sg)) { s->pagezero = true; continue; }
        if (!sg->vmsize) continue;
        if (sg->vmaddr + sg->vmsize < sg->vmaddr || sg->vmsize > MAX_VMSIZE)
            ERR("%s: segment %.16s: vm range wraps", path, sg->segname);
        if (sg->vmaddr & (PAGE - 1)) ERR("%s: segment %.16s: vmaddr %#llx not page aligned", path, sg->segname, (unsigned long long)sg->vmaddr);
        if (sg->filesize > sg->vmsize) ERR("%s: segment %.16s: filesize %#llx > vmsize %#llx", path, sg->segname,
                                           (unsigned long long)sg->filesize, (unsigned long long)sg->vmsize);
        if (sg->filesize) {
            if (!in_range(sg->fileoff, sg->filesize, slice))
                ERR("%s: segment %.16s: file range [%#llx, +%#llx) outside the slice (%#llx bytes)", path, sg->segname,
                    (unsigned long long)sg->fileoff, (unsigned long long)sg->filesize, (unsigned long long)slice);
            if (sg->fileoff & (PAGE - 1)) ERR("%s: segment %.16s: fileoff %#llx not page aligned", path, sg->segname, (unsigned long long)sg->fileoff);
        }
        for (int j = 0; j < si - 1; j++) {
            const seginfo *o = &segs[j];
            if (o->pagezero || !o->vmsize) continue;
            if (sg->vmaddr < o->vmaddr + o->vmsize && o->vmaddr < sg->vmaddr + sg->vmsize)
                ERR("%s: segment %.16s overlaps segment %d", path, sg->segname, j);
        }
        if (sg->nsects && !(sg->initprot & VM_PROT_READ))
            ERR("%s: segment %.16s has sections but is not readable", path, sg->segname);
        const struct section_64 *sec = (const void *)(sg + 1);
        for (uint32_t s2 = 0; s2 < sg->nsects; s2++) {
            if (sec[s2].addr < sg->vmaddr || !in_range(sec[s2].addr - sg->vmaddr, sec[s2].size, sg->vmsize))
                ERR("%s: section %.16s,%.16s outside its segment", path, sec[s2].segname, sec[s2].sectname);
            if (!strncmp(sec[s2].sectname, "__swift5_", 9) && sec[s2].size) swift = true;
        }
        if (sg->vmaddr < lo) lo = sg->vmaddr;
        if (sg->vmaddr + sg->vmsize > hi) hi = sg->vmaddr + sg->vmsize;
    }
    if (lo == UINT64_MAX) ERR("%s: no segments", path);
    /* The image starts with its mach header and load commands, readable: native code and hao read them
     * at img->base (dyld's own rule for the first segment; src/jit.c hashes that range for an AOT cache). */
    for (int k = 0; k < si; k++)
        if (!segs[k].pagezero && segs[k].vmsize && segs[k].vmaddr == lo) hdr_seg = k;
    if (hdr_seg < 0 || segs[hdr_seg].fileoff != 0 || !segs[hdr_seg].filesize)
        ERR("%s: the mach header is not mapped at the start of the image", path);
    if (segs[hdr_seg].filesize < sizeof(struct mach_header_64) + (uint64_t)sl->sizeofcmds)
        ERR("%s: the first segment (%#llx bytes of the file) does not hold the %u bytes of load commands", path,
            (unsigned long long)segs[hdr_seg].filesize, sl->sizeofcmds);
    if (!(segs[hdr_seg].initprot & VM_PROT_READ)) ERR("%s: the segment that holds the mach header is not readable", path);
    if (hi - lo > MAX_VMSIZE) ERR("%s: image spans %#llx bytes", path, (unsigned long long)(hi - lo));
    img->pref_base = lo;
    img->vmsize = hi - lo;
    img->identity_ranges = calloc((size_t)nseg, sizeof *img->identity_ranges);
    if (!img->identity_ranges) ERR("%s: no memory for mapped segment ranges", path);
    uint64_t room = hao_island_reserve && swift && !map_only ? hao_island_room(fstarts_bound) : 0;
    void *region = img->is_pie ? mmap(NULL, img->vmsize + room, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
    if (region == MAP_FAILED && img->is_pie && room) {
        room = 0;
        region = mmap(NULL, img->vmsize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    if (!img->is_pie) region = reserve_fixed(lo, img->vmsize);
    if (region == MAP_FAILED)
        ERR("%s: reserve %#llx bytes%s: %s", path, (unsigned long long)img->vmsize,
            img->is_pie ? "" : " at its fixed (non-PIE) address", img->is_pie ? strerror(errno) : "address range in use");
    img->base = (uint64_t)(uintptr_t)region;
    img->slide = img->base - lo;
    for (int k = 0; k < nseg; k++) {
        if (segs[k].pagezero || !segs[k].vmsize) continue;
        uint32_t q = img->n_identity_ranges++;
        img->identity_ranges[q][0] = segs[k].vmaddr + img->slide;
        img->identity_ranges[q][1] = img->identity_ranges[q][0] + segs[k].vmsize;
    }
    if (room) map_island_room(img, room, img->is_pie);
    /* HAO_CODE_ALIAS: hao reads the code segments from a second mapping of the file (hao/code.h) */
    if (hao_code_alias_wanted() && !hao_code_alias_reserve(img))
        ERR("%s: reserve %#llx bytes for the code alias: %s", path, (unsigned long long)img->vmsize, strerror(errno));

    /* pass 2: map segments, collect commands. Every mapping lies inside the
     * reserved region: segments are within [lo, hi) and page aligned. */
    const uint8_t *chained_lc = NULL;
    uint32_t chained_size = 0;
    uint64_t tlv_lo = UINT64_MAX, tlv_hi = 0, tlv_reg_hi = 0;
    int tlv_seg = -1;
    si = 0;
    FOR_EACH_CMD(p) {
        const struct load_command *lc = (const void *)p;
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *sg = (const void *)p;
            si++;
            if (is_pagezero(sg) || !sg->vmsize) continue;
            uint64_t addr = sg->vmaddr + img->slide;
            uint64_t fpage = (sg->filesize + PAGE - 1) & ~(PAGE - 1);
            if (sl->copied) {
                /* The checked copy's bytes, in memory of the image's own. Whole pages are taken from the
                 * copy without copying them (the kernel shares them until one side writes; the slice is
                 * closed when the load ends), so the image does not cost its size twice while it loads. */
                void *m = mmap((void *)(uintptr_t)addr, sg->vmsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
                if (m == MAP_FAILED) ERR("%s: map %.16s: %s", path, sg->segname, strerror(errno));
                uint64_t whole = sg->filesize & ~(PAGE - 1);
                if (whole && mach_vm_copy(mach_task_self(), (mach_vm_address_t)(uintptr_t)(sl->bytes + sg->fileoff), whole,
                                          (mach_vm_address_t)addr) != KERN_SUCCESS)
                    whole = 0;
                memcpy((uint8_t *)m + whole, sl->bytes + sg->fileoff + whole, sg->filesize - whole);
            } else {
                if (sg->filesize) {
                    void *m = mmap((void *)(uintptr_t)addr, sg->filesize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
                                   img->fd, (off_t)(img->slice_off + sg->fileoff));
                    if (m == MAP_FAILED) ERR("%s: map %.16s: %s", path, sg->segname, strerror(errno));
                }
                if (sg->vmsize > fpage) {
                    void *m = mmap((void *)(uintptr_t)(addr + fpage), sg->vmsize - fpage, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
                    if (m == MAP_FAILED) ERR("%s: zero-fill %.16s: %s", path, sg->segname, strerror(errno));
                }
                if (sg->filesize & (PAGE - 1)) {
                    /* zero the tail of the last file page beyond filesize */
                    uint64_t tail = addr + sg->filesize, end = addr + fpage;
                    if (end > addr + sg->vmsize) end = addr + sg->vmsize;
                    if (end > tail) memset((void *)(uintptr_t)tail, 0, end - tail);
                }
            }
            if (sg->initprot & VM_PROT_EXECUTE) {
                if (img->ncode >= HAO_MAX_RANGES) ERR("%s: more than %d code segments", path, HAO_MAX_RANGES);
                if (!hao_code_alias_segment(img, addr, sg->filesize, sg->vmsize, img->fd, (off_t)(img->slice_off + sg->fileoff),
                                            sl->copied ? sl->bytes + sg->fileoff : NULL))
                    ERR("%s: map %.16s into the code alias: %s", path, sg->segname, strerror(errno));
                img->code_lo[img->ncode] = addr;
                img->code_hi[img->ncode] = addr + sg->vmsize;
                img->ncode++;
            }
            const struct section_64 *sec = (const void *)(sg + 1);
            for (uint32_t s = 0; s < sg->nsects; s++) {
                uint32_t type = sec[s].flags & SECTION_TYPE;
                uint64_t sa = sec[s].addr + img->slide;
                if (img->nsects < 128) {
                    memcpy(img->sects[img->nsects].seg, sec[s].segname, 16);
                    memcpy(img->sects[img->nsects].sect, sec[s].sectname, 16);
                    img->sects[img->nsects].addr = sa;
                    img->sects[img->nsects].size = sec[s].size;
                    img->sects[img->nsects].flags = sec[s].flags;
                    img->nsects++;
                }
                bool writable = sg->initprot & VM_PROT_WRITE;
                switch (type) {
                case S_MOD_INIT_FUNC_POINTERS: img->init_ptrs = sa; img->init_ptrs_count = sec[s].size / 8; break;
                case 0x16 /* S_INIT_FUNC_OFFSETS */: img->init_offs = sa; img->init_offs_count = sec[s].size / 4; break;
                case S_THREAD_LOCAL_VARIABLES:
                    if (!writable) ERR("%s: thread-local descriptors in read-only segment %.16s", path, sg->segname);
                    img->tlv_desc = sa; img->tlv_desc_count = sec[s].size / 24; break;
                case S_THREAD_LOCAL_REGULAR:
                case S_THREAD_LOCAL_ZEROFILL:
                    /* the template is copied as one range: it must not span segments */
                    if (tlv_seg >= 0 && tlv_seg != si) ERR("%s: thread-local template spans segments", path);
                    tlv_seg = si;
                    if (sa < tlv_lo) tlv_lo = sa;
                    if (sa + sec[s].size > tlv_hi) tlv_hi = sa + sec[s].size;
                    if (type == S_THREAD_LOCAL_REGULAR && sa + sec[s].size > tlv_reg_hi) tlv_reg_hi = sa + sec[s].size;
                    break;
                default: break;
                }
            }
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB: {
            const struct dylib_command *dc = (const void *)p;
            if (img->ndeps >= HAO_MAX_DEPS) ERR("%s: more than %d dependencies", path, HAO_MAX_DEPS);
            img->deps[img->ndeps] = strdup((const char *)p + dc->dylib.name.offset);
            img->dep_weak[img->ndeps] = lc->cmd == LC_LOAD_WEAK_DYLIB;
            img->dep_reexport[img->ndeps] = lc->cmd == LC_REEXPORT_DYLIB;
            img->dep_upward[img->ndeps] = lc->cmd == LC_LOAD_UPWARD_DYLIB;
            img->ndeps++;
            break;
        }
        case LC_RPATH: {
            const struct rpath_command *rc = (const void *)p;
            if (img->nrpaths >= HAO_MAX_RPATHS) ERR("%s: more than %d LC_RPATH entries", path, HAO_MAX_RPATHS);
            img->rpaths[img->nrpaths++] = strdup((const char *)p + rc->path.offset);   /* terminated: pass 0 */
            break;
        }
        case LC_LAZY_LOAD_DYLIB_INFO: {
            /* read by the runtime's __dyld_lazy_load on first use (lazy_bind bounds its contents) */
            const struct linkedit_data_command *ld = (const void *)p;
            if (ld->datasize < 24) break;
            if (!in_range(ld->dataoff, ld->datasize, slice)) ERR("%s: lazy-load info outside the slice", path);
            if (ld->datasize > MAX_LAZY_BLOB) ERR("%s: lazy-load info of %u bytes", path, ld->datasize);
            if (img->nlazy >= HAO_MAX_LAZY) ERR("%s: more than %d lazy-load dylibs", path, HAO_MAX_LAZY);
            uint8_t *buf = malloc((size_t)ld->datasize + 1);
            if (!buf) ERR("%s: out of memory", path);
            memcpy(buf, sl->bytes + ld->dataoff, ld->datasize);
            buf[ld->datasize] = 0;
            img->lazy_info[img->nlazy] = buf;
            img->lazy_info_size[img->nlazy] = ld->datasize;
            img->nlazy++;
            break;
        }
        case LC_UUID: {
            const struct uuid_command *u = (const void *)p;
            memcpy(img->uuid, u->uuid, 16);
            img->has_uuid = true;
            break;
        }
        case LC_FUNCTION_STARTS: {
            const struct linkedit_data_command *ld = (const void *)p;
            if (!in_range(ld->dataoff, ld->datasize, slice)) ERR("%s: function starts outside the slice", path);
            if (ld->datasize > MAX_BLOB) ERR("%s: function starts of %u bytes", path, ld->datasize);
            img->func_starts_off = ld->dataoff;
            img->func_starts_size = ld->datasize;
            break;
        }
        case LC_MAIN: {
            const struct entry_point_command *ep = (const void *)p;
            if (img->entry) ERR("%s: more than one LC_MAIN", path);
            if (ep->entryoff >= img->vmsize) ERR("%s: entry offset %#llx outside the image", path, (unsigned long long)ep->entryoff);
            img->entry = img->base + ep->entryoff;
            img->stack_size = ep->stacksize;
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS: case LC_DYLD_EXPORTS_TRIE: {
            const struct linkedit_data_command *ld = (const void *)p;
            if (!in_range(ld->dataoff, ld->datasize, slice))
                ERR("%s: %s [%#x, +%#x) outside the slice", path, lc->cmd == LC_DYLD_CHAINED_FIXUPS ? "chained fixups" : "exports trie",
                    ld->dataoff, ld->datasize);
            if (lc->cmd == LC_DYLD_CHAINED_FIXUPS ? chained_lc != NULL : img->exports_trie != NULL)
                ERR("%s: duplicate %s", path, lc->cmd == LC_DYLD_CHAINED_FIXUPS ? "LC_DYLD_CHAINED_FIXUPS" : "exports trie");
            if (ld->datasize > MAX_BLOB)
                ERR("%s: %s of %u bytes", path, lc->cmd == LC_DYLD_CHAINED_FIXUPS ? "chained fixups" : "exports trie", ld->datasize);
            if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) {
                /* read in place: the slice outlives the fixups */
                chained_lc = chained = sl->bytes + ld->dataoff;
                chained_size = ld->datasize;
            } else if (!(img->exports_trie = blob_copy(sl->bytes + ld->dataoff, ld->datasize))) {
                ERR("%s: out of memory", path);
            } else img->exports_trie_size = ld->datasize;
            break;
        }
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: {
            const struct dyld_info_command *di = (const void *)p;
            if (legacy_seen) ERR("%s: duplicate LC_DYLD_INFO", path);
            legacy_seen = true;
            uint32_t offsets[4] = {di->rebase_off, di->bind_off, di->lazy_bind_off, di->weak_bind_off};
            uint32_t sizes[4] = {di->rebase_size, di->bind_size, di->lazy_bind_size, di->weak_bind_size};
            for (int k = 0; k < 4; k++) {
                if (!in_range(offsets[k], sizes[k], slice)) ERR("%s: legacy dyld stream outside the slice", path);
                if (sizes[k] > MAX_BLOB) ERR("%s: legacy dyld stream of %u bytes", path, sizes[k]);
                if (!sizes[k]) continue;
                legacy[k] = sl->bytes + offsets[k];   /* read in place */
                legacy_size[k] = sizes[k];
            }
            if (!in_range(di->export_off, di->export_size, slice)) ERR("%s: export trie outside the slice", path);
            if (di->export_size > MAX_BLOB) ERR("%s: exports trie of %u bytes", path, di->export_size);
            if (di->export_size) {
                if (img->exports_trie) ERR("%s: duplicate exports trie", path);
                if (!(img->exports_trie = blob_copy(sl->bytes + di->export_off, di->export_size))) ERR("%s: out of memory", path);
                img->exports_trie_size = di->export_size;
            }
            break;
        }
        default: break;
        }
    }

    if (tlv_seg >= 0) {
        img->tlv_init_start = tlv_lo;
        img->tlv_total_size = tlv_hi - tlv_lo;
        img->tlv_init_size = tlv_reg_hi > tlv_lo ? tlv_reg_hi - tlv_lo : 0;
    }

    /* the entry point must be code */
    if (img->entry) {
        bool code = false;
        for (int k = 0; k < img->ncode; k++) code |= img->entry >= img->code_lo[k] && img->entry + 4 <= img->code_hi[k];
        if (!code || (img->entry & 3))
            ERR("%s: entry point +%#llx is not an instruction in a code segment", path, (unsigned long long)(img->entry - img->base));
    }

    /* function starts: exact entry points, for callback thunking */
    if (img->func_starts_size) {
        const uint8_t *fs = sl->bytes + img->func_starts_off;
        size_t cap = 256;
        img->fstarts = malloc(cap * 8);
        const uint8_t *q = fs, *e = fs + img->func_starts_size;
        uint64_t a = img->base, limit = img->base + img->vmsize;
        bool bad = false;
        while (q < e && img->fstarts) {
            uint64_t d = read_uleb(&q, e, &bad);
            if (bad || !d) break;
            if (d >= limit - a) { bad = true; break; }
            a += d;
            if (img->nfstarts == cap) {
                if (cap >= MAX_FSTARTS) { bad = true; break; }
                cap *= 2;
                uint64_t *n = realloc(img->fstarts, cap * 8);
                if (!n) { bad = true; break; }
                img->fstarts = n;
            }
            img->fstarts[img->nfstarts++] = a;
        }
        if (bad || !img->fstarts) ERR("%s: malformed LC_FUNCTION_STARTS", path);
    }
    if (img->entry && !img->nfstarts) {
        img->fstarts = malloc(8);
        if (!img->fstarts) ERR("%s: out of memory", path);
        img->fstarts[0] = img->entry;
        img->nfstarts = 1;
    }

    for (int k = 0; k < 4; k++) {
        if (!legacy[k]) continue;
        if (chained) ERR("%s: both chained and legacy dyld fixups", path);
        if (legacy_stream(img, legacy[k], legacy_size[k], k, segs, nseg, resolve, false, why, sizeof why) < 0)
            ERR("%s: %s", path, why);
    }

    if (img->code_alias) hao_code_alias_publish(img, (int64_t)(img->code_alias - img->base));

    /* link into the image list before binding so self-references resolve,
     * and let the caller record it (a dependency cycle then finds it) */
    hao_image_register(img);
    linked = true;
    if (mapped) { mapped(img, ctx); visible = true; }

    if (!map_only && chained && apply_chained(img, chained, chained_size, resolve, segs, nseg, why, sizeof why) < 0)
        ERR("%s: %s", path, why);

    for (int k = 0; !map_only && k < 4; k++)
        if (legacy[k] && legacy_stream(img, legacy[k], legacy_size[k], k, segs, nseg, resolve, true, why, sizeof why) < 0)
            ERR("%s: %s", path, why);

    /* final protections: never executable for the host */
    FOR_EACH_CMD(p) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)p;
        if (is_pagezero(sg) || !sg->vmsize) continue;
        int prot = 0;
        if (sg->initprot & VM_PROT_READ) prot |= PROT_READ;
        if (sg->initprot & VM_PROT_WRITE) prot |= PROT_WRITE;
        if (sg->initprot & VM_PROT_EXECUTE) prot |= PROT_READ;  /* code is data for the translator */
        /* __DATA_CONST is made read-only by dyld after binding; keep that behaviour */
        mprotect((void *)(uintptr_t)(sg->vmaddr + img->slide), sg->vmsize, prot);
    }
    hao_code_poison(img);   /* HAO_CODE_ALIAS */
#undef FOR_EACH_CMD
    free(segs);
    hao_slice_close(sl);
    /* everything is mapped or copied: the file is not needed again, and an
     * open descriptor would leak into every child the guest spawns */
    close(img->fd);
    img->fd = -1;
    return img;

fail:
    free(segs);
    hao_slice_close(sl);
    if (!linked) img->next = NULL;
    release_image(img, visible);
    return NULL;
}
