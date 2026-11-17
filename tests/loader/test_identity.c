/*
 * Unit test of the carried-section reader of libhao_identity
 * (src/process_identity.c load_carried), under ASan. It reads a guest file
 * before hao has checked its signature, so each case is a file a hostile
 * guest could be: the reader must keep every copy inside what it allocated,
 * refuse spans it will not allocate, and answer only from what it read.
 *   test_identity SCRATCH_DIR
 */
#include "../../src/process_identity.c"
#include <stdio.h>

static int fails, passes;
static char scratch[1024];

static void note(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passes++; else fails++;
}

/* A thin arm64 MH_EXECUTE with one segment SEG of the given sections; each
 * section's bytes are filled with its index + 1 at its file offset. */
typedef struct { const char *name; uint64_t addr, size; uint32_t offset; } sect;
static size_t build(uint8_t *d, size_t cap, const char *seg, uint32_t initprot, uint64_t vmaddr, uint64_t vmsize,
                    const sect *s, uint32_t ns, size_t fsize)
{
    memset(d, 0, cap);
    struct mach_header_64 *mh = (void *)d;
    mh->magic = MH_MAGIC_64;
    mh->cputype = CPU_TYPE_ARM64;
    mh->cpusubtype = CPU_SUBTYPE_ARM64E;
    mh->filetype = MH_EXECUTE;
    mh->ncmds = 1;
    mh->sizeofcmds = (uint32_t)(sizeof(struct segment_command_64) + ns * sizeof(struct section_64));
    struct segment_command_64 *sg = (void *)(mh + 1);
    sg->cmd = LC_SEGMENT_64;
    sg->cmdsize = mh->sizeofcmds;
    strncpy(sg->segname, seg, 16);
    sg->vmaddr = vmaddr;
    sg->vmsize = vmsize;
    sg->initprot = sg->maxprot = initprot;
    sg->nsects = ns;
    struct section_64 *x = (void *)(sg + 1);
    for (uint32_t k = 0; k < ns; k++) {
        strncpy(x[k].sectname, s[k].name, 16);
        strncpy(x[k].segname, seg, 16);
        x[k].addr = s[k].addr;
        x[k].size = s[k].size;
        x[k].offset = s[k].offset;
        if (s[k].offset < fsize) memset(d + s[k].offset, (int)k + 1, s[k].size < fsize - s[k].offset ? s[k].size : fsize - s[k].offset);
    }
    return fsize;
}

static void put(const char *path, const uint8_t *d, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, d, n) != (ssize_t)n) { perror(path); exit(2); }
    close(fd);
}

/* Read the carried segments of d[0..n) afresh. */
static void carry(const uint8_t *d, size_t n)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/carried.bin", scratch);
    put(path, d, n);
    csegs = NULL;
    ncsegs = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(2); }
    load_carried(fd, (uint64_t)n);
    close(fd);
}

/* The carried section SEG,SECT as native code gets it: its bytes and size. */
static const uint8_t *data_of(const char *seg, const char *sect, unsigned long *size)
{
    *size = 0;
    return hao_getsectiondata(main_header(), seg, sect, size);
}

static bool all(const uint8_t *p, unsigned long n, uint8_t v)
{
    for (unsigned long k = 0; k < n; k++)
        if (p[k] != v) return false;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: test_identity SCRATCH_DIR\n"); return 2; }
    snprintf(scratch, sizeof scratch, "%s", argv[1]);
    size_t cap = 0x4000;
    uint8_t *d = malloc(cap);
    unsigned long size;
    const uint8_t *p;

    /* Two carried __TEXT sections 65 MiB apart with small contents: a span
     * the reader will not allocate. It once allocated one byte for it and
     * copied the first section into that byte. Reached as native code
     * reaches it: a lookup on the main executable's header with the guest
     * path set. */
    {
        sect s[] = {{"__info_plist", 0x100000000ull, 0x1000, 0x1000}, {"__config", 0x100000000ull + (65ull << 20), 0x10, 0x2000}};
        size_t n = build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x5000000, s, 2, 0x3000);
        char path[1100];
        snprintf(path, sizeof path, "%s/guest.bin", scratch);
        put(path, d, n);
        hao_identity_set(path);
        note(!carried_sect(main_header(), "__TEXT", "__info_plist") && !carried_sect(main_header(), "__TEXT", "__config"),
             "carried __TEXT sections spanning 65 MiB: not carried");
    }

    /* the same two sections next to each other: carried, with their bytes */
    {
        sect s[] = {{"__info_plist", 0x100000000ull, 0x1000, 0x1000}, {"__config", 0x100001000ull, 0x10, 0x2000}};
        carry(d, build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x4000, s, 2, 0x3000));
        p = data_of("__TEXT", "__info_plist", &size);
        bool a = p && size == 0x1000 && all(p, size, 1);
        p = data_of("__TEXT", "__config", &size);
        note(a && p && size == 0x10 && all(p, size, 2), "adjacent carried __TEXT sections: carried with their bytes");
    }

    /* exactly CARRY_MAX apart (end to start) is the largest span carried */
    {
        sect s[] = {{"__info_plist", 0x100000000ull, 0x10, 0x1000}, {"__config", 0x100000000ull + CARRY_MAX - 0x10, 0x10, 0x2000}};
        carry(d, build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, CARRY_MAX, s, 2, 0x3000));
        p = data_of("__TEXT", "__config", &size);
        note(p && size == 0x10 && all(p, size, 2), "carried sections spanning exactly 64 MiB: carried");
        sect t[] = {{"__info_plist", 0x100000000ull, 0x10, 0x1000}, {"__config", 0x100000000ull + CARRY_MAX - 0xf, 0x10, 0x2000}};
        carry(d, build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, CARRY_MAX, t, 2, 0x3000));
        note(!data_of("__TEXT", "__config", &size), "one byte more: not carried");
    }

    /* a section whose address + size wraps */
    {
        sect s[] = {{"__info_plist", 0xfffffffffffff000ull, 0x2000, 0x1000}};
        carry(d, build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x4000, s, 1, 0x3000));
        note(!data_of("__TEXT", "__info_plist", &size), "carried section whose end wraps: not carried");
    }

    /* a segment the toolchain does not make, whose section lies 65 MiB past it */
    {
        sect s[] = {{"__marker", 0x200000000ull + (65ull << 20), 0x100, 0x1000}};
        carry(d, build(d, cap, "__MARK", VM_PROT_READ, 0x200000000ull, 0x4000, s, 1, 0x3000));
        note(!data_of("__MARK", "__marker", &size) && !carried_seg(main_header(), "__MARK"),
             "marker segment with a section 65 MiB past it: not carried");
        sect t[] = {{"__marker", 0x200000000ull, 0x100, 0x1000}};
        carry(d, build(d, cap, "__MARK", VM_PROT_READ, 0x200000000ull, 0x4000, t, 1, 0x3000));
        p = data_of("__MARK", "__marker", &size);
        const struct segment_command_64 *c = carried_seg(main_header(), "__MARK");
        note(p && size == 0x100 && all(p, size, 1) && c && c->vmsize == 0x4000, "marker segment: carried whole");
        carry(d, build(d, cap, "__MARK", VM_PROT_READ, 0xfffffffffffff000ull, 0x2000, t, 1, 0x3000));
        note(!carried_seg(main_header(), "__MARK"), "marker segment whose end wraps: not carried");
    }

    /* a section whose bytes are not in the file: carried empty */
    {
        sect s[] = {{"__info_plist", 0x100000000ull, 0x1000, 0x2800}};
        carry(d, build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x4000, s, 1, 0x3000));
        p = data_of("__TEXT", "__info_plist", &size);
        note(!p || size == 0, "carried section past the end of the file: empty");
    }

    /* the arm64 slice of a fat file */
    {
        sect s[] = {{"__info_plist", 0x100000000ull, 0x100, 0x1000}};
        uint8_t *f = calloc(1, 0x8000);
        build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x4000, s, 1, 0x3000);
        memcpy(f + 0x4000, d, 0x3000);
        struct fat_header *fh = (void *)f;
        fh->magic = OSSwapHostToBigInt32(FAT_MAGIC);
        fh->nfat_arch = OSSwapHostToBigInt32(1);
        struct fat_arch *fa = (void *)(fh + 1);
        fa->cputype = (cpu_type_t)OSSwapHostToBigInt32(CPU_TYPE_ARM64);
        fa->cpusubtype = (cpu_subtype_t)OSSwapHostToBigInt32(CPU_SUBTYPE_ARM64E);
        fa->offset = OSSwapHostToBigInt32(0x4000);
        fa->size = OSSwapHostToBigInt32(0x3000);
        carry(f, 0x7000);
        p = data_of("__TEXT", "__info_plist", &size);
        note(p && size == 0x100 && all(p, size, 1), "fat file: the arm64 slice's sections carried");
        fa->size = OSSwapHostToBigInt32(0x4000);           /* past the end of the file */
        carry(f, 0x7000);
        note(!data_of("__TEXT", "__info_plist", &size), "fat file whose arm64 slice overruns it: nothing carried");
        free(f);
    }

    /* load commands that claim more than the slice holds */
    {
        sect s[] = {{"__info_plist", 0x100000000ull, 0x100, 0x1000}};
        build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x4000, s, 1, 0x3000);
        ((struct mach_header_64 *)d)->sizeofcmds = 0x3000;
        carry(d, 0x3000);
        bool a = !data_of("__TEXT", "__info_plist", &size);
        build(d, cap, "__TEXT", VM_PROT_READ | VM_PROT_EXECUTE, 0x100000000ull, 0x4000, s, 1, 0x3000);
        ((struct mach_header_64 *)d)->ncmds = 0xffffffff;
        carry(d, 0x3000);
        note(a && !data_of("__TEXT", "__info_plist", &size), "load commands overrunning the slice, or too many: nothing carried");
    }

    free(d);
    printf("test_identity: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
