/* Narrow, inert Mach-O envelope. The first page contains only the Mach
 * header, its commands and the v9 header. codesign appends the native
 * signature and adjusts __LINKEDIT; there is no dyld entry or initializer. */
#include "hao/aot_mapped.h"
#include <errno.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_BYTES 4096u
#define ALIGN_PAGE(n) (((n) + PAGE_BYTES - 1) & ~(uint64_t)(PAGE_BYTES - 1))
static struct segment_command_64 segment(const char *name, uint64_t off, uint64_t size, int prot)
{
    struct segment_command_64 s = {.cmd = LC_SEGMENT_64, .cmdsize = sizeof s, .fileoff = off, .vmaddr = off,
                                   .filesize = size, .vmsize = ALIGN_PAGE(size), .initprot = prot, .maxprot = prot};
    snprintf(s.segname, sizeof s.segname, "%s", name);
    return s;
}

static bool write_at(int fd, const void *data, size_t count, off_t off)
{
    const uint8_t *p = data;
    while (count) {
        ssize_t n = pwrite(fd, p, count, off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        off += n;
        count -= (size_t)n;
    }
    return true;
}

bool hao_aot_mapped_write(int fd, const uint8_t *legacy, size_t legacy_size, const uint64_t *by_guest,
                          const hao_aot_pc_range *ranges, uint32_t range_count)
{
    const hao_aot_hdr *h = (const void *)legacy;
    uint64_t forward_size = (uint64_t)h->nblocks * sizeof(uint32_t);
    uint64_t range_size = (uint64_t)range_count * sizeof *ranges;
    uint64_t prefix = legacy_size - h->code_size + 16;
    uint64_t range_off = PAGE_BYTES + 2 * forward_size;
    uint64_t text = ALIGN_PAGE(range_off + range_size + prefix);
    uint64_t text_size = h->code_size - 16;
    uint64_t link = ALIGN_PAGE(text + text_size);
    if (!text_size || link >= HAO_AOT_MAX_FILE || text % 8 || prefix % 8) return false;
    hao_aot_mapped_header info = {"HAOMAP9", 9, sizeof info, h->nblocks, range_count,
                                  PAGE_BYTES, range_off, text - prefix, legacy_size, text, text_size};
    struct mach_header_64 mh = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_X86_64, .cpusubtype = CPU_SUBTYPE_X86_64_ALL,
                                .filetype = MH_BUNDLE, .ncmds = 3, .sizeofcmds = 3 * sizeof(struct segment_command_64)};
    /* Header is __TEXT as native signing tools expect. Only __HAOCODE
     * pages are mapped with EXEC in the runtime. */
    struct segment_command_64 segments[] = {segment("__TEXT", 0, text, VM_PROT_READ),
            segment("__HAOCODE", text, link - text, VM_PROT_READ | VM_PROT_EXECUTE),
            segment("__LINKEDIT", link, 0, VM_PROT_READ)};
    if (ftruncate(fd, (off_t)link) || !write_at(fd, &mh, sizeof mh, 0) ||
        !write_at(fd, segments, sizeof segments, sizeof mh) || !write_at(fd, &info, sizeof info, HAO_AOT_MAPPED_INFO_OFFSET) ||
        !write_at(fd, ranges, range_size, range_off) || !write_at(fd, legacy, legacy_size, info.legacy_offset)) return false;
    const hao_aot_ent *ents = (const void *)(legacy + sizeof *h);
    uint32_t buf[1024], ends[1024], row = 0;
    for (uint32_t i = 0; i < h->nblocks;) {
        size_t n = h->nblocks - i < 1024 ? h->nblocks - i : 1024;
        for (size_t k = 0; k < n; k++) {
            uint32_t b = i + (uint32_t)k;
            uint32_t limit = b + 1 < h->nblocks ? ents[b + 1].code_off : (uint32_t)h->code_size;
            if (row >= range_count || ranges[row].start != ents[b].code_off) return false;
            while (row + 1 < range_count && ranges[row + 1].start < limit) row++;
            ends[k] = ranges[row++].end;
            buf[k] = (uint32_t)by_guest[b];
        }
        if (!write_at(fd, buf, n * sizeof *buf, PAGE_BYTES + (uint64_t)i * sizeof *buf)) return false;
        if (!write_at(fd, ends, n * sizeof *ends, PAGE_BYTES + forward_size + (uint64_t)i * sizeof *ends)) return false;
        i += (uint32_t)n;
    }
    return true;
}

#define REJECT(...) do { snprintf(why, why_size, __VA_ARGS__); return -1; } while (0)

int hao_aot_mapped_check(const void *file, size_t size, const hao_aot_expect *expect, hao_aot_mapped_view *out,
                         char *why, size_t why_size)
{
    const uint8_t *p = file;
    if ((uintptr_t)p % 8 || size < PAGE_BYTES || size > HAO_AOT_MAX_FILE) REJECT("invalid mapped file size or alignment");
    const struct mach_header_64 *mh = file;
    const size_t cmds = 3 * sizeof(struct segment_command_64) + sizeof(struct linkedit_data_command);
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_X86_64 || mh->cpusubtype != CPU_SUBTYPE_X86_64_ALL ||
        mh->filetype != MH_BUNDLE || mh->flags || mh->reserved || mh->ncmds != 4 || mh->sizeofcmds != cmds)
        REJECT("unexpected mapped Mach-O commands");
    const hao_aot_mapped_header *h = (const void *)(p + HAO_AOT_MAPPED_INFO_OFFSET);
    if (memcmp(h->magic, "HAOMAP9", 8) || h->version != 9 || h->header_size != sizeof *h ||
        !h->block_count || !h->range_count) REJECT("bad mapped format");
    uint64_t fs = (uint64_t)h->block_count * 4, rs = (uint64_t)h->range_count * sizeof(hao_aot_pc_range);
    if (h->forward_offset != PAGE_BYTES || h->ranges_offset != PAGE_BYTES + 2 * fs || h->ranges_offset + rs > size ||
        h->legacy_offset < h->ranges_offset + rs || h->legacy_offset % 8 || h->legacy_offset > size ||
        h->legacy_size > size - h->legacy_offset || h->legacy_size < sizeof(hao_aot_hdr) ||
        h->text_offset % PAGE_BYTES || !h->text_size || h->text_offset > size || h->text_size > size - h->text_offset ||
        h->legacy_offset + h->legacy_size != h->text_offset + h->text_size) REJECT("invalid mapped section extents");
    uint64_t link = ALIGN_PAGE(h->text_offset + h->text_size);
    if (link >= size) REJECT("missing mapped code signature");
    const struct segment_command_64 *segs = (const void *)(mh + 1);
    struct segment_command_64 segments[] = {segment("__TEXT", 0, h->text_offset, VM_PROT_READ),
            segment("__HAOCODE", h->text_offset, link - h->text_offset, VM_PROT_READ | VM_PROT_EXECUTE),
            segment("__LINKEDIT", link, size - link, VM_PROT_READ)};
    /* codesign on an arm64 build machine rounds __LINKEDIT VM size to 16 KiB. */
    uint64_t wide = (size - link + 16383u) & ~(uint64_t)16383;
    if (segs[2].vmsize == wide) segments[2].vmsize = wide;
    if (memcmp(segments, segs, sizeof segments)) REJECT("unexpected mapped segments");
    const struct linkedit_data_command *sig = (const void *)(segs + 3);
    if (sig->cmd != LC_CODE_SIGNATURE || sig->cmdsize != sizeof *sig || sig->dataoff != link ||
        sig->datasize != size - link) REJECT("invalid mapped signature extent");
    hao_aot_view legacy;
    int rc = hao_aot_check(p + h->legacy_offset, h->legacy_size, expect, &legacy, why, why_size);
    if (rc) return rc;
    if (legacy.hdr->nblocks != h->block_count || legacy.code + 16 != p + h->text_offset ||
        legacy.hdr->code_size != h->text_size + 16 || legacy.ents[0].code_off != 16)
        REJECT("mapped text disagrees with translation metadata");
    const uint32_t *forward = (const void *)(p + h->forward_offset);
    for (uint32_t i = 0; i < h->block_count; i++) {
        if (forward[i] >= h->block_count || (i && legacy.ents[forward[i - 1]].guest_off >= legacy.ents[forward[i]].guest_off))
            REJECT("invalid forward index");
    }
    const hao_aot_pc_range *ranges = (const void *)(p + h->ranges_offset);
    const uint32_t *ends = forward + h->block_count;
    uint32_t b = 0;
    for (uint32_t i = 0; i < h->range_count; i++) {
        const hao_aot_pc_range *r = &ranges[i];
        if (r->start >= r->end || r->end > legacy.hdr->code_size || r->flags_in_host > 1 ||
            (i && ranges[i - 1].end > r->start)) REJECT("invalid reverse interval");
        if (i && b + 1 < h->block_count && r->start >= legacy.ents[b + 1].code_off) {
            if (ranges[i - 1].end != ends[b]) REJECT("incomplete reverse block");
            b++;
        }
        uint32_t start = legacy.ents[b].code_off, guest = legacy.ents[b].guest_off;
        if (r->start < start || (b + 1 < h->block_count && r->end > legacy.ents[b + 1].code_off) ||
            r->end > ends[b] ||
            (r->start != start && (!i || r->start != ranges[i - 1].end)) ||
            r->guest_off < guest || (r->guest_off - guest) % 4 || r->guest_off - guest > 96 * 4 ||
            r->guest_off > expect->vmsize) REJECT("reverse interval outside block");
        if ((!i || (b && ranges[i - 1].end <= start)) && r->start != start) REJECT("missing reverse block entry");
    }
    if (b + 1 != h->block_count || ends[b] != ranges[h->range_count - 1].end || ends[b] != legacy.hdr->code_size)
        REJECT("incomplete reverse map");
    for (uint32_t k = 0; k + 1 < h->block_count; k++) {
        uint32_t next = legacy.ents[k + 1].code_off;
        if (ends[k] > next || next - ends[k] >= 16) REJECT("invalid block padding");
        for (uint32_t j = ends[k]; j < next; j++) if (legacy.code[j] != 0xcc) REJECT("unexpected block padding");
    }
    for (uint32_t i = 0, j = 0; i < legacy.hdr->nlinks; i++) {
        uint32_t s = legacy.links[i].site;
        while (j < h->range_count && ranges[j].end <= s) j++;
        if ((i && s <= legacy.links[i - 1].site) || (s & 3) || j == h->range_count ||
            s <= ranges[j].start || (uint64_t)s + 4 > ranges[j].end || legacy.code[s - 1] != 0xe9)
            REJECT("invalid immutable direct branch site");
    }
    if (out) *out = (hao_aot_mapped_view){h, forward, ranges, legacy, sig->dataoff, sig->datasize};
    return 0;
}
