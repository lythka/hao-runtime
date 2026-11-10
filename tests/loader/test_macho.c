/*
 * Unit test of the validating Mach-O loader (src/macho.c). Starting from the
 * clang-built seeds in tests/fuzz/corpus/macho, each case corrupts one
 * field the way a hostile guest file would and checks that the load fails
 * with a message naming the problem (and, under ASan, without touching
 * memory it should not). The unmodified seeds must load and unload.
 *   test_macho CORPUS_DIR SCRATCH_DIR
 */
#include "hao/macho.h"
#include <fcntl.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;
static char scratch[1024], corpus[1024];

static uint8_t *slurp(const char *name, size_t *n)
{
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", corpus, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "test_macho: %s: missing\n", p); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    uint8_t *d = malloc(*n + 0x10000);
    if (fread(d, 1, *n, f) != *n) exit(2);
    fclose(f);
    return d;
}

static uint64_t resolver(hao_image *img, int ord, const char *name, bool weak, bool *found)
{
    (void)img; (void)ord; (void)name; (void)weak;
    *found = true;
    return 0x1234;
}

/* Load d[0..n) from a file; expect success (want == NULL) or an error containing want. */
static void expect(const char *what, const uint8_t *d, size_t n, const char *want)
{
    char path[1100], err[512] = "";
    snprintf(path, sizeof path, "%s/case.bin", scratch);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, d, n) != (ssize_t)n) { perror(path); exit(2); }
    close(fd);
    hao_image *img = hao_load_image(path, resolver, err, sizeof err);
    bool ok = want ? !img && strstr(err, want) : img != NULL;
    if (img && img->fd != -1) { ok = false; snprintf(err, sizeof err, "image fd %d left open", img->fd); }
    if (img) {
        struct stat st;
        stat(path, &st);
        if (img->dev != (uint64_t)st.st_dev || img->ino != (uint64_t)st.st_ino) { ok = false; snprintf(err, sizeof err, "dev/ino not recorded"); }
        hao_unload_image(img);
    }
    if (hao_images && !img) { ok = false; snprintf(err, sizeof err, "a failed load stayed on the image list"); }
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, ok ? "" : ": ", ok ? "" : (img ? "loaded" : err));
    if (ok) passes++; else fails++;
}

/* ------------------------------------------------- locating structures */
static uint32_t rd32(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static void wr32(uint8_t *d, size_t o, uint32_t v) { memcpy(d + o, &v, 4); }
static void wr64(uint8_t *d, size_t o, uint64_t v) { memcpy(d + o, &v, 8); }

/* offset of the k-th load command with this cmd (or of the segment named seg) */
static size_t find_cmd(const uint8_t *d, uint32_t cmd, const char *seg)
{
    uint32_t ncmds = rd32(d, 16);
    size_t off = 32;
    for (uint32_t k = 0; k < ncmds; k++) {
        if (rd32(d, off) == cmd && (!seg || !strncmp((const char *)d + off + 8, seg, 16))) return off;
        off += rd32(d, off + 4);
    }
    fprintf(stderr, "test_macho: command %#x %s not found\n", cmd, seg ? seg : "");
    exit(2);
}

/* An own synthetic image isolates opcode validation from the system linker.
 * Each malformed stream must fail before the image is registered/resolved. */
static unsigned legacy_calls;
static uint64_t legacy_resolve(hao_image *im, int ord, const char *name, bool weak, bool *found)
{
    (void)im;
    legacy_calls++;
    if (!strcmp(name, "_missing")) { *found = false; return 0; }
    *found = true;
    return 0x12340000ull + (uint64_t)(ord + 3) * 0x100 + (weak ? 8 : 0);
}

static void legacy_case(const char *what, const uint8_t *bytes, size_t size, int mode, const char *want,
                        uint64_t expect_value, unsigned expect_rebase, unsigned expect_bind)
{
    int variant=mode/4; mode%=4;
    uint8_t d[0x3000] = {0};
    struct mach_header_64 mh = {.magic=MH_MAGIC_64,.cputype=CPU_TYPE_ARM64,.filetype=MH_DYLIB,
                               .ncmds=4,.sizeofcmds=3*sizeof(struct segment_command_64)+sizeof(struct dyld_info_command)};
    memcpy(d,&mh,sizeof mh);
    size_t cursor=sizeof mh;
    for (unsigned k=0;k<3;k++) {
        struct segment_command_64 sg={.cmd=LC_SEGMENT_64,.cmdsize=sizeof sg,
            .vmaddr=0x100000000ull+k*0x1000,.vmsize=0x1000,.fileoff=k*0x1000,.filesize=0x1000,
            .maxprot=VM_PROT_READ|VM_PROT_WRITE,.initprot=VM_PROT_READ|(k==1?VM_PROT_WRITE:0)};
        strcpy(sg.segname,k==0?"__TEXT":k==1?"__DATA":"__LINKEDIT");
        memcpy(d+cursor,&sg,sizeof sg);cursor+=sizeof sg;
    }
    struct dyld_info_command di={.cmd=LC_DYLD_INFO_ONLY,.cmdsize=sizeof di};
    uint32_t *fields=&di.rebase_off;
    /* LC order: rebase, normal, weak, lazy. */
    fields[2*mode]=0x2000;fields[2*mode+1]=(uint32_t)size;
    if (variant==1) fields[2*mode]=0x3000;
    if (variant==4) {di.export_off=0x3000;di.export_size=1;}
    memcpy(d+cursor,&di,sizeof di);memcpy(d+0x2000,bytes,size);
    if (variant==2) {
        memcpy(d+cursor+sizeof di,&di,sizeof di);
        mh.ncmds++;mh.sizeofcmds+=sizeof di;memcpy(d,&mh,sizeof mh);
    }
    if (variant==3) {
        struct linkedit_data_command cf={.cmd=LC_DYLD_CHAINED_FIXUPS,.cmdsize=sizeof cf,.dataoff=0x2800,.datasize=1};
        memcpy(d+cursor+sizeof di,&cf,sizeof cf);
        mh.ncmds++;mh.sizeofcmds+=sizeof cf;memcpy(d,&mh,sizeof mh);
    }
    if (variant==5) {
        struct dylib_command dep={.cmd=LC_LOAD_DYLIB,.cmdsize=32,.dylib={.name={.offset=sizeof dep}}};
        memcpy(d+cursor+sizeof di,&dep,sizeof dep);
        memcpy(d+cursor+sizeof di+sizeof dep,"unit",5);
        mh.ncmds++;mh.sizeofcmds+=32;memcpy(d,&mh,sizeof mh);
    }
    for (unsigned k=0;k<128;k+=8) wr64(d,0x1000+k,0x100001080ull);
    char path[1100],err[512]="";
    snprintf(path,sizeof path,"%s/legacy.bin",scratch);
    FILE *f=fopen(path,"wb");if(!f || fwrite(d,1,sizeof d,f)!=sizeof d) exit(2);fclose(f);
    legacy_calls=0;
    hao_image *im=hao_load_image(path,legacy_resolve,err,sizeof err);
    bool ok=want ? !im && strstr(err,want) && !legacy_calls : im && im->n_rebase==expect_rebase && im->n_bind==expect_bind;
    if (ok && im) {
        if (variant==5 && im->dep_cxx[0]) ok=false;
        uint64_t value;memcpy(&value,(const void *)(uintptr_t)(im->base+0x1000),8);
        ok=ok && value==(expect_rebase ? im->base+0x1080 : expect_value);
    }
    if (im) hao_unload_image(im);
    if (hao_images) ok=false;
    printf("%s legacy %s%s%s\n",ok?"PASS":"FAIL",what,ok?"":": ",ok?"":err);
    ok?passes++:fails++;
}

static void legacy_tests(void)
{
    static const uint8_t rebase[]={0x11,0x21,0,0x51,0x60,1,0x70,8,0x80,2,8,0x40,0x30,0,0};
    legacy_case("all pointer rebase actions",rebase,sizeof rebase,0,NULL,0,5,0);
    legacy_case("rebase blob outside slice",rebase,sizeof rebase,4,"stream outside the slice",0,0,0);
    legacy_case("duplicate legacy command",rebase,sizeof rebase,8,"duplicate LC_DYLD_INFO",0,0,0);
    legacy_case("mixed chained and legacy",rebase,sizeof rebase,12,"both chained and legacy",0,0,0);
    legacy_case("export blob outside slice",rebase,sizeof rebase,16,"export trie outside",0,0,0);
    static const uint8_t bind[]={0x30,0x40,'_','u',0,0x51,0x71,0,0x90,0x60,0x7f,0xa0,0,0xb0,0xc0,2,0,0};
    legacy_case("all pointer bind actions and signed addend",bind,sizeof bind,1,NULL,0x12340300,0,5);
    static const uint8_t unused_cxx[]={0x11,0x40,'_','_','Z','u',0,0x40,'_','u',0,0x51,0x71,0,0x90,0};
    legacy_case("unused C++ state does not change dependency policy",unused_cxx,sizeof unused_cxx,21,NULL,0x12340400,0,1);
    static const uint8_t weak[]={0x40,'_','u',0,0x51,0x71,0,0x90,0};
    legacy_case("weak definition lookup ordinal",weak,sizeof weak,2,NULL,0x12340008,0,1);
    legacy_case("weak blob outside slice",weak,sizeof weak,6,"stream outside the slice",0,0,0);
    static const uint8_t lazy[]={0x30,0x40,'_','u',0,0x71,0,0x90,0,0x30,0x40,'_','u',0,0x71,8,0x90,0};
    legacy_case("multiple lazy records with implicit pointer type",lazy,sizeof lazy,3,NULL,0x12340300,0,2);
    legacy_case("lazy blob outside slice",lazy,sizeof lazy,7,"stream outside the slice",0,0,0);
    static const uint8_t backwards[]={0x51,0x30,0x40,'_','u',0,0x71,8,0x90,0x80,0xf0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1,0x90,0};
    legacy_case("unsigned wrapping backward offset",backwards,sizeof backwards,1,NULL,0x12340300,0,2);
    static const uint8_t absent[]={0x51,0x3e,0x41,'_','m','i','s','s','i','n','g',0,0x71,0,0x90,0};
    legacy_case("missing weak import is null",absent,sizeof absent,1,NULL,0,0,1);
#define BAD(name,mode,want,...) do { const uint8_t v[]={__VA_ARGS__}; legacy_case(name,v,sizeof v,mode,want,0,0,0); } while(0)
    BAD("truncated ULEB",0,"malformed LEB128",0x21,0x80);
    BAD("overflow ULEB",0,"malformed LEB128",0x21,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,2);
    BAD("truncated SLEB",1,"malformed LEB128",0x60,0x80);
    BAD("overflow SLEB",1,"malformed LEB128",0x60,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,2);
    BAD("unknown rebase opcode",0,"unknown rebase",0x90,0);
    BAD("unknown bind opcode",1,"unknown bind",0xe0,0);
    BAD("threaded bind refused",1,"threaded",0xd0,0);
    BAD("text rebase refused",0,"unsupported rebase type",0x12,0);
    BAD("text bind refused",1,"unsupported bind type",0x52,0);
    BAD("unknown symbol flags",1,"unsupported symbol flags",0x42,'_','u',0,0);
    BAD("strong definition refused",2,"strong-definition",0x48,'_','u',0,0);
    BAD("unterminated symbol",1,"symbol name",0x40,'_','u');
    BAD("unset segment",0,"segment not mapped",0x11,0x51,0);
    BAD("segment out of range",0,"segment not mapped",0x11,0x2f,0,0x51,0);
    BAD("readonly destination",0,"not writable",0x11,0x20,0,0x51,0);
    BAD("write straddles segment",0,"outside its segment",0x11,0x21,0xfc,0x1f,0x51,0);
    BAD("unset type",0,"type not set",0x21,0,0x51,0);
    BAD("unset regular bind type",1,"type not set",0x30,0x40,'_','u',0,0x71,0,0x90,0);
    BAD("unset weak bind type",2,"type not set",0x40,'_','u',0,0x71,0,0x90,0);
    BAD("unset bind symbol/ordinal",1,"symbol or library",0x51,0x71,0,0x90,0);
    BAD("positive library ordinal out of range",1,"ordinal out of range",0x11,0);
    BAD("special library ordinal out of range",1,"ordinal out of range",0x3c,0);
    BAD("missing DONE",0,"missing DONE",0x11,0x21,0,0x51);
    BAD("lazy record state reset",3,"segment not mapped",0x30,0x40,'_','u',0,0x71,0,0x90,0,0x90,0);
    BAD("nonzero trailing bytes",0,"after DONE",0,0x11);
    BAD("count bomb",0,"count exceeds",0x11,0x21,0,0x60,0xff,0xff,0xff,0xff,0x7f,0);
#undef BAD
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: test_macho CORPUS_DIR SCRATCH_DIR\n"); return 2; }
    snprintf(corpus, sizeof corpus, "%s", argv[1]);
    snprintf(scratch, sizeof scratch, "%s", argv[2]);
    legacy_tests();
    size_t n, ln, fn;
    uint8_t *seed = slurp("exec-arm64e", &n), *lib = slurp("lib-arm64e", &ln), *fat = slurp("fat-x86_64-arm64e", &fn);
    uint8_t *d = malloc(n + 0x10000);
#define FRESH() memcpy(d, seed, n)

    FRESH(); expect("seed exec-arm64e loads", d, n, NULL);
    { size_t a; uint8_t *x = slurp("exec-arm64", &a); expect("seed exec-arm64 loads", x, a, NULL); free(x); }
    expect("seed lib-arm64e loads", lib, ln, NULL);
    expect("seed fat-x86_64-arm64e loads", fat, fn, NULL);

    /* header and load commands */
    FRESH(); wr32(d, 20, (uint32_t)n); expect("sizeofcmds past the slice", d, n, "overrun the slice");
    FRESH(); wr32(d, 16, 100000); expect("ncmds cannot fit in sizeofcmds", d, n, "cannot fit");
    FRESH(); wr32(d, find_cmd(d, LC_UUID, NULL) + 4, 0); expect("cmdsize 0", d, n, "bad cmdsize");
    FRESH(); wr32(d, find_cmd(d, LC_UUID, NULL) + 4, 0x7ffffff8); expect("cmdsize past sizeofcmds", d, n, "bad cmdsize");
    FRESH(); wr32(d, find_cmd(d, LC_UUID, NULL) + 4, 12); expect("cmdsize not a multiple of 8", d, n, "bad cmdsize");
    FRESH(); wr32(d, find_cmd(d, LC_SEGMENT_64, "__DATA") + 64, 1000); expect("nsects overrun cmdsize", d, n, "do not fit");
    FRESH(); { size_t o = find_cmd(d, LC_LOAD_DYLIB, NULL); wr32(d, o + 8, rd32(d, o + 4) + 100); }
    expect("dylib name offset outside the command", d, n, "dylib name offset");

    /* segments */
    FRESH(); wr64(d, find_cmd(d, LC_SEGMENT_64, "__DATA") + 40, 0x100000000ull);
    expect("segment fileoff past the slice", d, n, "outside the slice");
    FRESH(); wr64(d, find_cmd(d, LC_SEGMENT_64, "__DATA") + 48, 0x5000);
    expect("segment filesize > vmsize", d, n, "> vmsize");
    FRESH(); { size_t o = find_cmd(d, LC_SEGMENT_64, "__DATA"); wr64(d, o + 32, 0x5000); wr64(d, o + 48, 0x5000); }
    expect("segment file range past the slice (slice_size bound)", d, n, "outside the slice");
    FRESH(); wr64(d, find_cmd(d, LC_SEGMENT_64, "__DATA") + 24, 0x100001000ull);
    expect("overlapping segments (MAP_FIXED would clobber)", d, n, "overlaps");
    FRESH(); wr64(d, find_cmd(d, LC_SEGMENT_64, "__DATA") + 24, 0x100002800ull);
    expect("segment vmaddr not page aligned", d, n, "not page aligned");
    FRESH(); wr64(d, find_cmd(d, LC_SEGMENT_64, "__DATA") + 32, 0xfffffffffffff000ull);
    expect("segment vm range wraps", d, n, "wraps");
    FRESH(); { size_t o = find_cmd(d, LC_SEGMENT_64, "__DATA") + 72; wr64(d, o + 32, 0x100009000ull); }
    expect("section outside its segment", d, n, "outside its segment");
    FRESH(); wr64(d, find_cmd(d, LC_SEGMENT_64, "__TEXT") + 40, 0x1000);
    expect("no segment maps the header", d, n, "mach header is not mapped");

    /* entry point and function starts */
    FRESH(); wr64(d, find_cmd(d, LC_MAIN, NULL) + 8, 0x2000); expect("entry point in data", d, n, "not an instruction");
    FRESH(); wr64(d, find_cmd(d, LC_MAIN, NULL) + 8, 0xffd); expect("entry point straddles __TEXT's end", d, n, "not an instruction");
    FRESH(); { size_t o = rd32(d, find_cmd(d, LC_FUNCTION_STARTS, NULL) + 8); d[o] = 0xff; d[o + 1] = 0xff; d[o + 2] = 0x7f; }
    expect("function start outside the image", d, n, "LC_FUNCTION_STARTS");
    FRESH(); wr32(d, find_cmd(d, LC_FUNCTION_STARTS, NULL) + 12, 0x7fffffff); expect("function starts past the slice", d, n, "function starts outside");

    /* chained fixups */
    size_t cf = find_cmd(seed, LC_DYLD_CHAINED_FIXUPS, NULL), co = rd32(seed, cf + 8), csz = rd32(seed, cf + 12);
    FRESH(); wr32(d, cf + 12, 0x7fffffff); expect("chained fixups past the slice", d, n, "outside the slice");
    FRESH(); wr32(d, co + 16, 0x01000000); expect("imports_count overruns the blob", d, n, "overrun the blob");
    FRESH(); wr32(d, co + 4, csz + 4); expect("starts_offset past the blob", d, n, "starts offset");
    FRESH(); wr32(d, co + 12, 0x10000); expect("symbols_offset past the blob", d, n, "symbols offset");
    FRESH(); { uint32_t io = rd32(d, co + 8); wr32(d, co + io, rd32(d, co + io) | 0xffff0000u); }
    expect("import name offset outside the symbol table", d, n, "outside the symbol table");
    FRESH(); { uint32_t io = rd32(d, co + 8); wr32(d, co + io, (rd32(d, co + io) & ~0xffu) | 0x7f); }
    expect("import library ordinal past the dependencies", d, n, "library ordinal");
    FRESH(); { uint32_t so = rd32(d, co + 4); wr32(d, co + so, 200); } expect("more segment starts than segments", d, n, "segment");
    {   /* __DATA's starts_in_segment: first page start -> past the page, then a chain into the next segment */
        uint32_t so = rd32(seed, co + 4), nsegs = rd32(seed, co + so);
        size_t ds = 0;
        for (uint32_t s = 0; s < nsegs; s++) if (rd32(seed, co + so + 4 + 4 * s)) ds = co + so + rd32(seed, co + so + 4 + 4 * s);
        FRESH(); memcpy(d + ds + 22, &(uint16_t){0x1ff8}, 2); expect("page start past the page", d, n, "past the page");
        FRESH(); memcpy(d + ds + 22, &(uint16_t){0xff8}, 2);    /* last slot of the segment: its next must leave it */
        { size_t data = (size_t)rd32(seed, find_cmd(seed, LC_SEGMENT_64, "__DATA") + 40);
          uint64_t raw = (1ull << 51) * 0x10;                      /* rebase, next = 16 slots */
          wr64(d, data + 0xff8, raw); }
        expect("fixup chain leaves its segment (arbitrary write)", d, n, "outside the segment");
        FRESH(); memcpy(d + ds + 8, &(uint64_t){0x1000}, 8); expect("segment_offset does not match vmaddr", d, n, "segment_offset");
        FRESH(); memcpy(d + ds + 4, &(uint16_t){0x8000}, 2); expect("bad fixup page size", d, n, "page size");
        FRESH(); memcpy(d + ds + 4, &(uint16_t){0x1000}, 2); memcpy(d + ds + 20, &(uint16_t){2}, 2);
        expect("page_count exceeds the segment", d, n, "exceed");
    }

    /* fat */
    {
        uint8_t *x = malloc(fn);
        memcpy(x, fat, fn);
        /* fat_arch[1] (arm64e): offset at +8, size at +12 (big endian) */
        uint32_t big = __builtin_bswap32((uint32_t)fn);
        memcpy(x + 8 + 20 + 12, &big, 4);
        expect("fat slice past the end of the file", x, fn, "outside the file");
        memcpy(x, fat, fn);
        big = __builtin_bswap32(1000); memcpy(x + 4, &big, 4);
        expect("fat header with 1000 architectures", x, fn, "architectures");
        memcpy(x, fat, fn);
        big = __builtin_bswap32(__builtin_bswap32(rd32(fat, 8 + 20 + 8)) - 0x10); memcpy(x + 8 + 20 + 8, &big, 4);
        expect("fat slice not page aligned", x, fn, "not page aligned");
        free(x);
    }

    /* non-PIE image over memory hao already uses: refused, not clobbered */
    FRESH();
    {
        uint64_t here = (uint64_t)(uintptr_t)&main & ~0xfffull, delta = here - 0x100000000ull;
        wr32(d, 24, rd32(d, 24) & ~(uint32_t)MH_PIE);
        uint32_t ncmds = rd32(d, 16);
        size_t off = 32;
        for (uint32_t k = 0; k < ncmds; k++) {
            if (rd32(d, off) == LC_SEGMENT_64 && strncmp((const char *)d + off + 8, "__PAGEZERO", 16)) {
                uint64_t v; memcpy(&v, d + off + 24, 8); wr64(d, off + 24, v + delta);
                uint32_t ns = rd32(d, off + 64);
                for (uint32_t s = 0; s < ns; s++) { size_t so = off + 72 + 80 * s; memcpy(&v, d + so + 32, 8); wr64(d, so + 32, v + delta); }
            }
            off += rd32(d, off + 4);
        }
        uint8_t before[64];
        memcpy(before, (void *)(uintptr_t)here, sizeof before);
        expect("non-PIE image over hao's own code", d, n, "in use");
        if (memcmp(before, (void *)(uintptr_t)here, sizeof before)) { printf("FAIL hao's code was overwritten\n"); fails++; }
    }

    /* exports trie: a cycle through empty edges, a child past the end, and real lookups */
    {
        char path[1100], err[512];
        snprintf(path, sizeof path, "%s/lib.bin", scratch);
        FILE *f = fopen(path, "wb"); fwrite(lib, 1, ln, f); fclose(f);
        hao_image *img = hao_load_image(path, resolver, err, sizeof err);
        uint64_t a = 0;
        bool ok = img && hao_image_export(img, "_foobar", &a) && a > img->base && a < img->base + img->vmsize &&
                  hao_image_export(img, "_fizz", &a) && !hao_image_export(img, "_foob", &a) && !hao_image_export(img, "_nope", &a);
        printf("%s exports trie lookups\n", ok ? "PASS" : "FAIL"); ok ? passes++ : fails++;
        if (img) {
            const uint8_t *saved = img->exports_trie;
            uint32_t saved_n = img->exports_trie_size;
            static const uint8_t cyc[] = {0, 1, 0, 0};          /* root: no terminal, 1 child, edge "", child = root */
            img->exports_trie = cyc; img->exports_trie_size = sizeof cyc;
            bool c1 = !hao_image_export(img, "_x", &a);
            static const uint8_t cyc2[] = {0, 1, '_', 0, 0};    /* edge "_" back to the root: consumes the name */
            img->exports_trie = cyc2; img->exports_trie_size = sizeof cyc2;
            bool c2 = !hao_image_export(img, "______________________________x", &a);
            static const uint8_t past[] = {0, 1, '_', 0, 0x7f};
            img->exports_trie = past; img->exports_trie_size = sizeof past;
            bool c3 = !hao_image_export(img, "_x", &a);
            static const uint8_t term[] = {4, 0, 0xff, 0xff, 0x7f};   /* terminal offset past the image */
            img->exports_trie = term; img->exports_trie_size = sizeof term;
            bool c4 = !hao_image_export(img, "", &a);
            static const uint8_t trunc[] = {0x80};
            img->exports_trie = trunc; img->exports_trie_size = sizeof trunc;
            bool c5 = !hao_image_export(img, "_x", &a);
            printf("%s exports trie: empty-edge cycle, name-consuming cycle, child past the end, terminal outside, truncated uleb\n",
                   c1 && c2 && c3 && c4 && c5 ? "PASS" : "FAIL");
            (c1 && c2 && c3 && c4 && c5) ? passes++ : fails++;
            img->exports_trie = saved; img->exports_trie_size = saved_n;
            hao_unload_image(img);
        }
    }

    /* the descriptor never leaks into children */
    FRESH();
    {
        char path[1100], err[512];
        snprintf(path, sizeof path, "%s/case.bin", scratch);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (write(fd, d, n) != (ssize_t)n) exit(2);
        close(fd);
        int before = 0, after = 0;
        for (int k = 0; k < 256; k++) if (fcntl(k, F_GETFD) != -1) before++;
        hao_image *img = hao_load_image(path, resolver, err, sizeof err);
        for (int k = 0; k < 256; k++) if (fcntl(k, F_GETFD) != -1) after++;
        bool ok = img && before == after;
        printf("%s no descriptor left open after load (%d before, %d after)\n", ok ? "PASS" : "FAIL", before, after);
        ok ? passes++ : fails++;
        if (img) hao_unload_image(img);
    }

    /* code-signature reader (src/libval.c): the seed's linker signature, every
     * truncation of it (exact-size copies, so ASan sees any overread), a
     * corrupt index, and hand-built blobs with and without the
     * disable-library-validation entitlement */
    FRESH();
    {
        size_t o = find_cmd(d, LC_CODE_SIGNATURE, NULL);
        uint32_t so = rd32(d, o + 8), ss = rd32(d, o + 12);
        hao_codesig cs;
        bool ok = so + ss <= n && hao_codesig_parse(d + so, ss, &cs) && (cs.flags & 0x2) && !cs.platform && !cs.dlv &&
                  !hao_codesig_library_validated(&cs);
        for (uint32_t k = 0; k < ss && k < 4096; k++) {
            uint8_t *t = malloc(k ? k : 1);
            memcpy(t, d + so, k);
            hao_codesig_parse(t, k, &cs);
            free(t);
        }
        uint8_t *t = malloc(ss);
        memcpy(t, d + so, ss);
        t[8] = 0x7f;   /* count */
        ok &= !hao_codesig_parse(t, ss, &cs);
        memcpy(t, d + so, ss);
        t[16] = 0x7f;  /* first blob offset */
        hao_codesig_parse(t, ss, &cs);
        free(t);
        printf("%s code signature: seed is ad hoc, not library-validated; truncated and corrupt blobs\n", ok ? "PASS" : "FAIL");
        ok ? passes++ : fails++;

        static const char ents_t[] = "<plist><dict><key>com.apple.security.cs.disable-library-validation</key>\n\t<true/></dict></plist>";
        static const char ents_f[] = "<plist><dict><key>com.apple.security.cs.disable-library-validation</key><false/></dict></plist>";
        for (int v = 0; v < 3; v++) {
            const char *e = v == 1 ? ents_t : ents_f;
            size_t el = strlen(e), cd = 52, total = 12 + 16 + cd + 8 + el;
            uint8_t *b = calloc(1, total);
#define BE32(p, x) do { uint32_t _x = (x); (p)[0] = _x >> 24; (p)[1] = _x >> 16; (p)[2] = _x >> 8; (p)[3] = _x; } while (0)
            BE32(b, 0xfade0cc0); BE32(b + 4, (uint32_t)total); BE32(b + 8, 2);
            BE32(b + 12, 0); BE32(b + 16, 28);
            BE32(b + 20, 5); BE32(b + 24, (uint32_t)(28 + cd));
            BE32(b + 28, 0xfade0c02); BE32(b + 32, (uint32_t)cd); BE32(b + 36, 0x20400);
            BE32(b + 40, v == 2 ? 0x10000 : 0);   /* runtime */
            b[28 + 38] = v == 2 ? 0 : 26;           /* platform identifier */
            BE32(b + 28 + cd, 0xfade7171); BE32(b + 28 + cd + 4, (uint32_t)(8 + el));
            memcpy(b + 28 + cd + 8, e, el);
            bool p = hao_codesig_parse(b, total, &cs);
            bool want_lv = v != 1;
            bool good = p && cs.dlv == (v == 1) && hao_codesig_library_validated(&cs) == want_lv;
            printf("%s code signature: %s\n", good ? "PASS" : "FAIL",
                   v == 0 ? "platform, no DLV: library-validated" : v == 1 ? "platform with DLV <true/>: not validated"
                                                                  : "runtime, DLV <false/>: library-validated");
            good ? passes++ : fails++;
            free(b);
        }
    }

    printf("test_macho: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
