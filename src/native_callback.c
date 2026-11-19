/*
 * hao — native callback-call contracts (hao/native_callback.h): their validation, their binding to a loaded
 * image, and the records hao ships. Depends on nothing but the Mach-O image in memory, so the unit test
 * binds fixture records against its own image.
 */
#include "hao/native_callback.h"
#include <mach-o/loader.h>
#include <string.h>

const char *hao_native_callback_recipe_check(const hao_swift_reverse *s)
{
    if (!s || ((uintptr_t)s & 7)) return "recipe missing or misaligned";
    if (s->version != HAO_SWIFT_REVERSE_VERSION) return "recipe version";
    if (s->nargs > 128 || s->nrets > 8 || s->stack_bytes > 4096 || (s->stack_bytes & 15)) return "recipe bounds";
    uint64_t written = 0;   /* guest registers the arguments set: x0-x30 */
    for (unsigned j = 0; j < (unsigned)s->nargs + s->nrets; j++) {
        const hao_swift_move *m = &s->moves[j];
        bool ret = j >= s->nargs;
        if (m->reserved || (m->size != 1 && m->size != 2 && m->size != 4 && m->size != 8 && m->size != 16)) return "move size";
        if (m->arm_kind == HAO_SR_GP) {
            if (m->arm >= (ret ? 4 : 31) || m->size > 8 || (!ret && m->arm == 18)) return "guest register";
            if (!ret) {
                if (written & (1ull << m->arm)) return "guest register set twice";
                written |= 1ull << m->arm;
            }
        } else if (m->arm_kind == HAO_SR_FP) { if (m->arm >= (ret ? 4 : 8)) return "guest vector register"; }
        else if (m->arm_kind == HAO_SR_STACK && !ret) { if (m->arm + m->size > s->stack_bytes) return "guest stack slot"; }
        else return "guest location kind";
        if (m->native_kind == HAO_SR_GP) { if (m->native >= (ret ? 4 : 6) || m->size > 8) return "native register"; }
        else if (m->native_kind == HAO_SR_FP) { if (m->native >= (ret ? 4 : 8)) return "native vector register"; }
        else if (m->native_kind == HAO_SR_STACK && !ret) { if (m->native + m->size > 4096) return "native stack slot"; }
        else if (m->native_kind == HAO_SR_SPECIAL && !ret) { if (m->native >= 4 || m->size != 8) return "native special register"; }
        else return "native location kind";
    }
    return 0;
}

/* The image's LC_UUID and whether [lo, hi) lies in an executable segment of it (by its own load commands). */
static bool image_facts(const struct mach_header_64 *h, uint8_t uuid[16], uint64_t lo, uint64_t hi, bool *code)
{
    if (h->magic != MH_MAGIC_64 || h->sizeofcmds > (1u << 24)) return false;
    const uint8_t *p = (const uint8_t *)(h + 1), *end = p + h->sizeofcmds;
    bool have_uuid = false;
    *code = false;
    for (uint32_t k = 0; k < h->ncmds; k++) {
        const struct load_command *lc = (const void *)p;
        if (p + sizeof *lc > end || lc->cmdsize < sizeof *lc || p + lc->cmdsize > end) return false;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            memcpy(uuid, ((const struct uuid_command *)p)->uuid, 16);
            have_uuid = true;
        } else if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *s = (const void *)p;
            /* __TEXT begins at the Mach-O header: an offset from the header is one into __TEXT */
            if (!strncmp(s->segname, "__TEXT", 16) && (s->initprot & VM_PROT_EXECUTE) && lo < hi && hi <= s->vmsize)
                *code = true;
        }
        p += lc->cmdsize;
    }
    return have_uuid;
}

unsigned hao_native_callback_bind(hao_gmap *map, const void *header, const hao_native_callback_record *rows,
                                  unsigned n, unsigned *rejected)
{
    const struct mach_header_64 *h = header;
    uint8_t uuid[16];
    bool code;
    unsigned bound = 0;
    if (!h || !image_facts(h, uuid, 0, 0, &code)) return 0;
    for (unsigned k = 0; k < n; k++) {
        const hao_native_callback_record *r = &rows[k];
        if (memcmp(r->uuid, uuid, 16)) continue;
        bool ok = r->version == HAO_NATIVE_CALLBACK_VERSION && r->ret_offset >= 2 &&
                  image_facts(h, uuid, r->ret_offset - 2, r->ret_offset, &code) && code &&
                  !hao_native_callback_recipe_check(r->recipe);
        /* the return address follows an indirect near call through a register: FF /2, mod 11 (2 bytes) */
        const uint8_t *call = (const uint8_t *)h + r->ret_offset - 2;
        ok = ok && call[0] == 0xFF && (call[1] & 0xF8) == 0xD0;
        uint64_t pc = (uint64_t)(uintptr_t)h + (ok ? r->ret_offset : 0);
        if (ok) {
            const hao_swift_reverse *had = hao_native_callback_lookup(map, pc);
            if (had && had != r->recipe) ok = false;   /* two records disagree: the first stays */
            else if (!had) {
                bool fresh;
                uint64_t *slot = hao_gmap_claim1(map, pc, &fresh);
                if (!slot) ok = false;
                else if (fresh) {
                    slot[1] = (uint64_t)(uintptr_t)r->recipe;
                    hao_gmap_publish1(map, slot, pc);
                }
            }
        }
        if (ok) bound++;
        else if (rejected) ++*rejected;
    }
    return bound;
}

/* ------------------------------------------------------------------ shipped records
 *
 * Not generator output: each row names one native call whose outgoing signature is known. Format:
 * {HAO_NATIVE_CALLBACK_VERSION, LC_UUID, return-address offset, recipe}.
 */

/* (input: indirect, context) -> result: indirect. x0 the input's address (rdi), x20 the closure context
 * (r13), x8 the result's buffer (rax). */
static const struct __attribute__((aligned(8))) {
    uint16_t version, nargs, nrets, stack_bytes;
    hao_swift_move moves[3];
} indirect_in_out_ctx = {
    HAO_SWIFT_REVERSE_VERSION, 3, 0, 0,
    {
        {0, 0, HAO_SR_GP, HAO_SR_GP, 8, 0},
        {20, 1, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0},
        {8, 3, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0},
    },
};
#define RECIPE(r) ((const hao_swift_reverse *)(const void *)&(r))

const hao_native_callback_record hao_native_callback_rows[] = {
    /* SwiftUICore, macOS 26B131 x86_64: a closure forwarder calling a stored closure */
    {HAO_NATIVE_CALLBACK_VERSION,
     {0x38, 0x25, 0xDD, 0xEC, 0x72, 0x01, 0x3B, 0x96, 0x8F, 0xE9, 0xAB, 0x77, 0xC5, 0x87, 0x42, 0xCF},
     0x756fb8, RECIPE(indirect_in_out_ctx)},
};
const unsigned hao_native_callback_nrows = sizeof hao_native_callback_rows / sizeof hao_native_callback_rows[0];
