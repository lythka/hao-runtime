/* Recognize only the complete opaque spawn attribute shape for which this
 * runtime has a native ABI witness. Unknown attributes follow libSystem. */
#include <dlfcn.h>
#include <mach/machine.h>
#include <mach-o/loader.h>
#include <malloc/malloc.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool hao_spawn_attr_abi(const void *original)
{
    static const unsigned char expected[] = {
        0x7c,0x05,0xb2,0x44,0xcd,0xe5,0x3c,0x39,0x83,0xab,0x59,0x55,0x9f,0x79,0x40,0xae
    };
    Dl_info info;
    if (!dladdr(original, &info) || !info.dli_fname ||
        strcmp(info.dli_fname, "/usr/lib/system/libsystem_kernel.dylib")) return false;
    uintptr_t base = (uintptr_t)info.dli_fbase;
    if ((uintptr_t)original != base + 0x6355 ||
        (uintptr_t)posix_spawnattr_init != base + 0x5f7d ||
        (uintptr_t)posix_spawnattr_destroy != base + 0x6665 ||
        (uintptr_t)posix_spawnattr_getflags != base + 0x10a22 ||
        (uintptr_t)posix_spawnattr_getarchpref_np != base + 0x10ae7 ||
        (uintptr_t)posix_spawnattr_setflags != base + 0x6092 ||
        (uintptr_t)posix_spawnattr_setbinpref_np != base + 0xa4fc) return false;
    const struct mach_header_64 *header = info.dli_fbase;
    if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_X86_64) return false;
    const unsigned char *p = (const unsigned char *)(header + 1);
    size_t remaining = header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (remaining < sizeof(struct load_command)) return false;
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > remaining) return false;
        if (lc->cmd == LC_UUID) {
            const struct uuid_command *uuid = (const void *)p;
            return lc->cmdsize == sizeof(*uuid) && !memcmp(uuid->uuid, expected, sizeof(expected));
        }
        remaining -= lc->cmdsize;
        p += lc->cmdsize;
    }
    return false;
}

/* On success the caller owns OUT and destroys it on a returning syscall.
 * No pointer field, unknown flag, or non-default state is copied. */
bool hao_spawn_host_attr(const void *original, const posix_spawnattr_t *guest, posix_spawnattr_t *out)
{
    if (!guest || !*guest || !hao_spawn_attr_abi(original) || malloc_size(*guest) != 256) return false;
    short flags;
    if (posix_spawnattr_getflags(guest, &flags) || (flags != 0 && flags != POSIX_SPAWN_SETEXEC)) return false;
    cpu_type_t arm = CPU_TYPE_ARM64, host = CPU_TYPE_X86_64;
    size_t actual = 0;
    if (posix_spawnattr_init(out)) return false;
    bool ok = malloc_size(*out) == 256 && !posix_spawnattr_setflags(out, flags) &&
              !posix_spawnattr_setbinpref_np(out, 1, &arm, &actual) && actual == 1 &&
              !memcmp(*guest, *out, 256) &&
              !posix_spawnattr_setbinpref_np(out, 1, &host, &actual) && actual == 1;
    if (!ok) posix_spawnattr_destroy(out);
    return ok;
}

#include "hao/macho.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <stdio.h>
#include <sys/acl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GENERATIONS "/private/var/db/hao-spawn-generations/"
#define SAFE_OPEN (O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_CLOFORK | O_NONBLOCK)

static bool protected_node(int fd, bool directory, dev_t device)
{
    struct stat st;
    if (fstat(fd, &st) || st.st_uid || (st.st_mode & (022 | S_ISUID | S_ISGID)) ||
        st.st_dev != device || (directory ? !S_ISDIR(st.st_mode) :
        (!S_ISREG(st.st_mode) || st.st_nlink != 1))) return false;
    acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!acl) return errno == ENOENT;
    acl_entry_t entry;
    errno = 0;
    int status = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    bool empty = status < 0 && errno == EINVAL;
    acl_free(acl);
    return empty;
}

/* All namespace objects are permanently published in a root-managed
 * generation. Follow no pathname alias, including at the root of the store. */
static int open_member(const char *path)
{
    if (strncmp(path, GENERATIONS, sizeof(GENERATIONS)-1)) return -1;
    int fd = open("/", SAFE_OPEN | O_DIRECTORY);
    struct stat root;
    if (fd < 0) return -1;
    if (fstat(fd, &root) || !protected_node(fd, true, root.st_dev)) goto fail;
    for (const char *p = path + 1; *p;) {
        const char *end = strchr(p, '/');
        size_t size = end ? (size_t)(end-p) : strlen(p);
        char component[NAME_MAX + 1];
        if (!size || size > NAME_MAX || (size == 1 && *p == '.') ||
            (size == 2 && !memcmp(p, "..", 2)) || (end && !end[1])) goto fail;
        memcpy(component, p, size);
        component[size] = 0;
        int next = openat(fd, component, SAFE_OPEN | (end ? O_DIRECTORY : 0));
        if (next < 0) goto fail;
        close(fd); fd = next;
        if (!protected_node(fd, end != NULL, root.st_dev)) goto fail;
        if (!end) return fd;
        p = end + 1;
    }
fail:
    close(fd);
    return -1;
}

struct target_receipt {
    char magic[8];
    uint8_t sha256[32], host[21], guest[21];
};

static bool load_receipt(const char *path, struct target_receipt *receipt)
{
    if (strncmp(path, GENERATIONS, sizeof(GENERATIONS)-1)) return false;
    const char *generation = path + sizeof(GENERATIONS)-1;
    const char *slash = strchr(generation, '/');
    if (!slash || slash == generation || !slash[1] || !strncmp(slash+1, ".hao-receipts/", 14)) return false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(path, (CC_LONG)strlen(path), digest);
    const char alphabet[] = "0123456789abcdef";
    char hex[65], filename[PATH_MAX];
    for (size_t i = 0; i < sizeof digest; i++) {
        hex[2*i] = alphabet[digest[i] >> 4];
        hex[2*i+1] = alphabet[digest[i] & 15];
    }
    hex[64] = 0;
    if (snprintf(filename, sizeof filename, "%.*s/.hao-receipts/%s",
                 (int)(slash-path), path, hex) >= (int)sizeof filename) return false;
    int fd = open_member(filename);
    if (fd < 0) return false;
    struct stat st;
    bool ok = !fstat(fd, &st) && st.st_size == sizeof *receipt &&
              pread(fd, receipt, sizeof *receipt, 0) == sizeof *receipt &&
              !memcmp(receipt->magic, "haospwn1", 8);
    close(fd);
    return ok;
}

static bool host_binding(const uint8_t *bytes, size_t length, const struct target_receipt *receipt, hao_slice *arm)
{
    if (length < sizeof(struct mach_header_64)) return false;
    const struct mach_header_64 *h = (const void *)bytes;
    if (h->magic != MH_MAGIC_64 || h->cputype != CPU_TYPE_X86_64 ||
        h->cpusubtype != CPU_SUBTYPE_X86_64_ALL || h->filetype != MH_EXECUTE ||
        h->sizeofcmds > length - sizeof *h) return false;
    const uint8_t *cmds = (const void *)(h+1);
    char why[256]; int segments;
    if (hao_macho_check_cmds(cmds,h->sizeofcmds,h->ncmds,&segments,why,sizeof why)) return false;
    hao_slice native = {.bytes=bytes,.size=length,.cmds=cmds,.ncmds=h->ncmds};
    const hao_slice_record *marker = NULL;
    for (const uint8_t *p = cmds; p < cmds+h->sizeofcmds;) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd == LC_CODE_SIGNATURE) {
            const struct linkedit_data_command *sig = (const void *)p;
            if (native.sig_size) return false;
            native.sig_off = sig->dataoff; native.sig_size = sig->datasize;
        }
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const void *)p;
            const struct section_64 *section = (const void *)(seg+1);
            for (uint32_t i = 0; i < seg->nsects; i++) {
                const struct section_64 *s = section+i;
                if (strncmp(seg->segname,"__TEXT",16) || strncmp(s->segname,"__TEXT",16) ||
                    strncmp(s->sectname,HAO_SLICE_SECTION,16)) continue;
                if (marker || (s->flags & SECTION_TYPE) != S_REGULAR || s->size != sizeof *marker ||
                    seg->fileoff > length || seg->filesize > length-seg->fileoff ||
                    s->offset < seg->fileoff || s->addr < seg->vmaddr) return false;
                uint64_t delta = s->offset - seg->fileoff;
                if (delta != s->addr-seg->vmaddr ||
                    delta > seg->filesize || s->size > seg->filesize-delta ||
                    delta > seg->vmsize || s->size > seg->vmsize-delta) return false;
                marker = (const void *)(bytes+s->offset);
            }
        }
        p += lc->cmdsize;
    }
    return marker && !hao_slice_verify(&native,NULL,why,sizeof why) &&
           (size_t)((const uint8_t *)marker-bytes) + sizeof *marker <= native.sig.code_limit &&
           receipt->host[0] == native.sig.hash_type && !memcmp(receipt->host+1,native.sig.cdhash,20) &&
           !hao_libval_bound(marker,sizeof *marker,arm,why,sizeof why) &&
           receipt->guest[0] == arm->sig.hash_type && !memcmp(receipt->guest+1,arm->sig.cdhash,20);
}

/* Caller holds runtime loader lock for the existing slice and signer helpers. */
bool hao_spawn_generation(const char *path)
{
    struct target_receipt receipt;
    if (!load_receipt(path, &receipt)) return false;
    int fd = open_member(path);
    if (fd < 0) return false;
    struct stat st;
    bool ok = false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH], *bytes = MAP_FAILED;
    hao_slice *arm = NULL;
    if (fstat(fd,&st) || !(st.st_mode & 0111) || st.st_size < 48 || st.st_size > UINT32_MAX) goto out;
    bytes = mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    if (bytes == MAP_FAILED) goto out;
    CC_SHA256(bytes,(CC_LONG)st.st_size,digest);
    if (memcmp(digest,receipt.sha256,sizeof digest)) goto out;
    const struct fat_header *fat = (const void *)bytes;
    if (OSSwapBigToHostInt32(fat->magic) != FAT_MAGIC || OSSwapBigToHostInt32(fat->nfat_arch) != 2) goto out;
    const struct fat_arch *arch = (const void *)(fat+1);
    uint32_t hostoff = 0, hostlen = 0, armoff = 0, armlen = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t off = OSSwapBigToHostInt32(arch[i].offset), len = OSSwapBigToHostInt32(arch[i].size);
        if (off < 48 || (off & 7) || !len || (uint64_t)off+len > (uint64_t)st.st_size) goto out;
        switch (OSSwapBigToHostInt32(arch[i].cputype)) {
        case CPU_TYPE_X86_64: hostoff = off; hostlen = len; break;
        case CPU_TYPE_ARM64: armoff = off; armlen = len; break;
        default: goto out;
        }
    }
    if (!hostlen || !armlen || ((uint64_t)hostoff+hostlen > armoff && (uint64_t)armoff+armlen > hostoff)) goto out;
    char why[256];
    arm = hao_slice_open(fd,why,sizeof why);
    ok = arm && arm->filetype == MH_EXECUTE && arm->off == armoff && arm->size == armlen &&
         host_binding(bytes+hostoff,hostlen,&receipt,arm) &&
         hao_spawn_signed_slice(path,CPU_TYPE_X86_64,CPU_SUBTYPE_X86_64_ALL,receipt.host+1) &&
         hao_spawn_signed_slice(path,CPU_TYPE_ARM64,arm->cpusubtype,receipt.guest+1);
out:
    if (arm) hao_slice_close(arm);
    if (bytes != MAP_FAILED) munmap(bytes,st.st_size);
    close(fd);
    return ok;
}
