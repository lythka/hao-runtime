/*
 * hao — native entry points for guest function pointers in guest data.
 *
 * Native code reaches guest functions through pointers the guest image
 * stores statically: rb_tree_ops_t and CF callback structures, Swift value
 * witness tables, heap-metadata destroy functions and closure destructors,
 * C++ vtables, handler tables. hao_thunk_native_args only sees x0-x7 at a
 * call, so a native caller that loads such a pointer jumped to a
 * non-executable guest page: one fault, and one kernel log line, per call.
 *
 * Every such pointer is a chained-fixup rebase. macho.c records each rebase
 * slot whose target is a guest function start (LC_FUNCTION_STARTS); after
 * the image's ObjC classes and global blocks are registered (objc.c and
 * blocks.c give method IMPs and block invokes signature thunks where the
 * type needs marshalling) and before any guest code runs, the slots that
 * still hold a guest function start are rewritten to its x86 thunk
 * (hao_thunk_for). No other static slot carries a type, and a plain thunk
 * is exactly the register bridge the fault path used, so behaviour for
 * native callers is unchanged, minus the fault.
 *
 * Guest code that loads a rewritten slot and branches to it lands on the
 * thunk, which the dispatcher maps back to the guest function
 * (hao_guest_of_entry) and aliases in the translation table (hao_jit_alias),
 * so translated code reaches it through the inline probe.
 *
 * Pointer identity: thunks are unique per function, so two loaded slots
 * still compare equal, and equal to the pointer native code gets in a
 * register. Code that materializes a function's address itself (adrp+add,
 * adr; arm64e then signs it) gets the guest address, and comparing that to
 * a loaded slot would fail: bash's readline compares keymap entries with
 * &rl_insert this way, find(1) compares its option table entries with
 * &f_print. In the 27.0 command-line tools (1,456 arm64e files) 247 such
 * comparisons appear in 26 files. So every function whose address any code
 * section materializes keeps its guest address in every slot (3.8% of the
 * slots, 0.8% of the functions there; none in launchd); its native callers
 * keep the fault path. The scan is conservative: an adrp whose page plus a
 * later add's offset (within 16 instructions, same base register) lands on
 * a candidate function counts, whatever happens in between.
 *
 * Skipped: __mod_init_func/__mod_term_func (only hao reads them), slots
 * outside the __DATA and __AUTH segments. Off with HAO_NO_FIXUP_THUNKS=1
 * (an ordinary process), or in a HAO_TEST_SWITCHES build with boot argument
 * hao_no_fixup_thunks=1.
 */
#include "hao/runtime.h"
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t st_slots, st_thunked, st_addr_taken, st_skipped, st_images;

static bool enabled(void) { return !hao_opt.no_fixup_thunks; }

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static long find_u64(const uint64_t *v, size_t n, uint64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo < n && v[lo] == key ? (long)lo : -1;
}

static inline int64_t sext21(uint32_t v) { return (int64_t)((uint64_t)v << 43) >> 43; }

/* Mark (taken[k] = 1) every target fn[k] whose address the image's code
 * materializes with adr, or adrp followed within 16 instructions by
 * add Xd, Xn, #imm12 on the adrp's register, or a direct Swift computed-keypath
 * identity materializes in read-only data. */
void hao_fnslots_address_taken(hao_image *img, const uint64_t *fn, size_t nfn, uint8_t *taken)
{
    /* A computed Swift key path can name its identity directly by rel32 or
     * indirectly through a GOT slot. The identity is data, never a callback:
     * changing only the absolute slot to a thunk makes equivalent paths unequal
     * (and changes their hash). Preserve that function's address as we do for
     * ADR/ADRP identities below. Swift KeyPath.h defines kind=2 and pointer-ID
     * resolution=0; stored-property/vtable IDs and callable resolvers differ.
     * A conservative extra match only retains the existing fault fallback. */
    for (int s = 0; s < img->nsects; s++) {
        if (img->sects[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) continue;
        if (strcmp(img->sects[s].sect, "__const")) continue;
        uint64_t a = img->sects[s].addr, size = img->sects[s].size;
        for (uint64_t off = 0; off + 8 <= size; off += 4) {
            uint32_t header;
            int32_t relative;
            memcpy(&header, (const void *)(uintptr_t)(a + off), 4);
            if (((header >> 24) & 0x7f) != 2 || (header & 0x0030000fu)) continue;
            memcpy(&relative, (const void *)(uintptr_t)(a + off + 4), 4);
            uint64_t target = a + off + 4 + (uint64_t)(int64_t)relative;
            long k = find_u64(fn, nfn, target);
            if (k >= 0) taken[k] = 1;
        }
    }
    for (int s = 0; s < img->nsects; s++) {
        if (!(img->sects[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))) continue;
        uint64_t a = img->sects[s].addr, n = img->sects[s].size / 4;
        if (!hao_image_for_code(a)) continue;
        const uint32_t *w = hao_code_ptr(a);   /* the section lies in one image: one alias */
        uint64_t page[32];
        uint64_t at[32];
        memset(at, 0xff, sizeof at);
        for (uint64_t i = 0; i < n; i++) {
            uint32_t x = w[i];
            uint64_t pc = a + 4 * i, t;
            if ((x & 0x9F000000u) == 0x90000000u) {                       /* ADRP */
                uint32_t imm = ((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2);
                page[x & 31] = (pc & ~0xFFFull) + (uint64_t)(sext21(imm) << 12);
                at[x & 31] = i;
                continue;
            }
            if ((x & 0x9F000000u) == 0x10000000u) {                       /* ADR */
                uint32_t imm = ((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2);
                t = pc + (uint64_t)sext21(imm);
            } else if ((x & 0xFFC00000u) == 0x91000000u) {                /* ADD Xd, Xn, #imm12 */
                unsigned rn = (x >> 5) & 31;
                if (at[rn] == ~0ull || i - at[rn] > 16) continue;
                t = page[rn] + ((x >> 10) & 0xFFF);
            } else continue;
            long k = find_u64(fn, nfn, t);
            if (k >= 0) taken[k] = 1;
        }
    }
}

/* the section holding a (cached: slots come in ascending runs) */
static int section_of(hao_image *img, uint64_t a, int *last)
{
    if (*last >= 0 && a - img->sects[*last].addr < img->sects[*last].size) return *last;
    for (int s = 0; s < img->nsects; s++)
        if (a - img->sects[s].addr < img->sects[s].size) return *last = s;
    return -1;
}

void hao_fnslots_image(hao_image *img)
{
    size_t n = img->nfnslots;
    uint64_t *slots = img->fnslots;
    img->fnslots = NULL;
    img->nfnslots = img->capfnslots = 0;
    if (!n || !enabled()) { free(slots); return; }
    st_images++;

    /* distinct targets, then the address-taken ones among them */
    uint64_t *fn = malloc(n * sizeof *fn);
    uint8_t *taken = calloc(n, 1);
    if (!fn || !taken) { free(fn); free(taken); free(slots); return; }
    size_t nfn = 0;
    for (size_t k = 0; k < n; k++) fn[nfn++] = *(const uint64_t *)(uintptr_t)slots[k];
    qsort(fn, nfn, sizeof *fn, cmp_u64);
    size_t u = 0;
    for (size_t k = 0; k < nfn; k++) if (!u || fn[k] != fn[u - 1]) fn[u++] = fn[k];
    nfn = u;
    hao_fnslots_address_taken(img, fn, nfn, taken);

    uint64_t thunked = 0, kept = 0, skipped = 0;
    int last = -1;
    for (size_t k = 0; k < n; k++) {
        uint64_t *slot = (uint64_t *)(uintptr_t)slots[k];
        int s = section_of(img, slots[k], &last);
        uint32_t type = s >= 0 ? (img->sects[s].flags & SECTION_TYPE) : 0;
        if (s < 0 || type == S_MOD_INIT_FUNC_POINTERS || type == S_MOD_TERM_FUNC_POINTERS ||
            (strncmp(img->sects[s].seg, "__DATA", 6) && strncmp(img->sects[s].seg, "__AUTH", 6))) { skipped++; continue; }
        uint64_t g = *slot;
        if (!hao_is_guest_function(g)) { skipped++; continue; }     /* objc.c/blocks.c gave it a typed entry */
        long i = find_u64(fn, nfn, g);
        if (i >= 0 && taken[i]) { kept++; continue; }
        uint64_t e = hao_thunk_for(g);
        if (e == g) { skipped++; continue; }                         /* thunk table full (logged) */
        *slot = e;
        thunked++;
    }
    st_slots += n;
    st_thunked += thunked;
    st_addr_taken += kept;
    st_skipped += skipped;
    if (hao_verbose)
        fprintf(stderr, "hao: %s: %zu function-pointer slots: %llu thunked, %llu kept (address taken in code), %llu skipped\n",
                strrchr(img->path, '/') ? strrchr(img->path, '/') + 1 : img->path, n, (unsigned long long)thunked,
                (unsigned long long)kept, (unsigned long long)skipped);
    free(fn);
    free(taken);
    free(slots);
}

void hao_fnslots_stats(void)
{
    fprintf(stderr, "hao-fnslots: images=%llu slots=%llu thunked=%llu kept_address_taken=%llu skipped=%llu%s\n",
            (unsigned long long)st_images, (unsigned long long)st_slots, (unsigned long long)st_thunked,
            (unsigned long long)st_addr_taken, (unsigned long long)st_skipped, enabled() ? "" : " (disabled)");
}
