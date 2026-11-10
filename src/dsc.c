/*
 * hao — guest images from the arm64e dyld shared cache.
 *
 * 187 libraries of macOS 27.0 exist only in the arm64e shared cache (no
 * x86_64 build, no file on disk); 18 OS programs link one. hao loads such a
 * library as a guest image straight from the cache:
 *
 *  1. The whole arm64e cache (main file and every subcache) is mapped
 *     read-only, copy-on-write, at its unslid address when that range is
 *     free, else anywhere with one slide. Cache code is position independent
 *     across the whole cache (ADRP into other images, uniqued GOTs, stub
 *     islands), so the layout must be kept as is; nothing is copied.
 *  2. The image becomes a hao_image (segments, sections, exports trie and
 *     function starts in the shared __LINKEDIT, dependencies, initializers).
 *  3. Binding. Cache images have no fixups of their own: dyld's cache
 *     builder pre-bound them and the cache's slide info (v5, or v3) lists
 *     every pointer. hao rewrites, through the slide-info chains, every
 *     pointer in the image's own data segments and every GOT slot its code
 *     loads (ADRP+LDR, ADRP+ADD+LDR for the authenticated GOTs) or reaches
 *     through a stub island (BL into __TEXT_STUBS), and every slot outside
 *     the image that its Swift metadata reaches through a relative indirect
 *     reference (swiftmeta.c hao_swiftmeta_slots: mangled names' \x02,
 *     conformances' protocols and resilient witnesses' requirements, ...;
 *     27.0: 6,455 references through 520 uniqued GOT slots in 12 of the 185
 *     arm64e-only images, which native libswiftCore follows). A pointer into a guest
 *     image is slid; a pointer into another cache image is named by that
 *     image's export trie (reverse lookup) and bound to the native x86_64
 *     symbol through the runtime's usual path (interposers, dlsym); anything
 *     else (selector strings, shared constants) is slid and stays arm64e
 *     data. Exports may lie in any of the image's segments (data symbols,
 *     classes), which are recorded as img->seg_lo/hi.
 *  4. Direct branches from the image into other cache images (the cache
 *     builder turns calls into BL when the target is in range) reach the
 *     dispatcher with an arm64e address: hao_dsc_redirect names the target
 *     the same way and returns the native function.
 *  5. Objective-C. The cache builder rewrote the image's selector loads:
 *     objc_msgSend stubs outside the image (adrp x1, sel; add x1, x1, sel;
 *     b _objc_msgSend) and ADRP+ADD pairs in its code build the arm64e
 *     selector string, which is no native SEL. hao_dsc_redirect runs a stub
 *     as "x1 = native selector, then the native objc_msgSend", and the code
 *     pairs become ADRP+LDR of the image's own selector reference again
 *     (which objc.c registers), or of a slot hao allocates in a hole of the
 *     cache region when the reference is out of ADRP range. objc.c rebuilds
 *     the class and protocol metadata (direct method lists, canonical
 *     protocols, lists of lists) before the native runtime sees it.
 *     ADRP+ADD into another, native-backed image's data (GOT-load
 *     elimination) gets a slot with the native address the same way; 27.0's
 *     185 arm64e-only images have none (measured), so that path is counted
 *     (-v) rather than exercised.
 *
 * The cache is read as untrusted input like any guest file (it can come from
 * HAO_GUEST_ROOT): an image's load commands pass the loader's validator
 * (hao_macho_check_cmds) inside one mapping, and its segments, sections,
 * exports trie and function starts must lie in the cache's mappings before
 * hao reads, scans or rewrites them.
 */
#include "hao/runtime.h"
#include "near_map.h"
#include <fcntl.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <objc/runtime.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_TEXT_STUBS 0x8   /* dyld_cache_mapping_and_slide_info flags */

typedef struct {
    uint64_t addr, size, fileoff, flags;
    uint8_t *slide;          /* slide info (malloc'd), or NULL */
    uint32_t slide_size;
} dmap;

typedef struct { uint64_t addr; const char *name; } rev_entry;

typedef struct {
    uint64_t addr;           /* unslid mach header */
    char *path;
    hao_image *img;          /* loaded as a guest image */
    rev_entry *rev;          /* sorted exports, built on first use */
    size_t nrev;
    bool rev_built;
    int8_t hdr_ok;           /* load commands validated: 0 not yet, 1 good, -1 bad */
} dimg;

typedef struct { uint64_t lo, hi; uint32_t img; } drange;
typedef struct { uint64_t pc, native, x1; } redir_t;

static struct {
    int state;               /* 0 untried, 1 mapped, -1 unavailable */
    uint64_t start, size;
    int64_t slide;
    int64_t code_delta;      /* HAO_CODE_ALIAS: the alias of every mapping, this far from the cache (hao/code.h) */
    dmap maps[512];
    int nmaps;
    dimg *images;
    uint32_t nimages;
    drange *ranges;
    size_t nranges;
    int nloaded;
    hao_dsc_binder bind;
    uint64_t *fixed;         /* open-addressing set of rewritten slots (unslid) */
    size_t nfixed, capfixed;
    redir_t *redir;          /* open addressing on pc (0 empty) */
    size_t nredir, capredir;
    uint64_t selbase;        /* unslid start of the cache's selector buffer, or 0 */
    uint64_t n_bound, n_slid, n_unnamed, n_unbound, n_selcode, n_xdata, n_xleft;
    uint64_t n_selsites, n_selfar, n_selmulti, n_selleft;   /* selectors built in code (scan_code) */
} D;
static pthread_mutex_t dsc_lock = PTHREAD_MUTEX_INITIALIZER;
/* fork child: see the lock comment in jit.c */
void hao_dsc_fork_child(void) { dsc_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void *at(uint64_t unslid) { return (void *)(uintptr_t)(unslid + (uint64_t)D.slide); }
/* Code bytes at unslid (hao/code.h: hao reads guest code only through these): the alias when there is one */
static const void *code_at(uint64_t unslid) { return (const void *)(uintptr_t)(unslid + (uint64_t)D.slide + (uint64_t)D.code_delta); }
static uint32_t code32(uint64_t unslid) { uint32_t v; memcpy(&v, code_at(unslid), 4); return v; }

static uint64_t uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t r = 0;
    int sh = 0;
    while (*p < end && sh < 64) {
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    return r;
}

/* ------------------------------------------------------------ mapping */

static bool add_file(const char *path, bool main_file)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t h[0x4000];
    ssize_t n = pread(fd, h, sizeof h, 0);
    if (n < 0x200 || memcmp(h, "dyld_v1  arm64e", 15) != 0) { close(fd); return false; }
    uint32_t moff = u32(h + 0x10), mcount = u32(h + 0x14), soff = u32(h + 0x138), scount = u32(h + 0x13c);
    bool with_slide = soff && scount;
    uint32_t cnt = with_slide ? scount : mcount, esz = with_slide ? 56 : 32, off = with_slide ? soff : moff;
    if ((uint64_t)off + (uint64_t)cnt * esz > (uint64_t)n) { close(fd); return false; }
    if (main_file) {
        D.start = u64(h + 0xe0);
        D.size = u64(h + 0xe8);
        void *want = (void *)(uintptr_t)D.start;
        void *r = mmap(want, D.size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) { close(fd); return false; }
        D.slide = (int64_t)((uint64_t)(uintptr_t)r - D.start);
        if (hao_code_alias_wanted()) {
            void *a = mmap(NULL, D.size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (a == MAP_FAILED) { munmap(r, D.size); close(fd); return false; }
            D.code_delta = (int64_t)((uint64_t)(uintptr_t)a - (uint64_t)(uintptr_t)r);
        }
    }
    for (uint32_t k = 0; k < cnt && D.nmaps < 512; k++) {
        const uint8_t *e = h + off + (size_t)k * esz;
        dmap *m = &D.maps[D.nmaps];
        memset(m, 0, sizeof *m);
        m->addr = u64(e);
        m->size = u64(e + 8);
        m->fileoff = u64(e + 16);
        if (m->addr < D.start || m->addr + m->size > D.start + D.size) continue;
        if (with_slide) {
            uint64_t sio = u64(e + 24), sis = u64(e + 32);
            m->flags = u64(e + 40);
            if (sis && sis < (64u << 20)) {
                m->slide = hao_alloc_check(malloc(sis), "shared cache tables");
                if (pread(fd, m->slide, sis, (off_t)sio) != (ssize_t)sis) { free(m->slide); m->slide = NULL; }
                else m->slide_size = (uint32_t)sis;
            }
        }
        if (mmap(at(m->addr), m->size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, (off_t)m->fileoff) == MAP_FAILED) continue;
        if (D.code_delta && mmap((void *)(uintptr_t)code_at(m->addr), m->size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, (off_t)m->fileoff) == MAP_FAILED)
            continue;
        D.nmaps++;
    }
    if (main_file) {
        /* subcaches: v2 entries {uuid, vm offset, suffix[32]} when the header has cacheSubType */
        uint32_t sco = u32(h + 0x188), scn = u32(h + 0x18c);
        bool v2 = moff > 0x1c8;
        char sub[1200];
        for (uint32_t k = 0; k < scn; k++) {
            uint64_t eo = (uint64_t)sco + (uint64_t)k * (v2 ? 56 : 24);
            char suffix[33] = "";
            if (v2) {
                if (pread(fd, suffix, 32, (off_t)(eo + 24)) != 32) break;
                suffix[32] = 0;
            } else
                snprintf(suffix, sizeof suffix, ".%u", k + 1);
            snprintf(sub, sizeof sub, "%s%s", path, suffix);
            add_file(sub, false);
        }
        /* images: {address, modTime, inode, pathFileOffset, pad} */
        uint32_t io = u32(h + 0x1c0), ic = u32(h + 0x1c4);
        if (!ic) { io = u32(h + 0x18); ic = u32(h + 0x1c); }
        if (ic > 100000) ic = 0;
        D.images = hao_alloc_check(calloc(ic ? ic : 1, sizeof *D.images), "shared cache tables");
        uint8_t *ii = hao_alloc_check(malloc((size_t)ic * 32 + 1), "shared cache tables");
        if (pread(fd, ii, (size_t)ic * 32, io) == (ssize_t)ic * 32) {
            for (uint32_t k = 0; k < ic; k++) {
                char p[1024];
                ssize_t pn = pread(fd, p, sizeof p - 1, u32(ii + 32 * k + 24));
                if (pn <= 0) continue;
                p[pn] = 0;
                D.images[D.nimages].addr = u64(ii + 32 * k);
                D.images[D.nimages].path = hao_alloc_check(strdup(p), "shared cache tables");
                D.nimages++;
            }
        }
        free(ii);
    }
    close(fd);
    return true;
}

static const dmap *map_of(uint64_t a)
{
    for (int k = 0; k < D.nmaps; k++)
        if (a >= D.maps[k].addr && a - D.maps[k].addr < D.maps[k].size) return &D.maps[k];
    return NULL;
}

/* [a, a+n) lies in the cache's mappings (consecutive mappings may share it) */
static bool mapped_span(uint64_t a, uint64_t n)
{
    if (a + n < a) return false;
    while (n) {
        const dmap *m = map_of(a);
        if (!m) return false;
        uint64_t left = m->addr + m->size - a;
        if (left >= n) return true;
        a += left;
        n -= left;
    }
    return true;
}

static const struct mach_header_64 *image_header(uint32_t idx)
{
    dimg *im = &D.images[idx];
    if (im->hdr_ok < 0) return NULL;
    if (!mapped_span(im->addr, sizeof(struct mach_header_64))) { im->hdr_ok = -1; return NULL; }
    const struct mach_header_64 *mh = at(im->addr);
    if (im->hdr_ok > 0) return mh;
    char why[160];
    if (mh->magic != MH_MAGIC_64 || mh->sizeofcmds > 0x100000 || !mapped_span(im->addr + sizeof *mh, mh->sizeofcmds) ||
        hao_macho_check_cmds((const uint8_t *)(mh + 1), mh->sizeofcmds, mh->ncmds, NULL, why, sizeof why) != 0) {
        if (hao_verbose && mh->magic == MH_MAGIC_64) fprintf(stderr, "hao: dsc: %s: %s\n", im->path, why);
        im->hdr_ok = -1;
        return NULL;
    }
    im->hdr_ok = 1;
    return mh;
}

#define FOR_EACH_CMD(mh, lc)                                                                                         \
    for (const struct load_command *lc = (const void *)((mh) + 1), *lc##_end = (const void *)((const uint8_t *)((mh) + 1) + (mh)->sizeofcmds); \
         lc < lc##_end && lc->cmdsize >= 8 && (const uint8_t *)lc + lc->cmdsize <= (const uint8_t *)lc##_end; \
         lc = (const void *)((const uint8_t *)lc + lc->cmdsize))

static int range_cmp(const void *a, const void *b)
{
    const drange *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static void build_ranges(void)
{
    size_t cap = 4096;
    D.ranges = hao_alloc_check(malloc(cap * sizeof *D.ranges), "shared cache tables");
    for (uint32_t k = 0; k < D.nimages; k++) {
        if (k && D.images[k].addr == D.images[k - 1].addr) continue;  /* alias entry */
        const struct mach_header_64 *mh = image_header(k);
        if (!mh) continue;
        FOR_EACH_CMD(mh, lc) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *sg = (const void *)lc;
            if (!sg->vmsize || !strncmp(sg->segname, "__LINKEDIT", 16)) continue;
            if (D.nranges == cap) { cap *= 2; D.ranges = hao_alloc_check(realloc(D.ranges, cap * sizeof *D.ranges), "shared cache tables"); }
            D.ranges[D.nranges++] = (drange){sg->vmaddr, sg->vmaddr + sg->vmsize, k};
        }
    }
    qsort(D.ranges, D.nranges, sizeof *D.ranges, range_cmp);
}

/* The cache's ObjC optimisation header (dyld_cache_header.objcOptsOffset at
 * 0x1d0, its size at 0x1d8; dyld's ObjCOptimizationHeader): {version,
 * flags, headerInfoRO, headerInfoRW, selector, class and protocol hash
 * tables, relativeMethodSelectorBaseAddressOffset, ...}, offsets from the
 * cache's start. Direct relative method lists (method_list_t flag
 * 0x40000000; 0x20000000 for their types) hold uint32 offsets from that
 * selector base instead of selector references. */
static void read_objc_opts(void)
{
    const uint8_t *h = at(D.start);
    if (u32(h + 0x10) < 0x1e0) return;   /* the header ends before objcOptsOffset */
    uint64_t off = u64(h + 0x1d0), size = u64(h + 0x1d8);
    if (!off || size < 0x38 || off >= D.size || !mapped_span(D.start + off, 0x38)) return;
    const uint8_t *o = at(D.start + off);
    uint64_t rel = u64(o + 0x30);
    if (u32(o) < 2 || !rel || rel >= D.size || !mapped_span(D.start + rel, 1)) return;
    D.selbase = D.start + rel;
    if (hao_verbose > 1) fprintf(stderr, "hao: dsc: objc opts v%u, selector base %#llx\n", u32(o), (unsigned long long)D.selbase);
}

/* The cache's selector strings (the cache builder's uniqued selectors, which
 * are the arm64e SELs): the segment that holds the selector base (27.0:
 * libobjc's __OBJC_RO, the cache's read-only ObjC optimisation data), or the
 * stretch of its mapping no image's segment covers. objc_msgSend maps one
 * that reaches it (hao_dsc_sel_map); hao_dsc_sel_lo/span are slid. */
uint64_t hao_dsc_sel_lo, hao_dsc_sel_span, hao_dsc_sels_mapped;

static void sel_range(void)
{
    const dmap *m = D.selbase ? map_of(D.selbase) : NULL;
    if (!m) return;
    uint64_t lo = m->addr, hi = m->addr + m->size;
    int owner = -1;
    for (size_t k = 0; k < D.nranges; k++) {
        const drange *r = &D.ranges[k];
        if (r->lo <= D.selbase && D.selbase < r->hi) {
            lo = r->lo > m->addr ? r->lo : m->addr;
            hi = r->hi < m->addr + m->size ? r->hi : m->addr + m->size;
            owner = (int)r->img;
            break;
        }
        if (r->hi <= D.selbase && r->hi > lo) lo = r->hi;
        if (r->lo > D.selbase && r->lo < hi) hi = r->lo;
    }
    hao_dsc_sel_lo = lo + (uint64_t)D.slide;
    hao_dsc_sel_span = hi - lo;
    if (hao_verbose > 1)
        fprintf(stderr, "hao: dsc: selector strings %#llx-%#llx (%s)\n", (unsigned long long)lo, (unsigned long long)hi,
                owner >= 0 ? D.images[owner].path : "no image");
}

uint64_t hao_dsc_sel_map(uint64_t x1)
{
    uint64_t left = hao_dsc_sel_span - (x1 - hao_dsc_sel_lo);
    const char *str = (const char *)(uintptr_t)x1;
    if (x1 - hao_dsc_sel_lo >= hao_dsc_sel_span || strnlen(str, left < 4096 ? left : 4096) == (left < 4096 ? left : 4096)) return x1;
    __atomic_fetch_add(&hao_dsc_sels_mapped, 1, __ATOMIC_RELAXED);
    if (hao_opt.bridge_check) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "dsc: objc_msgSend with the arm64e cache's selector string %s at %#llx: mapped to the native selector\n",
                str, (unsigned long long)x1);
    }
    return (uint64_t)(uintptr_t)sel_registerName(str);
}

static bool dsc_open(void)
{
    if (D.state) return D.state > 0;
    D.state = -1;
    /* the OS cryptex as grafted at boot, its Preboot location, then the System volume */
    static const char *rel[] = {"/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e",
                                "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e",
                                "/System/Library/dyld/dyld_shared_cache_arm64e"};
    for (unsigned k = 0; k < sizeof rel / sizeof *rel; k++) {
        char p[1200];
        snprintf(p, sizeof p, "%s%s", hao_opt.guest_root, rel[k]);
        if (access(p, R_OK) != 0) continue;
        if (!add_file(p, true)) continue;
        build_ranges();
        read_objc_opts();
        sel_range();
        D.state = 1;
        if (hao_verbose)
            fprintf(stderr, "hao: arm64e shared cache %s: %u images, %d mappings at %#llx (slide %#llx)\n", p, D.nimages,
                    D.nmaps, (unsigned long long)(D.start + (uint64_t)D.slide), (unsigned long long)D.slide);
        return true;
    }
    return false;
}

static int find_image(const char *path)
{
    for (uint32_t k = 0; k < D.nimages; k++)
        if (!strcmp(D.images[k].path, path)) {
            /* alias entries share the address of the real image */
            uint32_t j = k;
            while (j > 0 && D.images[j - 1].addr == D.images[k].addr) j--;
            return (int)j;
        }
    return -1;
}

static int owner_of(uint64_t a)
{
    size_t lo = 0, hi = D.nranges;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (D.ranges[mid].lo <= a) lo = mid + 1; else hi = mid;
    }
    if (lo && a < D.ranges[lo - 1].hi) return (int)D.ranges[lo - 1].img;
    return -1;
}

/* ------------------------------------------------------- export names */

static const uint8_t *linkedit_ptr(const struct mach_header_64 *mh, uint32_t dataoff, uint32_t size)
{
    FOR_EACH_CMD(mh, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (strncmp(sg->segname, "__LINKEDIT", 16)) continue;
        if (dataoff < sg->fileoff || (uint64_t)dataoff + size > sg->fileoff + sg->filesize) return NULL;
        uint64_t va = sg->vmaddr + (dataoff - sg->fileoff);
        return mapped_span(va, size ? size : 1) ? at(va) : NULL;
    }
    return NULL;
}

static const uint8_t *exports_trie(const struct mach_header_64 *mh, uint32_t *size)
{
    FOR_EACH_CMD(mh, lc) {
        if (lc->cmd == LC_DYLD_EXPORTS_TRIE) {
            const struct linkedit_data_command *ld = (const void *)lc;
            *size = ld->datasize;
            return linkedit_ptr(mh, ld->dataoff, ld->datasize);
        }
        if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) {
            const struct dyld_info_command *di = (const void *)lc;
            *size = di->export_size;
            return di->export_size ? linkedit_ptr(mh, di->export_off, di->export_size) : NULL;
        }
    }
    return NULL;
}

typedef struct { rev_entry *v; size_t n, cap, visits; uint64_t base; const uint8_t *start, *end; char name[4096]; } rev_ctx;

static void rev_walk(rev_ctx *x, const uint8_t *node, size_t len, int depth)
{
    /* children may precede their parent (the cache builder's layout); the
     * depth and visit budget bound a malformed trie */
    if (node >= x->end || depth > 128 || ++x->visits > 4000000) return;
    const uint8_t *q = node;
    uint64_t term = uleb(&q, x->end);
    if (term > (uint64_t)(x->end - q)) return;
    const uint8_t *children = q + term;
    if (term && q < x->end) {
        uint64_t flags = uleb(&q, x->end);
        if (!(flags & EXPORT_SYMBOL_FLAGS_REEXPORT)) {
            uint64_t off = uleb(&q, x->end);
            uint64_t a = (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE ? off : x->base + off;
            if (x->n == x->cap) { x->cap = x->cap ? 2 * x->cap : 256; x->v = hao_alloc_check(realloc(x->v, x->cap * sizeof *x->v), "shared cache tables"); }
            x->v[x->n].addr = a;
            x->v[x->n].name = hao_alloc_check(strndup(x->name, len), "shared cache tables");
            x->n++;
        }
    }
    if (children >= x->end) return;
    uint8_t nchild = *children++;
    const uint8_t *e = children;
    for (uint8_t k = 0; k < nchild && e < x->end; k++) {
        size_t el = strnlen((const char *)e, (size_t)(x->end - e));
        if (len + el >= sizeof x->name) return;
        memcpy(x->name + len, e, el);
        e += el + 1;
        uint64_t child = uleb(&e, x->end);
        if (child < (uint64_t)(x->end - x->start) && x->start + child != node) rev_walk(x, x->start + child, len + el, depth + 1);
    }
}

static int rev_cmp(const void *a, const void *b)
{
    const rev_entry *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    /* the plainest alias first: no '$' variant, then the shortest name */
    bool dx = strchr(x->name, '$') != NULL, dy = strchr(y->name, '$') != NULL;
    if (dx != dy) return dx ? 1 : -1;
    size_t lx = strlen(x->name), ly = strlen(y->name);
    return lx < ly ? -1 : lx > ly;
}

/* The exported name of unslid address `a` in cache image idx, or NULL. */
static const char *rev_name(uint32_t idx, uint64_t a)
{
    dimg *im = &D.images[idx];
    if (!im->rev_built) {
        im->rev_built = true;
        const struct mach_header_64 *mh = image_header(idx);
        uint32_t size = 0;
        const uint8_t *t = mh ? exports_trie(mh, &size) : NULL;
        if (t && size) {
            rev_ctx *x = hao_alloc_check(calloc(1, sizeof *x), "shared cache tables");
            x->base = im->addr;
            x->start = t;
            x->end = t + size;
            rev_walk(x, t, 0, 0);
            qsort(x->v, x->n, sizeof *x->v, rev_cmp);
            im->rev = x->v;
            im->nrev = x->n;
            free(x);
        }
        if (hao_verbose > 1) fprintf(stderr, "hao: dsc: %s: %zu exports\n", im->path, im->nrev);
    }
    size_t lo = 0, hi = im->nrev;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (im->rev[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return lo < im->nrev && im->rev[lo].addr == a ? im->rev[lo].name : NULL;
}

/* An address inside a vtable another cache image exports: the vtable's
 * name and the offset (*off). A class's vtable is laid out by the Itanium
 * C++ ABI (2.5: offset to top, RTTI pointer, then one pointer per virtual
 * function, every entry pointer-sized) from the same declarations in both
 * slices, so the offset names the same entry in the native vtable. Typeinfo
 * objects point at a vtable's address point (+16), not at its start. The
 * offset must lie before the next export. Any other interior address has no
 * name: NULL. */
static const char *rev_vtable_interior(uint32_t idx, uint64_t a, uint64_t *off)
{
    (void)rev_name(idx, a);   /* builds the sorted exports */
    const dimg *im = &D.images[idx];
    size_t lo = 0, hi = im->nrev;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (im->rev[mid].addr <= a) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const rev_entry *e = &im->rev[lo - 1];
    size_t nx = lo;
    while (nx < im->nrev && im->rev[nx].addr == e->addr) nx++;
    uint64_t lim = nx < im->nrev ? im->rev[nx].addr : ~0ull;
    /* the plainest alias sorts first among equal addresses */
    size_t first = lo - 1;
    while (first > 0 && im->rev[first - 1].addr == e->addr) first--;
    e = &im->rev[first];
    if (strncmp(e->name, "__ZTV", 5) || a >= lim || a - e->addr > 0x10000 || ((a - e->addr) & 7)) return NULL;
    *off = a - e->addr;
    return e->name;
}

/* ----------------------------------------------------------- binding */

/* What an unslid cache address becomes in this process. */
static uint64_t resolve_target(uint64_t target)
{
    int o = owner_of(target);
    if (o < 0 || D.images[o].img) { D.n_slid++; return target + (uint64_t)D.slide; }
    const char *name = rev_name((uint32_t)o, target);
    uint64_t off = 0;
    if (!name) name = rev_vtable_interior((uint32_t)o, target, &off);
    if (!name) {
        D.n_unnamed++;
        if (hao_verbose > 1) fprintf(stderr, "hao: dsc: %#llx in %s has no exported name\n", (unsigned long long)target, D.images[o].path);
        return target + (uint64_t)D.slide;
    }
    bool ok = true;
    uint64_t v = D.bind(D.images[o].path, name, &ok);
    if (!ok) {
        D.n_unbound++;
        if (hao_verbose > 1) fprintf(stderr, "hao: dsc: %s from %s not bound\n", name, D.images[o].path);
        return target + (uint64_t)D.slide;
    }
    D.n_bound++;
    return v + off;
}

static bool fixed_has(uint64_t a)
{
    if (!D.capfixed) return false;
    for (size_t k = (a >> 3) & (D.capfixed - 1);; k = (k + 1) & (D.capfixed - 1)) {
        if (D.fixed[k] == a) return true;
        if (!D.fixed[k]) return false;
    }
}

static void fixed_add(uint64_t a)
{
    if (2 * (D.nfixed + 1) > D.capfixed) {
        size_t oc = D.capfixed;
        uint64_t *old = D.fixed;
        D.capfixed = oc ? 2 * oc : 4096;
        D.fixed = hao_alloc_check(calloc(D.capfixed, 8), "shared cache tables");
        D.nfixed = 0;
        for (size_t k = 0; k < oc; k++) if (old[k]) fixed_add(old[k]);
        free(old);
    }
    size_t k = (a >> 3) & (D.capfixed - 1);
    while (D.fixed[k] && D.fixed[k] != a) k = (k + 1) & (D.capfixed - 1);
    if (!D.fixed[k]) { D.fixed[k] = a; D.nfixed++; }
}

/* Decode one slide-info pointer: the unslid target and the top byte. */
static bool decode_ptr(const dmap *m, uint64_t raw, uint64_t *target, uint64_t *high8)
{
    uint32_t ver = u32(m->slide);
    uint64_t add = u64(m->slide + 16);
    if (ver == 5) {
        *target = add + (raw & ((1ull << 34) - 1));
        *high8 = raw >> 63 ? 0 : (raw >> 34) & 0xff;
        return true;
    }
    if (ver == 3) {
        if (raw >> 63) { *target = add + (raw & 0xffffffffull); *high8 = 0; }
        else { *target = (uint64_t)(((int64_t)raw << 13) >> 13) & 0x7ffffffffffull; *high8 = (raw >> 43) & 0xff; }
        return true;
    }
    return false;
}

typedef void (*loc_fn)(const dmap *m, uint64_t loc, uint64_t raw, void *ctx);

/* The pointer chain of one slide-info page, read once before hao rewrites
 * any of its slots (a rewritten slot no longer holds its chain link). */
typedef struct { uint64_t page; uint32_t n; struct { uint64_t loc, raw; } *e; } chain_t;
static chain_t *chains;
static size_t nchains, capchains;

static chain_t *page_chain(const dmap *m, uint64_t page)
{
    uint32_t ver = u32(m->slide), psize = u32(m->slide + 4), count = u32(m->slide + 8);
    uint64_t base = m->addr + page * psize;
    if (capchains) {
        for (size_t k = (base >> 14) & (capchains - 1);; k = (k + 1) & (capchains - 1)) {
            if (chains[k].page == base) return &chains[k];
            if (!chains[k].page) break;
        }
    }
    if (2 * (nchains + 1) > capchains) {
        size_t oc = capchains;
        chain_t *old = chains;
        capchains = oc ? 2 * oc : 1024;
        chains = hao_alloc_check(calloc(capchains, sizeof *chains), "shared cache tables");
        for (size_t k = 0; k < oc; k++) {
            if (!old[k].page) continue;
            size_t j = (old[k].page >> 14) & (capchains - 1);
            while (chains[j].page) j = (j + 1) & (capchains - 1);
            chains[j] = old[k];
        }
        free(old);
    }
    size_t j = (base >> 14) & (capchains - 1);
    while (chains[j].page) j = (j + 1) & (capchains - 1);
    chain_t *c = &chains[j];
    c->page = base;
    nchains++;
    uint16_t start;
    memcpy(&start, m->slide + 24 + 2 * page, 2);
    if (start == 0xffff) return c;
    uint32_t cap = 64;
    c->e = hao_alloc_check(malloc(cap * sizeof *c->e), "shared cache tables");
    for (uint64_t off = start; off + 8 <= psize && c->n < 4096 && base + off + 8 <= m->addr + m->size;) {
        uint64_t raw = *(const uint64_t *)at(base + off);
        if (c->n == cap) { cap *= 2; c->e = hao_alloc_check(realloc(c->e, cap * sizeof *c->e), "shared cache tables"); }
        c->e[c->n].loc = base + off;
        c->e[c->n].raw = raw;
        c->n++;
        uint64_t next = (raw >> (ver == 5 ? 52 : 51)) & 0x7ff;
        if (!next) break;
        off += next * 8;
    }
    (void)count;
    return c;
}

typedef void (*loc_fn)(const dmap *m, uint64_t loc, uint64_t raw, void *ctx);

/* Every pointer location of one page of a slid mapping (v5/v3 chains). */
static void walk_page(const dmap *m, uint64_t page, loc_fn fn, void *ctx)
{
    if (!m->slide || m->slide_size < 24) return;
    uint32_t ver = u32(m->slide), psize = u32(m->slide + 4), count = u32(m->slide + 8);
    if ((ver != 5 && ver != 3) || !psize || page >= count || 24 + 2 * (uint64_t)count > m->slide_size) return;
    chain_t *c = page_chain(m, page);
    for (uint32_t k = 0; k < c->n; k++) fn(m, c->e[k].loc, c->e[k].raw, ctx);
}

static void make_writable(uint64_t unslid, uint64_t len)
{
    uint64_t lo = (unslid + (uint64_t)D.slide) & ~0xfffull, hi = (unslid + (uint64_t)D.slide + len + 0xfff) & ~0xfffull;
    mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE);
}

static void fix_loc(const dmap *m, uint64_t loc, uint64_t raw)
{
    if (fixed_has(loc)) return;
    uint64_t target, high8;
    if (!decode_ptr(m, raw, &target, &high8)) return;
    uint64_t v = resolve_target(target);
    if (v == target + (uint64_t)D.slide) v |= high8 << 56;
    make_writable(loc, 8);
    *(uint64_t *)at(loc) = v;
    fixed_add(loc);
}

struct seg_ctx { uint64_t lo, hi; };
static void fix_in_range(const dmap *m, uint64_t loc, uint64_t raw, void *c)
{
    struct seg_ctx *s = c;
    if (loc >= s->lo && loc < s->hi) fix_loc(m, loc, raw);
}

struct slot_ctx { uint64_t slot; bool done; };
static void fix_if_slot(const dmap *m, uint64_t loc, uint64_t raw, void *c)
{
    struct slot_ctx *s = c;
    if (loc == s->slot) { fix_loc(m, loc, raw); s->done = true; }
}

/* A GOT slot the image's code loads: rewrite it if slide info lists it. */
static void fix_slot(uint64_t slot)
{
    if (slot & 7 || fixed_has(slot)) return;
    const dmap *m = map_of(slot);
    if (!m || !m->slide) return;
    uint32_t psize = u32(m->slide + 4);
    if (!psize) return;
    struct slot_ctx s = {slot, false};
    walk_page(m, (slot - m->addr) / psize, fix_if_slot, &s);
    if (!s.done && hao_verbose > 1) fprintf(stderr, "hao: dsc: GOT %#llx is not in the slide info\n", (unsigned long long)slot);
}

static bool is_adrp(uint32_t i) { return (i & 0x9f000000) == 0x90000000; }
static uint64_t adrp_target(uint64_t pc, uint32_t i)
{
    int64_t imm = (int64_t)((((i >> 5) & 0x7ffff) << 2) | ((i >> 29) & 3));
    if (imm & (1 << 20)) imm -= 1 << 21;
    return (pc & ~0xfffull) + (uint64_t)(imm << 12);
}

/* Stub island at unslid `pc`: adrp x17,GOT; add x17,x17,#; ldr x16,[x17]; br(aa) x16
 * gives the GOT slot (*slot); adrp x16,T; add x16,x16,#; br x16 gives the target. */
static bool decode_island(uint64_t pc, uint64_t *slot, uint64_t *target)
{
    const dmap *m = map_of(pc);
    if (!m || !map_of(pc + 12)) return false;
    const uint32_t *w = code_at(pc);
    if (!is_adrp(w[0]) || (w[1] & 0xffc00000) != 0x91000000) return false;
    unsigned rd = w[0] & 31;
    if ((w[1] & 31) != rd || ((w[1] >> 5) & 31) != rd) return false;
    uint64_t a = adrp_target(pc, w[0]) + ((w[1] >> 10) & 0xfff);
    if ((w[2] & 0xffc00000) == 0xf9400000 && ((w[2] >> 5) & 31) == rd) {
        *slot = a + ((w[2] >> 10) & 0xfff) * 8;
        *target = 0;
        return true;
    }
    *slot = 0;
    *target = a;
    return true;
}

/* ------------------------------------------------- selectors in code */

/* Selector references of the image being bound: selector string (runtime
 * address) -> its __objc_selrefs slot (runtime). Open addressing, 0 empty. */
typedef struct { uint64_t str, slot; } selent;
typedef struct { selent *v; size_t cap; } selmap;

static void selmap_build(selmap *sm, hao_image *img)
{
    sm->v = NULL;
    sm->cap = 0;
    uint64_t n = 0, a = hao_image_section(img, NULL, "__objc_selrefs", &n);
    if (!a || n < 8) return;
    size_t cap = 16;
    while (cap < n / 8 * 2) cap *= 2;
    sm->v = hao_alloc_check(calloc(cap, sizeof *sm->v), "shared cache tables");
    sm->cap = cap;
    for (uint64_t k = 0; k < n / 8; k++) {
        uint64_t slot = a + 8 * k, str = *(const uint64_t *)(uintptr_t)slot & ((1ull << 56) - 1);
        if (!str) continue;
        size_t h = (str >> 2) & (cap - 1);
        while (sm->v[h].str && sm->v[h].str != str) h = (h + 1) & (cap - 1);
        if (!sm->v[h].str) sm->v[h] = (selent){str, slot};
    }
}

static uint64_t selmap_slot(const selmap *sm, uint64_t str)
{
    if (!sm->cap || !str) return 0;
    for (size_t h = (str >> 2) & (sm->cap - 1);; h = (h + 1) & (sm->cap - 1)) {
        if (sm->v[h].str == str) return sm->v[h].slot;
        if (!sm->v[h].str) return 0;
    }
}

/* Slot pages for code patches: carved from the holes between the cache's
 * mappings (inside the region hao reserved for the cache, so nothing else
 * lives there), near the code that loads them (ADRP reaches +-4 GB). */
static struct { uint64_t page, used; } pool[64];
static int npool;

static bool adrp_reach(uint64_t pc, uint64_t a)
{
    int64_t d = (int64_t)(a >> 12) - (int64_t)(pc >> 12);
    return d >= -(1ll << 20) && d < (1ll << 20);
}

static int addr_cmp(const void *x, const void *y)
{
    const dmap *const *a = x, *const *b = y;
    return (*a)->addr < (*b)->addr ? -1 : (*a)->addr > (*b)->addr;
}

/* n zeroed 8-byte slots (unslid) on one 4 KB page (one ADRP reaches them
 * all), within ADRP range of every pc in [pclo, pchi]; 0 if none. */
static uint64_t pool_alloc(uint64_t pclo, uint64_t pchi, unsigned n)
{
    const uint64_t PG = 0x4000;
    if (!n || n > 0x1000 / 8) return 0;
    int k;
    uint64_t at_ = 0;
    for (k = 0; k < npool; k++) {
        at_ = pool[k].used;
        if ((at_ & 0xfff) + 8ull * n > 0x1000) at_ = (at_ + 0xfff) & ~0xfffull;
        if (at_ + 8ull * n <= PG && adrp_reach(pclo, pool[k].page + at_) && adrp_reach(pchi, pool[k].page + at_)) break;
    }
    if (k == npool) {
        if (npool == (int)(sizeof pool / sizeof *pool)) return 0;
        /* the hole nearest pclo: mappings sorted by address, holes between them */
        const dmap *ms[512];
        for (int j = 0; j < D.nmaps; j++) ms[j] = &D.maps[j];
        qsort(ms, (size_t)D.nmaps, sizeof *ms, addr_cmp);
        uint64_t best = 0, bestd = ~0ull, prev = D.start;
        for (int j = 0; j <= D.nmaps; j++) {
            uint64_t next = j < D.nmaps ? ms[j]->addr : D.start + D.size;
            uint64_t lo = (prev + PG - 1) & ~(PG - 1);
            for (bool moved = true; moved;) {   /* skip pages already taken */
                moved = false;
                for (int q = 0; q < npool; q++)
                    if (pool[q].page == lo) { lo += PG; moved = true; }
            }
            if (next > lo && next - lo >= PG) {
                uint64_t d = pclo > lo ? pclo - lo : lo - pclo;
                if (d < bestd) { bestd = d; best = lo; }
            }
            if (j < D.nmaps && ms[j]->addr + ms[j]->size > prev) prev = ms[j]->addr + ms[j]->size;
        }
        if (!best || !adrp_reach(pclo, best) || !adrp_reach(pclo, best + PG) || !adrp_reach(pchi, best) ||
            !adrp_reach(pchi, best + PG))
            return 0;
        if (mmap(at(best), PG, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) return 0;
        pool[npool].page = best;
        pool[npool].used = 0;
        k = npool++;
        at_ = 0;
    }
    pool[k].used = at_ + 8ull * n;
    return pool[k].page + at_;
}

/* An 8-byte slot (unslid) holding `value`, within ADRP range of `pc`; 0 if none. */
static uint64_t pool_slot(uint64_t pc, uint64_t value)
{
    uint64_t slot = pool_alloc(pc, pc, 1);
    if (slot) *(uint64_t *)at(slot) = value;
    return slot;
}

/* the one writer of guest instructions: the cache mapping and its alias (hao/code.h) */
static void put_insn(uint64_t pc, uint32_t w)
{
    hao_code_write((uint64_t)(uintptr_t)at(pc), D.code_delta, &w, 4);
}

/* adrp xd, page(target) at pc; ldr xt, [xn, #(slot & 0xfff)] (all unslid) */
static uint32_t enc_adrp(uint64_t pc, unsigned xd, uint64_t target)
{
    int64_t d = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    return 0x90000000u | (uint32_t)((d & 3) << 29) | (uint32_t)(((d >> 2) & 0x7ffff) << 5) | xd;
}
static uint32_t enc_ldr(unsigned xt, unsigned xn, uint64_t slot)
{
    return 0xf9400000u | (uint32_t)(((slot & 0xfff) >> 3) << 10) | (xn << 5) | xt;
}

/* ------------------------------------------------- code analysis
 *
 * The cache builder rewrites an image's ADRP+LDR loads of selector
 * references into ADRP+ADD of the uniqued selector string (the arm64e SEL),
 * and does so for every reader of the ADRP: the compiler may have hoisted
 * the ADRP into a callee-saved register far from its loads (AutoUnlockDaemon
 * -[SDAuthenticationTransportIDSService init]: adrp x28 at 0x22f503760, its
 * `init` ADDs 48 and 101 instructions later, calls between). hao turns them
 * back into loads of a native selector. Retargeting an ADRP is only right
 * when hao rewrites every instruction that reads it, and an instruction it
 * turns into a load must be reached by no other value of the register. So,
 * per function (LC_FUNCTION_STARTS bounds), from each ADRP:
 *
 *   forward:  follow the register along every direct branch, and from a
 *             call to its landing pad (the LSDA call-site table of
 *             __unwind_info's index), until it is written or a call
 *             clobbers it (x0-x17, x30); collect every reader. A call
 *             directly followed by a landing pad does not return. The value
 *             live at an indirect branch, used by a callee of this image
 *             (machine-outliner functions read and write the caller's
 *             registers without saving them), or reaching an instruction
 *             hao does not decode makes the ADRP unpatchable. A page left in
 *             an argument or result register at a call or return is no use:
 *             the cache builder retargeted the ADRP, which it does only when
 *             the selector loads are its uses.
 *   webs:     ADRPs that reach a common reader are patched together, and
 *             only if every reader of every one of them is a selector ADD
 *             into the same page;
 *   backward: from each such reader, every path back must end at an ADRP of
 *             the web (not at another write, a clobbering call, a callee
 *             that uses the register, or the function's entry);
 *   hidden entries (an indirect branch, an LSDA hao does not read, code no
 *             edge reaches): a web is patched only if every ADRP of its page
 *             in the function is, and none is live across a call while
 *             unreached code reads the register.
 *
 * A web's ADRPs then point at one page holding a native selector for each of
 * its strings (the image's selector references when they share one page,
 * else slots of the pool) and each ADD becomes an LDR. Readers of other
 * ADRPs keep their old treatment (GOT slots bound, shared-GOT addresses,
 * addresses of native-backed data), now at any distance. Selector ADDs left
 * unpatched are counted and named (-v, HAO_BRIDGE_CHECK); objc_msgSend maps
 * a selector string of the cache that still reaches it (hao_dsc_sel_*). */

enum { CF_NEXT, CF_B, CF_BCOND, CF_BL, CF_BLR, CF_BR, CF_RET, CF_SVC, CF_STOP, CF_UNKNOWN };
enum { LS_NONE, LS_STORE, LS_LOAD };
typedef struct { uint32_t rd, wr; uint8_t cf, ls, base; int64_t off; } a64op;

#define RB(n) ((n) == 31 ? 0u : 1u << (n))

static int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

/* The general registers x0-x30 one instruction reads and writes (bit n),
 * its control flow and, for loads and stores of general registers, the
 * direction and base register. Instructions whose register use is not
 * decoded here read and write every register field (bits 0-4, 5-9, 10-14,
 * 16-20): conservative in both directions. */
static void a64_decode(uint32_t i, a64op *o)
{
    unsigned Rd = i & 31, Rn = (i >> 5) & 31, Ra = (i >> 10) & 31, Rm = (i >> 16) & 31, op0 = (i >> 25) & 15;
    *o = (a64op){0, 0, CF_NEXT, LS_NONE, 31, 0};
    if ((op0 & 0xe) == 0x8) {   /* data processing, immediate */
        switch ((i >> 23) & 7) {
        case 0: case 1: o->wr = RB(Rd); break;                            /* ADR, ADRP */
        case 2: case 3: case 4: o->rd = RB(Rn); o->wr = RB(Rd); break;    /* add/sub, with tags, min/max, logical */
        case 5: o->wr = RB(Rd); if (((i >> 29) & 3) == 3) o->rd = RB(Rd); break;          /* MOVN/MOVZ; MOVK keeps bits */
        case 6: o->rd = RB(Rn) | (((i >> 29) & 3) == 1 ? RB(Rd) : 0); o->wr = RB(Rd); break;   /* SBFM/UBFM; BFM inserts */
        default: o->rd = RB(Rn) | RB(Rm); o->wr = RB(Rd); break;          /* EXTR */
        }
        return;
    }
    if ((op0 & 0xe) == 0xa) {   /* branches, exception generation, system */
        if ((i & 0x7c000000) == 0x14000000) { o->cf = i >> 31 ? CF_BL : CF_B; o->off = sext(i & 0x3ffffff, 26) * 4; return; }
        if ((i & 0xff000000) == 0x54000000) { o->cf = CF_BCOND; o->off = sext((i >> 5) & 0x7ffff, 19) * 4; return; }
        if ((i & 0x7e000000) == 0x34000000) { o->cf = CF_BCOND; o->rd = RB(Rd); o->off = sext((i >> 5) & 0x7ffff, 19) * 4; return; }
        if ((i & 0x7e000000) == 0x36000000) { o->cf = CF_BCOND; o->rd = RB(Rd); o->off = sext((i >> 5) & 0x3fff, 14) * 4; return; }
        if ((i & 0xfe000000) == 0xd6000000) {   /* branch to register: BR, BLR, RET, ERET, BRAA, BLRAA, ... */
            unsigned opc = (i >> 21) & 15;
            o->rd = RB(Rn) | (opc & 8 ? RB(Rd) : 0);
            o->cf = (opc & 7) == 0 ? CF_BR : (opc & 7) == 1 ? CF_BLR : (opc & 7) == 2 ? CF_RET : CF_STOP;
            return;
        }
        if ((i & 0xff000000) == 0xd4000000) { o->cf = (i & 0xffe0001f) == 0xd4000001 ? CF_SVC : CF_STOP; return; }
        if ((i & 0xffc00000) == 0xd5000000) {   /* hints, barriers, MSR/MRS, SYS/SYSL */
            if ((i >> 21) & 1) o->wr = RB(Rd); else o->rd = RB(Rd);
            return;
        }
        o->cf = CF_UNKNOWN;
        goto unknown;
    }
    if ((op0 & 5) == 4) {   /* loads and stores */
        unsigned fam = (i >> 27) & 7, V = (i >> 26) & 1;
        o->base = (uint8_t)Rn;
        if (fam == 7) {   /* load/store register */
            unsigned size = i >> 30, opc = (i >> 22) & 3, op4 = (i >> 10) & 3;
            uint32_t idx = 0;
            bool wb = false;
            if (!((i >> 24) & 1)) {
                if ((i >> 21) & 1) {
                    if (op4 == 2) idx = RB(Rm);   /* register offset */
                    else if (op4 == 0) {          /* atomic memory operations */
                        if (V) goto unknown;
                        o->rd = RB(Rm) | RB(Rn);
                        o->wr = RB(Rd);
                        return;
                    } else {                      /* LDRAA, LDRAB */
                        if (V) goto unknown;
                        o->rd = RB(Rn);
                        o->wr = RB(Rd) | ((i >> 11) & 1 ? RB(Rn) : 0);
                        o->ls = LS_LOAD;
                        return;
                    }
                } else
                    wb = op4 == 1 || op4 == 3;    /* post-, pre-index */
            }
            o->rd = RB(Rn) | idx;
            if (wb) o->wr = RB(Rn);
            if (V) return;   /* SIMD&FP register data */
            if (opc == 0) { o->rd |= RB(Rd); o->ls = LS_STORE; }
            else if (!(size == 3 && opc == 2)) { o->wr |= RB(Rd); o->ls = LS_LOAD; }   /* not PRFM */
            return;
        }
        if (fam == 5) {   /* load/store pair */
            unsigned mode = (i >> 23) & 3;
            o->rd = RB(Rn);
            if (mode == 1 || mode == 3) o->wr = RB(Rn);
            if (V) return;
            if ((i >> 22) & 1) { o->wr |= RB(Rd) | RB(Ra); o->ls = LS_LOAD; }
            else { o->rd |= RB(Rd) | RB(Ra); o->ls = LS_STORE; }
            return;
        }
        if (fam == 3 && !((i >> 24) & 1)) {   /* load register (literal) */
            if (!V && (i >> 30) != 3) o->wr = RB(Rd);
            return;
        }
        if (fam == 1 && V && !(i >> 31)) {    /* Advanced SIMD load/store structures */
            o->rd = RB(Rn);
            if ((i >> 23) & 1) { o->rd |= RB(Rm); o->wr = RB(Rn); }
            return;
        }
        goto unknown;   /* exclusives, ordered, compare-and-swap, tags, RCpc, memory copy */
    }
    if ((op0 & 7) == 5) {   /* data processing, register */
        unsigned op2 = (i >> 21) & 15;
        if (!((i >> 28) & 1)) { o->rd = RB(Rn) | RB(Rm); o->wr = RB(Rd); return; }   /* logical, add/sub */
        if (op2 & 8) { o->rd = RB(Rn) | RB(Rm) | RB(Ra); o->wr = RB(Rd); return; }    /* 3-source */
        switch (op2) {
        case 0: if (((i >> 10) & 63) == 0) { o->rd = RB(Rn) | RB(Rm); o->wr = RB(Rd); return; } break;   /* ADC, SBC */
        case 2: o->rd = RB(Rn) | ((i >> 11) & 1 ? 0 : RB(Rm)); return;                                  /* CCMN, CCMP */
        case 4: o->rd = RB(Rn) | RB(Rm); o->wr = RB(Rd); return;                                         /* CSEL, ... */
        case 6: if (!((i >> 30) & 1)) { o->rd = RB(Rn) | RB(Rm); o->wr = RB(Rd); return; } break;        /* 2-source */
        default: break;
        }
        goto unknown;   /* 1-source (PAC*, AUT*, XPAC* read Rd), RMIF, SETF */
    }
    if ((op0 & 7) == 7) {   /* SIMD and FP: general registers only in conversions and copies */
        unsigned b = (i >> 24) & 31;
        if (b == 0x1e && !((i >> 30) & 1) && (!((i >> 21) & 1) || ((i >> 10) & 63) == 0)) goto unknown;
        if (b == 0x0e && !(i >> 31) && ((i >> 21) & 7) == 0 && !((i >> 15) & 1) && ((i >> 10) & 1)) goto unknown;
        return;
    }
    if ((i >> 16) == 0) { o->cf = CF_STOP; return; }   /* UDF */
unknown:
    o->rd = o->wr = RB(Rd) | RB(Rn) | RB(Ra) | RB(Rm);
}

enum { RK_SEL, RK_GOT, RK_SHARED, RK_XDATA, RK_OTHER };
typedef struct { int32_t adrp, at; uint8_t kind; } reader_t;
enum { AF_BAD = 1, AF_CALL = 2, AF_SEL = 4, AF_NONSEL = 8 };

typedef struct {
    hao_image *img;
    const selmap *sm;
    uint64_t text;               /* unslid mach header */
    uint64_t *fs;                /* function starts, unslid, sorted */
    size_t nfs;
    struct { uint64_t lo, hi; } sec[32];   /* instruction sections, unslid */
    int nsec;
    uint64_t *selpg;             /* pages of the selector strings the image's references name (unslid), open addressing */
    size_t selpgcap;
    const uint8_t *lsda;         /* __unwind_info LSDA index: {function offset, LSDA offset} */
    uint32_t nlsda;
    /* the function being analysed */
    uint64_t lo;
    const uint32_t *w;
    int32_t n;
    bool hidden;
    uint32_t hidden_reads;       /* registers read by code no direct edge reaches */
    /* scratch, grown with the largest function */
    int32_t cap;
    uint32_t *gen, g;
    int32_t *uf, *first, *stack, *lp;   /* lp: the landing pad of a call (-1 none) */
    uint8_t *aflags, *reach, *pad;
    const char **why;
    int32_t *why_at, *ns_at;     /* ns_at: the first reader of an ADRP that is no selector load */
    reader_t *rv;
    size_t nrv, caprv;
    uint64_t *edges;             /* target << 32 | source: direct branches inside the function, sorted */
    size_t nedges, capedges;
    struct { uint64_t key; int8_t v; } *cc;   /* callee checks: T << 5 | r */
    size_t ccap, ccn;
    /* totals for the image */
    uint64_t sites, patched, far, multi, left, got, steps;
    const char *first_why;
    uint64_t first_at, first_adrp;
    char first_sel[64];
} codescan;

#define WALK_CAP (1 << 20)

static int32_t uf_find(codescan *s, int32_t k)
{
    while (s->uf[k] != k) { s->uf[k] = s->uf[s->uf[k]]; k = s->uf[k]; }
    return k;
}

static bool in_sections(const codescan *s, uint64_t a, uint64_t *sec_hi)
{
    for (int k = 0; k < s->nsec; k++)
        if (a >= s->sec[k].lo && a < s->sec[k].hi) { *sec_hi = s->sec[k].hi; return true; }
    return false;
}

/* The function containing unslid a ends at the next function start or its section's end. */
static uint64_t func_end(const codescan *s, uint64_t a, uint64_t sec_hi)
{
    size_t lo = 0, hi = s->nfs;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (s->fs[m] <= a) lo = m + 1; else hi = m; }
    return lo < s->nfs && s->fs[lo] < sec_hi ? s->fs[lo] : sec_hi;
}

/* A callee of this image leaves x_r alone: it never names the register, or
 * the first instruction that does saves it to the stack and it is loaded
 * back from the stack (a callee-saved register the callee uses). A
 * machine-outliner function reads or writes the caller's register without
 * saving it first. */
static bool callee_keeps(codescan *s, uint64_t t, unsigned r)
{
    uint64_t sec_hi;
    if (!in_sections(s, t, &sec_hi)) return true;   /* another image: the procedure call standard holds */
    uint64_t key = t << 5 | r;
    if (s->ccap) {
        for (size_t h = (key * 0x9e3779b97f4a7c15ull >> 40) & (s->ccap - 1);; h = (h + 1) & (s->ccap - 1)) {
            if (s->cc[h].key == key) return s->cc[h].v;
            if (!s->cc[h].key) break;
        }
    }
    uint64_t end = func_end(s, t, sec_hi);
    bool keeps = end - t <= 4ull * 65536, named = false, restored = false;
    for (uint64_t p = t; keeps && p < end && !restored; p += 4) {
        a64op o;
        a64_decode(code32(p), &o);
        uint32_t bit = 1u << r;
        if (!((o.rd | o.wr) & bit)) continue;
        if (!named && !(o.ls == LS_STORE && o.base == 31 && !(o.wr & bit))) keeps = false;   /* not saved first */
        if (named && o.ls == LS_LOAD && o.base == 31 && !(o.rd & bit)) restored = true;
        named = true;
    }
    if (named && !restored) keeps = false;
    if (2 * (s->ccn + 1) > s->ccap) {
        size_t oc = s->ccap;
        __typeof__(s->cc) old = s->cc;
        s->ccap = oc ? 2 * oc : 256;
        s->cc = hao_alloc_check(calloc(s->ccap, sizeof *s->cc), "shared cache tables");
        s->ccn = 0;
        for (size_t k = 0; k < oc; k++)
            if (old[k].key) {
                size_t h = (old[k].key * 0x9e3779b97f4a7c15ull >> 40) & (s->ccap - 1);
                while (s->cc[h].key) h = (h + 1) & (s->ccap - 1);
                s->cc[h] = old[k];
                s->ccn++;
            }
        free(old);
    }
    size_t h = (key * 0x9e3779b97f4a7c15ull >> 40) & (s->ccap - 1);
    while (s->cc[h].key) h = (h + 1) & (s->ccap - 1);
    s->cc[h].key = key;
    s->cc[h].v = keeps;
    s->ccn++;
    return keeps;
}

static void bad(codescan *s, int32_t k, const char *why, int32_t at_)
{
    if (s->aflags[k] & AF_BAD) return;
    s->aflags[k] |= AF_BAD;
    s->why[k] = why;
    s->why_at[k] = at_;
}

/* Reader j of the ADRP at k (register r, page): what it does with the page. */
static void add_reader(codescan *s, int32_t k, int32_t j, unsigned r, uint64_t page)
{
    uint32_t i = s->w[j];
    uint8_t kind = RK_OTHER;
    if ((i & 0xffc00000) == 0x91000000 && ((i >> 5) & 31) == r) {   /* add xn, xr, #imm12 */
        uint64_t t = page + ((i >> 10) & 0xfff);
        int o = owner_of(t);
        if (selmap_slot(s->sm, t + (uint64_t)D.slide)) kind = RK_SEL;
        else if (o < 0) {
            /* a slot in the cache's shared GOTs: its address is loaded
             * through (adrp x17; add x17; ldr x16, [x17]; blraa x16, x17) or
             * passed on */
            kind = RK_SHARED;
            uint32_t l = j + 1 < s->n ? s->w[j + 1] : 0;
            if ((l & 0xffc00000) == 0xf9400000 && ((l >> 5) & 31) == (i & 31)) fix_slot(t + ((l >> 10) & 0xfff) * 8);
            else fix_slot(t);
        } else if (!D.images[o].img) kind = RK_XDATA;
    } else if ((i & 0xffc00000) == 0xf9400000 && ((i >> 5) & 31) == r) {   /* ldr xt, [xr, #imm] */
        kind = RK_GOT;
        fix_slot(page + ((i >> 10) & 0xfff) * 8);
        s->got++;
    }
    if (s->nrv == s->caprv) {
        s->caprv = s->caprv ? 2 * s->caprv : 1024;
        s->rv = hao_alloc_check(realloc(s->rv, s->caprv * sizeof *s->rv), "shared cache tables");
    }
    s->rv[s->nrv++] = (reader_t){k, j, kind};
    if (kind != RK_SEL && !(s->aflags[k] & AF_NONSEL)) s->ns_at[k] = j;
    s->aflags[k] |= kind == RK_SEL ? AF_SEL : AF_NONSEL;
    if (s->first[j] < 0) s->first[j] = k;
    else {
        int32_t a = uf_find(s, k), b = uf_find(s, s->first[j]);
        if (a != b) s->uf[a] = b;
    }
}

/* Every reader of the ADRP at k, along every direct edge of the function. */
static void walk_forward(codescan *s, int32_t k)
{
    uint32_t a = s->w[k], g = ++s->g, steps = 0, bit;
    unsigned r = a & 31;
    uint64_t page = adrp_target(s->lo + 4ull * k, a);
    size_t sp = 0;
    bit = 1u << r;
#define SUCC(t, from)                                                                                  \
    do {                                                                                               \
        int64_t t_ = (t);                                                                              \
        if (t_ >= s->n) { if (s->w[from] >> 26 != 0x25) bad(s, k, "falls off the function", from); } \
        else if (s->gen[t_] != g) { s->gen[t_] = g; s->stack[sp++] = (int32_t)t_; }                   \
    } while (0)
    SUCC(k + 1, k);
    while (sp) {
        int32_t j = s->stack[--sp];
        if (++steps > WALK_CAP) { bad(s, k, "too long a walk", j); break; }
        a64op o;
        a64_decode(s->w[j], &o);
        if (o.rd & bit) add_reader(s, k, j, r, page);
        if (o.wr & bit) continue;   /* written: this value ends here */
        int64_t t = j + o.off / 4;
        switch (o.cf) {
        case CF_NEXT: SUCC(j + 1, j); break;
        case CF_B: case CF_BCOND:
            if (t >= 0 && t < s->n) SUCC(t, j);
            else if (r >= 19 && !callee_keeps(s, s->lo + 4ull * (uint64_t)t, r)) bad(s, k, "used by a callee of this image", j);
            if (o.cf == CF_BCOND) SUCC(j + 1, j);
            break;
        case CF_BL: case CF_BLR:
            if (r <= 17 || r == 30) break;   /* clobbered by the call */
            if (o.cf == CF_BL && !callee_keeps(s, s->lo + 4ull * (uint64_t)t, r)) bad(s, k, "used by a callee of this image", j);
            s->aflags[k] |= AF_CALL;
            if (s->lp[j] >= 0) SUCC(s->lp[j], j);           /* the unwinder restores callee-saved registers */
            if (j + 1 >= s->n || !s->pad[j + 1]) SUCC(j + 1, j);   /* a call followed by a landing pad does not return */
            break;
        case CF_BR: bad(s, k, "live at an indirect branch", j); break;
        case CF_RET: break;
        case CF_SVC: if (r <= 17) bad(s, k, "live at a system call", j); else SUCC(j + 1, j); break;
        case CF_STOP: break;
        default: bad(s, k, "an instruction hao does not decode", j); break;
        }
    }
#undef SUCC
    s->steps += steps;
}

static int edge_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* One predecessor p on a path back to a reader of x_r: -1 another value
 * reaches the reader, 0 an ADRP of the web (the path ends), 1 go on. */
static int back_visit(codescan *s, int32_t p, unsigned r, int32_t root)
{
    a64op o;
    a64_decode(s->w[p], &o);
    if (o.wr & (1u << r))
        return is_adrp(s->w[p]) && (s->w[p] & 31) == r && (s->aflags[p] & AF_SEL) && uf_find(s, p) == root ? 0 : -1;
    if ((o.cf == CF_BL || o.cf == CF_BLR) && (r <= 17 || r == 30)) return -1;   /* clobbered by the call */
    if (o.cf == CF_BL && !callee_keeps(s, s->lo + 4ull * (uint64_t)(p + o.off / 4), r)) return -1;
    return 1;
}

/* Every path back from reader j0 to a write of x_r ends at an ADRP of web root. */
static bool walk_backward(codescan *s, int32_t j0, unsigned r, int32_t root)
{
    uint32_t g = ++s->g, steps = 0;
    size_t sp = 0;
    s->stack[sp++] = j0;
    while (sp) {
        int32_t q = s->stack[--sp];
        if (q == 0) return false;   /* live into the function */
        if (++steps > WALK_CAP) return false;
        a64op po;
        a64_decode(s->w[q - 1], &po);
        bool falls = po.cf != CF_B && po.cf != CF_BR && po.cf != CF_RET && po.cf != CF_STOP &&
                     !((po.cf == CF_BL || po.cf == CF_BLR) && s->pad[q]);
        size_t lo = 0, hi = s->nedges;
        while (lo < hi) { size_t m = (lo + hi) / 2; if ((s->edges[m] >> 32) < (uint64_t)q) lo = m + 1; else hi = m; }
        for (size_t e = lo; falls || (e < s->nedges && (s->edges[e] >> 32) == (uint64_t)q);) {
            int32_t p;
            if (falls) { p = q - 1; falls = false; }
            else p = (int32_t)(s->edges[e++] & 0xffffffff);
            if (s->gen[p] == g) continue;
            s->gen[p] = g;
            int v = back_visit(s, p, r, root);
            if (v < 0) return false;
            if (v > 0) s->stack[sp++] = p;
        }
    }
    return true;
}

static bool grow(codescan *s, int32_t n)
{
    if (n <= s->cap) return true;
    int32_t c = s->cap ? s->cap : 4096;
    while (c < n) c *= 2;
#define G(f) do { void *p_ = realloc(s->f, (size_t)c * sizeof *s->f); if (!p_) return false; s->f = p_; } while (0)
    G(gen); G(uf); G(first); G(stack); G(aflags); G(reach); G(why); G(why_at); G(ns_at); G(lp); G(pad);
#undef G
    memset(s->gen, 0, (size_t)c * sizeof *s->gen);
    s->g = 0;
    s->cap = c;
    return true;
}

typedef struct { uint64_t page, str; int32_t adrp, at; } selsite;
static int selsite_cmp(const void *a, const void *b)
{
    const selsite *x = a, *y = b;
    if (x->page != y->page) return x->page < y->page ? -1 : 1;
    if (x->str != y->str) return x->str < y->str ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

static void note_left(codescan *s, int32_t adrp, int32_t j, uint64_t str, const char *why, int32_t why_at)
{
    s->left++;
    uint64_t pc = s->lo + 4ull * (uint64_t)j, apc = s->lo + 4ull * (uint64_t)adrp, wpc = s->lo + 4ull * (uint64_t)why_at;
    if (!s->first_why) {
        s->first_why = why;
        s->first_at = pc;
        s->first_adrp = apc;
        snprintf(s->first_sel, sizeof s->first_sel, "%s", (const char *)(uintptr_t)str);
    }
    if (hao_verbose || hao_opt.bridge_check)
        fprintf(stderr, "hao: dsc: %s: selector %s built at %#llx (ADRP %#llx) not patched: %s at %#llx\n", s->img->path,
                (const char *)(uintptr_t)str, (unsigned long long)pc, (unsigned long long)apc, why, (unsigned long long)wpc);
}

static void add_edge(codescan *s, int32_t to, int32_t from)
{
    if (s->nedges == s->capedges) {
        s->capedges = s->capedges ? 2 * s->capedges : 1024;
        s->edges = hao_alloc_check(realloc(s->edges, s->capedges * sizeof *s->edges), "shared cache tables");
    }
    s->edges[s->nedges++] = (uint64_t)(uint32_t)to << 32 | (uint32_t)from;
}

/* One DWARF EH pointer encoding of an offset (format only: no pc-relative or other application). */
static bool eh_read(const uint8_t **p, const uint8_t *end, uint8_t enc, uint64_t *v)
{
    size_t w = 0;
    switch (enc) {
    case 0x01: *v = uleb(p, end); return *p <= end;
    case 0x02: w = 2; break;
    case 0x03: case 0x0b: w = 4; break;
    case 0x04: case 0x0c: w = 8; break;
    default: return false;
    }
    if ((size_t)(end - *p) < w) return false;
    *v = 0;
    memcpy(v, *p, w);
    *p += w;
    return true;
}

/* The landing pads of the function [lo, lo + 4n) from its LSDA (__unwind_info's LSDA index, then the
 * Itanium C++ ABI call-site table libunwind and libc++abi read: LPStart, TType and call-site encodings,
 * {start, length, landing pad, action} relative to the function): s->pad marks each pad, s->lp names the
 * pad of each call in a range, with an edge call -> pad. false: an LSDA hao does not read. */
static bool read_callsites(codescan *s, uint64_t lo, int32_t n)
{
    if (!s->nlsda) return true;
    uint64_t flo = lo - s->text, fhi = flo + 4ull * (uint64_t)n, hi = lo + 4ull * (uint64_t)n;
    uint32_t a = 0, b = s->nlsda;
    while (a < b) { uint32_t m = (a + b) / 2; if (u32(s->lsda + 8 * m) < flo) a = m + 1; else b = m; }
    for (; a < s->nlsda && u32(s->lsda + 8 * a) < fhi; a++) {
        uint64_t fstart = s->text + u32(s->lsda + 8 * a), l = s->text + u32(s->lsda + 8 * a + 4);
        const dmap *m = map_of(l);
        if (!m) return false;
        uint64_t left = m->addr + m->size - l;
        const uint8_t *p = at(l), *end = p + (left < (1u << 20) ? left : (1u << 20));
        if (end - p < 3 || *p++ != 0xff) return false;   /* LPStart: the function's start */
        uint8_t tt = *p++;
        if (tt != 0xff) (void)uleb(&p, end);
        if (p >= end) return false;
        uint8_t ce = *p++;
        uint64_t len = uleb(&p, end);
        if (p > end || len > (uint64_t)(end - p)) return false;
        const uint8_t *ce_end = p + len;
        while (p < ce_end) {
            uint64_t st, ln, lpo;
            if (!eh_read(&p, ce_end, ce, &st) || !eh_read(&p, ce_end, ce, &ln) || !eh_read(&p, ce_end, ce, &lpo)) return false;
            (void)uleb(&p, ce_end);
            if (!lpo) continue;
            uint64_t pad = fstart + lpo, c0 = fstart + st, c1 = c0 + ln;
            if (pad < lo || pad >= hi || (pad & 3) || c0 < lo || c1 > hi || c1 < c0) return false;
            int32_t pi = (int32_t)((pad - lo) / 4);
            s->pad[pi] = 1;
            for (uint64_t c = c0 & ~3ull; c < c1; c += 4) {
                int32_t j = (int32_t)((c - lo) / 4);
                a64op o;
                a64_decode(s->w[j], &o);
                if (o.cf != CF_BL && o.cf != CF_BLR) continue;
                s->lp[j] = pi;
                add_edge(s, pi, j);
            }
        }
    }
    return true;
}

static bool selpage(const codescan *s, uint64_t pg)
{
    if (!s->selpgcap) return false;
    for (size_t h = (pg >> 12) & (s->selpgcap - 1);; h = (h + 1) & (s->selpgcap - 1)) {
        if (s->selpg[h] == pg) return true;
        if (!s->selpg[h]) return false;
    }
}

/* The control flow the forward walks do not need: landing pads, sorted edges, code no edge reaches. */
static void flow(codescan *s, bool has_br, bool has_unknown)
{
    int32_t n = s->n;
    bool eh_ok = read_callsites(s, s->lo, n);
    qsort(s->edges, s->nedges, sizeof *s->edges, edge_cmp);
    size_t sp = 0;
    s->reach[0] = 1;
    s->stack[sp++] = 0;
    while (sp) {
        int32_t j = s->stack[--sp];
        a64op o;
        a64_decode(s->w[j], &o);
        int64_t t = j + o.off / 4;
        int64_t nx[3] = {-1, -1, s->lp[j]};
        if (o.cf == CF_B || o.cf == CF_BCOND) nx[0] = t;
        if (o.cf != CF_B && o.cf != CF_BR && o.cf != CF_RET && o.cf != CF_STOP &&
            !((o.cf == CF_BL || o.cf == CF_BLR) && s->pad[j + 1]))
            nx[1] = j + 1;
        for (int q = 0; q < 3; q++)
            if (nx[q] >= 0 && nx[q] < n && !s->reach[nx[q]]) { s->reach[nx[q]] = 1; s->stack[sp++] = (int32_t)nx[q]; }
    }
    s->hidden = has_br || has_unknown || !eh_ok;
    s->hidden_reads = 0;
    for (int32_t j = 0; j < n; j++) {
        uint32_t i = s->w[j];
        if (s->reach[j] || i == 0 || i == 0xd503201f || (i & 0xffe0001f) == 0xd4200000) continue;   /* padding: UDF, NOP, BRK */
        a64op o;
        a64_decode(i, &o);
        s->hidden = true;
        s->hidden_reads |= o.rd;
    }
}

/* One function: [lo, lo + 4n). */
static void scan_function(codescan *s, uint64_t lo, int32_t n)
{
    if (n <= 0 || !grow(s, n + 1)) return;
    s->lo = lo;
    s->w = code_at(lo);
    s->n = n;
    s->nrv = 0;
    s->nedges = 0;
    memset(s->first, 0xff, (size_t)n * sizeof *s->first);
    memset(s->aflags, 0, (size_t)n);
    memset(s->reach, 0, (size_t)n);
    memset(s->lp, 0xff, (size_t)n * sizeof *s->lp);
    memset(s->pad, 0, (size_t)n + 1);
    /* direct edges; whether an ADRP names a page of selector strings */
    bool any_adrp = false, any_sel = false, has_br = false, has_unknown = false;
    for (int32_t j = 0; j < n; j++) {
        a64op o;
        uint32_t i = s->w[j];
        a64_decode(i, &o);
        if (is_adrp(i) && (i & 31) != 31) {
            any_adrp = true;
            any_sel = any_sel || selpage(s, adrp_target(lo + 4ull * (uint64_t)j, i));
        }
        has_br |= o.cf == CF_BR;
        has_unknown |= o.cf == CF_UNKNOWN;
        int64_t t = j + o.off / 4;
        if ((o.cf == CF_B || o.cf == CF_BCOND) && t >= 0 && t < n) add_edge(s, (int32_t)t, j);
    }
    if (!any_adrp) return;
    /* without selector strings the walks only bind GOT slots, which needs no more than direct edges */
    if (any_sel) flow(s, has_br, has_unknown);
    for (int32_t k = 0; k < n; k++)
        if (is_adrp(s->w[k]) && (s->w[k] & 31) != 31) { s->uf[k] = k; s->why[k] = NULL; }
    for (int32_t k = 0; k < n; k++)
        if (is_adrp(s->w[k]) && (s->w[k] & 31) != 31) walk_forward(s, k);

    /* webs: one bad member makes its web bad (the verdict is kept on the root) */
    for (int32_t k = 0; k < n; k++) {
        if (!is_adrp(s->w[k]) || (s->w[k] & 31) == 31) continue;
        int32_t root = uf_find(s, k);
        const char *why = NULL;
        int32_t wat = k;
        if (s->aflags[k] & AF_BAD) { why = s->why[k]; wat = s->why_at[k]; }
        else if ((s->aflags[k] & AF_NONSEL) || !(s->aflags[k] & AF_SEL)) {
            why = "a reader that is no selector load";
            if (s->aflags[k] & AF_NONSEL) wat = s->ns_at[k];
        }
        else if (adrp_target(lo + 4ull * k, s->w[k]) != adrp_target(lo + 4ull * root, s->w[root])) why = "readers reached from two pages";
        else if (s->hidden && (s->aflags[k] & AF_CALL) && (s->hidden_reads & (1u << (s->w[k] & 31))))
            why = "live across a call in a function whose unreached code reads it";
        if (why) bad(s, root, why, wat);
    }
    if (!any_sel) {   /* an address of native-backed data needs the whole flow */
        for (size_t e = 0; e < s->nrv && !any_sel; e++) any_sel = s->rv[e].kind == RK_XDATA;
        if (any_sel) flow(s, has_br, has_unknown);
    }
    /* each reader of a good web is reached by the web's ADRPs only */
    for (size_t e = 0; e < s->nrv; e++) {
        const reader_t *rd = &s->rv[e];
        if (rd->kind != RK_SEL) continue;
        int32_t root = uf_find(s, rd->adrp);
        if (!(s->aflags[root] & AF_BAD) && !walk_backward(s, rd->at, s->w[rd->adrp] & 31, root))
            bad(s, root, "reached by another value of the register", rd->at);
    }
    /* hidden entries: a page's webs are patched only if every ADRP of the page in this function is */
    if (s->hidden) {
        size_t na = 0;
        for (int32_t k = 0; k < n; k++) na += is_adrp(s->w[k]) && (s->w[k] & 31) != 31;
        uint64_t *pa = malloc(na * sizeof *pa);
        if (!pa) return;
        na = 0;
        for (int32_t k = 0; k < n; k++)
            if (is_adrp(s->w[k]) && (s->w[k] & 31) != 31)
                pa[na++] = adrp_target(lo + 4ull * k, s->w[k]) >> 12 << 32 | (uint32_t)k;   /* pages below 2^44 */
        qsort(pa, na, sizeof *pa, edge_cmp);
        for (size_t a = 0, b; a < na; a = b) {
            int32_t unpatched = -1;
            for (b = a; b < na && pa[b] >> 32 == pa[a] >> 32; b++) {
                int32_t k = (int32_t)(pa[b] & 0xffffffff);
                if (!(s->aflags[k] & AF_SEL) || (s->aflags[uf_find(s, k)] & AF_BAD)) unpatched = k;
            }
            if (unpatched >= 0)
                for (size_t e = a; e < b; e++)
                    bad(s, uf_find(s, (int32_t)(pa[e] & 0xffffffff)),
                        "an ADRP of the same page that cannot be patched, in a function with hidden entries", unpatched);
        }
        free(pa);
    }

    /* the selector sites: patch the good webs page by page */
    size_t nsel = 0;
    for (size_t e = 0; e < s->nrv; e++) nsel += s->rv[e].kind == RK_SEL;
    selsite *ss = nsel ? malloc(nsel * sizeof *ss) : NULL;
    size_t m = 0;
    for (size_t e = 0; ss && e < s->nrv; e++) {
        const reader_t *rd = &s->rv[e];
        if (rd->kind != RK_SEL) continue;
        uint64_t pg = adrp_target(lo + 4ull * rd->adrp, s->w[rd->adrp]);
        ss[m++] = (selsite){pg, pg + ((s->w[rd->at] >> 10) & 0xfff), rd->adrp, rd->at};
    }
    qsort(ss, m, sizeof *ss, selsite_cmp);
    uint32_t g = ++s->g;   /* marks readers counted */
    for (size_t a = 0, b; a < m; a = b) {
        for (b = a; b < m && ss[b].page == ss[a].page; b++) {}
        /* the good sites of this page: distinct strings, their selector references' page, the ADRPs' pcs */
        unsigned nstr = 0;
        uint64_t q = 0, pclo = ~0ull, pchi = 0, last = 0;
        bool one_page = true;
        for (size_t e = a; e < b; e++) {
            if (s->aflags[uf_find(s, ss[e].adrp)] & AF_BAD) continue;
            uint64_t apc = lo + 4ull * (uint64_t)ss[e].adrp;
            if (apc < pclo) pclo = apc;
            if (apc > pchi) pchi = apc;
            if (nstr && ss[e].str == last) continue;
            last = ss[e].str;
            uint64_t ref = selmap_slot(s->sm, ss[e].str + (uint64_t)D.slide) - (uint64_t)D.slide;
            if (!nstr) q = ref & ~0xfffull;
            else if ((ref & ~0xfffull) != q) one_page = false;
            nstr++;
        }
        /* the image's selector references when one ADRP reaches them all, else pool slots */
        uint64_t base = 0;
        if (nstr && !(one_page && adrp_reach(pclo, q) && adrp_reach(pchi, q)) && !(base = pool_alloc(pclo, pchi, nstr))) {
            for (size_t e = a; e < b; e++) bad(s, uf_find(s, ss[e].adrp), "no slot page within ADRP range", ss[e].adrp);
            nstr = 0;
        }
        unsigned si = 0;
        for (size_t e = a; nstr && e < b; e++) {
            if (s->aflags[uf_find(s, ss[e].adrp)] & AF_BAD) continue;
            uint64_t str = ss[e].str + (uint64_t)D.slide, slot;
            if (base) {
                if (!si || ss[e].str != last) {
                    *(uint64_t *)at(base + 8ull * si) = (uint64_t)(uintptr_t)sel_registerName((const char *)(uintptr_t)str);
                    last = ss[e].str;
                    si++;
                }
                slot = base + 8ull * (si - 1);
            } else
                slot = selmap_slot(s->sm, str) - (uint64_t)D.slide;
            uint64_t apc = lo + 4ull * (uint64_t)ss[e].adrp, pc = lo + 4ull * (uint64_t)ss[e].at;
            uint32_t ai = s->w[ss[e].adrp], ri = s->w[ss[e].at];
            put_insn(apc, enc_adrp(apc, ai & 31, slot));
            put_insn(pc, enc_ldr(ri & 31, (ri >> 5) & 31, slot));
        }
        for (size_t e = a; e < b; e++) {
            if (s->gen[ss[e].at] == g) continue;   /* a reader of several ADRPs: once */
            s->gen[ss[e].at] = g;
            s->sites++;
            int32_t root = uf_find(s, ss[e].adrp);
            if (!(s->aflags[root] & AF_BAD)) {
                s->patched++;
                if (ss[e].at - ss[e].adrp > 4) s->far++;
            } else
                note_left(s, ss[e].adrp, ss[e].at, ss[e].str + (uint64_t)D.slide, s->why[root] ? s->why[root] : "?", s->why_at[root]);
        }
    }
    /* webs with more than one selector reader */
    uint32_t g2 = ++s->g;
    for (size_t e = 0; e < m; e++) s->reach[uf_find(s, ss[e].adrp)] = 0;
    for (size_t e = 0; e < m; e++) {
        int32_t root = uf_find(s, ss[e].adrp);
        if ((s->aflags[root] & AF_BAD) || s->gen[ss[e].at] == g2) continue;
        s->gen[ss[e].at] = g2;
        if (s->reach[root] < 2 && ++s->reach[root] == 2) s->multi++;
    }
    free(ss);

    /* the address of another cache image's data (ADRP+ADD the cache builder
     * made from a GOT load). Into a native-backed library it must become the
     * native symbol's address: load that from a slot, when the ADD is the
     * ADRP's only reader and only that ADRP reaches it. */
    for (size_t e = 0; e < s->nrv; e++) {
        const reader_t *rd = &s->rv[e];
        if (rd->kind != RK_XDATA) continue;
        int32_t k = rd->adrp;
        uint32_t ai = s->w[k], ri = s->w[rd->at];
        uint64_t apc = lo + 4ull * (uint64_t)k, pc = lo + 4ull * (uint64_t)rd->at;
        uint64_t t = adrp_target(apc, ai) + ((ri >> 10) & 0xfff), v = resolve_target(t), slot = 0;
        unsigned c = 0;
        for (size_t f = 0; f < s->nrv; f++) c += s->rv[f].adrp == k || s->rv[f].at == rd->at;
        if (c == 1 && !(s->aflags[k] & AF_BAD) && v != t + (uint64_t)D.slide && (slot = pool_slot(apc, v))) {
            /* walk_backward needs the ADRP marked as a web member */
            uint8_t f0 = s->aflags[k];
            s->aflags[k] |= AF_SEL;
            bool only = walk_backward(s, rd->at, ai & 31, uf_find(s, k));
            s->aflags[k] = f0;
            if (only) {
                put_insn(apc, enc_adrp(apc, ai & 31, slot));
                put_insn(pc, enc_ldr(ri & 31, (ri >> 5) & 31, slot));
                D.n_xdata++;
                continue;
            }
        }
        D.n_xleft++;
        if (hao_verbose) fprintf(stderr, "hao: dsc: %#llx: address of %#llx in %s stays arm64e\n", (unsigned long long)pc,
                                 (unsigned long long)t, D.images[owner_of(t)].path);
    }
}

/* __TEXT,__unwind_info's LSDA index (compact unwind format, <mach-o/compact_unwind_encoding.h>):
 * {functionOffset, lsdaOffset} pairs sorted by function, between the first and the last index entry's
 * lsdaIndexArraySectionOffset. */
static void read_lsda(codescan *s)
{
    uint64_t n = 0, a = hao_image_section(s->img, "__TEXT", "__unwind_info", &n);
    if (!a || n < 28 || !mapped_span(a - (uint64_t)D.slide, n)) return;
    const uint8_t *u = (const uint8_t *)(uintptr_t)a;
    uint32_t io = u32(u + 20), ic = u32(u + 24);
    if (u32(u) != 1 || !ic || io > n || (uint64_t)ic * 12 > n - io) return;
    uint32_t l0 = u32(u + io + 8), l1 = u32(u + io + 12 * (ic - 1) + 8);
    if (l1 < l0 || l1 > n || (l1 - l0) % 8) return;
    s->lsda = u + l0;
    s->nlsda = (l1 - l0) / 8;
}

static void scan_code(hao_image *img, const selmap *sm)
{
    codescan s;
    memset(&s, 0, sizeof s);
    s.img = img;
    s.sm = sm;
    s.text = img->base - (uint64_t)D.slide;
    s.fs = malloc((img->nfstarts + 1) * sizeof *s.fs);
    if (!s.fs) return;
    for (size_t k = 0; k < img->nfstarts; k++) s.fs[s.nfs++] = img->fstarts[k] - (uint64_t)D.slide;
    for (int k = 0; k < img->nsects && s.nsec < 32; k++) {
        if (!(img->sects[k].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) || img->sects[k].size < 4) continue;
        uint64_t a = img->sects[k].addr - (uint64_t)D.slide;
        if (!mapped_span(a, img->sects[k].size)) continue;
        s.sec[s.nsec].lo = a;
        s.sec[s.nsec].hi = a + (img->sects[k].size & ~3ull);
        s.nsec++;
    }
    read_lsda(&s);
    if (sm->cap) {
        s.selpgcap = sm->cap;
        s.selpg = calloc(s.selpgcap, sizeof *s.selpg);
        for (size_t k = 0; s.selpg && k < sm->cap; k++) {
            if (!sm->v[k].str) continue;
            uint64_t pg = (sm->v[k].str - (uint64_t)D.slide) & ~0xfffull;
            size_t h = (pg >> 12) & (s.selpgcap - 1);
            while (s.selpg[h] && s.selpg[h] != pg) h = (h + 1) & (s.selpgcap - 1);
            s.selpg[h] = pg;
        }
        if (!s.selpg) s.selpgcap = 0;
    }
    for (int k = 0; k < s.nsec; k++) {
        /* branches into stub islands: their GOT slots */
        for (uint64_t pc = s.sec[k].lo; pc < s.sec[k].hi; pc += 4) {
            uint32_t i = code32(pc);
            if ((i & 0x7c000000) != 0x14000000) continue;   /* b / bl */
            uint64_t t = pc + (uint64_t)(sext(i & 0x3ffffff, 26) * 4), slot, target;
            const dmap *m = map_of(t);
            if (m && (m->flags & MAP_TEXT_STUBS) && decode_island(t, &slot, &target) && slot) fix_slot(slot);
        }
        /* functions: from each start to the next, or to the section's end */
        for (uint64_t lo = s.sec[k].lo; lo < s.sec[k].hi;) {
            uint64_t hi = func_end(&s, lo, s.sec[k].hi);
            if (hi - lo <= 4ull * (1u << 24)) scan_function(&s, lo, (int32_t)((hi - lo) / 4));
            lo = hi;
        }
    }
    D.n_selcode += s.patched;
    D.n_selsites += s.sites;
    D.n_selfar += s.far;
    D.n_selmulti += s.multi;
    D.n_selleft += s.left;
    if (s.left) {
        /* not silently wrong: objc_msgSend maps such a selector (hao_dsc_sel_map), nothing else does */
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "%s: %llu of %llu selectors built in code not patched (first: %s at %#llx, ADRP %#llx: %s)\n", img->path,
                (unsigned long long)s.left, (unsigned long long)s.sites, s.first_sel, (unsigned long long)s.first_at,
                (unsigned long long)s.first_adrp, s.first_why);
    }
    if (hao_verbose > 1)
        fprintf(stderr, "hao: dsc: %s: code analysis: %llu GOT loads, %llu walk steps\n", img->path, (unsigned long long)s.got,
                (unsigned long long)s.steps);
    free(s.fs); free(s.gen); free(s.uf); free(s.first); free(s.stack); free(s.aflags); free(s.reach); free(s.why); free(s.lp); free(s.pad);
    free(s.why_at); free(s.ns_at); free(s.rv); free(s.edges); free(s.cc); free(s.selpg);
}

/* ------------------------------------------------------- eligibility */

/* What hao_dsc_eligible found: references into other cache images that are
 * native-backed and have no name (they would stay arm64e addresses). */
typedef struct { uint32_t self; uint64_t lo, hi, refs, bad; uint64_t first_at, first_to; const char *kind; } elig_t;

static void elig_ref(elig_t *e, uint64_t at_, uint64_t target, const char *kind)
{
    int o = owner_of(target);
    if (o < 0 || (uint32_t)o == e->self || D.images[o].img) return;   /* shared region, itself, a guest image */
    e->refs++;
    uint64_t off;
    if (rev_name((uint32_t)o, target) || rev_vtable_interior((uint32_t)o, target, &off)) return;
    if (!e->bad++) { e->first_at = at_; e->first_to = target; e->kind = kind; }
}

static void elig_loc(const dmap *m, uint64_t loc, uint64_t raw, void *c)
{
    elig_t *e = c;
    uint64_t target, high8;
    if (loc >= e->lo && loc < e->hi && !fixed_has(loc) && decode_ptr(m, raw, &target, &high8)) elig_ref(e, loc, target, "pointer");
}

/* A GOT slot the code loads: its slide-info target, if not rewritten yet. */
static void elig_slot(elig_t *e, uint64_t slot)
{
    if ((slot & 7) || fixed_has(slot) || !mapped_span(slot, 8)) return;
    const dmap *m = map_of(slot);
    uint64_t target, high8;
    if (!m || !m->slide || m->slide_size < 24) return;
    uint64_t raw = *(const uint64_t *)at(slot);
    if (raw && decode_ptr(m, raw, &target, &high8)) elig_ref(e, slot, target, "GOT slot");
}

static void elig_code(elig_t *e, uint64_t lo, uint64_t hi)
{
    for (uint64_t pc = lo; pc + 4 <= hi; pc += 4) {
        uint32_t i = code32(pc);
        if (is_adrp(i)) {
            unsigned rd = i & 31;
            uint64_t page = adrp_target(pc, i);
            for (int k = 1; k <= 4 && pc + 4 * k + 4 <= hi; k++) {
                uint32_t j = code32(pc + 4 * k);
                if ((j & 0xffc00000) == 0xf9400000 && ((j >> 5) & 31) == rd) { elig_slot(e, page + ((j >> 10) & 0xfff) * 8); break; }
                if ((j & 0xffc00000) == 0x91000000 && ((j >> 5) & 31) == rd) {
                    uint64_t t = page + ((j >> 10) & 0xfff);
                    if (owner_of(t) < 0) {
                        uint32_t l = pc + 4 * k + 8 <= hi ? code32(pc + 4 * k + 4) : 0;
                        elig_slot(e, (l & 0xffc00000) == 0xf9400000 && ((l >> 5) & 31) == (j & 31) ? t + ((l >> 10) & 0xfff) * 8 : t);
                    } else
                        elig_ref(e, pc + 4 * k, t, "address in code");
                    if ((j & 31) == rd) break;
                    continue;
                }
                if ((j & 31) == rd) break;
            }
        } else if ((i & 0x7c000000) == 0x14000000) {
            int64_t off = (int64_t)(i & 0x3ffffff);
            if (off & (1 << 25)) off -= 1 << 26;
            uint64_t t = pc + (uint64_t)(off * 4), slot, target;
            const dmap *m = map_of(t);
            if (m && (m->flags & MAP_TEXT_STUBS)) {
                if (decode_island(t, &slot, &target)) {
                    if (slot) elig_slot(e, slot);
                    else elig_ref(e, pc, target, "branch through a stub island");
                }
            } else
                elig_ref(e, pc, t, "direct branch");
        }
    }
}

bool hao_dsc_eligible(const char *path, char *why, size_t n)
{
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    why[0] = 0;
    int idx = dsc_open() ? find_image(path) : -1;
    const struct mach_header_64 *mh = idx >= 0 ? image_header((uint32_t)idx) : NULL;
    elig_t e = {.self = (uint32_t)idx};
    if (!mh || mh->cputype != CPU_TYPE_ARM64 || mh->filetype != MH_DYLIB) {
        snprintf(why, n, "%s: not an arm64e dylib in the shared cache", path);
        goto out;
    }
    if (D.images[idx].img) goto out;   /* loaded already: it was admitted */
    /* metadata the native runtimes would have to share: ObjC classes, Swift types, thread-locals */
    FOR_EACH_CMD(mh, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        const struct section_64 *sc = (const void *)(sg + 1);
        for (uint32_t k = 0; k < sg->nsects; k++) {
            uint32_t type = sc[k].flags & SECTION_TYPE;
            if (!strncmp(sc[k].sectname, "__objc_", 7) || !strncmp(sc[k].sectname, "__swift5_", 9) ||
                (type >= S_THREAD_LOCAL_REGULAR && type <= S_THREAD_LOCAL_INIT_FUNCTION_POINTERS)) {
                snprintf(why, n, "%s: has %.16s,%.16s", path, sc[k].segname, sc[k].sectname);
                goto out;
            }
        }
    }
    /* every pointer its data holds, every GOT slot and every address its code
     * builds, and every direct branch, into another native-backed image must
     * name an export */
    FOR_EACH_CMD(mh, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (!sg->vmsize || !strncmp(sg->segname, "__LINKEDIT", 16)) continue;
        if (sg->initprot & VM_PROT_EXECUTE) {
            const struct section_64 *sc = (const void *)(sg + 1);
            for (uint32_t k = 0; k < sg->nsects; k++)
                if (sc[k].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) elig_code(&e, sc[k].addr, sc[k].addr + sc[k].size);
            continue;
        }
        for (uint64_t a = sg->vmaddr; a < sg->vmaddr + sg->vmsize;) {
            const dmap *m = map_of(a);
            if (!m) break;
            uint64_t end = m->addr + m->size < sg->vmaddr + sg->vmsize ? m->addr + m->size : sg->vmaddr + sg->vmsize;
            if (m->slide && m->slide_size >= 24) {
                uint32_t psize = u32(m->slide + 4);
                e.lo = sg->vmaddr;
                e.hi = sg->vmaddr + sg->vmsize;
                for (uint64_t pg = (a - m->addr) / (psize ? psize : 1); psize && m->addr + pg * psize < end; pg++)
                    walk_page(m, pg, elig_loc, &e);
            }
            a = end;
        }
    }
    if (e.bad) {
        int o = owner_of(e.first_to);
        snprintf(why, n, "%s: %llu of %llu references into native-backed images have no exported name (first: %s at %#llx to %#llx in %s)",
                 path, (unsigned long long)e.bad, (unsigned long long)e.refs, e.kind, (unsigned long long)e.first_at,
                 (unsigned long long)e.first_to, o >= 0 ? D.images[o].path : "?");
        goto out;
    }
    if (hao_verbose) fprintf(stderr, "hao: dsc: %s is eligible as a guest image (%llu references into native-backed images, all named)\n",
                             path, (unsigned long long)e.refs);
out:
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return !why[0];
}

/* ------------------------------------------------------------ public */

bool hao_dsc_has(const char *path)
{
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    bool r = dsc_open() && find_image(path) >= 0;
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return r;
}

hao_image *hao_dsc_load(const char *path, hao_mapped_fn mapped, void *ctx, char *err, size_t errlen)
{
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    int idx = dsc_open() ? find_image(path) : -1;
    const struct mach_header_64 *mh = idx >= 0 ? image_header((uint32_t)idx) : NULL;
    if (!mh || mh->cputype != CPU_TYPE_ARM64 || mh->filetype != MH_DYLIB) {
        snprintf(err, errlen, "%s: not an arm64e dylib in the shared cache", path);
        pthread_mutex_unlock(&dsc_lock);
        hao_crit_leave();
        return NULL;
    }
    hao_image *img = calloc(1, sizeof *img);
    if (!img || !(img->identity_ranges = calloc(mh->ncmds ? mh->ncmds : 1, sizeof *img->identity_ranges))) {
        free(img);
        snprintf(err, errlen, "%s: no memory for mapped segment ranges", path);
        pthread_mutex_unlock(&dsc_lock); hao_crit_leave(); return NULL;
    }
    snprintf(img->path, sizeof img->path, "%s", path);
    img->fd = -1;
    img->cpusubtype = (uint32_t)mh->cpusubtype;
    img->is_dylib = img->is_pie = true;
    img->dsc = true;
    img->base = (uint64_t)(uintptr_t)mh;
    img->slide = (uint64_t)D.slide;
    uint32_t fs_off = 0, fs_size = 0;
    const char *bad = NULL;
    FOR_EACH_CMD(mh, lc) {
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *sg = (const void *)lc;
            /* everything hao reads or rewrites must be in the cache's mappings */
            if (sg->vmsize && !mapped_span(sg->vmaddr, sg->vmsize)) { bad = "a segment outside the cache mappings"; break; }
            bool secs_ok = true;
            const struct section_64 *sc = (const void *)(sg + 1);
            for (uint32_t s = 0; s < sg->nsects; s++)
                secs_ok &= sc[s].addr >= sg->vmaddr && sc[s].size <= sg->vmsize && sc[s].addr - sg->vmaddr <= sg->vmsize - sc[s].size;
            if (!secs_ok) { bad = "a section outside its segment"; break; }
            if (sg->vmsize && strncmp(sg->segname, "__LINKEDIT", 16)) {
                uint32_t q = img->n_identity_ranges++;
                img->identity_ranges[q][0] = sg->vmaddr + img->slide;
                img->identity_ranges[q][1] = sg->vmaddr + sg->vmsize + img->slide;
            }
            if (!strncmp(sg->segname, "__TEXT", 16)) { img->pref_base = sg->vmaddr; img->vmsize = sg->vmsize; }
            if (sg->vmsize && strncmp(sg->segname, "__LINKEDIT", 16) && img->nseg < HAO_MAX_RANGES) {
                img->seg_lo[img->nseg] = sg->vmaddr + img->slide;
                img->seg_hi[img->nseg] = sg->vmaddr + sg->vmsize + img->slide;
                img->nseg++;
            }
            if ((sg->initprot & VM_PROT_EXECUTE) && img->ncode < HAO_MAX_RANGES) {
                img->code_lo[img->ncode] = sg->vmaddr + img->slide;
                img->code_hi[img->ncode] = sg->vmaddr + sg->vmsize + img->slide;
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
                if (type == S_MOD_INIT_FUNC_POINTERS) { img->init_ptrs = sa; img->init_ptrs_count = sec[s].size / 8; }
                else if (type == 0x16) { img->init_offs = sa; img->init_offs_count = sec[s].size / 4; }
                else if (type == S_THREAD_LOCAL_VARIABLES) { img->tlv_desc = sa; img->tlv_desc_count = sec[s].size / 24; }
            }
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB: {
            const struct dylib_command *dc = (const void *)lc;
            if (img->ndeps < HAO_MAX_DEPS) {   /* name offset and terminator: hao_macho_check_cmds */
                img->deps[img->ndeps] = hao_alloc_check(strdup((const char *)lc + dc->dylib.name.offset), "shared cache tables");
                img->dep_weak[img->ndeps] = lc->cmd == LC_LOAD_WEAK_DYLIB;
                img->dep_reexport[img->ndeps] = lc->cmd == LC_REEXPORT_DYLIB;
                img->dep_upward[img->ndeps] = lc->cmd == LC_LOAD_UPWARD_DYLIB;
                img->ndeps++;
            }
            break;
        }
        case LC_UUID: memcpy(img->uuid, ((const struct uuid_command *)lc)->uuid, 16); img->has_uuid = true; break;
        case LC_FUNCTION_STARTS: {
            const struct linkedit_data_command *ld = (const void *)lc;
            fs_off = ld->dataoff; fs_size = ld->datasize;
            break;
        }
        default: break;
        }
    }
    img->exports_trie = exports_trie(mh, &img->exports_trie_size);
    if (!img->exports_trie) img->exports_trie_size = 0;
    if (!bad && !img->vmsize) bad = "no __TEXT segment";
    const uint8_t *fs = !bad && fs_size ? linkedit_ptr(mh, fs_off, fs_size) : NULL;
    if (fs) {
        size_t cap = 256;
        img->fstarts = hao_alloc_check(malloc(cap * 8), "shared cache tables");
        const uint8_t *q = fs, *e = fs + fs_size;
        uint64_t a = img->base, limit = img->base + img->vmsize;   /* starts are in __TEXT */
        while (q < e && img->fstarts) {
            uint64_t d = uleb(&q, e);
            if (!d) break;
            if (d >= limit - a) { bad = "a function start outside __TEXT"; break; }
            a += d;
            if (img->nfstarts == cap) {
                uint64_t *n2 = cap < (1u << 24) ? realloc(img->fstarts, cap * 2 * 8) : NULL;
                if (!n2) { bad = "too many function starts"; break; }
                img->fstarts = n2;
                cap *= 2;
            }
            img->fstarts[img->nfstarts++] = a;
        }
    }
    if (bad) {
        snprintf(err, errlen, "%s: shared-cache image has %s", path, bad);
        for (int k = 0; k < img->ndeps; k++) free(img->deps[k]);
        free(img->fstarts);
        free(img->identity_ranges);
        free(img);
        pthread_mutex_unlock(&dsc_lock);
        hao_crit_leave();
        return NULL;
    }
    D.images[idx].img = img;
    D.nloaded++;
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    hao_code_alias_publish(img, D.code_delta);   /* before it is listed */
    hao_image_register(img);
    if (mapped) mapped(img, ctx);
    if (hao_verbose) fprintf(stderr, "hao: mapped %s from the arm64e shared cache at %#llx\n", path, (unsigned long long)img->base);
    return img;
}

/* A slot that the image's Swift metadata reaches through a relative
 * indirect(able) reference, outside the image (hao_swiftmeta_slots): a
 * uniqued GOT slot that no code of the image may load, which native
 * libswiftCore reads (a mangled name's \x02 context descriptor, a
 * conformance's protocol, a resilient witness's requirement, ...). It is
 * bound like a GOT slot the code loads, once cache-wide: the cache mapping
 * is hao's own, so every guest reader of a uniqued slot wants the same
 * native (or slid guest) target. */
typedef struct { const hao_image *img; uint64_t refs, bound, before, left; int first_kind; uint64_t first_field, first_slot; } swift_slots;

static void swift_slot(void *c, int kind, uint64_t field, uint64_t slot_rt)
{
    swift_slots *s = c;
    uint64_t slot = slot_rt - (uint64_t)D.slide;
    s->refs++;
    if (fixed_has(slot)) { s->before++; return; }   /* bound by an earlier reference, image or code scan */
    fix_slot(slot);
    if (fixed_has(slot)) { s->bound++; return; }
    if (!s->left++) { s->first_kind = kind; s->first_field = field - (uint64_t)D.slide; s->first_slot = slot; }
    if (hao_verbose > 1)
        fprintf(stderr, "hao: dsc: %s Swift metadata %s at %#llx: slot %#llx not bound\n", s->img->path, hao_swiftmeta_slot_kind(kind),
                (unsigned long long)(field - (uint64_t)D.slide), (unsigned long long)slot);
}

void hao_dsc_bind(hao_image *img, hao_dsc_binder bind)
{
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    D.bind = bind;
    const struct mach_header_64 *mh = (const void *)(uintptr_t)img->base;
    uint64_t b0 = D.n_bound, s0 = D.n_slid, u0 = D.n_unnamed, x0 = D.n_unbound, c0 = D.n_selcode, d0 = D.n_xdata, l0 = D.n_xleft;
    uint64_t t0 = D.n_selsites, f0 = D.n_selfar, m0 = D.n_selmulti, e0 = D.n_selleft;
    /* the image's own data first (the selector references the code scan
     * looks up), then its code */
    for (int pass = 0; pass < 2; pass++) {
        selmap sm = {NULL, 0};
        if (pass) selmap_build(&sm, img);
        FOR_EACH_CMD(mh, lc) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *sg = (const void *)lc;
            if (!sg->vmsize || !strncmp(sg->segname, "__LINKEDIT", 16)) continue;
            if ((sg->initprot & VM_PROT_EXECUTE) != 0) continue;
            if (pass) continue;
            /* every pointer the slide info lists */
            for (uint64_t a = sg->vmaddr; a < sg->vmaddr + sg->vmsize;) {
                const dmap *m = map_of(a);
                if (!m) break;
                uint64_t end = m->addr + m->size < sg->vmaddr + sg->vmsize ? m->addr + m->size : sg->vmaddr + sg->vmsize;
                if (m->slide) {
                    uint32_t psize = u32(m->slide + 4);
                    struct seg_ctx s = {sg->vmaddr, sg->vmaddr + sg->vmsize};
                    for (uint64_t pg = (a - m->addr) / psize; psize && m->addr + pg * psize < end; pg++) walk_page(m, pg, fix_in_range, &s);
                }
                a = end;
            }
            if (sg->initprot & VM_PROT_WRITE) make_writable(sg->vmaddr, sg->vmsize);
        }
        if (pass) scan_code(img, &sm);   /* every instruction section */
        free(sm.v);
    }
    hao_code_poison(img);   /* HAO_CODE_ALIAS: after the code patches */
    /* then the slots outside the image that its Swift metadata reaches (data, not code: every mode) */
    swift_slots ss = {.img = img};
    (void)hao_swiftmeta_slots(img, swift_slot, &ss);
    if (ss.left) {
        /* not silently wrong: native libswiftCore reads these slots raw */
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "%s: %llu of %llu Swift metadata references through slots outside the image not bound (first: %s at "
                          "%#llx, slot %#llx)\n", img->path, (unsigned long long)ss.left, (unsigned long long)ss.refs,
                hao_swiftmeta_slot_kind(ss.first_kind), (unsigned long long)ss.first_field, (unsigned long long)ss.first_slot);
    }
    if (hao_verbose)
        fprintf(stderr, "hao: dsc bind %s: %llu native, %llu slid, %llu unnamed, %llu unbound; code: %llu data addresses patched, "
                        "%llu left; selectors built in code: %llu of %llu patched (%llu beyond 4 instructions, "
                        "%llu webs with several), %llu left; Swift metadata: %llu references through slots outside the image, %llu "
                        "slots bound, %llu bound before, %llu left\n", img->path,
                (unsigned long long)(D.n_bound - b0), (unsigned long long)(D.n_slid - s0), (unsigned long long)(D.n_unnamed - u0),
                (unsigned long long)(D.n_unbound - x0), (unsigned long long)(D.n_xdata - d0), (unsigned long long)(D.n_xleft - l0), (unsigned long long)(D.n_selcode - c0),
                (unsigned long long)(D.n_selsites - t0), (unsigned long long)(D.n_selfar - f0), (unsigned long long)(D.n_selmulti - m0),
                (unsigned long long)(D.n_selleft - e0), (unsigned long long)ss.refs,
                (unsigned long long)ss.bound, (unsigned long long)ss.before, (unsigned long long)ss.left);
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
}

/*
 * One type identity for a library that runs both as a guest image and
 * natively (guest libc++, runtime.c). A std::type_info is {vtable pointer,
 * __type_name} (Itanium C++ ABI 2.9.5; the SDK's <typeinfo>), and both
 * slices compare type_infos by their __type_name pointer: x86_64 libc++abi
 * (catch matching, dynamic_cast, type_info::operator== and hash_code:
 * _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION 1, unique names) and inline
 * arm64 code (implementation 3: equal pointers, or strcmp only when both
 * names carry the non-unique bit 63, which an exported type's name does
 * not). Pointing the __type_name of each type_info the guest image exports
 * at the native library's _ZTS of the same mangled name makes the two
 * copies one type to both: an exception of a libc++-owned type thrown by
 * native libc++ is caught by guest code by its exact type, and the other
 * way round. A type_info whose name the native library does not export
 * keeps its own (guest-only types). Returns how many were shared.
 */
int hao_dsc_share_type_names(hao_image *img, hao_dsc_binder bind, const char *native_lib)
{
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    int idx = -1, n = 0, kept = 0;
    for (uint32_t k = 0; k < D.nimages && idx < 0; k++) if (D.images[k].img == img) idx = (int)k;
    if (idx >= 0) {
        (void)rev_name((uint32_t)idx, 0);   /* the sorted exports */
        const dimg *im = &D.images[idx];
        for (size_t k = 0; k < im->nrev; k++) {
            const rev_entry *ti = &im->rev[k];
            if (strncmp(ti->name, "__ZTI", 5)) continue;
            char zts[1024];
            if ((size_t)snprintf(zts, sizeof zts, "__ZTS%s", ti->name + 5) >= sizeof zts) continue;
            uint64_t loc = ti->addr + 8, own = 0;
            for (size_t j = 0; j < im->nrev && !own; j++) if (!strcmp(im->rev[j].name, zts)) own = im->rev[j].addr;
            /* only a type_info whose name pointer is this image's own _ZTS (bound and slid) */
            if (!own || !mapped_span(loc, 8) || owner_of(loc) != idx ||
                (*(const uint64_t *)at(loc) & ((1ull << 56) - 1)) != own + (uint64_t)D.slide)
                continue;
            bool ok = false;
            uint64_t v = bind(native_lib, zts, &ok);
            if (!ok || !v) {
                if (!kept++ || hao_verbose) {
                    /* not silently wrong: say which identity is split */
                    hao_diag_prefix(HAO_DIAG);
                    fprintf(HAO_DIAG, "%s: native %s has no %s: an exception of this type thrown by native code is not "
                                      "caught by its exact type in guest code\n", img->path, native_lib, zts + 1);
                }
                continue;
            }
            make_writable(loc, 8);
            *(uint64_t *)at(loc) = v;
            fixed_add(loc);
            n++;
        }
    }
    if (hao_verbose)
        fprintf(stderr, "hao: dsc: %s: %d type_info names shared with native %s, %d guest-only\n", img->path, n, native_lib, kept);
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return n;
}

/* hash slot of pc in D.redir (pc != 0) */
static size_t redir_find(uint64_t pc)
{
    size_t k = (pc >> 2) & (D.capredir - 1);
    while (D.redir[k].pc && D.redir[k].pc != pc) k = (k + 1) & (D.capredir - 1);
    return k;
}

static void redir_add(uint64_t pc, uint64_t native, uint64_t x1)
{
    if (2 * (D.nredir + 1) > D.capredir) {
        size_t oc = D.capredir;
        redir_t *old = D.redir;
        D.capredir = oc ? 2 * oc : 256;
        D.redir = hao_alloc_check(calloc(D.capredir, sizeof *D.redir), "shared cache tables");
        for (size_t k = 0; k < oc; k++) if (old[k].pc) D.redir[redir_find(old[k].pc)] = old[k];
        free(old);
    }
    size_t k = redir_find(pc);
    if (!D.redir[k].pc) D.nredir++;
    D.redir[k] = (redir_t){pc, native, x1};
}

/* objc_msgSend stub at unslid t, as the cache builder emits it
 * (adrp x1, sel@PAGE; add x1, x1, sel@PAGEOFF; b _objc_msgSend...): the
 * native selector and the branch target, or false. */
static bool decode_objc_stub(uint64_t t, uint64_t *sel, uint64_t *target)
{
    if (!mapped_span(t, 12)) return false;
    const uint32_t *w = code_at(t);
    if (!is_adrp(w[0]) || (w[0] & 31) != 1 || (w[1] & 0xffc003ff) != 0x91000021 || (w[2] & 0xfc000000) != 0x14000000) return false;
    uint64_t s = adrp_target(t, w[0]) + ((w[1] >> 10) & 0xfff);
    int64_t off = (int64_t)(w[2] & 0x3ffffff);
    if (off & (1 << 25)) off -= 1 << 26;
    uint64_t b = t + 8 + (uint64_t)(off * 4);
    int o = owner_of(b);
    const char *name = o >= 0 && !D.images[o].img ? rev_name((uint32_t)o, b) : NULL;
    if (!name || strncmp(name, "_objc_msgSend", 13)) return false;
    /* the selector string: NUL-terminated inside the cache's mappings */
    const dmap *m = map_of(s);
    if (!m) return false;
    uint64_t left = m->addr + m->size - s, lim = left < 4096 ? left : 4096;
    const char *str = at(s);
    if (strnlen(str, lim) == lim) return false;
    *sel = (uint64_t)(uintptr_t)sel_registerName(str);
    *target = b;
    return true;
}

/* A guest branch to a cache address outside every guest image: a stub
 * island, an objc_msgSend stub or a direct BL into a native-backed library. */
bool hao_dsc_redirect(uint64_t pc, uint64_t *native, uint64_t *x1)
{
    *x1 = 0;
    if (__atomic_load_n(&D.nloaded, __ATOMIC_ACQUIRE) == 0) return false;
    uint64_t t = pc - (uint64_t)D.slide;
    if (t < D.start || t - D.start >= D.size) return false;
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    bool ok = false;
    if (D.capredir) {
        size_t k = redir_find(pc);
        if (D.redir[k].pc) { *native = D.redir[k].native; *x1 = D.redir[k].x1; ok = true; }
    }
    if (!ok) {
        const dmap *m = map_of(t);
        uint64_t v = 0, slot = 0, target = t, sel = 0;
        if (decode_objc_stub(t, &sel, &target)) {
            /* the stub sets x1, then branches: x1 is the native selector */
        } else if (m && (m->flags & MAP_TEXT_STUBS) && decode_island(t, &slot, &target) && slot) {
            fix_slot(slot);
            target = 0;
            if (fixed_has(slot)) v = *(const uint64_t *)at(slot);
        }
        if (target) {
            int o = owner_of(target);
            if (o >= 0 && !D.images[o].img) {
                v = resolve_target(target);
                if (v == target + (uint64_t)D.slide) v = 0;
            }
        }
        if (hao_verbose > 1)
            fprintf(stderr, "hao: dsc: branch to %#llx (slot %#llx target %#llx%s) -> %#llx\n", (unsigned long long)t,
                    (unsigned long long)slot, (unsigned long long)target, sel ? ", objc_msgSend stub" : "", (unsigned long long)v);
        /* still an arm64e address outside guest code: not resolvable */
        if (v && v - (uint64_t)D.slide - D.start < D.size && !hao_image_for_code(v)) v = 0;
        if (v) {
            redir_add(pc, v, sel);
            *native = v;
            *x1 = sel;
            ok = true;
        }
    }
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return ok;
}

/* ------------------------------------------------------ ObjC support */

uint64_t hao_dsc_selector_base(void)
{
    return D.state > 0 && D.selbase ? D.selbase + (uint64_t)D.slide : 0;
}

bool hao_dsc_foreign(uint64_t a)
{
    if (D.state <= 0) return false;
    uint64_t t = a - (uint64_t)D.slide;
    return t >= D.start && t - D.start < D.size && !hao_image_for_addr(a);
}

uint64_t hao_dsc_load_ptr(uint64_t a)
{
    if (D.state <= 0 || (a & 7)) return 0;
    uint64_t t = a - (uint64_t)D.slide;
    if (t < D.start || t - D.start >= D.size || !mapped_span(t, 8)) return 0;
    uint64_t raw = *(const uint64_t *)at(t);
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    bool rewritten = fixed_has(t);
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    if (rewritten) return raw;
    const dmap *m = map_of(t);
    uint64_t target, high8;
    if (!raw || !m || !m->slide || m->slide_size < 24 || !decode_ptr(m, raw, &target, &high8)) return raw;
    return target + (uint64_t)D.slide;
}

bool hao_dsc_bind_slot(uint64_t a)
{
    if (D.state <= 0 || (a & 7) || !D.bind) return false;
    uint64_t t = a - (uint64_t)D.slide;
    if (t < D.start || t - D.start >= D.size || !mapped_span(t, 8)) return false;
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    fix_slot(t);
    bool ok = fixed_has(t);
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return ok;
}

uint64_t hao_dsc_bound_ptr(uint64_t a)
{
    if (D.state <= 0 || (a & 7)) return 0;
    uint64_t t = a - (uint64_t)D.slide;
    if (t < D.start || t - D.start >= D.size || !mapped_span(t, 8)) return 0;
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    bool rewritten = fixed_has(t);
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return rewritten ? *(const uint64_t *)at(t) : 0;
}

/* Executable room of size bytes within rel32 reach of [lo, hi) (slid addresses) for Swift metadata island
 * entries (swiftmeta.c), carved out of the cache's own reservation where no mapping of the cache lies (the
 * arm64e cache leaves a few MB between its subcaches' regions: 42 gaps of 0.48 GB in 27.2's). The cache's
 * images lie inside its 7 GB reservation, so a free range outside it is more than 2 GB away from most of them
 * (hao_map_near's search found none for ConnectedMode, ULPNHeuristicsClientFramework, CoreRepairUI,
 * AutoUnlockDaemon). The piece is unmapped and asked for again by address with MAP_JIT (no MAP_FIXED with
 * it on x86_64), then checked; a piece another thread took in between is not used. NULL if none fits. */
static struct { uint64_t lo, hi; } carved[256];
static int ncarved;
static int cmp_u64pair(const void *a, const void *b)
{
    const uint64_t *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}

void *hao_dsc_island(uint64_t lo, uint64_t hi, uint64_t size)
{
    void *res = NULL;
    hao_crit_enter();
    pthread_mutex_lock(&dsc_lock);
    uint64_t (*used)[2] = D.state > 0 ? malloc(sizeof *used * (size_t)(D.nmaps + ncarved + 1)) : NULL;
    if (!used || ncarved >= (int)(sizeof carved / sizeof *carved)) goto out;
    int nu = 0;
    for (int k = 0; k < D.nmaps; k++) { used[nu][0] = D.maps[k].addr; used[nu++][1] = D.maps[k].addr + D.maps[k].size; }
    for (int k = 0; k < ncarved; k++) { used[nu][0] = carved[k].lo; used[nu++][1] = carved[k].hi; }
    qsort(used, (size_t)nu, sizeof *used, cmp_u64pair);
    uint64_t pg = (uint64_t)getpagesize(), prev = D.start, end = D.start + D.size;
    for (int k = 0; k <= nu && !res; k++) {
        uint64_t g = (prev + pg - 1) & ~(pg - 1), ge = (k < nu ? used[k][0] : end) & ~(pg - 1);
        if (k < nu && used[k][1] > prev) prev = used[k][1];
        if (ge <= g || ge - g < size) continue;
        /* the gap's end nearest to the fields */
        uint64_t cand[2] = {g, ge - size};
        for (int c = 0; c < 2 && !res; c++) {
            uint64_t a = cand[c] + (uint64_t)D.slide;
            if (!hao_near_range(a, lo, hi, size)) continue;
            munmap((void *)(uintptr_t)a, size);
            void *p = mmap((void *)(uintptr_t)a, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
            if (p == (void *)(uintptr_t)a) {
                carved[ncarved].lo = cand[c];
                carved[ncarved++].hi = cand[c] + size;
                res = p;
                break;
            }
            if (p != MAP_FAILED) munmap(p, size);
            /* the reservation back, if nobody took the range */
            mach_vm_address_t r = a;
            if (mach_vm_allocate(mach_task_self(), &r, size, VM_FLAGS_FIXED) == KERN_SUCCESS)
                mprotect((void *)(uintptr_t)r, size, PROT_NONE);
        }
    }
out:
    free(used);
    pthread_mutex_unlock(&dsc_lock);
    hao_crit_leave();
    return res;
}

void hao_dsc_writable(uint64_t a, uint64_t n)
{
    uint64_t t = a - (uint64_t)D.slide;
    if (D.state > 0 && t >= D.start && t - D.start < D.size && n <= D.size - (t - D.start)) make_writable(t, n);
}
