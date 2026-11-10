/*
 * Unit test of the validating Mach-O loader (src/macho.c). Starting from the
 * clang-built seeds in tests/fuzz/corpus/macho, each case corrupts one
 * field the way a hostile guest file would and checks that the load fails
 * with a message naming the problem (and, under ASan, without touching
 * memory it should not). The unmodified seeds must load and unload.
 *   test_macho CORPUS_DIR SCRATCH_DIR
 */
#include "hao/macho.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
static uint32_t be32at(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
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

/* ------------------------------------------------------------------ slices */

static void note(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? passes++ : fails++;
}

static void put_file(const char *path, const uint8_t *d, size_t n, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, d, n) != (ssize_t)n || (len > n && ftruncate(fd, (off_t)len) != 0)) { perror(path); exit(2); }
    close(fd);
}

/* overwrite [off, off+n) of the file in place (the same inode), as its owner can at any moment */
static void scribble(const char *path, size_t off, size_t n)
{
    uint8_t *junk = malloc(n);
    memset(junk, 0xa5, n);
    int fd = open(path, O_WRONLY);
    if (fd < 0 || pwrite(fd, junk, n, (off_t)off) != (ssize_t)n || fsync(fd) != 0) { perror(path); exit(2); }
    close(fd);
    free(junk);
}

/*
 * The image is built from bytes taken once (hao_slice): a file its owner
 * rewrites in place after the load, or between the moment a caller takes
 * the slice to check it and the load, does not reach the image. What a
 * private mapping shows of a file rewritten later is the kernel's choice
 * (POSIX leaves it open), so only files nobody but root can change are
 * mapped; the "note" line reports what this kernel did, and is not a pass
 * or a failure.
 */
static void slice_tests(const uint8_t *seed, size_t n, const uint8_t *lib, size_t ln, const uint8_t *fat, size_t fn)
{
    char path[1100], err[512] = "";
    snprintf(path, sizeof path, "%s/slice.bin", scratch);
    size_t text = (size_t)rd32(seed, find_cmd(seed, LC_SEGMENT_64, "__TEXT") + 48);   /* __TEXT filesize: the file's first page */

    /* both ways of taking the slice build the same image */
    for (int way = 1; way <= 2; way++) {
        hao_slice_test_path = way;
        const struct { const char *name; const uint8_t *d; size_t n; } f[] = {{"exec-arm64e", seed, n}, {"lib-arm64e", lib, ln},
                                                                               {"fat-x86_64-arm64e", fat, fn}};
        for (size_t k = 0; k < sizeof f / sizeof f[0]; k++) {
            char what[200];
            snprintf(what, sizeof what, "seed %s loads from %s", f[k].name, way == 1 ? "the mapped file" : "a copy of the file");
            expect(what, f[k].d, f[k].n, NULL);
        }
        put_file(path, seed, n, n);
        hao_image *img = hao_load_image(path, resolver, err, sizeof err);
        bool same = img && !memcmp((const void *)(uintptr_t)img->base, seed, text);
        if (img) scribble(path, 0, text);
        bool kept = img && !memcmp((const void *)(uintptr_t)img->base, seed, text);
        if (way == 2) note(same && kept, "file rewritten in place after the load: the image keeps the bytes it was built from");
        else printf("note mapped file rewritten in place after the load: on this kernel the image %s\n",
                    !same ? "did not load" : kept ? "kept its bytes" : "shows the new bytes");
        if (img) hao_unload_image(img);
    }
    hao_slice_test_path = 0;

    /* a caller checks the slice, the file changes, the loader builds the image: from the slice the caller left */
    put_file(path, seed, n, n);
    int fd = open(path, O_RDONLY);
    hao_slice *sl = fd >= 0 ? hao_slice_open(fd, err, sizeof err) : NULL;
    bool ok = sl && sl->copied && sl->size == n && !memcmp(sl->bytes, seed, n);
    if (sl) {
        scribble(path, 0, n);
        hao_slice_hand(sl);
        hao_image *img = hao_load_image_fd_cb(path, fd, resolver, NULL, NULL, err, sizeof err);
        ok = ok && img && !memcmp((const void *)(uintptr_t)img->base, seed, text);
        if (img) hao_unload_image(img);
    }
    note(ok, "file rewritten between the caller's check and the load: the image is built from the slice the caller checked");
    /* without a slice left for it, the loader takes its own: the rewritten file does not load as the seed */
    put_file(path, seed, n, n);
    fd = open(path, O_RDONLY);
    scribble(path, 0, n);
    hao_image *img = hao_load_image_fd_cb(path, fd, resolver, NULL, NULL, err, sizeof err);
    note(!img && strstr(err, "no arm64/arm64e slice") == NULL && strstr(err, "Mach-O"), "a rewritten file with no slice left for the loader is read as it is now");
    if (img) hao_unload_image(img);

    /* a user's own file is copied by rule: owner, mode, ACL, volume */
    put_file(path, seed, n, n);
    fd = open(path, O_RDONLY);
    sl = hao_slice_open(fd, err, sizeof err);
    note(sl && sl->copied == (getuid() != 0), "a file its owner may write is copied, not mapped");
    hao_slice_close(sl);
    close(fd);
    fd = open("/usr/lib/dyld", O_RDONLY);
    if (fd >= 0) {
        sl = hao_slice_open(fd, err, sizeof err);
        note(sl && !sl->copied, "a root-owned system file (/usr/lib/dyld) is mapped, not copied");
        hao_slice_close(sl);
        close(fd);
    }
}

/* A synthetic image (as legacy_case's) whose fields the caps and the header rule are tried on. */
static void layout_case(const char *what, int variant, const char *want)
{
    enum { SIZE = 0x3000 };
    size_t file_len = SIZE;
    uint8_t *d = calloc(1, SIZE);
    struct mach_header_64 mh = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_DYLIB, .ncmds = 4,
                                .sizeofcmds = 3 * sizeof(struct segment_command_64) + sizeof(struct linkedit_data_command)};
    size_t cursor = sizeof mh;
    for (unsigned k = 0; k < 3; k++) {
        struct segment_command_64 sg = {.cmd = LC_SEGMENT_64, .cmdsize = sizeof sg, .vmaddr = 0x100000000ull + k * 0x1000,
                                        .vmsize = 0x1000, .fileoff = k * 0x1000, .filesize = 0x1000,
                                        .maxprot = VM_PROT_READ | VM_PROT_WRITE, .initprot = VM_PROT_READ | (k == 1 ? VM_PROT_WRITE : 0)};
        strcpy(sg.segname, k == 0 ? "__TEXT" : k == 1 ? "__DATA" : "__LINKEDIT");
        if (k == 0 && variant == 1) sg.initprot = 0;                          /* the header's segment: not readable */
        if (k == 0 && variant == 2) sg.filesize = sizeof mh + 8;             /* ... ends inside the load commands */
        if (k == 0 && variant == 3) { sg.fileoff = 0x1000; }                  /* ... is not the file's start */
        memcpy(d + cursor, &sg, sizeof sg);
        cursor += sizeof sg;
    }
    struct linkedit_data_command ld = {.cmd = LC_DYLD_EXPORTS_TRIE, .cmdsize = sizeof ld, .dataoff = 0x2000, .datasize = 8};
    if (variant == 4) { ld.cmd = LC_LAZY_LOAD_DYLIB_INFO; ld.datasize = (4u << 20) + 8; file_len = 0x2000 + ld.datasize; }
    if (variant == 5) { ld.datasize = (256u << 20) + 8; file_len = 0x2000 + (size_t)ld.datasize; }
    if (variant == 6) { ld.cmd = LC_DYLD_CHAINED_FIXUPS; ld.datasize = (256u << 20) + 8; file_len = 0x2000 + (size_t)ld.datasize; }
    if (variant == 7) { ld.cmd = LC_FUNCTION_STARTS; ld.datasize = (256u << 20) + 8; file_len = 0x2000 + (size_t)ld.datasize; }
    memcpy(d + cursor, &ld, sizeof ld);
    if (variant == 8) { mh.sizeofcmds = (4u << 20) + 8; file_len = (5u << 20); }   /* more load commands than any image has */
    memcpy(d, &mh, sizeof mh);
    char path[1100], err[512] = "";
    snprintf(path, sizeof path, "%s/layout.bin", scratch);
    put_file(path, d, SIZE, file_len);            /* longer files are sparse */
    free(d);
    /* the sparse giants are mapped, not read: the caps are the same either way */
    hao_slice_test_path = file_len > (64u << 20) ? 1 : 0;
    hao_image *img = hao_load_image(path, resolver, err, sizeof err);
    hao_slice_test_path = 0;
    bool ok = want ? !img && strstr(err, want) : img != NULL;
    if (img) hao_unload_image(img);
    printf("%s layout: %s%s%s\n", ok ? "PASS" : "FAIL", what, ok ? "" : ": ", ok ? "" : (img ? "loaded" : err));
    ok ? passes++ : fails++;
}

static void layout_tests(void)
{
    layout_case("the synthetic image loads", 0, NULL);
    layout_case("the mach header's segment is not readable", 1, "is not readable");
    layout_case("the first segment ends inside the load commands", 2, "does not hold the");
    layout_case("the first segment is not the start of the file", 3, "mach header is not mapped");
    layout_case("a lazy-load record over 4 MiB", 4, "lazy-load info of");
    layout_case("an exports trie over 256 MiB", 5, "exports trie of");
    layout_case("chained fixups over 256 MiB", 6, "chained fixups of");
    layout_case("function starts over 256 MiB", 7, "function starts of");
    layout_case("load commands over 4 MiB", 8, "bytes of load commands");
}

/* -------------------------------------------------------- code signatures */

#define BE32(p, x) do { uint32_t _x = (x); (p)[0] = _x >> 24; (p)[1] = _x >> 16; (p)[2] = _x >> 8; (p)[3] = _x; } while (0)

/* A well-formed signature: one SHA-256 CodeDirectory (identifier "t", no
 * code slots, five special slots) and, with ents, an XML entitlements blob
 * sealed in slot 5. Returns its size; the CodeDirectory is at *cd_at. */
static size_t make_sig(uint8_t *b, uint32_t flags, uint8_t platform, const char *ents, size_t *cd_at)
{
    size_t el = ents ? strlen(ents) : 0, cdl = 88 + 5 * 32 + 2, nblob = ents ? 2 : 1, at = 12 + 8 * nblob;
    size_t total = at + cdl + (ents ? 8 + el : 0);
    memset(b, 0, total);
    BE32(b, 0xfade0cc0); BE32(b + 4, (uint32_t)total); BE32(b + 8, (uint32_t)nblob);
    BE32(b + 12, 0); BE32(b + 16, (uint32_t)at);
    uint8_t *cd = b + at;
    BE32(cd, 0xfade0c02); BE32(cd + 4, (uint32_t)cdl); BE32(cd + 8, 0x20400); BE32(cd + 12, flags);
    BE32(cd + 16, 88 + 2 + 5 * 32);                 /* hashOffset: after the header, the identifier and the special slots */
    BE32(cd + 20, 88);                              /* identOffset */
    BE32(cd + 24, 5);                               /* nSpecialSlots */
    cd[36] = 32; cd[37] = 2; cd[38] = platform; cd[39] = 12;
    cd[88] = 't';
    if (ents) {
        uint8_t *e = cd + cdl;
        BE32(b + 20, 5); BE32(b + 24, (uint32_t)(at + cdl));
        BE32(e, 0xfade7171); BE32(e + 4, (uint32_t)(8 + el));
        memcpy(e + 8, ents, el);
        CC_SHA256(e, (CC_LONG)(8 + el), cd + 90);   /* slot -5 is the first of the five */
    }
    if (cd_at) *cd_at = at;
    return total;
}

/* ---------------------------------------------------- the loader's gate */

/* Load d[0..n) as FILE in the scratch directory: the image, or NULL with err filled. */
static hao_image *load_bytes(const char *file, const uint8_t *d, size_t n, char *err, size_t errlen)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", scratch, file);
    put_file(path, d, n, n);
    err[0] = 0;
    return hao_load_image(path, resolver, err, errlen);
}

static uint8_t *copy_of(const uint8_t *d, size_t n)
{
    uint8_t *c = malloc(n + 64);
    memcpy(c, d, n);
    return c;
}

/* the offset of the signature of a thin Mach-O */
static size_t sig_at(const uint8_t *d) { return rd32(d, find_cmd(d, LC_CODE_SIGNATURE, NULL) + 8); }

/* Run f in a child (library validation's policy is decided once per process); returns its failures and
 * adds its passes. */
typedef void gate_fn(const uint8_t *seed, size_t n, const uint8_t *lib, size_t ln);
static int in_child(gate_fn *f, const uint8_t *seed, size_t n, const uint8_t *lib, size_t ln, int *passed)
{
    int p[2];
    if (pipe(p) != 0) exit(2);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        passes = fails = 0;
        f(seed, n, lib, ln);
        fflush(stdout);
        int r[2] = {passes, fails};
        if (write(p[1], r, sizeof r) != sizeof r) _exit(2);
        _exit(0);
    }
    close(p[1]);
    int r[2] = {0, 1}, st = 0;
    if (read(p[0], r, sizeof r) != sizeof r) { r[0] = 0; r[1] = 1; }
    close(p[0]);
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("FAIL the gate's test process ended with status %#x\n", st); r[1]++; }
    *passed += r[0];
    return r[1];
}

/* An ad hoc main executable: library validation off, integrity on. Every signed image is hashed once. */
static void gate_plain(const uint8_t *seed, size_t n, const uint8_t *lib, size_t ln)
{
    char err[512], path[1100];
    uint8_t *d = copy_of(seed, n);
    d[sig_at(d) - 1] ^= 1;                       /* the last hashed byte before the signature: __LINKEDIT, not code */
    hao_libval_expect_main(true);
    hao_image *img = load_bytes("main.bin", d, n, err, sizeof err);
    note(!img && strstr(err, "does not match its hash"), "main executable with a page that does not match its hash: refused before its policy is read");
    note(hao_libval_mode() == HAO_LIBVAL_UNDECIDED, "a refused main executable decides nothing");
    free(d);

    /* as the CLI does: the caller verifies the slice and leaves it for the loader; the loader's gate reads
     * the policy from that verified signature without hashing the image again */
    snprintf(path, sizeof path, "%s/main.bin", scratch);
    put_file(path, seed, n, n);
    int fd = open(path, O_RDONLY);
    hao_slice *sl = fd >= 0 ? hao_slice_open(fd, err, sizeof err) : NULL;
    unsigned before = hao_slice_verify_passes;
    bool ok = sl && hao_slice_verify(sl, NULL, err, sizeof err) == 0 && hao_slice_verify(sl, NULL, err, sizeof err) == 0;
    if (sl) hao_slice_hand(sl);
    hao_libval_expect_main(true);
    img = ok ? hao_load_image_fd_cb(path, fd, resolver, NULL, NULL, err, sizeof err) : NULL;
    note(img && hao_slice_verify_passes == before + 1, "main executable checked before the load: hashed once in all");
    note(hao_libval_mode() == HAO_LIBVAL_OFF && !strcmp(hao_libval_reason(), "neither platform nor hardened"),
         "its policy: library validation off (neither platform nor hardened)");

    before = hao_slice_verify_passes;
    hao_image *l = load_bytes("lib.bin", lib, ln, err, sizeof err);
    note(l && hao_slice_verify_passes == before + 1 && !strcmp(l->libval_route, "integrity"), "library: loads, hashed once (integrity)");
    if (l) hao_unload_image(l);
    d = copy_of(lib, ln);
    d[sig_at(d) - 1] ^= 1;
    l = load_bytes("lib.bin", d, ln, err, sizeof err);
    note(!l && strstr(err, "not valid for use in process: code signature invalid (page") && strstr(err, "does not match its hash"),
         "library with a data page that does not match its hash: not loaded");
    memcpy(d, lib, ln);
    wr32(d, find_cmd(d, LC_CODE_SIGNATURE, NULL) + 12, 0);        /* no signature */
    l = load_bytes("lib.bin", d, ln, err, sizeof err);
    note(l != NULL, "unsigned library, library validation off: loads");
    if (l) hao_unload_image(l);
    /* a page of an executable segment: the process dies as the kernel kills it natively */
    memcpy(d, lib, ln);
    d[(size_t)rd32(d, find_cmd(d, LC_SEGMENT_64, "__TEXT") + 72 + 48) + 1] ^= 1;   /* its first section: __text */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 2);
        load_bytes("lib.bin", d, ln, err, sizeof err);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    note(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "library with a code page that does not match its hash: the process is killed");
    free(d);
}

static int signer_calls, signer_apple;
static int fake_signer(const hao_slice *sl, const char *path, const char *team, hao_imgsig *out, char *why, size_t n)
{
    (void)path; (void)team; (void)why; (void)n;
    signer_calls++;
    memset(out, 0, sizeof *out);
    if (!sl->sig.cd) return -1;                  /* asked about a signature nobody verified */
    out->apple = signer_apple;
    out->by = "the test";
    return 0;
}

/* A hardened main executable (the seed's linker signature with the runtime flag: ad hoc, so only its
 * cdhash changes): library validation on. The signer is asked in user space, as on a kernel that cannot
 * hold the signature, and the image is still hashed once. */
static void gate_hardened(const uint8_t *seed, size_t n, const uint8_t *lib, size_t ln)
{
    char err[512];
    uint8_t *d = copy_of(seed, n);
    size_t so = sig_at(d), cd = so + be32at(d + so + 16);        /* the first index entry: the CodeDirectory */
    d[cd + 13] |= 0x01;                                          /* flags (big-endian) |= 0x10000, runtime */
    hao_libval_set_verifier(fake_signer, true, NULL);
    hao_libval_expect_main(true);
    hao_image *img = load_bytes("main.bin", d, n, err, sizeof err);
    int m = hao_libval_mode();
    note(img && (m == HAO_LIBVAL_KERNEL || m == HAO_LIBVAL_PLATFORM) && strstr(hao_libval_reason(), "runtime"),
         "hardened main executable: library validation on");
    free(d);

    unsigned before = hao_slice_verify_passes;
    signer_calls = 0; signer_apple = 1;
    hao_image *l = load_bytes("lib.bin", lib, ln, err, sizeof err);
    note(l && hao_slice_verify_passes == before + 1 && signer_calls == 1 && strstr(l->libval_route, "user space") &&
         strstr(l->libval_route, "the test"), "library on the user-space route, signer says platform: loads, hashed once, signer asked once");
    if (l) hao_unload_image(l);
    before = hao_slice_verify_passes;
    signer_calls = 0; signer_apple = 0;
    l = load_bytes("lib.bin", lib, ln, err, sizeof err);
    note(!l && hao_slice_verify_passes == before + 1 && signer_calls == 1 && strstr(err, "not valid for use in process"),
         "the same library, signer says nobody: not loaded, hashed once");
    d = copy_of(lib, ln);
    d[sig_at(d) - 1] ^= 1;
    signer_calls = 0;
    l = load_bytes("lib.bin", d, ln, err, sizeof err);
    note(!l && signer_calls == 0 && strstr(err, "does not match its hash"), "library that fails integrity: the signer is not asked");
    memcpy(d, lib, ln);
    wr32(d, find_cmd(d, LC_CODE_SIGNATURE, NULL) + 12, 0);
    l = load_bytes("lib.bin", d, ln, err, sizeof err);
    note(!l && strstr(err, "mapped file is not code signed"), "unsigned library, library validation on: not loaded");
    free(d);
    hao_libval_set_verifier(NULL, true, NULL);
    l = load_bytes("lib.bin", lib, ln, err, sizeof err);
    note(!l && strstr(err, "can say who signed it"), "no signer check in the process: a library that needs one is not loaded");
}

/* Reads a list of "b FILE" (a signature blob) and "m FILE" (a thin Mach-O)
 * lines and prints, for each, what hao's one signature reader says: the
 * verdict, and for a valid signature its cdhash and the policy fields.
 * tests/tools/test_codesig.py compares the lines with tools/hao_codesig.py's. */
static void codesig_line(const char *verdict, const hao_codesig *s)
{
    printf("%s", verdict);
    if (s) {
        printf(" ");
        for (int k = 0; k < 20; k++) printf("%02x", s->cdhash[k]);
        printf(" flags=0x%x platform=%u team=", s->flags, s->platform);
        for (const char *t = s->team ? s->team : ""; *t; t++) printf("%02x", (uint8_t)*t);
        printf(" dlv=%d lv=%d", s->dlv, hao_codesig_library_validated(s));
    }
    printf("\n");
}

static int codesig_report(const char *list)
{
    FILE *f = fopen(list, "r");
    char line[2100], why[300];
    if (!f) { perror(list); return 2; }
    hao_slice_test_path = 2;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        const char *path = line + 2;
        int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) { perror(path); return 2; }
        if (line[0] == 'b') {
            hao_codesig s;
            uint8_t *b = malloc(st.st_size ? (size_t)st.st_size : 1);       /* exact size: ASan sees an overread */
            if (read(fd, b, (size_t)st.st_size) != st.st_size) return 2;
            bool ok = hao_codesig_parse(b, (size_t)st.st_size, &s, why, sizeof why);
            codesig_line(ok ? "valid" : "invalid", ok ? &s : NULL);
            free(b);
        } else {
            hao_slice *sl = hao_slice_open(fd, why, sizeof why);
            if (!sl) codesig_line("refused", NULL);
            else if (!sl->sig_size) codesig_line("unsigned", NULL);
            else if (hao_slice_verify(sl, NULL, why, sizeof why) != 0) codesig_line("invalid", NULL);
            else codesig_line("valid", &sl->sig);
            hao_slice_close(sl);
        }
        close(fd);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--codesig")) return codesig_report(argv[2]);
    if (argc != 3) { fprintf(stderr, "usage: test_macho CORPUS_DIR SCRATCH_DIR | test_macho --codesig LIST\n"); return 2; }
    snprintf(corpus, sizeof corpus, "%s", argv[1]);
    snprintf(scratch, sizeof scratch, "%s", argv[2]);
    legacy_tests();
    layout_tests();
    size_t n, ln, fn;
    uint8_t *seed = slurp("exec-arm64e", &n), *lib = slurp("lib-arm64e", &ln), *fat = slurp("fat-x86_64-arm64e", &fn);
    slice_tests(seed, n, lib, ln, fat, fn);
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
        /* the fat header is outside every signature: its subtype must be the slice's own */
        memcpy(x, fat, fn);
        big = __builtin_bswap32(0); memcpy(x + 8 + 20 + 4, &big, 4);            /* arm64e slice named arm64 (subtype 0) */
        expect("fat header names another cpusubtype than the slice's header", x, fn, "fat header names cpusubtype");
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

    /* the one code-signature reader (src/libval.c): the seed's linker signature, every
     * truncation of it (exact-size copies, so ASan sees any overread), a
     * corrupt index, and built signatures with and without the
     * disable-library-validation entitlement */
    FRESH();
    {
        size_t o = find_cmd(d, LC_CODE_SIGNATURE, NULL);
        uint32_t so = rd32(d, o + 8), ss = rd32(d, o + 12);
        hao_codesig cs;
        char w[300];
        bool ok = so + ss <= n && hao_codesig_parse(d + so, ss, &cs, w, sizeof w) && (cs.flags & 0x2) && !cs.platform && !cs.dlv &&
                  !cs.team && !hao_codesig_library_validated(&cs);
        for (uint32_t k = 0; k < ss && k < 4096; k++) {
            uint8_t *t = malloc(k ? k : 1);
            memcpy(t, d + so, k);
            ok &= !hao_codesig_parse(t, k, &cs, w, sizeof w) || be32at(d + so + 4) <= k;
            free(t);
        }
        uint8_t *t = malloc(ss);
        memcpy(t, d + so, ss);
        t[8] = 0x7f;   /* count */
        ok &= !hao_codesig_parse(t, ss, &cs, w, sizeof w);
        memcpy(t, d + so, ss);
        t[16] = 0x7f;  /* first blob offset */
        ok &= !hao_codesig_parse(t, ss, &cs, w, sizeof w);
        free(t);
        note(ok, "code signature: seed is ad hoc, not library-validated; truncated and corrupt blobs refused");

        static const char ents_t[] = "<plist><dict><key>com.apple.security.cs.disable-library-validation</key>\n\t<true/></dict></plist>";
        static const char ents_f[] = "<plist><dict><key>com.apple.security.cs.disable-library-validation</key><false/></dict></plist>";
        uint8_t b[1024];
        for (int v = 0; v < 3; v++) {
            size_t total = make_sig(b, v == 2 ? 0x10000 : 0, v == 2 ? 0 : 26, v == 1 ? ents_t : ents_f, NULL);
            uint8_t *x = malloc(total);
            memcpy(x, b, total);
            bool p = hao_codesig_parse(x, total, &cs, w, sizeof w);
            bool want_lv = v != 1;
            note(p && cs.dlv == (v == 1) && hao_codesig_library_validated(&cs) == want_lv,
                 v == 0 ? "code signature: platform, no DLV: library-validated" : v == 1 ? "code signature: platform with DLV <true/>: not validated"
                                                                                : "code signature: runtime, DLV <false/>: library-validated");
            free(x);
        }
        /* what one reader must refuse, so that no other reading of the same signature exists */
        size_t cd_at, total = make_sig(b, 0, 0, ents_t, &cd_at);
        b[cd_at + 90] ^= 1;                                   /* the entitlements' special slot */
        note(!hao_codesig_parse(b, total, &cs, w, sizeof w) && strstr(w, "does not match its hash"),
             "code signature: entitlements that do not match their special slot refused");
        total = make_sig(b, 0, 0, ents_t, &cd_at);
        BE32(b + 20, 0x1234);                                 /* the entitlements blob under an unsealed slot type */
        note(!hao_codesig_parse(b, total, &cs, w, sizeof w) && strstr(w, "is missing"),
             "code signature: a sealed blob taken out of the index refused");
        total = make_sig(b, 0, 0, NULL, &cd_at);
        note(hao_codesig_parse(b, total, &cs, w, sizeof w) && !cs.ents && !cs.dlv, "code signature: no entitlements, none sealed: valid");
        /* CodeDirectories of ranks SHA-256, SHA-1, SHA-1: the repeat is not of the best rank */
        {
            uint8_t s3[2048], one[512];
            size_t l1 = make_sig(one, 0, 0, NULL, &cd_at), cdl = l1 - cd_at, at = 12 + 3 * 8;
            memset(s3, 0, sizeof s3);
            BE32(s3, 0xfade0cc0); BE32(s3 + 4, (uint32_t)(at + 3 * cdl)); BE32(s3 + 8, 3);
            for (int k = 0; k < 3; k++) {
                BE32(s3 + 12 + 8 * k, k ? 0x1000 + (uint32_t)k - 1 : 0); BE32(s3 + 16 + 8 * k, (uint32_t)(at + k * cdl));
                memcpy(s3 + at + k * cdl, one + cd_at, cdl);
                if (k) {                                      /* SHA-1: 20-byte slots, five special slots before hashOffset */
                    uint8_t *c = s3 + at + k * cdl;
                    c[36] = 20; c[37] = 1;
                    BE32(c + 16, 88 + 2 + 5 * 20);
                }
            }
            note(!hao_codesig_parse(s3, at + 3 * cdl, &cs, w, sizeof w) && strstr(w, "two CodeDirectories"),
                 "code signature: a second CodeDirectory of a hash type that is not the best one refused");
            BE32(s3 + 4, (uint32_t)(at + 2 * cdl)); BE32(s3 + 8, 2);
            note(hao_codesig_parse(s3, at + 2 * cdl, &cs, w, sizeof w) && cs.hash_type == 2 && cs.cd == s3 + at,
                 "code signature: SHA-256 and SHA-1 CodeDirectories: the SHA-256 one is used");
        }
    }

    /* one LC_CODE_SIGNATURE: a second one, or one too short to hold its range, is not a loadable slice */
    FRESH();
    {
        uint32_t ncmds = rd32(d, 16), sizeofcmds = rd32(d, 20);
        size_t o = find_cmd(d, LC_CODE_SIGNATURE, NULL), end = 32 + sizeofcmds;
        memcpy(d + end, d + o, 16);
        wr32(d, end + 8, rd32(d, o + 8) + 16);      /* another range: the two readers of old took one each */
        wr32(d, 16, ncmds + 1); wr32(d, 20, sizeofcmds + 16);
        expect("two LC_CODE_SIGNATURE commands", d, n, "more than one LC_CODE_SIGNATURE");
        FRESH();
        wr32(d, o + 4, 8);                          /* cmdsize 8: its range would be read from the next command */
        wr32(d, o + 8, 0x7f000001); wr32(d, o + 12, 8);   /* (the 8 bytes it gave up, as a command of their own) */
        wr32(d, 16, ncmds + 1);
        expect("LC_CODE_SIGNATURE too short for its range", d, n, "cmdsize 8 < 16");
    }

    /* the loader's gate (src/libval.c): the main executable's policy and each library, in processes of their own */
    fflush(stdout);
    fails += in_child(gate_plain, seed, n, lib, ln, &passes);
    fails += in_child(gate_hardened, seed, n, lib, ln, &passes);

    printf("test_macho: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
