/*
 * hao — guest code bytes: the accessor's slow path, the writer, and
 * HAO_CODE_ALIAS (include/hao/code.h).
 *
 * HAO_CODE_ALIAS=1 checks that every hao reader of guest code goes through
 * hao_code_*: each image gets an alias of its executable segments (on-disk
 * images: the file mapped a second time, read-only, at another address;
 * shared-cache images: the cache's mappings mapped a second time, dsc.c),
 * hao_code_ptr returns the alias, and every full page of the image's
 * instruction sections at the guest address is made PROT_NONE. A read of
 * such a page then faults:
 *  - from translated code or the interpreter (host_scratch[HAO_SCRATCH_HELPER]
 *    names the guest instruction it runs) it is the guest's own load of its
 *    code, a jump table in __text: the page becomes readable and the load is
 *    resumed, counted (guest_pages);
 *  - from anywhere else it is a reader that bypassed the accessor: one line
 *    names the address and the reader, and the fault takes its course
 *    (bypasses).
 * The two mappings hold the same bytes, except where hao_code_write changed
 * both, so the program behaves as without the switch. --stats prints the
 * `hao-code:` line. The page a bypass first touches after a guest load
 * opened it is not caught: guest loads of code are rare (jump tables).
 */
#include "hao/code.h"
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* Self-contained over macho.c (the loader tests link the two alone): the
 * fault handler's classification and report are runtime.c's. */
int hao_code_aliased;
bool hao_code_alias_mode;

const void *hao_code_ptr_slow(uint64_t a)
{
    return hao_code_ptr_im(hao_image_for_code(a), a);
}

const void *hao_code_view_slow(uint64_t a, size_t n)
{
    hao_image *im = hao_image_for_code(a);
    return im && n && hao_image_for_code(a + n - 1) == im ? hao_code_ptr_im(im, a) : (const void *)(uintptr_t)a;
}

/* ------------------------------------------------------------ poisoned pages */

#define PG 0x1000ull   /* host page size (x86_64) */
#define POISON_MAX 8192
typedef struct {
    uint64_t lo, hi;            /* page aligned, at the guest address */
    const hao_image *img;
    uint8_t *open;              /* one byte per page: made readable again by a guest load */
    int ready;                  /* published (1) or dropped (0) */
} poison_t;
static poison_t poison[POISON_MAX];
unsigned hao_code_npoison;
#define npoison hao_code_npoison
static uint64_t st_images, st_pages, st_guest_pages, st_bypasses, st_writes;

static poison_t *poison_of(uint64_t a)
{
    unsigned n = __atomic_load_n(&npoison, __ATOMIC_ACQUIRE);
    if (n > POISON_MAX) n = POISON_MAX;
    for (unsigned k = 0; k < n; k++) {
        poison_t *e = &poison[k];
        if (__atomic_load_n(&e->ready, __ATOMIC_ACQUIRE) && a >= e->lo && a < e->hi) return e;
    }
    return NULL;
}

/* ------------------------------------------------------------------ writer */

/* The protection of the page at p now (-1: not mapped). */
static int page_prot(uint64_t p)
{
    mach_vm_address_t r = p;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS ||
        r > p)
        return -1;
    return info.protection & (PROT_READ | PROT_WRITE | PROT_EXEC);
}

/* n bytes at a, page by page: writable for the copy, then the protection
 * the page had (HAO_CODE_ALIAS: PROT_NONE while poisoned, readable once a
 * guest load opened it). False when a page could not be made writable: the
 * pages before it are written. A page that cannot be made read-only again
 * stops the process: guest code must not stay writable. */
static bool write_pages(uint64_t a, const void *src, size_t n, bool poisoned)
{
    for (uint64_t at = a, end = a + n; at < end;) {
        uint64_t p = at & ~(PG - 1), k = end - at < p + PG - at ? end - at : p + PG - at;
        int prot = page_prot(p);
        if (prot < 0 || mprotect((void *)(uintptr_t)p, PG, PROT_READ | PROT_WRITE)) return false;
        memcpy((void *)(uintptr_t)at, (const uint8_t *)src + (at - a), k);
        poison_t *e = poisoned ? poison_of(p) : NULL;
        if (e) prot = __atomic_load_n(&e->open[(p - e->lo) / PG], __ATOMIC_ACQUIRE) ? PROT_READ : PROT_NONE;
        if (mprotect((void *)(uintptr_t)p, PG, prot)) {
            fprintf(stderr, "hao: guest code page %#llx cannot be made read-only again; stopping\n", (unsigned long long)p);
            abort();
        }
        at += k;
    }
    return true;
}

bool hao_code_write(uint64_t a, int64_t delta, const void *src, size_t n)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* two writers of one page */
    if (!n) return true;
    if (a + n < a || (delta && a + (uint64_t)delta + n < a + (uint64_t)delta)) return false;
    pthread_mutex_lock(&lock);
    bool ok = write_pages(a, src, n, true) && (!delta || write_pages(a + (uint64_t)delta, src, n, false));
    pthread_mutex_unlock(&lock);
    if (ok) __atomic_fetch_add(&st_writes, 1, __ATOMIC_RELAXED);
    return ok;
}

/* ------------------------------------------------------------------ aliases */

bool hao_code_alias_wanted(void) { return hao_code_alias_mode; }

bool hao_code_alias_reserve(hao_image *img)
{
    void *r = mmap(NULL, img->vmsize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r == MAP_FAILED) return false;
    img->code_alias = (uint64_t)(uintptr_t)r;
    img->code_alias_size = img->vmsize;
    return true;
}

bool hao_code_alias_segment(hao_image *img, uint64_t addr, uint64_t filesize, uint64_t vmsize, int fd, off_t off,
                            const void *bytes)
{
    if (!img->code_alias) return true;
    uint64_t at = img->code_alias + (addr - img->base);
    uint64_t fpage = (filesize + PG - 1) & ~(PG - 1);
    if (filesize) {
        /* the loader's own source: the file, or the copy it checked (a file somebody else may write) */
        void *m = mmap((void *)(uintptr_t)at, bytes ? fpage : filesize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_FIXED | (bytes ? MAP_ANON : 0), bytes ? -1 : fd, bytes ? 0 : off);
        if (m == MAP_FAILED) return false;
        if (bytes) memcpy(m, bytes, filesize);
        uint64_t end = fpage < vmsize ? fpage : vmsize;
        if (!bytes && end > filesize) memset((void *)(uintptr_t)(at + filesize), 0, end - filesize);   /* as hao_load_image */
        mprotect((void *)(uintptr_t)at, fpage, PROT_READ);
    }
    if (vmsize > fpage &&
        mmap((void *)(uintptr_t)(at + fpage), vmsize - fpage, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0) == MAP_FAILED)
        return false;
    return true;
}

void hao_code_alias_publish(hao_image *img, int64_t delta)
{
    img->code_delta = delta;
    if (delta) {
        __atomic_fetch_add(&st_images, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&hao_code_aliased, 1, __ATOMIC_RELEASE);
    }
}

void hao_code_poison(hao_image *img)
{
    if (!hao_code_alias_mode || !img->code_delta) return;
    for (int s = 0; s < img->nsects; s++) {
        if (!(img->sects[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))) continue;
        uint64_t lo = (img->sects[s].addr + PG - 1) & ~(PG - 1), hi = (img->sects[s].addr + img->sects[s].size) & ~(PG - 1);
        if (hi <= lo || hao_image_for_code(lo) != img || hao_image_for_code(hi - 1) != img) continue;
        unsigned k = __atomic_load_n(&npoison, __ATOMIC_RELAXED);
        if (k >= POISON_MAX) break;
        uint8_t *open = mmap(NULL, (hi - lo) / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (open == MAP_FAILED) continue;
        if (mprotect((void *)(uintptr_t)lo, hi - lo, PROT_NONE)) { munmap(open, (hi - lo) / PG); continue; }
        poison[k] = (poison_t){lo, hi, img, open, 0};
        __atomic_store_n(&poison[k].ready, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&npoison, k + 1, __ATOMIC_RELEASE);   /* the loaders are serialised */
        st_pages += (hi - lo) / PG;
    }
}

void hao_code_release(hao_image *img, bool keep_range)
{
    unsigned n = __atomic_load_n(&npoison, __ATOMIC_ACQUIRE);
    for (unsigned k = 0; k < n && k < POISON_MAX; k++)
        if (poison[k].img == img) __atomic_store_n(&poison[k].ready, 0, __ATOMIC_RELEASE);
    if (img->code_alias_size) {
        /* readers that still hold the image read nothing, as at the guest address */
        if (keep_range) mprotect((void *)(uintptr_t)img->code_alias, img->code_alias_size, PROT_NONE);
        else munmap((void *)(uintptr_t)img->code_alias, img->code_alias_size);
    }
}

int hao_code_poison_fault(uint64_t addr, bool write, bool guest, const hao_image **img)
{
    poison_t *e = poison_of(addr);
    if (!e) return 0;
    *img = e->img;
    uint64_t page = addr & ~(PG - 1);
    uint8_t *open = &e->open[(page - e->lo) / PG];
    /* opened by another thread meanwhile: read again (a store faults again, on the readable page) */
    if (__atomic_load_n(open, __ATOMIC_ACQUIRE)) return write ? 0 : 1;
    if (!guest) {
        __atomic_fetch_add(&st_bypasses, 1, __ATOMIC_RELAXED);
        return -1;
    }
    /* the guest's own load of its code (a jump table): natively readable */
    if (mprotect((void *)(uintptr_t)page, PG, PROT_READ)) return 0;
    __atomic_store_n(open, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&st_guest_pages, 1, __ATOMIC_RELAXED);
    return 1;
}

void hao_code_stats(void)
{
    if (!hao_code_alias_mode) return;
    fprintf(stderr, "hao-code: alias images=%llu poisoned_pages=%llu guest_pages=%llu bypasses=%llu writes=%llu\n",
            (unsigned long long)st_images, (unsigned long long)st_pages, (unsigned long long)st_guest_pages,
            (unsigned long long)st_bypasses, (unsigned long long)st_writes);
}
