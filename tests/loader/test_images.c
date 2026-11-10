/*
 * hao_images under concurrent loads (src/macho.c). hao_image_for_code,
 * hao_image_for_addr and hao_is_guest_function run lock-free on every
 * thread (translation, native calls, NX faults, signal delivery) while the
 * loader, on another thread, adds images, and takes failed loads off the
 * list again. Built with ASan.
 *   1. held: a lookup's result stays readable after its image is released
 *      (hao_unload_image, and a load that fails after the image was listed:
 *      a corrupted import ordinal fails in the chained-fixup pass): its
 *      function starts, export trie and dependency names are not freed.
 *   2. stress: four reader threads read through the image the writer is
 *      about to release (unload, or fail after listing) while it does, and
 *      look up every image that stays loaded, which they must find every
 *      time.
 *   test_images CORPUS_DIR SCRATCH_DIR [ITERATIONS [held|stress]]
 */
#include "hao/macho.h"
#include <fcntl.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
static char good_path[1100], bad_path[1100];

static uint64_t resolver(hao_image *img, int ord, const char *name, bool weak, bool *found)
{
    (void)img; (void)ord; (void)name; (void)weak;
    *found = true;
    return 0x1234;
}

static uint8_t *slurp(const char *dir, const char *name, size_t *n)
{
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "test_images: %s: missing\n", p); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    uint8_t *d = malloc(*n);
    if (fread(d, 1, *n, f) != *n) exit(2);
    fclose(f);
    return d;
}

static void spill(const char *path, const uint8_t *d, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, d, n) != (ssize_t)n) { perror(path); exit(2); }
    close(fd);
}

static uint32_t rd32(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static void wr32(uint8_t *d, size_t o, uint32_t v) { memcpy(d + o, &v, 4); }

static size_t find_cmd(const uint8_t *d, uint32_t cmd)
{
    uint32_t ncmds = rd32(d, 16);
    size_t off = 32;
    for (uint32_t k = 0; k < ncmds; k++) {
        if (rd32(d, off) == cmd) return off;
        off += rd32(d, off + 4);
    }
    fprintf(stderr, "test_images: load command %#x missing\n", cmd);
    exit(2);
}

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* ------------------------------------------------------------ held */
static hao_image *seen;   /* set by the mapped callback: listed, not yet bound */
static void on_mapped(hao_image *img, void *ctx) { (void)ctx; seen = img; }

/* Everything a lookup reaches through an image, summed (ASan checks the reads). */
static uint64_t touch(const hao_image *im)
{
    uint64_t s = 0;
    for (size_t k = 0; k < im->nfstarts; k++) s += im->fstarts[k];
    for (uint32_t k = 0; k < im->exports_trie_size; k++) s += im->exports_trie[k];
    for (int k = 0; k < im->ndeps; k++) s += strlen(im->deps[k]);
    for (int k = 0; k < im->nrpaths; k++) s += strlen(im->rpaths[k]);
    return s;
}

static void held(void)
{
    char err[512] = "";
    hao_image *img = hao_load_image_cb(good_path, resolver, on_mapped, NULL, err, sizeof err);
    if (!img || !img->nfstarts) { fprintf(stderr, "test_images: seed did not load: %s\n", err); exit(2); }
    uint64_t f = img->fstarts[0];
    hao_image *h = hao_image_for_code(f);
    uint64_t want = touch(h);
    hao_unload_image(img);
    check(!hao_image_for_code(f) && !hao_image_for_addr(f) && !hao_is_guest_function(f), "unloaded image: lookups miss it");
    check(touch(h) == want, "unloaded image: a held lookup result is still readable");

    seen = NULL;
    img = hao_load_image_cb(bad_path, resolver, on_mapped, NULL, err, sizeof err);
    if (img || !seen || !strstr(err, "library ordinal")) { fprintf(stderr, "test_images: bad seed: %s\n", err); exit(2); }
    h = seen;
    check(!hao_image_for_code(h->base + 0x458) && !hao_image_for_addr(h->base), "failed load: lookups miss it");
    check(touch(h) > 0 && h->nfstarts > 0, "failed load: a held listed image is still readable");
    check(__atomic_load_n(&hao_images, __ATOMIC_ACQUIRE) == NULL, "image list empty again");
}

/* ---------------------------------------------------------- stress
 * The writer makes each image the readers' current target and waits until
 * they have found it, then releases it (an unload, or the failing bind
 * after its mapped callback) while they keep reading through it. */
#define KEEP 64
static uint64_t cur, cur_hits;         /* the image being released, and lookups that found it */
static uint64_t kept_fn[KEEP];         /* images that stay loaded */
static hao_image *kept_img[KEEP];
static int nkept, stop;
static uint64_t lookups, missing, wrong, waits_timed_out;

static void *reader(void *arg)
{
    (void)arg;
    uint64_t n = 0, miss = 0, bad = 0, sink = 0;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        for (int r = 0; r < 16; r++) {
            uint64_t a = __atomic_load_n(&cur, __ATOMIC_ACQUIRE);
            hao_image *im = hao_image_for_code(a);
            if (im) { __atomic_fetch_add(&cur_hits, 1, __ATOMIC_RELAXED); sink += touch(im); }
            sink += hao_is_guest_function(a);
            if ((im = hao_image_for_addr(a))) sink += im->nfstarts + touch(im);
            n++;
        }
        int nk = __atomic_load_n(&nkept, __ATOMIC_ACQUIRE);
        for (int k = 0; k < nk; k++) {
            hao_image *im = hao_image_for_code(kept_fn[k]);
            if (!im) miss++;
            else if (im != kept_img[k]) bad++;
            if (!hao_is_guest_function(kept_fn[k])) miss++;
        }
    }
    __atomic_fetch_add(&lookups, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&missing, miss, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wrong, bad, __ATOMIC_RELAXED);
    return (void *)(uintptr_t)(sink & 1);
}

static void target(uint64_t a)
{
    uint64_t h0 = __atomic_load_n(&cur_hits, __ATOMIC_ACQUIRE);
    __atomic_store_n(&cur, a, __ATOMIC_RELEASE);
    for (long spin = 0; __atomic_load_n(&cur_hits, __ATOMIC_ACQUIRE) < h0 + 8; spin++)
        if (spin > 50000000) { waits_timed_out++; break; }
}

static void on_mapped_target(hao_image *img, void *ctx) { (void)ctx; seen = img; target(img->fstarts[0]); }

static void stress(int iters)
{
    pthread_t th[4];
    for (int k = 0; k < 4; k++) pthread_create(&th[k], NULL, reader, NULL);
    int loads = 0, failed = 0, unloaded = 0;
    char err[512];
    for (int it = 0; it < iters; it++) {
        hao_image *img = hao_load_image_cb(good_path, resolver, on_mapped, NULL, err, sizeof err);
        if (!img) { fprintf(stderr, "test_images: load %d: %s\n", it, err); exit(2); }
        loads++;
        if (it % (iters / KEEP + 1) == 0 && nkept < KEEP) {
            kept_fn[nkept] = img->fstarts[0];
            kept_img[nkept] = img;
            __atomic_store_n(&nkept, nkept + 1, __ATOMIC_RELEASE);
        } else {
            target(img->fstarts[0]);
            hao_unload_image(img);
            unloaded++;
        }
        seen = NULL;
        if (hao_load_image_cb(bad_path, resolver, on_mapped_target, NULL, err, sizeof err) || !seen) {
            fprintf(stderr, "test_images: bad load %d did not fail after listing\n", it);
            exit(2);
        }
        failed++;
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);
    int listed = 0;
    for (hao_image *im = __atomic_load_n(&hao_images, __ATOMIC_ACQUIRE); im; im = __atomic_load_n(&im->next, __ATOMIC_ACQUIRE)) listed++;
    printf("stress: loads=%d unloaded=%d failed_after_listing=%d kept=%d listed=%d lookups=%llu found_released=%llu "
           "missing=%llu wrong=%llu waits_timed_out=%llu\n",
           loads, unloaded, failed, nkept, listed, (unsigned long long)lookups, (unsigned long long)cur_hits,
           (unsigned long long)missing, (unsigned long long)wrong, (unsigned long long)waits_timed_out);
    check(listed == nkept && !missing && !wrong && !waits_timed_out, "stress: every loaded image found by every reader, list intact");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_images CORPUS_DIR SCRATCH_DIR [ITERATIONS [held|stress]]\n"); return 2; }
    int iters = argc > 3 ? atoi(argv[3]) : 1500;
    size_t n;
    uint8_t *lib = slurp(argv[1], "lib-arm64e", &n);
    snprintf(good_path, sizeof good_path, "%s/images-good.dylib", argv[2]);
    snprintf(bad_path, sizeof bad_path, "%s/images-bad.dylib", argv[2]);
    spill(good_path, lib, n);
    /* the first import's library ordinal past the dependencies: rejected by
     * the chained-fixup pass, which runs after the image is listed */
    size_t cf = find_cmd(lib, LC_DYLD_CHAINED_FIXUPS), co = rd32(lib, cf + 8), io = rd32(lib, co + 8);
    wr32(lib, co + io, (rd32(lib, co + io) & ~0xffu) | 0x7f);
    spill(bad_path, lib, n);
    const char *only = argc > 4 ? argv[4] : "";
    if (strcmp(only, "stress")) held();
    if (strcmp(only, "held")) stress(iters);
    printf("test_images: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
