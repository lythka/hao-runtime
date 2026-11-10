/*
 * Fuzz target: the guest Mach-O loader (src/macho.c). Each input is written
 * to a file and loaded the way hao loads a guest: slice choice, load
 * commands, segment mapping, chained fixups (odd-sized inputs map only), the
 * exports trie and function starts. A loaded image is then touched where
 * the runtime reads it (every section, initializer and TLV range, exports,
 * function starts) and unloaded. Built with ASan and coverage by
 * `make fuzz`; run by tests/fuzz/run.sh.
 */
#include "hao/macho.h"
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char path[1024];

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    const char *t = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/hao-fuzz-macho.%d", t && *t ? t : "/tmp", getpid());
    return 0;
}

static uint64_t resolver(hao_image *img, int ord, const char *name, bool weak, bool *found)
{
    (void)img; (void)ord; (void)weak;
    size_t n = strlen(name);                       /* the name must be a NUL-terminated string */
    *found = n & 1;
    return 0x10000 + n;
}

static volatile uint8_t sink;
static void touch(uint64_t a, uint64_t n)
{
    if (!n) return;
    sink ^= *(volatile uint8_t *)(uintptr_t)a;
    sink ^= *(volatile uint8_t *)(uintptr_t)(a + n - 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* the code-signature reader (src/libval.c) on the input's first SuperBlob
     * (a signed seed's LC_CODE_SIGNATURE), copied to its exact size */
    for (size_t k = 0; k + 4 <= size; k++)
        if (data[k] == 0xfa && data[k + 1] == 0xde && data[k + 2] == 0x0c && data[k + 3] == 0xc0) {
            uint8_t *b = malloc(size - k);
            memcpy(b, data + k, size - k);
            hao_codesig cs;
            char w[200];
            if (hao_codesig_parse(b, size - k, &cs, w, sizeof w)) (void)hao_codesig_library_validated(&cs);
            free(b);
            break;
        }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) abort();
    if (write(fd, data, size) != (ssize_t)size) abort();
    close(fd);
    char err[512];
    hao_image *img = size & 1 ? hao_map_image_only(path, err, sizeof err) : hao_load_image(path, resolver, err, sizeof err);
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("FUZZ_VERBOSE") != NULL;
    if (verbose) fprintf(stderr, "fuzz_macho: %zu bytes: %s\n", size, img ? "loaded" : err);
    if (!img) return 0;
    for (int k = 0; k < img->nsects; k++) touch(img->sects[k].addr, img->sects[k].size);
    touch(img->init_ptrs, img->init_ptrs_count * 8);
    touch(img->init_offs, img->init_offs_count * 4);
    touch(img->tlv_desc, img->tlv_desc_count * 24);
    touch(img->tlv_init_start, img->tlv_init_size);
    if (img->entry) touch(img->entry, 4);
    static const char *names[] = {"_main", "__mh_execute_header", "_foo", "_foobar", "_foobaz", "_fizz", "_helper",
                                  "_exported_value", "", "_f", "_foobarbaz"};
    uint64_t a;
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
        int ord = -1;
        const char *imp = NULL;
        /* an absolute symbol's value is arbitrary; a re-export names a dependency and a string in the trie */
        if (hao_image_export_info(img, names[k], &a, &ord, &imp) == HAO_EXPORT_REEXPORT &&
            (ord < 0 || ord > img->ndeps || (imp && (imp < (const char *)img->exports_trie ||
                                                     imp + strlen(imp) >= (const char *)img->exports_trie + img->exports_trie_size))))
            abort();
    }
    for (size_t k = 0; k < img->nfstarts && k < 64; k++) {
        if (img->fstarts[k] < img->base || img->fstarts[k] >= img->base + img->vmsize) abort();
        (void)hao_is_guest_function(img->fstarts[k]);
    }
    (void)hao_image_section(img, NULL, "__text", NULL);
    for (int k = 0; k < img->ncode; k++) touch(img->code_lo[k], img->code_hi[k] - img->code_lo[k]);
    hao_unload_image(img);
    return 0;
}

/* Structure-aware mutation: rewrite one field of a load command, or a word
 * of the chained-fixups, exports or function-starts blob, or a fixup slot. */
static uint32_t rd32(const uint8_t *d, size_t n, size_t o) { uint32_t v = 0; if (o + 4 <= n) memcpy(&v, d + o, 4); return v; }

size_t hao_fuzz_custom_mutate(uint8_t *d, size_t n, size_t max, uint64_t r)
{
    (void)max;
    if (n < 32 || rd32(d, n, 0) != MH_MAGIC_64) return n;
    uint32_t ncmds = rd32(d, n, 16), sizeofcmds = rd32(d, n, 20);
    size_t off = 32, end = 32 + (size_t)sizeofcmds;
    if (end > n) end = n;
    size_t cmd_off[64], cmd_size[64], nc = 0;
    size_t blob_off[8], blob_size[8], nb = 0;
    size_t wseg_off[8], wseg_size[8], nw = 0;
    for (uint32_t k = 0; k < ncmds && nc < 64 && off + 8 <= end; k++) {
        uint32_t cmd = rd32(d, n, off), cs = rd32(d, n, off + 4);
        if (cs < 8 || off + cs > end) break;
        cmd_off[nc] = off; cmd_size[nc] = cs; nc++;
        if ((cmd == LC_DYLD_CHAINED_FIXUPS || cmd == LC_DYLD_EXPORTS_TRIE || cmd == LC_FUNCTION_STARTS) && nb < 8) {
            uint32_t o = rd32(d, n, off + 8), s = rd32(d, n, off + 12);
            if (o < n && s <= n - o && s) { blob_off[nb] = o; blob_size[nb] = s; nb++; }
        }
        if (cmd == LC_SEGMENT_64 && nw < 8 && (rd32(d, n, off + 60) & 2)) {   /* initprot & VM_PROT_WRITE */
            uint64_t fo, fs;
            memcpy(&fo, d + off + 40, 8); memcpy(&fs, d + off + 48, 8);
            if (fo < n && fs <= n - fo && fs >= 8) { wseg_off[nw] = (size_t)fo; wseg_size[nw] = (size_t)fs; nw++; }
        }
        off += cs;
    }
    static const uint32_t vals[] = {0, 1, 2, 3, 4, 7, 8, 0x10, 0x18, 0x20, 0x48, 0x50, 0xff, 0x100, 0xfff, 0x1000, 0x3fff,
                                    0x4000, 0xffff, 0x10000, 0x7fffffff, 0x80000000u, 0xfffffff0u, 0xffffffffu};
    uint32_t v = vals[(r >> 8) % (sizeof vals / sizeof *vals)];
    if ((r >> 40) % 4 == 0) v = rd32(d, n, 0) ^ (uint32_t)(r >> 13);
    size_t which = (size_t)(r % 4);
    if (which == 0 && nc) {
        size_t c = (size_t)((r >> 20) % nc);
        size_t o = cmd_off[c] + 4 * (size_t)((r >> 28) % (cmd_size[c] / 4));
        if (o + 4 <= n) memcpy(d + o, &v, 4);
    } else if (which == 1 && nb) {
        size_t b = (size_t)((r >> 20) % nb);
        size_t span = blob_size[b] < 512 ? blob_size[b] : 512;
        size_t o = blob_off[b] + (size_t)((r >> 28) % span);
        if ((r >> 50) & 1) { o &= ~(size_t)3; if (o + 4 <= n) memcpy(d + o, &v, 4); }
        else d[o] = (uint8_t)v;
    } else if (which == 2 && nw) {
        size_t s = (size_t)((r >> 20) % nw);
        size_t o = wseg_off[s] + 8 * (size_t)((r >> 28) % (wseg_size[s] / 8));
        uint64_t w;
        memcpy(&w, d + o, 8);
        w ^= 1ull << (32 + (r >> 50) % 32);                /* next, bind, auth, addend bits */
        memcpy(d + o, &w, 8);
    } else if (nc) {
        /* header fields: ncmds, sizeofcmds, flags, filetype */
        size_t o = 4 * (size_t)(1 + (r >> 20) % 6);
        memcpy(d + o, &v, 4);
    }
    return n;
}
