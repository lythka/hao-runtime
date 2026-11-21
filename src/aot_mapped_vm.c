/* Install only a checked v9 descriptor. The caller owns the storage and
 * provenance policy. No region is published until all protections succeed. */
#include "hao/aot_mapped_vm.h"
#include "hao/macho.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

bool hao_aot_mapped_open(int fd, size_t size, const uint8_t digest[32], const hao_aot_expect *expect,
                         uint64_t base, hao_aot_mapping *m, char *why, size_t n)
{
    if (size < 4096 || size > HAO_AOT_MAX_FILE) { snprintf(why, n, "invalid mapped file size"); return false; }
    uint8_t *file = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) { snprintf(why, n, "metadata mapping: %s", strerror(errno)); return false; }
    uint8_t actual[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(file, (CC_LONG)size, actual);
    hao_aot_mapped_view v;
    uint8_t *reservation = MAP_FAILED;
    size_t reserved = 0;
    if (memcmp(actual, digest, sizeof actual)) { snprintf(why, n, "unapproved mapped file digest"); goto fail; }
    if (hao_aot_mapped_check(file, size, expect, &v, why, n)) goto fail;
    const struct mach_header_64 *mh = (const void *)file;
    hao_slice sl = {.fd = fd, .bytes = file, .size = size, .cmds = (const void *)(mh + 1),
                    .ncmds = mh->ncmds, .sizeofcmds = mh->sizeofcmds, .sig_off = v.signature_offset,
                    .sig_size = v.signature_size};
    if (hao_slice_verify(&sl, NULL, why, n)) goto fail;
    if (sl.sig.code_limit != sl.sig_off || sl.sig.page_shift != 12) {
        snprintf(why, n, "unsupported mapped CodeDirectory extent or page size"); goto fail;
    }
    fsignatures_t fs = {.fs_blob_start = (void *)(uintptr_t)sl.sig_off, .fs_blob_size = sl.sig_size,
                        .fs_fsignatures_size = sizeof fs};
    if (fcntl(fd, F_ADDFILESIGS_INFO, &fs)) { snprintf(why, n, "signature registration: %s", strerror(errno)); goto fail; }
    if (memcmp(fs.fs_cdhash, sl.sig.cdhash, sizeof fs.fs_cdhash) || fs.fs_hash_type != sl.sig.hash_type) {
        snprintf(why, n, "different kernel CodeDirectory"); goto fail;
    }
    char message[256] = "";
    fchecklv_t lv = {.lv_error_message = message, .lv_error_message_size = sizeof message};
    if (fcntl(fd, F_CHECK_LV, &lv)) { snprintf(why, n, "host library validation: %s", message[0] ? message : strerror(errno)); goto fail; }
    const size_t page = 4096;
    size_t code_pages = v.signature_offset - v.header->text_offset;
    reserved = page + code_pages;
    reservation = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reservation == MAP_FAILED || mprotect(reservation, page, PROT_READ | PROT_WRITE)) goto vm_fail;
    uint8_t *slot = reservation + page - 16;
    memcpy(slot, &base, sizeof base);
    if (mmap(reservation + page, code_pages, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd,
             (off_t)v.header->text_offset) == MAP_FAILED) goto vm_fail;
    if (mach_vm_protect(mach_task_self(), (uintptr_t)reservation, page, TRUE, VM_PROT_READ) ||
        mach_vm_protect(mach_task_self(), (uintptr_t)(reservation + page), code_pages, TRUE, VM_PROT_READ | VM_PROT_EXECUTE) ||
        mach_vm_protect(mach_task_self(), (uintptr_t)file, size, TRUE, VM_PROT_READ)) {
        snprintf(why, n, "could not seal mapped protections"); goto fail;
    }
    *m = (hao_aot_mapping){file, size, reservation, reserved, slot, v};
    return true;
vm_fail:
    snprintf(why, n, "mapped VM layout: %s", strerror(errno));
fail:
    if (reservation != MAP_FAILED) munmap(reservation, reserved);
    munmap(file, size);
    return false;
}

void hao_aot_mapping_discard(hao_aot_mapping *m)
{
    munmap(m->reservation, m->reserved);
    munmap(m->file, m->size);
}
