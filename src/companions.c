/* Host launchers can directly link a separately signed framework companion.
 * Dyld owns that native load. Hao verifies its association with the original
 * ARM library before it publishes the guest image. */
#include "hao/runtime.h"
#include <CommonCrypto/CommonDigest.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

extern int csops(pid_t, unsigned, void *, size_t);
enum { CS_OPS_STATUS = 0, CS_VALID = 1, CS_SIGNED = 0x20000000 };

typedef struct companion {
    const uint8_t *guest, *host, *sha256;
    const char *path, *install, *adapter;
    struct companion *next;
} companion;
static companion *companions;

bool hao_companions_active(void)
{
    return __atomic_load_n(&companions, __ATOMIC_ACQUIRE) != NULL;
}

static const companion *lookup(const char *name, bool install)
{
    for (const companion *c = __atomic_load_n(&companions, __ATOMIC_ACQUIRE); c; c = c->next)
        if (!strcmp(install ? c->install : c->path, name)) return c;
    return NULL;
}

const char *hao_companion_original(const char *name)
{
    const companion *c = lookup(name, false);
    if (!c) c = lookup(name, true);
    return c ? c->path : NULL;
}

/* Use the canonical original chosen by the signed record. The loader consumes
 * this descriptor and checks the snapshot before publishing it. No second
 * realpath lookup can turn the selected request into an ordinary load. */
int hao_companion_open(const char *original, char *why, size_t n)
{
    int fd = open(original, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW_ANY);
    char actual[MAXPATHLEN];
    if (fd >= 0 && fcntl(fd, F_GETPATH, actual) >= 0 && !strcmp(actual, original)) return fd;
    if (fd >= 0) close(fd);
    snprintf(why, n, "cannot identify declared original framework at %s", original);
    return -1;
}

static const char *next_string(const uint8_t **p, const uint8_t *end)
{
    const char *s = (const char *)*p;
    const uint8_t *nul = memchr(*p, 0, (size_t)(end - *p));
    if (!nul || nul == *p || (size_t)(nul - *p) >= MAXPATHLEN) return NULL;
    *p = nul + 1;
    return s;
}

static unsigned dylib_commands(const uint8_t *p, unsigned count, uint32_t kind, const char *name)
{
    unsigned matches = 0;
    for (unsigned i = 0; i < count; i++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd == kind) {
            const struct dylib_command *d = (const void *)lc;
            if (!strcmp((const char *)p + d->dylib.name.offset, name)) matches++;
        }
        p += lc->cmdsize;
    }
    return matches;
}

static bool verify_original(const companion *c, hao_slice *sl, char *why, size_t n)
{
    if (sl->filetype != MH_DYLIB || dylib_commands(sl->cmds, sl->ncmds, LC_ID_DYLIB, c->install) != 1 ||
        hao_slice_verify(sl, NULL, why, n) != 0 || sl->sig.hash_type != c->guest[0] ||
        memcmp(sl->sig.cdhash, c->guest + 1, 20)) {
        snprintf(why, n, "original framework signature or install name differs from its launcher association");
        return false;
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    for (size_t pos = 0; pos < sl->size;) {
        size_t size = sl->size - pos;
        if (size > UINT32_MAX) size = UINT32_MAX;
        CC_SHA256_Update(&hash, sl->bytes + pos, (CC_LONG)size);
        pos += size;
    }
    CC_SHA256_Final(digest, &hash);
    if (memcmp(digest, c->sha256, sizeof digest)) {
        snprintf(why, n, "original framework SHA-256 differs from its launcher association");
        return false;
    }
    HAO_LOG(1, "verified original ARM framework %s", c->path);
    return true;
}

static bool verify_guest(hao_image *im, hao_slice *sl, char *why, size_t n)
{
    const companion *c = lookup(im->path, false);
    if (!c) return true;
    im->companion_verified = verify_original(c, sl, why, n);
    return im->companion_verified;
}

bool hao_companion_resident(const char *path, const hao_image *image)
{
    const companion *c = lookup(path, false);
    return !c || (image && image->companion_verified && !strcmp(image->path, path) && image->cdhash_type == c->guest[0] &&
                  !memcmp(image->cdhash, c->guest + 1, 20));
}

/* A prepared adapter is inert, with exactly two consecutive file-backed
 * segments and no fixups to its signed bytes. Read its actual mapped image,
 * including the CodeDirectory. Reopening its pathname could select a new file. */
static bool verify_adapter(const struct mach_header_64 *h, intptr_t slide, const companion *c, char *why, size_t n)
{
    if (h->magic != MH_MAGIC_64 || h->cputype != CPU_TYPE_X86_64 || h->filetype != MH_DYLIB) return false;
    const uint8_t *cmds = (const void *)(h + 1);
    int nsegs;
    if (hao_macho_check_cmds(cmds, h->sizeofcmds, h->ncmds, &nsegs, why, n) || nsegs != 2) return false;
    const struct segment_command_64 *text = NULL, *link = NULL;
    const struct linkedit_data_command *cs = NULL, *exports = NULL;
    unsigned deps = 0;
    const struct section_64 *marker = NULL;
    for (const uint8_t *p = cmds; p < cmds + h->sizeofcmds;) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)p;
            if (!strncmp(s->segname, "__TEXT", 16)) {
                text = s;
                const struct section_64 *sections = (const void *)(s + 1);
                for (unsigned j = 0; j < s->nsects; j++) {
                    const struct section_64 *sec = &sections[j];
                    if (!strncmp(sec->sectname, "__hao_adapter", 16)) {
                        if (marker) return false;
                        marker = sec;
                    } else if (sec->size) return false;
                }
            } else if (!strncmp(s->segname, "__LINKEDIT", 16)) link = s;
        } else if (lc->cmd == LC_CODE_SIGNATURE) cs = (const void *)p;
        else if (lc->cmd == LC_DYLD_EXPORTS_TRIE) exports = (const void *)p;
        else if (lc->cmd == LC_LOAD_DYLIB) {
            const struct dylib_command *d = (const void *)lc;
            if (strcmp((const char *)p + d->dylib.name.offset, "/usr/lib/libSystem.B.dylib")) return false;
            deps++;
        } else if (lc->cmd == LC_REEXPORT_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB ||
                   lc->cmd == LC_LOAD_UPWARD_DYLIB || lc->cmd == LC_LAZY_LOAD_DYLIB ||
                   lc->cmd == LC_ROUTINES_64 || lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) return false;
        p += lc->cmdsize;
    }
    if (deps != 1 || !text || !link || !cs || !marker || text->fileoff || text->filesize != link->fileoff ||
        text->vmaddr + (uintptr_t)slide != (uintptr_t)h || text->vmaddr + link->fileoff != link->vmaddr ||
        (text->initprot & VM_PROT_WRITE) || (link->initprot & VM_PROT_WRITE) ||
        text->filesize < sizeof *h + (uint64_t)h->sizeofcmds || marker->size != sizeof(hao_slice_record) ||
        marker->offset > text->filesize || marker->size > text->filesize - marker->offset ||
        marker->addr != text->vmaddr + marker->offset ||
        cs->dataoff < link->fileoff || cs->dataoff - link->fileoff > link->filesize ||
        cs->datasize != link->filesize - (cs->dataoff - link->fileoff)) return false;
    size_t size = (size_t)cs->dataoff + cs->datasize;
    uint8_t *copy = malloc(size);
    if (!copy) return false;
    mach_vm_size_t got = 0;
    bool ok = mach_vm_read_overwrite(mach_task_self(), (uintptr_t)h, size, (uintptr_t)copy, &got) == KERN_SUCCESS && got == size;
    if (ok && exports && exports->datasize) {
        ok = exports->datasize >= 2 && exports->dataoff <= size && exports->datasize <= size - exports->dataoff &&
             copy[exports->dataoff] == 0 && copy[exports->dataoff + 1] == 0;
    }
    if (ok) {
        hao_slice sl = {.bytes = copy, .size = size, .cmds = copy + sizeof *h, .ncmds = h->ncmds,
                        .sig_off = cs->dataoff, .sig_size = cs->datasize};
        const hao_slice_record *r = (const void *)(copy + marker->offset);
        ok = !hao_slice_verify(&sl, NULL, why, n) && sl.sig.hash_type == c->host[0] &&
             !memcmp(sl.sig.cdhash, c->host + 1, 20) && !memcmp(r->magic, HAO_SLICE_MAGIC, 8) &&
             r->version == 1 && !r->zero[0] && !r->zero[1] && r->hash_type == c->guest[0] &&
             !memcmp(r->cdhash, c->guest + 1, 20);
    }
    free(copy);
    return ok;
}

bool hao_companions_init(char *why, size_t n)
{
    const struct mach_header_64 *main = (const void *)_NSGetMachExecuteHeader();
    unsigned long size = 0;
    const uint8_t *start = getsectiondata(main, "__TEXT", "__hao_companions", &size);
    if (!start || hao_companions_active()) return true;
    companion *pending = NULL;
    unsigned status = 0;
    if (csops(getpid(), CS_OPS_STATUS, &status, sizeof status) ||
        (status & (CS_VALID | CS_SIGNED)) != (CS_VALID | CS_SIGNED) ||
        size <= 8 || memcmp(start, "haocomp1", 8)) {
        snprintf(why, n, "invalid signed framework association header");
        return false;
    }
    const uint8_t *end = start + size;
    for (const uint8_t *p = start + 8; p < end;) {
        if (end - p < 74) goto malformed;
        companion record = {.guest = p, .host = p + 21, .sha256 = p + 42};
        p += 74;
        record.path = next_string(&p, end);
        record.install = next_string(&p, end);
        record.adapter = next_string(&p, end);
        if (!record.path || !record.install || !record.adapter || record.path[0] != '/' || record.adapter[0] != '/' ||
            !strcmp(record.path, record.adapter)) goto malformed;
        for (const companion *c = pending; c; c = c->next)
            if (!strcmp(c->path, record.path) || !strcmp(c->install, record.install) ||
                !strcmp(c->path, record.install) || !strcmp(c->install, record.path) || !strcmp(c->adapter, record.adapter))
                goto malformed;
        if (dylib_commands((const void *)(main + 1), main->ncmds, LC_LOAD_DYLIB, record.adapter) != 1) goto malformed;
        unsigned matches = 0;
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const char *name = _dyld_get_image_name(i);
            if (!name || strcmp(name, record.adapter)) continue;
            if (!verify_adapter((const void *)_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), &record, why, n)) {
                snprintf(why, n, "mapped native framework companion failed verification: %s", name);
                goto failed;
            }
            matches++;
        }
        if (matches != 1) goto malformed;
        HAO_LOG(1, "verified mapped native framework companion %s for %s", record.adapter, record.path);
        companion *saved = malloc(sizeof *saved);
        if (!saved) goto malformed;
        *saved = record;
        saved->next = pending;
        pending = saved;
    }
    /* A stale declaration fails before any guest code. Verify again on the
     * exact slice used for a later mapping, since this file can be replaced. */
    for (const companion *c = pending; c; c = c->next) {
        int fd = hao_companion_open(c->path, why, n);
        if (fd < 0) goto failed;
        hao_slice *slice = hao_slice_open(fd, why, n);
        bool ok = slice && verify_original(c, slice, why, n);
        hao_slice_close(slice);
        close(fd);
        if (!ok) goto failed;
    }
    __atomic_store_n(&hao_companion_rollback_gate, hao_rollback_companion_load, __ATOMIC_RELEASE);
    __atomic_store_n(&hao_companion_dependency_gate, hao_prepare_companion_dependencies, __ATOMIC_RELEASE);
    __atomic_store_n(&hao_companion_slice_gate, verify_guest, __ATOMIC_RELEASE);
    __atomic_store_n(&companions, pending, __ATOMIC_RELEASE);
    return true;
malformed:
    snprintf(why, n, "invalid or ambiguous direct framework companion declaration");
failed:
    while (pending) {
        companion *next = pending->next;
        free(pending);
        pending = next;
    }
    return false;
}
