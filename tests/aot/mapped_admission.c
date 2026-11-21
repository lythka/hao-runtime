/* Measure the kernel's admission of an inert, signed x86_64 fixture. Never
 * execute its contents. All operations refer to the same open descriptor.
 * The harness supplies the digest of the known test fixture independently. */
#include "hao/macho.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int probe(int fd, const char *expected)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 4096 || st.st_size > (8 << 20))
        return 1;
    size_t size = (size_t)st.st_size;
    uint8_t *bytes = malloc(size);
    if (!bytes || pread(fd, bytes, size, 0) != (ssize_t)size) return 1;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, (CC_LONG)size, digest);
    char hex[2 * sizeof digest + 1];
    for (size_t i = 0; i < sizeof digest; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hex, expected)) { fprintf(stderr, "fixture digest differs\n"); return 1; }
    struct mach_header_64 h;
    memcpy(&h, bytes, sizeof h);
    if (h.magic != MH_MAGIC_64 || h.cputype != CPU_TYPE_X86_64 || h.filetype != MH_BUNDLE ||
        !h.ncmds || h.sizeofcmds > size - sizeof h) return 1;
    hao_slice sl = {.fd = fd, .bytes = bytes, .size = size, .cmds = bytes + sizeof h,
                    .ncmds = h.ncmds, .sizeofcmds = h.sizeofcmds};
    size_t at = sizeof h, end = at + h.sizeofcmds;
    off_t text = -1;
    for (uint32_t i = 0; i < h.ncmds; i++) {
        struct load_command lc;
        if (end - at < sizeof lc) return 1;
        memcpy(&lc, bytes + at, sizeof lc);
        if (lc.cmdsize < sizeof lc || (lc.cmdsize & 7) || lc.cmdsize > end - at) return 1;
        if (lc.cmd == LC_SEGMENT_64) {
            struct segment_command_64 sg;
            if (lc.cmdsize < sizeof sg) return 1;
            memcpy(&sg, bytes + at, sizeof sg);
            if ((uint64_t)sg.nsects * sizeof(struct section_64) != lc.cmdsize - sizeof sg ||
                sg.fileoff > size || sg.filesize > size - sg.fileoff) return 1;
            if (sg.initprot == (VM_PROT_READ | VM_PROT_EXECUTE) && sg.filesize >= 4096 &&
                !(sg.fileoff & 4095)) text = (off_t)sg.fileoff;
        } else if (lc.cmd == LC_CODE_SIGNATURE) {
            struct linkedit_data_command sig;
            if (lc.cmdsize != sizeof sig || sl.sig_size) return 1;
            memcpy(&sig, bytes + at, sizeof sig);
            sl.sig_off = sig.dataoff;
            sl.sig_size = sig.datasize;
        }
        at += lc.cmdsize;
    }
    char why[256];
    if (at != end || text < 0 || !sl.sig_size) return 1;
    if (hao_slice_verify(&sl, NULL, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 1; }
    fsignatures_t fs = {.fs_blob_start = (void *)(uintptr_t)sl.sig_off, .fs_blob_size = sl.sig_size,
                        .fs_fsignatures_size = sizeof fs};
    if (fcntl(fd, F_ADDFILESIGS_INFO, &fs)) { perror("F_ADDFILESIGS_INFO"); return 2; }
    if (fs.fs_hash_type != sl.sig.hash_type || memcmp(fs.fs_cdhash, sl.sig.cdhash, sizeof fs.fs_cdhash))
        return 1;
    char message[256] = "";
    fchecklv_t lv = {.lv_error_message = message, .lv_error_message_size = sizeof message};
    if (fcntl(fd, F_CHECK_LV, &lv)) { perror("F_CHECK_LV"); fprintf(stderr, "%s\n", message); return 2; }
    void *rx = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, text);
    if (rx == MAP_FAILED) { perror("mmap RX"); return 2; }
    int failed = memcmp(rx, bytes + text, 4096) != 0;
    munmap(rx, 4096);
    free(bytes);
    if (!failed) puts("registered matching cdhash, passed host LV, mapped and read RX file page");
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    int fd = open(argv[1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 1;
    int result = probe(fd, argv[2]);
    close(fd);
    return result;
}
