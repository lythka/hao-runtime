/* Generated per-build tables of native Swift types (Tables.h). Both are
 * generator output for one macOS build (include/hao/gen_stamp.h), sorted by
 * image UUID then descriptor offset, and looked up by the defining image's
 * LC_UUID: a different build of an image has another UUID and no row, so a
 * row never describes a binary it was not derived from. */
#include "Tables.h"
#include "hao/gen_stamp.h"
#include "hao/gmap.h"
#include "hao/macho.h"
#include "hao/runtime.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

static const struct { const char *uuid; uint64_t offset; uint32_t payload, empty, xi, flags; } mpenums[] = {
#include "mpenum_gen.inc"
};
static const struct { const char *uuid; uint64_t offset; char change; } archdiffs[] = {
#include "archdiff_gen.inc"
};

static void hex(const uint8_t uuid[16], char out[33])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned k = 0; k < 16; ++k) { out[2 * k] = digits[uuid[k] >> 4]; out[2 * k + 1] = digits[uuid[k] & 15]; }
    out[32] = 0;
}
static int order(const char *uuid, uint64_t offset, const char *row_uuid, uint64_t row_offset)
{
    int c = strcmp(uuid, row_uuid);
    return c ? c : offset < row_offset ? -1 : offset > row_offset;
}
#define FIND(table, uuid_text, offset) ({                                               \
    long lo_ = 0, hi_ = (long)(sizeof table / sizeof *table) - 1, at_ = -1;             \
    while (lo_ <= hi_) {                                                               \
        long mid_ = (lo_ + hi_) / 2;                                                   \
        int c_ = order(uuid_text, offset, table[mid_].uuid, table[mid_].offset);       \
        if (!c_) { at_ = mid_; break; }                                                \
        if (c_ < 0) hi_ = mid_ - 1; else lo_ = mid_ + 1;                               \
    }                                                                                  \
    at_; })

int mpenum_lookup(const uint8_t uuid[16], uint64_t offset, mpenum_row *out)
{
    char text[33];
    hex(uuid, text);
    long at = FIND(mpenums, text, offset);
    if (at < 0) return 0;
    if (out) *out = (mpenum_row){mpenums[at].payload, mpenums[at].empty, mpenums[at].xi, mpenums[at].flags};
    return 1;
}

int archdiff_lookup(const uint8_t uuid[16], uint64_t offset)
{
    char text[33];
    hex(uuid, text);
    long at = FIND(archdiffs, text, offset);
    return at < 0 ? 0 : archdiffs[at].change;
}

static bool header_uuid(const struct mach_header_64 *h, uint8_t uuid[16])
{
    if (h->magic != MH_MAGIC_64 || h->sizeofcmds > (1u << 24)) return false;
    const uint8_t *p = (const uint8_t *)(h + 1), *end = p + h->sizeofcmds;
    for (uint32_t k = 0; k < h->ncmds && p + sizeof(struct load_command) <= end; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > (uint64_t)(end - p)) return false;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            memcpy(uuid, ((const struct uuid_command *)(const void *)lc)->uuid, 16);
            return true;
        }
        p += lc->cmdsize;
    }
    return false;
}

/* Nominal descriptor -> word: the archdiff change (bits 0-7), the mpenum row
 * index + 1 (bits 8-39). Native descriptors live in images that stay loaded. */
static hao_gmap rows_by_descriptor = HAO_GMAP_INIT(2, 1, 6, 14);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void tables_fork_child(void) { lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

static uint64_t rows_of(const void *type)
{
    if (!type) return 0;
    uintptr_t kind = *(const uintptr_t *)type;
    if (kind != 0x200 && kind != 0x201 && kind != 0x202) return 0;
    uint64_t desc = (uint64_t)(uintptr_t)((const void *const *)type)[1];
    if (!desc) return 0;
    const uint64_t *e = hao_gmap_get1(&rows_by_descriptor, desc);
    if (e) return e[1];
    uint64_t word = 0;
    Dl_info info;
    uint8_t uuid[16];
    /* A guest image's descriptor: its layout is the arm64 one by definition. */
    if (!hao_image_for_addr(desc) && dladdr((const void *)(uintptr_t)desc, &info) && info.dli_fbase &&
        (uint64_t)(uintptr_t)info.dli_fbase < desc &&
        header_uuid((const struct mach_header_64 *)info.dli_fbase, uuid)) {
        char text[33];
        hex(uuid, text);
        uint64_t offset = desc - (uint64_t)(uintptr_t)info.dli_fbase;
        long a = FIND(archdiffs, text, offset), m = FIND(mpenums, text, offset);
        word = (a >= 0 ? (uint64_t)(unsigned char)archdiffs[a].change : 0) | (m >= 0 ? (uint64_t)(m + 1) << 8 : 0);
    }
    hao_crit_enter(); pthread_mutex_lock(&lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&rows_by_descriptor, desc, &fresh);
    if (slot && fresh) { slot[1] = word; hao_gmap_publish1(&rows_by_descriptor, slot, desc); }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return word;
}

int mpenum_type(const void *type, mpenum_row *out)
{
    uint64_t m = rows_of(type) >> 8;
    if (!m) return 0;
    if (out) *out = (mpenum_row){mpenums[m - 1].payload, mpenums[m - 1].empty, mpenums[m - 1].xi, mpenums[m - 1].flags};
    return 1;
}

int archdiff_type(const void *type) { return (int)(rows_of(type) & 0xff); }

/* Metadata -> 1 holds a listed type inline, 2 does not (Recipe.c). */
static hao_gmap holds = HAO_GMAP_INIT(2, 1, 6, 14);
int holds_archdiff_memo(const void *type)
{
    const uint64_t *e = hao_gmap_get1(&holds, (uint64_t)(uintptr_t)type);
    return e ? (int)e[1] - 1 : -1;
}
void holds_archdiff_remember(const void *type, int listed)
{
    if (!type) return;
    hao_crit_enter(); pthread_mutex_lock(&lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&holds, (uint64_t)(uintptr_t)type, &fresh);
    if (slot && fresh) { slot[1] = listed ? 2 : 1; hao_gmap_publish1(&holds, slot, (uint64_t)(uintptr_t)type); }
    pthread_mutex_unlock(&lock); hao_crit_leave();
}
