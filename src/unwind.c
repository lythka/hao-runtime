/*
 * hao — exceptions and longjmp across the guest/native boundary.
 *
 * Every exception runtime the guest uses is native: its __cxa_throw,
 * objc_exception_throw, __cxa_begin_catch and personality imports bind to
 * the x86_64 libc++abi, libobjc and libunwind. So every exception object is
 * a native _Unwind_Exception, and the only thing the native two-phase
 * unwinder cannot do is see guest frames: they run on the guest stack, and
 * their unwind info is arm64 compact unwind and DWARF CFI.
 *
 * hao therefore takes part in native unwinding as one frame per guest
 * segment:
 *
 *   - Each native call or trap a segment makes runs inside hao_eh_guard
 *     (bridge_x86_64.S) with kind NATIVE, a frame whose personality is
 *     hao_eh_personality. In the search phase, the personality walks the
 *     segment's guest frames with the guest's own unwind info and runs the
 *     C++ LSDA matching libc++abi would (types are matched by the catch
 *     type's native can_catch, which also covers ObjC @catch). Found: the
 *     guard frame is the handler frame. Not found: unwinding continues into
 *     the native frames that entered the segment.
 *   - In the cleanup phase, a NATIVE guard whose segment has a landing pad
 *     stops the native unwind there. hao then installs guest landing pads
 *     one by one, the way libunwind would: registers from the guest unwind,
 *     x0 = exception, x1 = selector. A cleanup ends in _Unwind_Resume, which
 *     is a trap (hao_eh_trap_resume) that continues with the next guest
 *     frame. When the segment is exhausted without its handler, the native
 *     _Unwind_Resume continues the native phase 2 from hao's own frames.
 *   - Each segment as a whole runs inside an ENTRY guard. Its personality
 *     restores the enclosing segment's guest state when the native unwind
 *     removes the segment, which is what hao_callback would have done on
 *     return.
 *
 * longjmp: a guest jmp_buf records the segment depth. A longjmp to an
 * enclosing segment restores that segment's guest state, applies the
 * jmp_buf and native-longjmps to the segment's hao_run frame, discarding
 * the native frames in between as a native longjmp would.
 */
#include "hao/runtime.h"
#include <dlfcn.h>
#include <mach-o/compact_unwind_encoding.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <unwind.h>

#define RA_MASK 0x0000ffffffffffffull   /* arm64e signs saved return addresses; hao's PAC is identity, strip anyway */

/* ------------------------------------------------------- encodings */

static uint64_t uleb(const uint8_t **p)
{
    uint64_t r = 0;
    int s = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        if (s < 64) r |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    } while (b & 0x80);
    return r;
}

static int64_t sleb(const uint8_t **p)
{
    uint64_t r = 0;
    int s = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        if (s < 64) r |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    } while (b & 0x80);
    if (s < 64 && (b & 0x40)) r |= ~0ull << s;
    return (int64_t)r;
}

#define PE_OMIT 0xff

/* DW_EH_PE pointer: absolute and pc-relative forms, optionally indirect */
static bool rd_enc(const uint8_t **p, uint8_t enc, uint64_t *out)
{
    *out = 0;
    if (enc == PE_OMIT) return true;
    const uint8_t *at = *p;
    uint64_t v;
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: memcpy(&v, at, 8); *p += 8; break;
    case 0x01: v = uleb(p); break;
    case 0x09: v = (uint64_t)sleb(p); break;
    case 0x02: { uint16_t x; memcpy(&x, at, 2); v = x; *p += 2; break; }
    case 0x0a: { int16_t x; memcpy(&x, at, 2); v = (uint64_t)(int64_t)x; *p += 2; break; }
    case 0x03: { uint32_t x; memcpy(&x, at, 4); v = x; *p += 4; break; }
    case 0x0b: { int32_t x; memcpy(&x, at, 4); v = (uint64_t)(int64_t)x; *p += 4; break; }
    default: return false;
    }
    if (!v && (enc & 0x70)) { *out = 0; return true; }  /* a null relative pointer stays null */
    switch (enc & 0x70) {
    case 0x00: break;
    case 0x10: v += (uint64_t)(uintptr_t)at; break;
    default: return false;   /* textrel/datarel/funcrel are not used on Darwin */
    }
    if (enc & 0x80) memcpy(&v, (const void *)(uintptr_t)v, 8);
    *out = v;
    return true;
}

static int enc_size(uint8_t enc)
{
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    default: return 0;
    }
}

/* ------------------------------------------------ guest frames */

/* Register state of one guest frame at a call site. x[19..30], x[31]=sp
 * and d[8..15] are the callee-saved state an unwinder restores. */
typedef struct {
    uint64_t x[32];
    uint64_t d[16];
    uint64_t pc;      /* return address into this frame */
    uint64_t cfa;     /* filled by gstep */
} gframe;

typedef struct {
    hao_image *img;
    uint64_t func_start;
    uint32_t enc;     /* compact encoding (mode DWARF when only an FDE describes it) */
    uint64_t fde;
    uint64_t lsda;
    uint64_t personality;
} ginfo;

static void frame_from_cpu(gframe *f, const hao_cpu *c)
{
    memset(f, 0, sizeof *f);
    for (int k = 19; k < 32; k++) f->x[k] = c->x[k];
    for (int k = 8; k < 16; k++) f->d[k] = c->v[k].d[0];
    f->pc = c->x[30] & RA_MASK;
}

/* CIE/FDE */
typedef struct {
    uint64_t code_align;
    int64_t data_align;
    int ra;
    uint8_t fde_enc, lsda_enc;
    bool has_z;
    uint64_t personality;
    const uint8_t *insns, *end;
} cie_t;

static bool parse_cie(const uint8_t *c, cie_t *ci)
{
    uint32_t len;
    memcpy(&len, c, 4);
    if (len == 0 || len == 0xffffffffu) return false;
    const uint8_t *end = c + 4 + len, *p = c + 8;
    uint8_t ver = *p++;
    const char *aug = (const char *)p;
    p += strlen(aug) + 1;
    memset(ci, 0, sizeof *ci);
    ci->code_align = uleb(&p);
    ci->data_align = sleb(&p);
    ci->ra = ver == 1 ? *p++ : (int)uleb(&p);
    ci->fde_enc = 0;
    ci->lsda_enc = PE_OMIT;
    if (aug[0] == 'z') {
        ci->has_z = true;
        uint64_t alen = uleb(&p);
        const uint8_t *aend = p + alen;
        for (const char *a = aug + 1; *a; a++) {
            if (*a == 'R') ci->fde_enc = *p++;
            else if (*a == 'L') ci->lsda_enc = *p++;
            else if (*a == 'P') { uint8_t e = *p++; if (!rd_enc(&p, e, &ci->personality)) return false; }
            else if (*a == 'S' || *a == 'B' || *a == 'G') continue;
            else break;
        }
        p = aend;
    } else if (aug[0]) return false;
    ci->insns = p;
    ci->end = end;
    return true;
}

typedef struct {
    cie_t cie;
    uint64_t pc_begin, pc_end, lsda;
    const uint8_t *insns, *end;
} fde_t;

static bool parse_fde(const uint8_t *f, fde_t *fd)
{
    uint32_t len, cie_off;
    memcpy(&len, f, 4);
    memcpy(&cie_off, f + 4, 4);
    if (len == 0 || len == 0xffffffffu || cie_off == 0) return false;
    if (!parse_cie(f + 4 - cie_off, &fd->cie)) return false;
    const uint8_t *p = f + 8, *end = f + 4 + len;
    uint64_t range;
    if (!rd_enc(&p, fd->cie.fde_enc, &fd->pc_begin)) return false;
    if (!rd_enc(&p, fd->cie.fde_enc & 0x0f, &range)) return false;
    fd->pc_end = fd->pc_begin + range;
    fd->lsda = 0;
    if (fd->cie.has_z) {
        uint64_t alen = uleb(&p);
        const uint8_t *aend = p + alen;
        if (fd->cie.lsda_enc != PE_OMIT && alen) { if (!rd_enc(&p, fd->cie.lsda_enc, &fd->lsda)) return false; }
        p = aend;
    }
    fd->insns = p;
    fd->end = end;
    return true;
}

/* scan __eh_frame for the FDE covering pc (images without __unwind_info) */
static uint64_t find_fde(hao_image *im, uint64_t pc)
{
    uint64_t sz, eh = hao_image_section(im, "__TEXT", "__eh_frame", &sz);
    const uint8_t *p = (const uint8_t *)(uintptr_t)eh, *end = p + sz;
    while (eh && p + 8 <= end) {
        uint32_t len, id;
        memcpy(&len, p, 4);
        if (len == 0 || len == 0xffffffffu) break;
        memcpy(&id, p + 4, 4);
        fde_t fd;
        if (id && parse_fde(p, &fd) && pc >= fd.pc_begin && pc < fd.pc_end) return (uint64_t)(uintptr_t)p;
        p += 4 + len;
    }
    return 0;
}

static bool glookup(uint64_t pc, ginfo *gi)
{
    memset(gi, 0, sizeof *gi);
    hao_image *im = hao_image_for_code(pc);
    if (!im) return false;
    gi->img = im;
    uint64_t usz, ui = hao_image_section(im, "__TEXT", "__unwind_info", &usz);
    uint64_t esz, eh = hao_image_section(im, "__TEXT", "__eh_frame", &esz);
    const uint8_t *b = (const uint8_t *)(uintptr_t)ui;
    const struct unwind_info_section_header *h = (const void *)b;
    if (ui && usz >= sizeof *h && h->version == UNWIND_SECTION_VERSION && h->indexCount >= 2) {
        uint32_t off = (uint32_t)(pc - im->base);
        const struct unwind_info_section_header_index_entry *ix = (const void *)(b + h->indexSectionOffset);
        uint32_t lo = 0, hi = h->indexCount - 1;     /* last entry is the end sentinel */
        if (off < ix[0].functionOffset || off >= ix[hi].functionOffset) goto dwarf;
        while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (ix[m].functionOffset <= off) lo = m; else hi = m; }
        if (!ix[lo].secondLevelPagesSectionOffset) goto dwarf;
        const uint8_t *pg = b + ix[lo].secondLevelPagesSectionOffset;
        uint32_t kind, fstart, enc;
        memcpy(&kind, pg, 4);
        if (kind == UNWIND_SECOND_LEVEL_REGULAR) {
            const struct unwind_info_regular_second_level_page_header *ph = (const void *)pg;
            const struct unwind_info_regular_second_level_entry *e = (const void *)(pg + ph->entryPageOffset);
            if (!ph->entryCount || e[0].functionOffset > off) goto dwarf;
            uint32_t a = 0, z = ph->entryCount;
            while (z - a > 1) { uint32_t m = (a + z) / 2; if (e[m].functionOffset <= off) a = m; else z = m; }
            fstart = e[a].functionOffset;
            enc = e[a].encoding;
        } else if (kind == UNWIND_SECOND_LEVEL_COMPRESSED) {
            const struct unwind_info_compressed_second_level_page_header *ph = (const void *)pg;
            const uint32_t *e = (const void *)(pg + ph->entryPageOffset);
            uint32_t base = ix[lo].functionOffset;
            if (!ph->entryCount || base + UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(e[0]) > off) goto dwarf;
            uint32_t a = 0, z = ph->entryCount;
            while (z - a > 1) { uint32_t m = (a + z) / 2; if (base + UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(e[m]) <= off) a = m; else z = m; }
            fstart = base + UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(e[a]);
            uint32_t ei = UNWIND_INFO_COMPRESSED_ENTRY_ENCODING_INDEX(e[a]);
            const uint32_t *common = (const void *)(b + h->commonEncodingsArraySectionOffset);
            const uint32_t *local = (const void *)(pg + ph->encodingsPageOffset);
            if (ei < h->commonEncodingsArrayCount) enc = common[ei];
            else if (ei - h->commonEncodingsArrayCount < ph->encodingsCount) enc = local[ei - h->commonEncodingsArrayCount];
            else return false;
        } else goto dwarf;
        if (!enc) return false;          /* no unwind info for this function */
        gi->func_start = im->base + fstart;
        gi->enc = enc;
        if ((enc & UNWIND_ARM64_MODE_MASK) == UNWIND_ARM64_MODE_DWARF) {
            if (!eh) return false;
            gi->fde = eh + (enc & UNWIND_ARM64_DWARF_SECTION_OFFSET);
        } else {
            if (enc & UNWIND_HAS_LSDA) {
                const struct unwind_info_section_header_lsda_index_entry *l = (const void *)(b + ix[lo].lsdaIndexArraySectionOffset);
                uint32_t n = (ix[lo + 1].lsdaIndexArraySectionOffset - ix[lo].lsdaIndexArraySectionOffset) / sizeof *l;
                uint32_t a = 0, z = n;
                while (a < z) { uint32_t m = (a + z) / 2; if (l[m].functionOffset < fstart) a = m + 1; else z = m; }
                if (a < n && l[a].functionOffset == fstart) gi->lsda = im->base + l[a].lsdaOffset;
            }
            uint32_t pi = (enc & UNWIND_PERSONALITY_MASK) >> __builtin_ctz(UNWIND_PERSONALITY_MASK);
            if (pi && pi <= h->personalityArrayCount) {
                uint32_t po;
                memcpy(&po, b + h->personalityArraySectionOffset + 4 * (pi - 1), 4);
                memcpy(&gi->personality, (const void *)(uintptr_t)(im->base + po), 8);
            }
            return true;
        }
    } else {
dwarf:
        if (!eh || !(gi->fde = find_fde(im, pc))) return false;
        gi->enc = UNWIND_ARM64_MODE_DWARF;
    }
    fde_t fd;
    if (!parse_fde((const uint8_t *)(uintptr_t)gi->fde, &fd) || pc < fd.pc_begin || pc >= fd.pc_end) return false;
    gi->func_start = fd.pc_begin;
    gi->lsda = fd.lsda;
    gi->personality = fd.cie.personality;
    return true;
}

/* DWARF CFA program for arm64: registers 0-30 x, 31 sp, 64-95 v */
enum { R_SAME, R_OFF, R_VALOFF, R_REG, R_UNDEF };
typedef struct { uint8_t how; int64_t v; } rrule;
typedef struct { int cfa_reg; int64_t cfa_off; rrule r[96]; } crow;

static bool run_cfa(const uint8_t *p, const uint8_t *end, const cie_t *ci, uint64_t loc, uint64_t target,
                    crow *row, const crow *init)
{
    crow stack[8];
    int sp = 0;
    while (p < end && loc <= target) {
        uint8_t op = *p++;
        uint64_t reg, off;
        switch (op & 0xc0) {
        case 0x40: loc += (op & 0x3f) * ci->code_align; continue;
        case 0x80:
            off = uleb(&p);
            if ((op & 0x3f) < 96) row->r[op & 0x3f] = (rrule){R_OFF, (int64_t)off * ci->data_align};
            continue;
        case 0xc0:
            if (init && (op & 0x3f) < 96) row->r[op & 0x3f] = init->r[op & 0x3f];
            continue;
        }
        switch (op) {
        case 0x00: break;                                          /* nop */
        case 0x01: { uint64_t a; if (!rd_enc(&p, ci->fde_enc, &a)) return false; loc = a; break; }
        case 0x02: loc += *p++ * ci->code_align; break;
        case 0x03: { uint16_t d; memcpy(&d, p, 2); p += 2; loc += d * ci->code_align; break; }
        case 0x04: { uint32_t d; memcpy(&d, p, 4); p += 4; loc += d * ci->code_align; break; }
        case 0x05: reg = uleb(&p); off = uleb(&p); if (reg < 96) row->r[reg] = (rrule){R_OFF, (int64_t)off * ci->data_align}; break;
        case 0x06: reg = uleb(&p); if (init && reg < 96) row->r[reg] = init->r[reg]; break;
        case 0x07: reg = uleb(&p); if (reg < 96) row->r[reg] = (rrule){R_UNDEF, 0}; break;
        case 0x08: reg = uleb(&p); if (reg < 96) row->r[reg] = (rrule){R_SAME, 0}; break;
        case 0x09: reg = uleb(&p); off = uleb(&p); if (reg < 96) row->r[reg] = (rrule){R_REG, (int64_t)off}; break;
        case 0x0a: if (sp == 8) return false; stack[sp++] = *row; break;
        case 0x0b: if (!sp) return false; { int cr = row->cfa_reg; int64_t co = row->cfa_off; *row = stack[--sp]; row->cfa_reg = cr; row->cfa_off = co; } break;
        case 0x0c: row->cfa_reg = (int)uleb(&p); row->cfa_off = (int64_t)uleb(&p); break;
        case 0x0d: row->cfa_reg = (int)uleb(&p); break;
        case 0x0e: row->cfa_off = (int64_t)uleb(&p); break;
        case 0x11: reg = uleb(&p); { int64_t o = sleb(&p); if (reg < 96) row->r[reg] = (rrule){R_OFF, o * ci->data_align}; } break;
        case 0x12: row->cfa_reg = (int)uleb(&p); row->cfa_off = sleb(&p) * ci->data_align; break;
        case 0x13: row->cfa_off = sleb(&p) * ci->data_align; break;
        case 0x14: reg = uleb(&p); off = uleb(&p); if (reg < 96) row->r[reg] = (rrule){R_VALOFF, (int64_t)off * ci->data_align}; break;
        case 0x15: reg = uleb(&p); { int64_t o = sleb(&p); if (reg < 96) row->r[reg] = (rrule){R_VALOFF, o * ci->data_align}; } break;
        case 0x2d: break;                                          /* AArch64 negate_ra_state */
        case 0x2e: (void)uleb(&p); break;                          /* GNU_args_size */
        case 0x2f: reg = uleb(&p); off = uleb(&p); if (reg < 96) row->r[reg] = (rrule){R_OFF, -(int64_t)off * ci->data_align}; break;
        default: return false;                                     /* expressions: not supported */
        }
    }
    return true;
}

static uint64_t greg(const gframe *f, int r)
{
    if (r <= 31) return f->x[r];
    if (r >= 72 && r < 80) return f->d[r - 64];
    return 0;
}

static bool step_dwarf(const ginfo *gi, gframe *f)
{
    fde_t fd;
    if (!parse_fde((const uint8_t *)(uintptr_t)gi->fde, &fd)) return false;
    crow init, row;
    memset(&init, 0, sizeof init);
    init.cfa_reg = 31;
    if (!run_cfa(fd.cie.insns, fd.cie.end, &fd.cie, fd.pc_begin, ~0ull, &init, NULL)) return false;
    row = init;
    if (!run_cfa(fd.insns, fd.end, &fd.cie, fd.pc_begin, f->pc - 1, &row, &init)) return false;
    uint64_t cfa = greg(f, row.cfa_reg) + (uint64_t)row.cfa_off;
    gframe n = *f;
    for (int r = 0; r < 96; r++) {
        if (!(r <= 30 || (r >= 72 && r < 80))) continue;
        uint64_t v = greg(f, r);
        switch (row.r[r].how) {
        case R_OFF: memcpy(&v, (const void *)(uintptr_t)(cfa + (uint64_t)row.r[r].v), 8); break;
        case R_VALOFF: v = cfa + (uint64_t)row.r[r].v; break;
        case R_REG: v = greg(f, (int)row.r[r].v); break;
        default: break;
        }
        if (r <= 30) n.x[r] = v; else n.d[r - 64] = v;
    }
    int ra = fd.cie.ra < 31 ? fd.cie.ra : 30;
    n.x[31] = cfa;
    n.pc = n.x[ra] & RA_MASK;
    n.cfa = cfa;
    *f = n;
    return true;
}

/* Step f to its caller. On return f->cfa is the CFA of the frame just left
 * (the identity of that frame). */
static bool gstep(const ginfo *gi, gframe *f)
{
    uint32_t e = gi->enc;
    switch (e & UNWIND_ARM64_MODE_MASK) {
    case UNWIND_ARM64_MODE_FRAME: {
        uint64_t fp = f->x[29];
        if (!fp || (fp & 7)) return false;
        uint64_t cfa = fp + 16, loc = fp - 8, v;
        static const uint32_t pairs[] = {UNWIND_ARM64_FRAME_X19_X20_PAIR, UNWIND_ARM64_FRAME_X21_X22_PAIR,
                                         UNWIND_ARM64_FRAME_X23_X24_PAIR, UNWIND_ARM64_FRAME_X25_X26_PAIR,
                                         UNWIND_ARM64_FRAME_X27_X28_PAIR};
        for (int k = 0; k < 5; k++)
            if (e & pairs[k]) {
                memcpy(&f->x[19 + 2 * k], (const void *)(uintptr_t)loc, 8); loc -= 8;
                memcpy(&f->x[20 + 2 * k], (const void *)(uintptr_t)loc, 8); loc -= 8;
            }
        static const uint32_t dpairs[] = {UNWIND_ARM64_FRAME_D8_D9_PAIR, UNWIND_ARM64_FRAME_D10_D11_PAIR,
                                          UNWIND_ARM64_FRAME_D12_D13_PAIR, UNWIND_ARM64_FRAME_D14_D15_PAIR};
        for (int k = 0; k < 4; k++)
            if (e & dpairs[k]) {
                memcpy(&f->d[8 + 2 * k], (const void *)(uintptr_t)loc, 8); loc -= 8;
                memcpy(&f->d[9 + 2 * k], (const void *)(uintptr_t)loc, 8); loc -= 8;
            }
        memcpy(&v, (const void *)(uintptr_t)(fp + 8), 8);
        f->x[30] = v;
        f->pc = v & RA_MASK;
        memcpy(&f->x[29], (const void *)(uintptr_t)fp, 8);
        f->x[31] = cfa;
        f->cfa = cfa;
        return true;
    }
    case UNWIND_ARM64_MODE_FRAMELESS: {
        uint64_t cfa = f->x[31] + 16 * ((e & UNWIND_ARM64_FRAMELESS_STACK_SIZE_MASK) >> 12), loc = cfa - 8;
        static const uint32_t pairs[] = {UNWIND_ARM64_FRAME_X19_X20_PAIR, UNWIND_ARM64_FRAME_X21_X22_PAIR,
                                         UNWIND_ARM64_FRAME_X23_X24_PAIR, UNWIND_ARM64_FRAME_X25_X26_PAIR,
                                         UNWIND_ARM64_FRAME_X27_X28_PAIR};
        for (int k = 0; k < 5; k++)
            if (e & pairs[k]) {
                memcpy(&f->x[19 + 2 * k], (const void *)(uintptr_t)loc, 8); loc -= 8;
                memcpy(&f->x[20 + 2 * k], (const void *)(uintptr_t)loc, 8); loc -= 8;
            }
        f->pc = f->x[30] & RA_MASK;
        f->x[31] = cfa;
        f->cfa = cfa;
        return true;
    }
    case UNWIND_ARM64_MODE_DWARF:
        return step_dwarf(gi, f);
    }
    return false;
}

/* ------------------------------------------------ LSDA (gcc_except_table) */

#define CXX_CLASS 0x434C4E47432B2B00ull     /* "CLNGC++\0" */
#define CXX_DEP_CLASS 0x434C4E47432B2B01ull /* dependent (std::rethrow_exception) */

static bool is_native_cxx(const struct _Unwind_Exception *ue)
{
    return (ue->exception_class & ~0xffull) == CXX_CLASS;
}

/* libc++abi's __cxa_exception (x86_64) sits right below the unwind header */
#define HDR(ue, off) ((char *)(ue) + (off))
#define H_TYPE   (-80)
#define H_PRIMARY (-88)   /* __cxa_dependent_exception::primaryException */
#define H_SWITCH (-36)
#define H_ACTION (-32)
#define H_LSDA   (-24)
#define H_CTEMP  (-16)
#define H_ADJ    (-8)

static uint64_t thrown_object(struct _Unwind_Exception *ue)
{
    if (ue->exception_class == CXX_DEP_CLASS) { uint64_t p; memcpy(&p, HDR(ue, H_PRIMARY), 8); return p; }
    return (uint64_t)(uintptr_t)(ue + 1);
}

/* catchType->can_catch(thrownType, adjustedPtr): vtable slot 4 of
 * libc++abi's __shim_type_info, which objc_ehtype_vtable mirrors with
 * _objc_exception_do_catch. Both catch type infos (guest GOT entries bound
 * to native, or guest typeinfo objects with native vtables) and the thrown
 * type are native objects. */
static bool can_catch(uint64_t catch_ti, struct _Unwind_Exception *ue, uint64_t *adj)
{
    uint64_t thrown;
    memcpy(&thrown, HDR(ue, H_TYPE), 8);
    *adj = thrown_object(ue);
    if (!thrown || !*adj) return false;
    void **vt = *(void ***)(uintptr_t)catch_ti;
    bool (*fn)(uint64_t, uint64_t, uint64_t *) = (bool (*)(uint64_t, uint64_t, uint64_t *))vt[4];
    return fn(catch_ti, thrown, adj);
}

/*
 * Fail closed where one type identity is not enough (guest libc++,
 * runtime.c). A std::filesystem::filesystem_error keeps its paths in a
 * shared _Storage {path, path, string} that path1()/path2() read inline
 * (SDK c++/v1/__filesystem/filesystem_error.h): an object native libc++
 * built holds x86_64 std::strings, which guest code would read with the
 * arm64 layout. A guest handler for that type (the catch type_info names
 * native libc++'s _ZTS, hao_dsc_share_type_names) catching an object whose
 * vtable is not guest code is reported, once per process: the catch itself
 * and what()/code() are right (virtual, or layout-identical members), the
 * paths are not converted. Refusing the catch would break the programs that
 * never read the paths. Every other libc++-owned exception type is read
 * through virtual functions or holds layout-identical members (error_code,
 * regex error codes).
 */
static void note_native_fs_error(uint64_t catch_ti, uint64_t obj)
{
    static uint64_t fs_name;
    if (!fs_name) {
        void *n = dlsym(RTLD_DEFAULT, "_ZTSNSt3__14__fs10filesystem16filesystem_errorE");
        fs_name = n ? (uint64_t)(uintptr_t)n : 1;
    }
    if (fs_name == 1 || !catch_ti || !obj || *(const uint64_t *)(uintptr_t)(catch_ti + 8) != fs_name) return;
    uint64_t vptr = *(const uint64_t *)(uintptr_t)obj;
    static int said;
    if (hao_image_for_addr(vptr) || __atomic_exchange_n(&said, 1, __ATOMIC_RELAXED)) return;   /* guest-built, or said */
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG, "unwind: guest code catches a std::filesystem::filesystem_error that native libc++ built: "
                      "its path1()/path2() hold x86_64 std::strings, not converted to the arm64 layout\n");
    fflush(HAO_DIAG);
}

enum { A_NONE, A_CLEANUP, A_HANDLER, A_TERMINATE };

typedef struct {
    uint64_t lp, adj;
    int64_t sel;
    const uint8_t *action;
} lsda_res;

static bool get_ttype(const uint8_t *class_info, uint8_t enc, int64_t idx, uint64_t *ti)
{
    int sz = enc_size(enc);
    if (!class_info || !sz) return false;
    const uint8_t *p = class_info - idx * sz;
    return rd_enc(&p, enc, ti);
}

/* What the C++ personality would do for this frame and phase: search finds
 * a handler (A_HANDLER) or not (A_NONE); cleanup finds a pad to run
 * (A_CLEANUP, also for the handler frame when `handler`) or not. */
static int scan_lsda(const ginfo *gi, uint64_t pc, struct _Unwind_Exception *ue, bool search, lsda_res *res)
{
    memset(res, 0, sizeof *res);
    if (!gi->lsda) return A_NONE;
    const uint8_t *p = (const uint8_t *)(uintptr_t)gi->lsda;
    uint64_t ipoff = pc - 1 - gi->func_start;
    uint64_t lp_start = gi->func_start;
    uint8_t lpenc = *p++;
    if (lpenc != PE_OMIT && !rd_enc(&p, lpenc, &lp_start)) return A_TERMINATE;
    uint8_t ttenc = *p++;
    const uint8_t *class_info = NULL;
    if (ttenc != PE_OMIT) { uint64_t o = uleb(&p); class_info = p + o; }
    uint8_t csenc = *p++;
    uint64_t cslen = uleb(&p);
    const uint8_t *cs_end = p + cslen, *actions = cs_end;
    bool native = is_native_cxx(ue);
    while (p < cs_end) {
        uint64_t start, len, lp;
        if (!rd_enc(&p, csenc, &start) || !rd_enc(&p, csenc, &len) || !rd_enc(&p, csenc, &lp)) return A_TERMINATE;
        uint64_t action = uleb(&p);
        if (ipoff < start) return A_TERMINATE;      /* no call site entry: libc++abi calls terminate */
        if (ipoff >= start + len) continue;
        if (!lp) return A_NONE;
        res->lp = lp_start + lp;
        if (!action) return search ? A_NONE : A_CLEANUP;
        const uint8_t *ap = actions + action - 1;
        bool cleanup = false;
        for (;;) {
            const uint8_t *rec = ap;
            int64_t tti = sleb(&ap);
            const uint8_t *nextp = ap;
            int64_t next = sleb(&ap);
            if (tti > 0 && search) {
                uint64_t ti;
                if (!get_ttype(class_info, ttenc, tti, &ti)) return A_TERMINATE;
                uint64_t adj = 0;
                bool hit;
                if (!ti) { hit = true; adj = native ? thrown_object(ue) : 0; }
                else hit = native && can_catch(ti, ue, &adj);
                if (hit && ti) note_native_fs_error(ti, adj);
                if (hit) { res->sel = tti; res->adj = adj; res->action = rec; return A_HANDLER; }
            } else if (tti < 0 && search) {
                /* exception specification: a handler unless a listed type catches */
                bool allowed = false;
                if (native) {
                    const uint8_t *sp = class_info + (-tti - 1);
                    for (uint64_t k; (k = uleb(&sp)) != 0;) {
                        uint64_t ti, adj;
                        if (!get_ttype(class_info, ttenc, (int64_t)k, &ti)) return A_TERMINATE;
                        if (ti && can_catch(ti, ue, &adj)) { allowed = true; break; }
                    }
                }
                if (!allowed) { res->sel = tti; res->adj = native ? thrown_object(ue) : 0; res->action = rec; return A_HANDLER; }
            } else if (tti == 0) cleanup = true;
            if (!next) break;
            ap = nextp + next;
        }
        if (search) return A_NONE;
        return cleanup ? A_CLEANUP : A_NONE;
    }
    return A_TERMINATE;
}

/* -------------------------------------------------- terminate */

static void eh_terminate(struct _Unwind_Exception *ue, const char *why)
{
    if (hao_verbose) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "unwind: %s: terminate\n", why); }
    if (is_native_cxx(ue)) {
        void *(*begin)(void *) = (void *(*)(void *))dlsym(RTLD_DEFAULT, "__cxa_begin_catch");
        if (begin) begin(ue);
    }
    void (*term)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "_ZSt9terminatev");
    if (term) term();
    abort();
}

/* ------------------------------------- uncallable native personalities
 *
 * An exception that reaches a libdispatch client callout uncaught. Every
 * libdispatch callout (_dispatch_client_callout, ...2, ...3, ...3_a, ...4:
 * every queue, source, group, apply, once and mach callout) shares one CIE
 * whose personality is ___dispatch_noexcept_personality, `mov w0, #3; ret`
 * on arm64e and `movl $3, %eax; retq` on x86_64: _URC_FATAL_PHASE1_ERROR, so
 * _Unwind_RaiseException returns 3 and __cxa_throw calls std::terminate (the
 * uncaught-exception handler, "Terminating app due to uncaught exception",
 * SIGABRT). In the x86_64 libdispatch of the 27.0 and 27.2 shared caches that
 * CIE (eh_frame +0x3540, augmentation "zPR", encoding 0x9b = indirect|pcrel|
 * sdata4, value 4) names a personality slot 4 bytes past the field, inside
 * the CIE itself: libunwind calls the CIE's own bytes, 0x0020019008070c10, a
 * non-canonical address, and the process dies of SIGSEGV with no reason
 * logged (on Intel hardware #GP on the call, under Rosetta a fetch fault at
 * the target). It is the only CIE of 728 (27.0) / 746 (27.2) with a
 * personality whose slot is not a data pointer.
 *
 * Every guest library import binds to that libdispatch, so on_fault gives a
 * personality call that cannot be made the result the noexcept personality
 * returns. The rule keys on what makes the call impossible, not on
 * libdispatch: libunwind's personality sites are `callq *%rax` with
 * version 1 in %edi (_Unwind_RaiseException, unwind_phase2,
 * unwind_phase2_forced), and no x86_64 code can live at a non-canonical
 * address. Anything else is not touched.
 */
static uint64_t unw_lo, unw_hi;   /* libunwind's __TEXT */
uint64_t hao_eh_badpers_hits;

void hao_eh_fault_init(void)
{
    Dl_info d;
    if (!dladdr((void *)(uintptr_t)&_Unwind_RaiseException, &d) || !d.dli_fbase) return;
    const struct mach_header_64 *mh = d.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (strncmp(sg->segname, "__TEXT", sizeof sg->segname)) continue;
        /* __TEXT starts at the header, in the shared cache as on disk */
        unw_hi = (uint64_t)(uintptr_t)mh + sg->vmsize;
        unw_lo = (uint64_t)(uintptr_t)mh;
        return;
    }
}

static bool canonical(uint64_t a) { uint64_t top = a >> 47; return top == 0 || top == 0x1ffff; }
static bool call_rax_at(uint64_t pc)   /* ff d0: callq *%rax, inside libunwind's __TEXT */
{
    if (pc < unw_lo || pc + 2 > unw_hi) return false;
    const uint8_t *c = (const uint8_t *)(uintptr_t)pc;
    return c[0] == 0xff && c[1] == 0xd0;
}

bool hao_eh_fault_target(uint64_t rip) { return rip >= 2 && call_rax_at(rip - 2); }

bool hao_eh_fault(ucontext_t *uc)
{
    _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t rip = ss->__rip, rax = ss->__rax;
    if (!unw_lo || canonical(rax) || (uint32_t)ss->__rdi != 1) return false;
    uint64_t site;
    if (rip == rax) {   /* the call was taken and the fetch faulted (Rosetta): its return address is on top */
        uint64_t rsp = ss->__rsp;
        if (rsp & 7) return false;
        uint64_t ret = *(const uint64_t *)(uintptr_t)rsp;
        if (ret < 2 || !call_rax_at(ret - 2)) return false;
        site = ret - 2;
        ss->__rsp = rsp + 8;
        ss->__rip = ret;
    } else {            /* #GP on the call instruction itself (x86_64 hardware) */
        if (!call_rax_at(rip)) return false;
        site = rip;
        ss->__rip = rip + 2;
    }
    ss->__rax = _URC_FATAL_PHASE1_ERROR;   /* what ___dispatch_noexcept_personality returns */
    if (!__atomic_fetch_add(&hao_eh_badpers_hits, 1, __ATOMIC_RELAXED) && (hao_opt.crash_trace || hao_diag_file)) {
        char b[200];
        int n = snprintf(b, sizeof b, "hao[%s:%d]: unwind: personality %#llx is not an address (libunwind+%#llx): "
                         "_URC_FATAL_PHASE1_ERROR, as the callout's noexcept personality\n", getprogname(), (int)getpid(),
                         (unsigned long long)rax, (unsigned long long)(site - unw_lo));
        if (n > 0) hao_diag_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
    }
    return true;
}

/* ------------------------------------------------ handler records */

static hao_eh_rec *rec_find(hao_thread *t, void *ue)
{
    for (int k = 0; k < HAO_EH_RECS; k++) if (t->eh_rec[k].ue == ue) return &t->eh_rec[k];
    return NULL;
}

static void rec_drop(hao_thread *t, void *ue)
{
    hao_eh_rec *r = rec_find(t, ue);
    if (r) memset(r, 0, sizeof *r);
}

static hao_eh_rec *rec_new(hao_thread *t, void *ue)
{
    hao_eh_rec *r = rec_find(t, ue);
    if (!r) r = rec_find(t, NULL);
    if (!r) r = &t->eh_rec[0];   /* more than HAO_EH_RECS exceptions in flight: reuse the oldest slot */
    memset(r, 0, sizeof *r);
    r->ue = ue;
    return r;
}

void hao_eh_forget(hao_thread *t, int above_depth)
{
    for (int k = 0; k < HAO_EH_RECS; k++)
        if (t->eh_rec[k].ue && t->eh_rec[k].depth > above_depth) memset(&t->eh_rec[k], 0, sizeof t->eh_rec[k]);
}

/* -------------------------------------------------- segment walks */

/* Guest state of segment s at its pending native call. While inner
 * segments run, it is what the innermost of them saved on entry. */
static const hao_cpu *seg_state(hao_thread *t, hao_seg *s)
{
    if (t->seg == s) return &t->cpu;
    for (hao_seg *q = t->seg; q; q = q->prev)
        if (q->prev == s) return q->outer;
    return NULL;
}

/* search phase: does a guest frame of this segment catch ue? */
static bool seg_search(hao_thread *t, hao_seg *s, const hao_cpu *c, struct _Unwind_Exception *ue)
{
    gframe f;
    frame_from_cpu(&f, c);
    for (int n = 0; n < 100000; n++) {
        if (f.pc == HAO_RETURN_MAGIC) return false;
        ginfo gi;
        if (!glookup(f.pc - 1, &gi)) {
            if (hao_verbose) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "unwind: no unwind info for guest pc %#llx", (unsigned long long)f.pc); hao_diag_guest_addr(HAO_DIAG, f.pc); fprintf(HAO_DIAG, "\n"); }
            return false;
        }
        lsda_res r;
        int a = scan_lsda(&gi, f.pc, ue, true, &r);
        if (a == A_TERMINATE) eh_terminate(ue, "guest call site not in its LSDA");
        gframe g = f;
        if (!gstep(&gi, &g)) return false;
        if (a == A_HANDLER) {
            hao_eh_rec *rec = rec_new(t, ue);
            rec->seg = s;
            rec->depth = s->depth;
            rec->cfa = g.cfa;
            rec->lp = r.lp;
            rec->adj = r.adj;
            rec->sel = r.sel;
            rec->lsda = (const uint8_t *)(uintptr_t)gi.lsda;
            rec->action = r.action;
            return true;
        }
        f = g;
    }
    return false;
}

/* cleanup phase: is there a guest landing pad to run in this segment? */
static bool seg_has_pad(const hao_cpu *c, struct _Unwind_Exception *ue)
{
    gframe f;
    frame_from_cpu(&f, c);
    for (int n = 0; n < 100000; n++) {
        if (f.pc == HAO_RETURN_MAGIC) return false;
        ginfo gi;
        if (!glookup(f.pc - 1, &gi)) return false;
        lsda_res r;
        if (scan_lsda(&gi, f.pc, ue, false, &r) == A_CLEANUP) return true;
        if (!gstep(&gi, &f)) return false;
    }
    return false;
}

static void install(hao_cpu *c, const gframe *f, uint64_t lp, struct _Unwind_Exception *ue, int64_t sel)
{
    for (int k = 19; k < 31; k++) c->x[k] = f->x[k];
    c->x[31] = f->x[31];
    for (int k = 8; k < 16; k++) { c->v[k].d[0] = f->d[k]; c->v[k].d[1] = 0; }
    c->x[0] = (uint64_t)(uintptr_t)ue;
    c->x[1] = (uint64_t)sel;
    c->pc = lp;
    if (hao_verbose > 1) {
        fprintf(stderr, "hao: unwind: landing pad %#llx selector %lld", (unsigned long long)lp, (long long)sel);
        hao_diag_guest_addr(stderr, lp);
        fprintf(stderr, "\n");
    }
}

/* Continue phase 2 in the current segment from the guest frame t->cpu
 * describes (x30 is its return address): install the next landing pad, or
 * set t->eh_resume when the segment has none left. */
static void seg_continue(hao_thread *t, struct _Unwind_Exception *ue)
{
    hao_cpu *c = &t->cpu;
    hao_eh_rec *rec = rec_find(t, ue);
    if (rec && rec->seg != t->seg) rec = NULL;
    gframe f;
    frame_from_cpu(&f, c);
    for (int n = 0; n < 100000; n++) {
        if (f.pc == HAO_RETURN_MAGIC) break;
        ginfo gi;
        if (!glookup(f.pc - 1, &gi)) break;
        lsda_res r;
        int a = scan_lsda(&gi, f.pc, ue, false, &r);
        if (a == A_TERMINATE) eh_terminate(ue, "guest call site not in its LSDA");
        gframe g = f;
        if (!gstep(&gi, &g)) break;
        if (rec && g.cfa == rec->cfa) {
            /* the handler frame: what libc++abi caches in phase 1 */
            if (is_native_cxx(ue)) {
                int32_t sw = (int32_t)rec->sel;
                memcpy(HDR(ue, H_SWITCH), &sw, 4);
                memcpy(HDR(ue, H_ACTION), &rec->action, 8);
                memcpy(HDR(ue, H_LSDA), &rec->lsda, 8);
                memcpy(HDR(ue, H_CTEMP), &rec->lp, 8);
                memcpy(HDR(ue, H_ADJ), &rec->adj, 8);
            }
            uint64_t lp = rec->lp;
            int64_t sel = rec->sel;
            rec_drop(t, ue);
            install(c, &f, lp, ue, sel);
            return;
        }
        if (a == A_CLEANUP) { install(c, &f, r.lp, ue, 0); return; }
        f = g;
    }
    if (rec) {
        /* the search phase found a handler here that the cleanup phase cannot reach */
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "unwind: guest handler frame (cfa %#llx) not reached\n", (unsigned long long)rec->cfa);
        rec_drop(t, ue);
        eh_terminate(ue, "lost guest handler");
    }
    t->eh_resume = ue;
}

/* -------------------------------------------------- personality */

_Unwind_Reason_Code hao_eh_personality(int version, _Unwind_Action actions, uint64_t cls,
                                       struct _Unwind_Exception *ue, struct _Unwind_Context *ctx);

_Unwind_Reason_Code hao_eh_personality(int version, _Unwind_Action actions, uint64_t cls,
                                       struct _Unwind_Exception *ue, struct _Unwind_Context *ctx)
{
    (void)cls;
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;
    hao_seg *s = (hao_seg *)(uintptr_t)_Unwind_GetGR(ctx, 3);   /* rbx */
    long kind = (long)_Unwind_GetGR(ctx, 12);                    /* r12 */
    hao_thread *t = s->t;
    if (kind == HAO_GUARD_ENTRY) {
        /* the native unwind is removing this segment: undo its entry */
        if (!(actions & _UA_SEARCH_PHASE)) {
            if (s->outer) t->cpu = *s->outer;
            t->in_fault_handler = s->fault_at_entry;
            t->swift_values = s->prev ? s->prev->swift_values_at_entry : NULL;
            t->seg = s->prev;
            t->depth = s->depth - 1;
            hao_ncache_abandon(t, s->depth);
            if (hao_verbose > 1) fprintf(stderr, "hao: unwind: left segment depth %d\n", s->depth);
        }
        return _URC_CONTINUE_UNWIND;
    }
    if (actions & _UA_SEARCH_PHASE) {
        const hao_cpu *c = seg_state(t, s);
        if (c && seg_search(t, s, c, ue)) {
            if (hao_verbose > 1) fprintf(stderr, "hao: unwind: guest handler at depth %d\n", s->depth);
            return _URC_HANDLER_FOUND;
        }
        rec_drop(t, ue);
        return _URC_CONTINUE_UNWIND;
    }
    if (t->seg != s) {
        /* inner segments are restored by their ENTRY guards before we get here */
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "unwind: segment depth %d is not innermost (%d)\n", s->depth, t->seg ? t->seg->depth : -1);
        abort();
    }
    if ((actions & _UA_HANDLER_FRAME) || seg_has_pad(&t->cpu, ue)) {
        t->eh_landed = ue;
        _Unwind_SetIP(ctx, (uintptr_t)hao_eh_guard_pad());
        return _URC_INSTALL_CONTEXT;
    }
    return _URC_CONTINUE_UNWIND;
}

/* The NATIVE guard stopped an exception: start running guest pads. */
void hao_eh_landed(hao_thread *t)
{
    hao_ncache_abandon(t, t->depth);   /* the frames above the guard, among them a cache store at this depth */
    struct _Unwind_Exception *ue = t->eh_landed;
    t->eh_landed = NULL;
    seg_continue(t, ue);
    if (t->eh_resume) hao_eh_resume_native(t);
}

/* No guest pad left in this segment: continue the native cleanup phase
 * from here (run loop, then this segment's ENTRY guard, then the native
 * frames that entered it). */
void hao_eh_resume_native(hao_thread *t)
{
    struct _Unwind_Exception *ue = t->eh_resume;
    t->eh_resume = NULL;
    if (hao_verbose > 1) fprintf(stderr, "hao: unwind: resuming natively from depth %d\n", t->depth);
    _Unwind_Resume(ue);
}

/* guest _Unwind_Resume(ue): a cleanup pad finished; t->cpu is its frame */
void hao_eh_trap_resume(hao_thread *t, uint64_t arg)
{
    (void)arg;
    seg_continue(t, (struct _Unwind_Exception *)(uintptr_t)t->cpu.x[0]);
}

/* -------------------------------------------------- longjmp */

/* Re-enter segment `depth` from an inner one. The caller has already put
 * the guest state the longjmp restores into t->cpu. */
void hao_longjmp_segment(hao_thread *t, int depth)
{
    hao_seg *s = t->seg, *inner = NULL;
    while (s && s->depth > depth) { inner = s; s = s->prev; }
    if (!s || s->depth != depth || !inner) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "longjmp to segment depth %d, which is not live (current %d)\n", depth, t->depth);
        abort();
    }
    t->seg = s;
    t->swift_values = s->swift_values_at_entry;
    t->depth = depth;
    /* back in s: its own signal state. inner's entry values are not it when inner is a guest signal
     * handler's segment, entered after hao_signal_forward or tramp had set them (a SIGSEGV handler that
     * left by siglongjmp made the next fault fatal, and every handler left so kept sig_nest raised) */
    t->in_fault_handler = s->fault_at_entry;
    t->sig_nest = s->nest_at_entry;
    t->eh_landed = t->eh_resume = NULL;
    hao_eh_forget(t, depth);
    _longjmp(s->jb, 1);
}
