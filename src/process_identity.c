/*
 * hao — process identity seen by native code in the CLI / HaoExec route.
 * There dyld's main executable is hao itself. Native frameworks must see
 * the guest instead, including during their own initializers before main():
 *  - its path: CoreFoundation's process path and name, NSBundle.mainBundle,
 *    Swift ArgumentParser usage text (_NSGetExecutablePath);
 *  - the sections they read from the main executable by name: CFBundle's
 *    embedded Info.plist, libxpc's launchd/xpcproxy sections, marker
 *    segments such as SkyLight's __SLSERVER or dyld's __RESTRICT. The
 *    embedded route's stub carries copies of exactly these
 *    (tools/hao-embed CARRY); here the same copies come from the guest file
 *    and answer libmacho's lookups on the main executable's header whenever
 *    hao's own header lacks the section.
 * What the kernel records (process name, proc_pidpath, code identity, audit
 * token) stays hao's in this route; embedding is the remedy (docs/security-model.md).
 * Once libhao has mapped the guest (hao_identity_image), every section of
 * the guest answers such lookups, pointer sections included. The embedded
 * stub links this library too, for that: its carried copies cannot hold
 * the guest's pointers.
 *
 * hao and hao-stub link this library (rather than inserting it), so dyld
 * applies its __interpose tuples process-wide, also to shared-cache code,
 * while the main executable stays dyld image 0. The guest path comes from
 * HAO_EXEC, which stage 1 of hao_cli_main sets and stage 2 unsets before
 * main; it is taken only when it names the same file as argv[0], which is how
 * stage 2 recognises itself, and only in the CLI launcher (its
 * __TEXT,__hao_cli marker): a stub never reads HAO_EXEC, so the environment
 * cannot make an embedded program load another file under its identity.
 */
#include <crt_externs.h>
#include <dlfcn.h>
#include "../include/hao/identity.h"
#include <errno.h>
#include <libkern/OSByteOrder.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/stat.h>
#include <unistd.h>

static char guest_path[1100];

void hao_identity_set(const char *path)
{
    if (!guest_path[0]) {
        size_t n = strlen(path);
        if (n < sizeof guest_path) memcpy(guest_path, path, n + 1);
    }
}

/* HAO_EXEC is read at most twice: by this image's initializer, and by a
 * native initializer that asks before it runs. Later calls (HAO_EXEC is
 * unset before main) do not touch the environment. */
static int tried;
static const struct mach_header_64 *main_header(void);
static void snap(void)
{
    tried = 1;
    unsigned long n = 0;
    if (!getsectiondata(main_header(), "__TEXT", "__hao_cli", &n)) return;   /* not hao's CLI (src/front/cli.c) */
    const char *e = getenv("HAO_EXEC");
    char **argv = *_NSGetArgv();
    char a[1100], b[1100];
    if (e && e[0] == '/' && *_NSGetArgc() > 0 && argv[0] && realpath(argv[0], a) && realpath(e, b) && !strcmp(a, b))
        hao_identity_set(e);
}
__attribute__((constructor)) static void identity_init(void) { if (!guest_path[0]) snap(); }
static const char *guest(void)
{
    if (!guest_path[0] && !tried) snap();
    return guest_path[0] ? guest_path : NULL;
}

static int hao_NSGetExecutablePath(char *buf, uint32_t *size)
{
    if (!guest()) return _NSGetExecutablePath(buf, size);
    size_t need = strlen(guest_path) + 1;
    if (*size < need) { *size = (uint32_t)need; return -1; }
    memcpy(buf, guest_path, need);
    return 0;
}

/* ---- the guest's carried sections */

/* tools/hao-embed CARRY, the same rule: these __TEXT sections, and every
 * section of a non-toolchain, non-executable segment (no zerofill). */
static bool carried(const char *seg, const char *sect, uint32_t initprot, uint32_t flags)
{
    static const char *const text[] = {"__info_plist", "__config", "__launchd", "__launchctl", "__xpcproxy", "__launchd_plist"};
    static const char *const toolchain[] = {"__PAGEZERO", "__TEXT", "__DATA", "__DATA_CONST", "__DATA_DIRTY", "__AUTH",
                                            "__AUTH_CONST", "__LINKEDIT", "__OBJC", "__IMPORT", "__TEXT_EXEC", "__TPRO_CONST"};
    if (!strncmp(seg, "__TEXT", 16)) {
        for (size_t i = 0; i < sizeof text / sizeof *text; i++)
            if (!strncmp(sect, text[i], 16)) return true;
        return false;
    }
    for (size_t i = 0; i < sizeof toolchain / sizeof *toolchain; i++)
        if (!strncmp(seg, toolchain[i], 16)) return false;
    uint32_t type = flags & SECTION_TYPE;
    return !(initprot & VM_PROT_EXECUTE) && type != S_ZEROFILL && type != S_GB_ZEROFILL && type != S_THREAD_LOCAL_ZEROFILL;
}

/* A carried segment: a copy of its load command (only the carried sections)
 * and of its contents, the addresses rewritten so that address + the main
 * executable's slide is the copy, as for a section of the main executable. */
typedef struct { struct segment_command_64 *cmd; uint8_t *mem; bool text; } cseg;
static cseg *csegs;
static uint32_t ncsegs;

static const struct mach_header_64 *main_header(void) { return (const struct mach_header_64 *)_NSGetMachExecuteHeader(); }

static intptr_t slide_of(const struct mach_header_64 *mh)
{
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strncmp(((const struct segment_command_64 *)lc)->segname, "__TEXT", 16))
            return (intptr_t)mh - (intptr_t)((const struct segment_command_64 *)lc)->vmaddr;
    return 0;
}

static bool in_file(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size - off; }

/* The arm64 slice hao runs (src/macho.c choose_slice): plain arm64e > other arm64e > arm64. */
static bool arm_slice(const uint8_t *f, uint64_t fsize, uint64_t *off, uint64_t *len)
{
    uint32_t magic;
    if (fsize < 4) return false;
    memcpy(&magic, f, 4);
    if (magic == MH_MAGIC_64) {
        if (fsize < sizeof(struct mach_header_64) || ((const struct mach_header_64 *)f)->cputype != CPU_TYPE_ARM64) return false;
        *off = 0; *len = fsize;
        return true;
    }
    if (magic != FAT_CIGAM && magic != FAT_CIGAM_64) return false;
    uint32_t n = OSSwapBigToHostInt32(((const struct fat_header *)f)->nfat_arch);
    uint64_t esz = magic == FAT_CIGAM_64 ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
    if (n == 0 || n > 64 || !in_file(sizeof(struct fat_header), n * esz, fsize)) return false;
    int best = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *a = f + sizeof(struct fat_header) + k * esz;
        uint32_t cpu, sub;
        uint64_t o, z;
        if (magic == FAT_CIGAM_64) {
            const struct fat_arch_64 *x = (const void *)a;
            cpu = OSSwapBigToHostInt32((uint32_t)x->cputype); sub = OSSwapBigToHostInt32((uint32_t)x->cpusubtype);
            o = OSSwapBigToHostInt64(x->offset); z = OSSwapBigToHostInt64(x->size);
        } else {
            const struct fat_arch *x = (const void *)a;
            cpu = OSSwapBigToHostInt32((uint32_t)x->cputype); sub = OSSwapBigToHostInt32((uint32_t)x->cpusubtype);
            o = OSSwapBigToHostInt32(x->offset); z = OSSwapBigToHostInt32(x->size);
        }
        if (cpu != CPU_TYPE_ARM64) continue;
        uint32_t caps = sub & CPU_SUBTYPE_MASK;
        int rank = (sub & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E ? (caps == 0 || caps == 0x80000000u ? 3 : 2) : 1;
        if (rank > best && in_file(o, z, fsize)) { best = rank; *off = o; *len = z; }
    }
    return best > 0;
}

static void load_carried(const uint8_t *f, uint64_t fsize)
{
    uint64_t so, sl;
    if (!arm_slice(f, fsize, &so, &sl) || sl < sizeof(struct mach_header_64)) return;
    const uint8_t *mo = f + so;
    const struct mach_header_64 *mh = (const void *)mo;
    if (mh->magic != MH_MAGIC_64 || mh->filetype != MH_EXECUTE || !in_file(sizeof *mh, mh->sizeofcmds, sl)) return;
    const uint8_t *lc = mo + sizeof *mh, *end = lc + mh->sizeofcmds;
    intptr_t main_slide = slide_of(main_header());
    cseg *v = calloc(mh->ncmds ? mh->ncmds : 1, sizeof *v);
    if (!v) return;
    uint32_t nv = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *c = (const void *)lc;
        if (end - lc < (long)sizeof *c || c->cmdsize < sizeof *c || c->cmdsize > (uint64_t)(end - lc)) break;
        if (c->cmd == LC_SEGMENT_64 && c->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *sg = (const void *)c;
            const struct section_64 *s = (const void *)(sg + 1);
            uint32_t ns = sg->nsects <= (c->cmdsize - sizeof *sg) / sizeof *s ? sg->nsects : 0, keep = 0;
            bool text = !strncmp(sg->segname, "__TEXT", 16);
            for (uint32_t k = 0; k < ns; k++) keep += carried(sg->segname, s[k].sectname, sg->initprot, s[k].flags);
            if (keep && (text || sg->vmsize <= (64u << 20))) {
                /* __TEXT: only the carried sections' bytes (laid out at their
                 * offsets from the first); other segments: all of it. */
                uint64_t lo = text ? UINT64_MAX : sg->vmaddr, hi = text ? 0 : sg->vmaddr + sg->vmsize;
                for (uint32_t k = 0; k < ns; k++)
                    if (carried(sg->segname, s[k].sectname, sg->initprot, s[k].flags)) {
                        if (s[k].addr < lo) lo = s[k].addr;
                        if (s[k].addr + s[k].size > hi) hi = s[k].addr + s[k].size;
                    }
                struct segment_command_64 *cmd = calloc(1, sizeof *cmd + keep * sizeof *s);
                uint8_t *mem = hi > lo && hi - lo <= (64u << 20) ? calloc(1, hi - lo) : calloc(1, 1);
                if (!cmd || !mem) { free(cmd); free(mem); continue; }
                *cmd = *sg;
                cmd->cmdsize = (uint32_t)(sizeof *cmd + keep * sizeof *s);
                cmd->nsects = keep;
                cmd->vmaddr = (uint64_t)((intptr_t)mem - main_slide);
                cmd->vmsize = hi > lo ? hi - lo : 0;
                cmd->fileoff = cmd->filesize = 0;
                struct section_64 *d = (struct section_64 *)(cmd + 1);
                for (uint32_t k = 0, j = 0; k < ns; k++) {
                    if (!carried(sg->segname, s[k].sectname, sg->initprot, s[k].flags)) continue;
                    d[j] = s[k];
                    d[j].addr = cmd->vmaddr + (s[k].addr - lo);
                    d[j].offset = 0;
                    if (s[k].size && in_file(s[k].offset, s[k].size, sl) && s[k].addr >= lo && s[k].addr + s[k].size <= hi)
                        memcpy(mem + (s[k].addr - lo), mo + s[k].offset, s[k].size);
                    else d[j].size = 0;
                    j++;
                }
                v[nv++] = (cseg){cmd, mem, text};
            }
        }
        lc += c->cmdsize;
    }
    csegs = v;
    ncsegs = nv;
}

static pthread_once_t carried_once = PTHREAD_ONCE_INIT;
static void carried_init(void)
{
    int fd = open(guest_path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        void *f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (f != MAP_FAILED) { load_carried(f, (uint64_t)st.st_size); munmap(f, (size_t)st.st_size); }
    }
    close(fd);
}

/* The guest's own mapped image, once libhao has loaded it (hao_identity_image):
 * every segment, as load-command copies whose addresses + the main
 * executable's slide are the guest's memory. Its sections hold the guest's
 * rebased pointers, its function pointers thunked (src/fnslots.c), so native
 * code can also use pointer sections it reads from the main executable
 * (libsystem_darwin's os_subcommand_main: __DATA_CONST,__subcommands). */
typedef struct { struct segment_command_64 **cmd; uint32_t n; } mapped_t;
static mapped_t *_Atomic mapped;

void hao_identity_image(const void *base)
{
    const struct mach_header_64 *gh = base;
    if (!gh || gh->magic != MH_MAGIC_64 || atomic_load(&mapped)) return;
    intptr_t delta = slide_of(gh) - slide_of(main_header());
    mapped_t *m = calloc(1, sizeof *m);
    struct segment_command_64 **v = calloc(gh->ncmds ? gh->ncmds : 1, sizeof *v);
    if (!m || !v) { free(m); free(v); return; }
    const struct load_command *lc = (const void *)(gh + 1);
    for (uint32_t i = 0; i < gh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        struct segment_command_64 *c = malloc(lc->cmdsize);
        if (!c) continue;
        memcpy(c, lc, lc->cmdsize);
        c->vmaddr += (uint64_t)delta;
        struct section_64 *sc = (struct section_64 *)(c + 1);
        for (uint32_t k = 0; k < c->nsects; k++) sc[k].addr += (uint64_t)delta;
        v[m->n++] = c;
    }
    m->cmd = v;
    atomic_store(&mapped, m);
}

/* The guest's SEG (a segment the main header lacks) or SEG,SECT, for lookups
 * on the main executable's header: from its mapped image, or before libhao
 * has loaded it (native initializers in the HaoExec route), the carried copies
 * from its file. */
static const struct segment_command_64 *carried_seg(const struct mach_header_64 *mh, const char *seg)
{
    if (mh != main_header()) return NULL;
    mapped_t *m = atomic_load(&mapped);
    if (m) {
        for (uint32_t i = 0; i < m->n; i++)
            if (!strncmp(m->cmd[i]->segname, seg, 16)) return m->cmd[i];
        return NULL;
    }
    if (!guest()) return NULL;
    pthread_once(&carried_once, carried_init);
    for (uint32_t i = 0; i < ncsegs; i++)
        if (!csegs[i].text && !strncmp(csegs[i].cmd->segname, seg, 16)) return csegs[i].cmd;
    return NULL;
}
static const struct section_64 *find_sect(const struct segment_command_64 *c, const char *seg, const char *sect)
{
    if (strncmp(c->segname, seg, 16)) return NULL;
    const struct section_64 *s = (const void *)(c + 1);
    for (uint32_t k = 0; k < c->nsects; k++)
        if (!strncmp(s[k].sectname, sect, 16)) return &s[k];
    return NULL;
}
static const struct section_64 *carried_sect(const struct mach_header_64 *mh, const char *seg, const char *sect)
{
    if (mh != main_header()) return NULL;
    const struct section_64 *s;
    mapped_t *m = atomic_load(&mapped);
    if (m) {
        for (uint32_t i = 0; i < m->n; i++)
            if ((s = find_sect(m->cmd[i], seg, sect))) return s;
        /* Linker sets (os/linker_set.h, LINKER_SET_FOREACH on the main
         * executable) are in __DATA on x86_64 and in __DATA_CONST on arm64:
         * x86_64 libsystem_darwin's os_subcommand_main asks for
         * __DATA,__subcommands, arm64 programs have __DATA_CONST,__subcommands.
         * The entries are pointers either way. */
        if (!strncmp(seg, "__DATA", 16))
            for (uint32_t i = 0; i < m->n; i++)
                if ((s = find_sect(m->cmd[i], "__DATA_CONST", sect))) return s;
        return NULL;
    }
    if (!guest()) return NULL;
    pthread_once(&carried_once, carried_init);
    for (uint32_t i = 0; i < ncsegs; i++)
        if ((s = find_sect(csegs[i].cmd, seg, sect))) return s;
    return NULL;
}

/* ---- libmacho on the main executable: hao's own header first, then the carried copies.
 * The deprecated functions are interposed because native code still calls them.
 * As in libmacho, the functions that return a section_64, a segment or a
 * section's address (getsectbyname, getsegbyname, getsectdata, the
 * ...fromheader_64 forms) give unslid addresses: the copies' addresses, which
 * the caller slides by the main executable's slide (_dyld_get_image_slide,
 * as LINKER_SET_FOREACH does). getsectiondata and getsegmentdata return the
 * data itself: the copy's address plus the slide of the header they were
 * given (the main executable's: carried_sect/carried_seg answer for no other),
 * read from that header as libmacho reads it. That holds in both routes,
 * whether or not a lookup was made before the guest was mapped (an embedded
 * stub carries its copies itself, so there the carried path never runs). */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static const struct section_64 *hao_getsectbynamefromheader_64(const struct mach_header_64 *mh, const char *seg, const char *sect)
{
    const struct section_64 *s = getsectbynamefromheader_64(mh, seg, sect);
    return s ? s : carried_sect(mh, seg, sect);
}
static const struct section_64 *hao_getsectbyname(const char *seg, const char *sect)
{
    return hao_getsectbynamefromheader_64(main_header(), seg, sect);
}
static const struct segment_command_64 *hao_getsegbyname(const char *seg)
{
    const struct segment_command_64 *s = getsegbyname(seg);
    return s ? s : carried_seg(main_header(), seg);
}
static char *hao_getsectdatafromheader_64(const struct mach_header_64 *mh, const char *seg, const char *sect, uint64_t *size)
{
    char *d = getsectdatafromheader_64(mh, seg, sect, size);
    const struct section_64 *s;
    if (!d && (s = carried_sect(mh, seg, sect))) { *size = s->size; d = (char *)(uintptr_t)s->addr; }
    return d;
}
static char *hao_getsectdata(const char *seg, const char *sect, unsigned long *size)
{
    return hao_getsectdatafromheader_64(main_header(), seg, sect, (uint64_t *)size);
}
static uint8_t *hao_getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size)
{
    uint8_t *d = getsectiondata(mh, seg, sect, size);
    const struct section_64 *s;
    if (!d && (s = carried_sect(mh, seg, sect))) { *size = s->size; d = (uint8_t *)(uintptr_t)(s->addr + (uint64_t)slide_of(mh)); }
    return d;
}
static uint8_t *hao_getsegmentdata(const struct mach_header_64 *mh, const char *seg, unsigned long *size)
{
    uint8_t *d = getsegmentdata(mh, seg, size);
    const struct segment_command_64 *s;
    if (!d && (s = carried_seg(mh, seg))) { *size = s->vmsize; d = (uint8_t *)(uintptr_t)(s->vmaddr + (uint64_t)slide_of(mh)); }
    return d;
}

/* Native -> libdyld address queries never visit interpose.c's guest traps.
 * Publish one resolver; it reads only the loader's immutable, retained metadata.
 * Unknown/native addresses retain dyld's answer, including native callers in
 * this image's own initializers before hao has registered the resolver. */
static _Atomic(hao_identity_lookup_fn) image_lookup;
void hao_identity_set_image_lookup(hao_identity_lookup_fn lookup)
{
    atomic_store_explicit(&image_lookup, lookup, memory_order_release);
}
static bool guest_address(const void *address, hao_identity_info *info)
{
    hao_identity_lookup_fn lookup = atomic_load_explicit(&image_lookup, memory_order_acquire);
    return lookup && lookup(address, info);
}
extern const char *dyld_image_path_containing_address(const void *);
extern const struct mach_header *dyld_image_header_containing_address(const void *);
extern intptr_t _dyld_get_image_slide(const struct mach_header *);
static const char *hao_dyld_image_path_containing_address(const void *address)
{
    hao_identity_info info;
    return guest_address(address, &info) ? info.path : dyld_image_path_containing_address(address);
}
static const struct mach_header *hao_dyld_image_header_containing_address(const void *address)
{
    hao_identity_info info;
    return guest_address(address, &info) ? info.header : dyld_image_header_containing_address(address);
}
static intptr_t hao_dyld_get_image_slide(const struct mach_header *header)
{
    hao_identity_info info;
    return guest_address(header, &info) && info.header == header ? info.slide : _dyld_get_image_slide(header);
}
static int hao_dladdr(const void *address, Dl_info *result)
{
    hao_identity_info info;
    if (!guest_address(address, &info)) return dladdr(address, result);
    *result = (Dl_info){info.path, (void *)info.header, NULL, NULL};
    return 1;
}

/* ---- the tagged-pointer switch in the exec environment of hao children
 * libobjc reads OBJC_DISABLE_TAG_OBFUSCATION once, while it initialises
 * before main (objc4 environ_init), and hao needs it on (src/main.c,
 * HAO_TAG_ENV). hao_self_main re-execs an embedded stub to set it unless
 * libobjc already took it from the exec environment. That re-exec is an exec
 * native watchers see: launchd reports a job spawned on its first NOTE_EXEC,
 * and LaunchServices then resumes a start-suspended app with SIGCONT, which
 * a re-exec that comes first swallows. So every exec this process makes of
 * such a stub passes the switch, in a copy of the caller's environment
 * array. Every exec path reaches execve, __mac_execve or posix_spawn in
 * libsystem_kernel: execv/execvp/execvP/execl*, posix_spawnp, system and
 * popen in libsystem_c call them (interposed here, also for shared-cache
 * callers such as NSTask), guest imports bind to these interposers
 * (dlsym), and a guest's raw svc goes through hao_identity_exec_env
 * (src/syscall.c). Other targets get the environment as given: native
 * programs keep tagged-pointer obfuscation and their environment, a stub
 * marked __hao_noreexec never re-execs, and the CLI (the HaoExec route)
 * sets the switch for its own second stage. */
#define TAG_KEY "OBJC_DISABLE_TAG_OBFUSCATION="
static char tag_on[] = TAG_KEY "YES";

/* The x86_64 slice at OFF is a hao front end that re-execs: it links
 * libhao.dylib and has neither __TEXT,__hao_noreexec nor __TEXT,__hao_cli. */
static bool reexec_slice(int fd, uint64_t off, uint64_t fsize)
{
    struct mach_header_64 mh;
    if (!in_file(off, sizeof mh, fsize) || pread(fd, &mh, sizeof mh, (off_t)off) != (ssize_t)sizeof mh ||
        mh.magic != MH_MAGIC_64 || mh.cputype != CPU_TYPE_X86_64 || mh.filetype != MH_EXECUTE ||
        mh.sizeofcmds > (1u << 20) || !in_file(off + sizeof mh, mh.sizeofcmds, fsize))
        return false;
    uint64_t base = off & ~(uint64_t)(getpagesize() - 1), len = off - base + sizeof mh + mh.sizeofcmds;
    uint8_t *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, (off_t)base);
    if (m == MAP_FAILED) return false;
    const uint8_t *lc = m + (off - base) + sizeof mh, *end = lc + mh.sizeofcmds;
    bool hao = false, quiet = false;
    for (uint32_t i = 0; i < mh.ncmds && end - lc >= (long)sizeof(struct load_command); i++) {
        const struct load_command *c = (const void *)lc;
        if (c->cmdsize < sizeof *c || c->cmdsize > (uint64_t)(end - lc)) break;
        if ((c->cmd == LC_LOAD_DYLIB || c->cmd == LC_LOAD_WEAK_DYLIB || c->cmd == LC_REEXPORT_DYLIB ||
             c->cmd == LC_LAZY_LOAD_DYLIB || c->cmd == LC_LOAD_UPWARD_DYLIB) && c->cmdsize > sizeof(struct dylib_command)) {
            uint32_t at = ((const struct dylib_command *)c)->dylib.name.offset;
            if (at < c->cmdsize) {
                const char *nm = (const char *)lc + at;
                size_t nl = strnlen(nm, c->cmdsize - at);
                if (nl >= 12 && !memcmp(nm + nl - 12, "libhao.dylib", 12) && (nl == 12 || nm[nl - 13] == '/')) hao = true;
            }
        } else if (c->cmd == LC_SEGMENT_64 && c->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *sg = (const void *)c;
            const struct section_64 *s = (const void *)(sg + 1);
            if (!strncmp(sg->segname, "__TEXT", 16) && sg->nsects <= (c->cmdsize - sizeof *sg) / sizeof *s)
                for (uint32_t k = 0; k < sg->nsects; k++)
                    if (!strncmp(s[k].sectname, "__hao_noreexec", 16) || !strncmp(s[k].sectname, "__hao_cli", 16)) quiet = true;
        }
        lc += c->cmdsize;
    }
    munmap(m, len);
    return hao && !quiet;
}

/* PATH, as the kernel would run it, starts as a stub that re-execs: a Mach-O
 * file with such an x86_64 slice, or a "#!" script whose interpreter is one. */
static bool reexec_target(const char *path, int depth)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);   /* not a FIFO's writer wait */
    if (fd < 0) return false;
    bool r = false;
    struct stat st;
    uint8_t h[1024];
    ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? pread(fd, h, sizeof h, 0) : -1;
    uint32_t magic = 0;
    if (n >= 4) memcpy(&magic, h, 4);
    if (n >= 3 && h[0] == '#' && h[1] == '!') {
        if (depth == 0) {
            /* the interpreter path: after "#!" and blanks, up to a blank or the line's end */
            char ip[sizeof h];
            ssize_t k = 2, j = 0;
            while (k < n && (h[k] == ' ' || h[k] == '\t')) k++;
            while (k < n && h[k] != ' ' && h[k] != '\t' && h[k] != '\n' && h[k]) ip[j++] = (char)h[k++];
            ip[j] = 0;
            if (j && k < n) r = reexec_target(ip, 1);
        }
    } else if (magic == MH_MAGIC_64) {
        r = reexec_slice(fd, 0, (uint64_t)st.st_size);
    } else if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) {
        uint32_t na = OSSwapBigToHostInt32(((const struct fat_header *)(const void *)h)->nfat_arch);
        uint64_t esz = magic == FAT_CIGAM_64 ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
        for (uint32_t k = 0; k < na && sizeof(struct fat_header) + (k + 1) * esz <= (uint64_t)n && !r; k++) {
            const uint8_t *a = h + sizeof(struct fat_header) + k * esz;
            uint32_t cpu;
            uint64_t o;
            if (magic == FAT_CIGAM_64) {
                const struct fat_arch_64 *x = (const void *)a;
                cpu = OSSwapBigToHostInt32((uint32_t)x->cputype); o = OSSwapBigToHostInt64(x->offset);
            } else {
                const struct fat_arch *x = (const void *)a;
                cpu = OSSwapBigToHostInt32((uint32_t)x->cputype); o = OSSwapBigToHostInt32(x->offset);
            }
            if (cpu == CPU_TYPE_X86_64) r = reexec_slice(fd, o, (uint64_t)st.st_size);
        }
    }
    close(fd);
    return r;
}

/* ENVP for an exec of PATH: NULL to pass it as given, else a copy with the
 * switch on, *LEN bytes to munmap after the call. No malloc: a vfork child
 * or a forked child of a threaded parent may make it. errno is kept. */
char **hao_identity_exec_env(const char *path, char *const *envp, size_t *len)
{
    int e = errno;
    size_t n = 0, at = SIZE_MAX;
    for (; envp && envp[n]; n++)
        if (!strncmp(envp[n], TAG_KEY, sizeof TAG_KEY - 1)) at = n;   /* libobjc keeps the last one */
    char **v = NULL;
    if ((at == SIZE_MAX || strcmp(envp[at] + sizeof TAG_KEY - 1, "YES")) && path && reexec_target(path, 0)) {
        *len = (n + 2) * sizeof *v;
        v = mmap(NULL, *len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (v == MAP_FAILED) v = NULL;
        else {
            for (size_t k = 0; k < n; k++) v[k] = envp[k];
            if (at != SIZE_MAX) v[at] = tag_on;
            else v[n++] = tag_on;
            v[n] = NULL;
        }
    }
    errno = e;
    return v;
}

static void exec_env_done(char **v, size_t len)
{
    if (!v) return;
    int e = errno;
    munmap(v, len);
    errno = e;
}

static int hao_execve(const char *path, char *const argv[], char *const envp[])
{
    size_t len = 0;
    char **v = hao_identity_exec_env(path, envp, &len);
    int r = execve(path, argv, v ? v : envp);
    exec_env_done(v, len);
    return r;
}

extern int __mac_execve(char *path, char **argv, char **envp, void *mac);
static int hao_mac_execve(char *path, char **argv, char **envp, void *mac)
{
    size_t len = 0;
    char **v = hao_identity_exec_env(path, envp, &len);
    int r = __mac_execve(path, argv, v ? v : envp, mac);
    exec_env_done(v, len);
    return r;
}

static int hao_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                           char *const argv[], char *const envp[])
{
    size_t len = 0;
    char **v = hao_identity_exec_env(path, envp, &len);
    int r = posix_spawn(pid, path, fa, attr, argv, v ? v : envp);
    exec_env_done(v, len);
    return r;
}

/* ---- SIGSEGV and SIGBUS installed by native code
 * They are hao's (src/runtime.c on_fault): a native call of guest code
 * faults on its non-executable page and on_fault runs it through the
 * translator. A native library that installs its own handler for them with
 * the native API (LLVM's CrashRecoveryContext and signal handlers in
 * libBNNS, crash reporters, JIT runtimes) would replace on_fault in the
 * kernel, and every later native->guest call on any thread would reach that
 * handler instead. So libsystem_c's entry points that set a disposition
 * (sigaction, signal, bsd_signal, sigset, sigvec, sigignore: they call each
 * other inside libsystem_c, out of reach of interposing, so each one is
 * interposed) hand these two signals to libhao's shadow (src/signals.c
 * hao_native_sigaction), which keeps on_fault installed, reports back what
 * the caller installed and calls that handler for the faults that are not
 * hao's. Every other signal, and every call made before libhao registers the
 * hook (native initializers: hao_signals_init takes their handlers over from
 * the kernel), reaches libsystem_c unchanged. Each body mirrors the 27.0
 * x86_64 libsystem_c function it replaces. Interposing applies to every
 * image but this one, libhao included: libhao calls
 * hao_identity_real_sigaction. */
typedef int (*sig_hook_t)(int, const struct sigaction *, struct sigaction *);
static _Atomic(sig_hook_t) sig_hook;

void hao_identity_sig_hook(sig_hook_t fn) { atomic_store(&sig_hook, fn); }
int hao_identity_real_sigaction(int sig, const struct sigaction *act, struct sigaction *oact) { return sigaction(sig, act, oact); }

static sig_hook_t sig_owned(int sig) { return sig == SIGSEGV || sig == SIGBUS ? atomic_load(&sig_hook) : NULL; }

/* ---- signal masks that block SIGSEGV or SIGBUS
 * libhao reads arbitrary argument values with a load that may fault
 * (hao_probe_read8, src/signals.c), which needs SIGSEGV and SIGBUS
 * deliverable: xnu re-executes a fault whose signal is blocked forever. Each
 * change of a thread's mask through pthread_sigmask or sigprocmask (libc's
 * sigset/sighold, setjmp/longjmp and libhao's own calls reach them through
 * these interposers) is reported to libhao, and so is a handler installed
 * for any signal with SIGSEGV or SIGBUS in its sa_mask, which the kernel
 * applies while it runs. The calls themselves are unchanged. */
typedef void (*mask_hook_t)(int blocked);
static _Atomic(mask_hook_t) mask_hook;
static atomic_int handler_masks_sync;
static _Atomic(void (*)(void)) handler_hook;

/* libhao: fn(blocked) on every mask change, hfn() on such a handler; returns whether one was installed already */
int hao_identity_mask_hook(mask_hook_t fn, void (*hfn)(void))
{
    atomic_store(&handler_hook, hfn);
    atomic_store(&mask_hook, fn);
    return atomic_load(&handler_masks_sync);
}

static bool sync_masked(const sigset_t *m) { return sigismember(m, SIGSEGV) == 1 || sigismember(m, SIGBUS) == 1; }

static void mask_note(int how, const sigset_t *set, const sigset_t *old)
{
    mask_hook_t h = atomic_load(&mask_hook);
    if (!h) return;
    sigset_t n = *old;
    if (how == SIG_BLOCK) n |= *set;
    else if (how == SIG_UNBLOCK) n &= ~*set;
    else n = *set;
    h(sync_masked(&n));
}

static int hao_pthread_sigmask(int how, const sigset_t *set, sigset_t *oset)
{
    sigset_t s, o;
    if (set) s = *set;   /* oset may be set */
    int r = pthread_sigmask(how, set, oset ? oset : &o);
    if (r == 0 && set) mask_note(how, &s, oset ? oset : &o);
    return r;
}

static int hao_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    sigset_t s, o;
    if (set) s = *set;
    int r = sigprocmask(how, set, oset ? oset : &o);
    if (r == 0 && set) mask_note(how, &s, oset ? oset : &o);
    return r;
}

static void handler_note(const struct sigaction *act)
{
    if (!act || act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN || !sync_masked(&act->sa_mask)) return;
    atomic_store(&handler_masks_sync, 1);
    void (*h)(void) = atomic_load(&handler_hook);
    if (h) h();
}

/* ---- calls that can map memory or make it readable
 * libhao remembers argument values whose read faulted (src/blocks.c
 * read_words) for as long as no such call has succeeded since: each one
 * that does counts here. The calls themselves are unchanged. */
static _Atomic(uint64_t) vm_gen;

const uint64_t *hao_identity_vm_gen(void) { return (const uint64_t *)&vm_gen; }

static void vm_changed(void) { atomic_fetch_add_explicit(&vm_gen, 1, memory_order_release); }
#define VM_KR(call) do { kern_return_t kr_ = (call); if (kr_ == KERN_SUCCESS) vm_changed(); return kr_; } while (0)

static void *hao_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    void *r = mmap(addr, len, prot, flags, fd, off);
    if (r != MAP_FAILED) vm_changed();
    return r;
}

static int hao_mprotect(void *addr, size_t len, int prot)
{
    int r = mprotect(addr, len, prot);
    if (r == 0) vm_changed();
    return r;
}

static void *hao_shmat(int id, const void *addr, int flags)
{
    void *r = shmat(id, addr, flags);
    if (r != (void *)-1) vm_changed();
    return r;
}

static kern_return_t hao_mach_vm_allocate(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t sz, int fl)
{
    VM_KR(mach_vm_allocate(t, a, sz, fl));
}

static kern_return_t hao_mach_vm_map(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t sz, mach_vm_offset_t mask, int fl,
                                     mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy, vm_prot_t cur,
                                     vm_prot_t max, vm_inherit_t inh)
{
    VM_KR(mach_vm_map(t, a, sz, mask, fl, obj, off, copy, cur, max, inh));
}

static kern_return_t hao_mach_vm_remap(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t sz, mach_vm_offset_t mask, int fl,
                                       vm_map_t src, mach_vm_address_t sa, boolean_t copy, vm_prot_t *cur, vm_prot_t *max,
                                       vm_inherit_t inh)
{
    VM_KR(mach_vm_remap(t, a, sz, mask, fl, src, sa, copy, cur, max, inh));
}

static kern_return_t hao_mach_vm_remap_new(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t sz, mach_vm_offset_t mask, int fl,
                                           vm_map_read_t src, mach_vm_address_t sa, boolean_t copy, vm_prot_t *cur,
                                           vm_prot_t *max, vm_inherit_t inh)
{
    VM_KR(mach_vm_remap_new(t, a, sz, mask, fl, src, sa, copy, cur, max, inh));
}

static kern_return_t hao_mach_vm_protect(vm_map_t t, mach_vm_address_t a, mach_vm_size_t sz, boolean_t setmax, vm_prot_t p)
{
    VM_KR(mach_vm_protect(t, a, sz, setmax, p));
}

static kern_return_t hao_mach_vm_read(vm_map_read_t t, mach_vm_address_t a, mach_vm_size_t sz, vm_offset_t *data,
                                      mach_msg_type_number_t *cnt)
{
    VM_KR(mach_vm_read(t, a, sz, data, cnt));
}

static kern_return_t hao_vm_allocate(vm_map_t t, vm_address_t *a, vm_size_t sz, int fl) { VM_KR(vm_allocate(t, a, sz, fl)); }

static kern_return_t hao_vm_map(vm_map_t t, vm_address_t *a, vm_size_t sz, vm_address_t mask, int fl, mem_entry_name_port_t obj,
                                vm_offset_t off, boolean_t copy, vm_prot_t cur, vm_prot_t max, vm_inherit_t inh)
{
    VM_KR(vm_map(t, a, sz, mask, fl, obj, off, copy, cur, max, inh));
}

static kern_return_t hao_vm_remap(vm_map_t t, vm_address_t *a, vm_size_t sz, vm_address_t mask, int fl, vm_map_t src,
                                  vm_address_t sa, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh)
{
    VM_KR(vm_remap(t, a, sz, mask, fl, src, sa, copy, cur, max, inh));
}

static kern_return_t hao_vm_remap_new(vm_map_t t, vm_address_t *a, vm_size_t sz, vm_address_t mask, int fl, vm_map_read_t src,
                                      vm_address_t sa, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh)
{
    VM_KR(vm_remap_new(t, a, sz, mask, fl, src, sa, copy, cur, max, inh));
}

static kern_return_t hao_vm_protect(vm_map_t t, vm_address_t a, vm_size_t sz, boolean_t setmax, vm_prot_t p)
{
    VM_KR(vm_protect(t, a, sz, setmax, p));
}

static kern_return_t hao_vm_read(vm_map_read_t t, vm_address_t a, vm_size_t sz, vm_offset_t *data, mach_msg_type_number_t *cnt)
{
    VM_KR(vm_read(t, a, sz, data, cnt));
}

static int hao_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    sig_hook_t h = sig_owned(sig);
    if (h) return h(sig, act, oact);
    int r = sigaction(sig, act, oact);
    if (r == 0) handler_note(act);
    return r;
}

/* SA_RESTART unless siginterrupt() cleared it: libsystem_c keeps that in a
 * private set (_sigintr) this cannot read, so siginterrupt has no effect on
 * these two signals' later signal() calls */
static void (*hao_signal(int sig, void (*fn)(int)))(int)
{
    sig_hook_t h = sig_owned(sig);
    if (!h) return signal(sig, fn);
    struct sigaction sa, osa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_flags = SA_RESTART;
    return h(sig, &sa, &osa) < 0 ? SIG_ERR : osa.sa_handler;
}

/* the signal itself blocked while its handler runs, SA_RESTART */
static void (*hao_bsd_signal(int sig, void (*fn)(int)))(int)
{
    sig_hook_t h = sig_owned(sig);
    if (!h) return bsd_signal(sig, fn);
    struct sigaction sa, osa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_mask = sigmask(sig);
    sa.sa_flags = SA_RESTART;
    return h(sig, &sa, &osa) < 0 ? SIG_ERR : osa.sa_handler;
}

/* struct sigvec is struct sigaction's layout with SV_INTERRUPT the inverse of SA_RESTART */
static int hao_sigvec(int sig, struct sigvec *sv, struct sigvec *osv)
{
    sig_hook_t h = sig_owned(sig);
    if (!h) {
        struct sigvec v;
        if (sv) v = *sv;
        int r = sigvec(sig, sv, osv);
        if (r == 0 && sv) {
            struct sigaction a = {.sa_handler = v.sv_handler, .sa_mask = (sigset_t)v.sv_mask};
            handler_note(&a);
        }
        return r;
    }
    _Static_assert(sizeof(struct sigvec) == sizeof(struct sigaction) && SV_INTERRUPT == SA_RESTART, "sigvec layout");
    struct sigaction sa, osa;
    if (sv) { memcpy(&sa, sv, sizeof sa); sa.sa_flags ^= SV_INTERRUPT; }
    int r = h(sig, sv ? &sa : NULL, osv ? &osa : NULL);
    if (r == 0 && osv) { osa.sa_flags ^= SV_INTERRUPT; memcpy(osv, &osa, sizeof *osv); }
    return r;
}

static int hao_sigignore(int sig)
{
    if (!sig_owned(sig)) return sigignore(sig);
    return hao_signal(sig, SIG_IGN) == SIG_ERR ? -1 : 0;
}

static void (*hao_sigset(int sig, void (*disp)(int)))(int)
{
    sig_hook_t h = sig_owned(sig);
    if (!h) return sigset(sig, disp);
    sigset_t m;
    if (sigprocmask(SIG_BLOCK, NULL, &m) == -1) return SIG_ERR;
    bool held = sigismember(&m, sig);
    if (disp == SIG_HOLD) {
        if (held) return SIG_HOLD;
        struct sigaction oa;
        if (h(sig, NULL, &oa) == -1 || sighold(sig) == -1) return SIG_ERR;
        return oa.sa_handler;
    }
    if (held && sigrelse(sig) == -1) return SIG_ERR;
    void (*r)(int) = hao_signal(sig, disp);
    return r == SIG_ERR ? SIG_ERR : held ? SIG_HOLD : r;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } hao_interposers[] = {
    {(const void *)hao_dyld_image_path_containing_address, (const void *)dyld_image_path_containing_address},
    {(const void *)hao_dyld_image_header_containing_address, (const void *)dyld_image_header_containing_address},
    {(const void *)hao_dyld_get_image_slide, (const void *)_dyld_get_image_slide},
    {(const void *)hao_dladdr, (const void *)dladdr},
    {(const void *)hao_execve, (const void *)execve},
    {(const void *)hao_mac_execve, (const void *)__mac_execve},
    {(const void *)hao_posix_spawn, (const void *)posix_spawn},
    {(const void *)hao_sigaction, (const void *)sigaction},
    {(const void *)hao_signal, (const void *)signal},
    {(const void *)hao_bsd_signal, (const void *)bsd_signal},
    {(const void *)hao_sigset, (const void *)sigset},
    {(const void *)hao_sigvec, (const void *)sigvec},
    {(const void *)hao_sigignore, (const void *)sigignore},
    {(const void *)hao_pthread_sigmask, (const void *)pthread_sigmask},
    {(const void *)hao_sigprocmask, (const void *)sigprocmask},
    {(const void *)hao_mmap, (const void *)mmap},
    {(const void *)hao_mprotect, (const void *)mprotect},
    {(const void *)hao_shmat, (const void *)shmat},
    {(const void *)hao_mach_vm_allocate, (const void *)mach_vm_allocate},
    {(const void *)hao_mach_vm_map, (const void *)mach_vm_map},
    {(const void *)hao_mach_vm_remap, (const void *)mach_vm_remap},
    {(const void *)hao_mach_vm_remap_new, (const void *)mach_vm_remap_new},
    {(const void *)hao_mach_vm_protect, (const void *)mach_vm_protect},
    {(const void *)hao_mach_vm_read, (const void *)mach_vm_read},
    {(const void *)hao_vm_allocate, (const void *)vm_allocate},
    {(const void *)hao_vm_map, (const void *)vm_map},
    {(const void *)hao_vm_remap, (const void *)vm_remap},
    {(const void *)hao_vm_remap_new, (const void *)vm_remap_new},
    {(const void *)hao_vm_protect, (const void *)vm_protect},
    {(const void *)hao_vm_read, (const void *)vm_read},
    {(const void *)hao_NSGetExecutablePath, (const void *)_NSGetExecutablePath},
    {(const void *)hao_getsectbyname, (const void *)getsectbyname},
    {(const void *)hao_getsegbyname, (const void *)getsegbyname},
    {(const void *)hao_getsectdata, (const void *)getsectdata},
    {(const void *)hao_getsectbynamefromheader_64, (const void *)getsectbynamefromheader_64},
    {(const void *)hao_getsectdatafromheader_64, (const void *)getsectdatafromheader_64},
    {(const void *)hao_getsectiondata, (const void *)getsectiondata},
    {(const void *)hao_getsegmentdata, (const void *)getsegmentdata},
};
#pragma clang diagnostic pop

/* The native function an interposer above replaces (FN itself otherwise):
 * guest imports bind to the replacement (dlsym returns it), and libhao types
 * them as the function they stand for (src/bridge.c). */
const void *hao_oslog_original(const void *fn);   /* src/oslog.c */
const void *hao_bundle_original(const void *fn);
const void *hao_identity_original(const void *fn)
{
    const void *o = hao_oslog_original(fn);
    if (o) return o;
    o = hao_bundle_original(fn);
    if (o) return o;
    for (size_t i = 0; i < sizeof hao_interposers / sizeof *hao_interposers; i++)
        if (hao_interposers[i].replacement == fn) return hao_interposers[i].original;
    return fn;
}
