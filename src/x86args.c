/*
 * x86_64 instruction lengths and the stack-argument slots a native function
 * reads (hao/x86args.h).
 *
 * The decoder knows the general-purpose, x87, SSE-SSE4.2 and VEX encodings
 * (lengths, ModRM operands, the width of the common memory accesses) and
 * nothing it is not sure of: EVEX, 3DNow! and the opcodes invalid in 64-bit
 * mode end the decode with 0, and a caller then trusts nothing it would have
 * read from the function. A width it does not know is 0 ("some access").
 */
#include "hao/x86args.h"
#include <string.h>

enum { W_V = 0xfe };   /* operand size: 8 with REX.W, 2 with 66, else 4 */

static int opsz(int rex, bool p66) { return (rex & 8) ? 8 : p66 ? 2 : 4; }

/* the one-byte map: ModRM, immediate bytes (-1: 16/32 by operand size), width of the memory operand */
static bool map0(uint8_t op, int rex, bool p66, bool p67, int *modrm, int *imm, int *w, hao_x86_insn *o, int *rel)
{
    *modrm = 0, *imm = 0, *w = 0, *rel = 0;
    if (op < 0x40) {
        unsigned lo = op & 7;
        if (lo < 4) { *modrm = 1; *w = (op & 1) ? W_V : 1; return true; }
        if (lo == 4) { *imm = 1; return true; }
        if (lo == 5) { *imm = -1; return true; }
        return false;                                   /* push/pop seg, daa, das, aaa, aas; prefixes elsewhere */
    }
    if (op < 0x50) return false;                        /* a second REX */
    if (op < 0x60) return true;                         /* push, pop */
    switch (op) {
    case 0x63: *modrm = 1; *w = 4; return true;         /* movsxd */
    case 0x68: *imm = -1; return true;
    case 0x69: *modrm = 1; *imm = -1; *w = W_V; return true;
    case 0x6a: *imm = 1; return true;
    case 0x6b: *modrm = 1; *imm = 1; *w = W_V; return true;
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: return true;
    case 0x80: *modrm = 1; *imm = 1; *w = 1; return true;
    case 0x81: *modrm = 1; *imm = -1; *w = W_V; return true;
    case 0x83: *modrm = 1; *imm = 1; *w = W_V; return true;
    case 0x84: case 0x86: case 0x88: case 0x8a: *modrm = 1; *w = 1; return true;
    case 0x85: case 0x87: case 0x89: case 0x8b: *modrm = 1; *w = W_V; return true;
    case 0x8c: case 0x8e: *modrm = 1; *w = 2; return true;
    case 0x8d: *modrm = 1; o->access = false; return true;   /* lea */
    case 0x8f: *modrm = 1; *w = 8; return true;         /* pop r/m */
    case 0x9a: return false;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: *imm = p67 ? 4 : 8; return true;   /* mov moffs */
    case 0xa8: *imm = 1; return true;
    case 0xa9: *imm = -1; return true;
    case 0xc0: *modrm = 1; *imm = 1; *w = 1; return true;
    case 0xc1: *modrm = 1; *imm = 1; *w = W_V; return true;
    case 0xc2: *imm = 2; o->flow = HAO_X86_RET; return true;
    case 0xc3: o->flow = HAO_X86_RET; return true;
    case 0xc6: *modrm = 1; *imm = 1; *w = 1; return true;
    case 0xc7: *modrm = 1; *imm = -1; *w = W_V; return true;
    case 0xc8: *imm = 3; return true;                   /* enter */
    case 0xc9: return true;                             /* leave */
    case 0xca: *imm = 2; o->flow = HAO_X86_RET; return true;
    case 0xcb: case 0xcf: o->flow = HAO_X86_RET; return true;
    case 0xcc: o->flow = HAO_X86_STOP; return true;
    case 0xcd: *imm = 1; return true;
    case 0xd0: case 0xd2: *modrm = 1; *w = 1; return true;
    case 0xd1: case 0xd3: *modrm = 1; *w = W_V; return true;
    case 0xd7: return true;
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: *rel = 1; o->flow = HAO_X86_COND; return true;
    case 0xe4: case 0xe5: case 0xe6: case 0xe7: *imm = 1; return true;
    case 0xe8: *rel = 4; o->flow = HAO_X86_CALL; return true;
    case 0xe9: *rel = 4; o->flow = HAO_X86_JUMP; return true;
    case 0xeb: *rel = 1; o->flow = HAO_X86_JUMP; return true;
    case 0xec: case 0xed: case 0xee: case 0xef: case 0xf1: case 0xf5: return true;
    case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: return true;   /* clc stc cli sti cld std */
    case 0xf4: o->flow = HAO_X86_STOP; return true;
    case 0xf6: case 0xf7: case 0xfe: case 0xff: *modrm = 1; *w = (op & 1) ? W_V : 1; return true;   /* imm by reg, below */
    }
    if (op >= 0x70 && op <= 0x7f) { *rel = 1; o->flow = HAO_X86_COND; return true; }
    if (op >= 0x90 && op <= 0x9f) return true;
    if (op >= 0xa4 && op <= 0xaf) return true;
    if (op >= 0xb0 && op <= 0xb7) { *imm = 1; return true; }
    if (op >= 0xb8 && op <= 0xbf) { *imm = (rex & 8) ? 8 : p66 ? 2 : 4; return true; }
    if (op >= 0xd8 && op <= 0xdf) { *modrm = 1; return true; }   /* x87: width not tracked */
    return false;                                       /* 60-62, 82, c4/c5 (VEX, elsewhere), ce, d4-d6, ea */
}

/* the 0F map (legacy or VEX); rep is the F2/F3 prefix (or VEX.pp) */
static bool map1(uint8_t op, int rex, bool p66, int rep, bool vex, int vl, int *modrm, int *imm, int *w,
                 hao_x86_insn *o, int *rel)
{
    int vec = vl ? 32 : 16;
    *modrm = 1, *imm = 0, *w = 0, *rel = 0;
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: return true;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0e: *modrm = 0; return true;
    case 0x0b: *modrm = 0; o->flow = HAO_X86_STOP; return true;   /* ud2 */
    case 0x0d: o->access = false; return true;          /* prefetch */
    case 0x10: case 0x11: *w = rep == 0xf3 ? 4 : rep == 0xf2 ? 8 : vec; return true;
    case 0x12: case 0x13: case 0x16: case 0x17: *w = 8; return true;
    case 0x14: case 0x15: *w = vec; return true;
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        o->access = false; return true;                 /* prefetch, hint NOPs, endbr */
    case 0x20: case 0x21: case 0x22: case 0x23: return true;
    case 0x28: case 0x29: case 0x2b: *w = vec; return true;
    case 0x2a: *w = (rex & 8) ? 8 : 4; return true;
    case 0x2c: case 0x2d: *w = rep == 0xf2 ? 8 : rep == 0xf3 ? 4 : 16; return true;
    case 0x2e: case 0x2f: *w = p66 ? 8 : 4; return true;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x37: *modrm = 0; return true;
    case 0x6e: case 0x7e:
        *w = rep == 0xf3 && op == 0x7e ? 8 : (rex & 8) ? 8 : 4;
        return true;
    case 0x70: case 0x71: case 0x72: case 0x73: *imm = 1; *w = vec; return true;
    case 0x77: *modrm = 0; return true;                 /* emms, vzeroupper */
    case 0x78: case 0x79: return !vex;
    case 0xa0: case 0xa1: case 0xa2: case 0xa8: case 0xa9: case 0xaa: *modrm = 0; return true;
    case 0xa3: case 0xab: case 0xaf: case 0xb3: case 0xbb: case 0xbc: case 0xbd: case 0xb8: *w = W_V; return true;
    case 0xa4: case 0xac: *imm = 1; *w = W_V; return true;
    case 0xa5: case 0xad: *w = W_V; return true;
    case 0xae: return true;
    case 0xb0: case 0xc0: case 0xb6: case 0xbe: *w = 1; return true;
    case 0xb1: case 0xc1: *w = W_V; return true;
    case 0xb2: case 0xb4: case 0xb5: return true;
    case 0xb7: case 0xbf: *w = 2; return true;
    case 0xb9: return true;                             /* ud1 */
    case 0xba: *imm = 1; *w = W_V; return true;
    case 0xc2: *imm = 1; *w = rep == 0xf3 ? 4 : rep == 0xf2 ? 8 : vec; return true;
    case 0xc3: *w = W_V; return true;
    case 0xc4: *imm = 1; *w = 2; return true;           /* pinsrw */
    case 0xc5: *imm = 1; return true;
    case 0xc6: *imm = 1; *w = vec; return true;
    case 0xc7: return true;
    case 0xd6: *w = 8; return true;                     /* movq xmm to m64 */
    }
    if (op >= 0x40 && op <= 0x4f) { *w = W_V; return true; }   /* cmovcc */
    if (op >= 0x50 && op <= 0x5f) { *w = rep == 0xf3 ? 4 : rep == 0xf2 ? 8 : vec; return true; }
    if ((op >= 0x60 && op <= 0x6d) || op == 0x6f || op == 0x7f || (op >= 0x74 && op <= 0x76) ||
        (op >= 0x7c && op <= 0x7d)) { *w = vec; return true; }
    if (op >= 0x80 && op <= 0x8f) { *modrm = 0; *rel = 4; o->flow = HAO_X86_COND; return true; }
    if (op >= 0x90 && op <= 0x9f) { *w = 1; return true; }   /* setcc */
    if (op >= 0xc8 && op <= 0xcf) { *modrm = 0; return true; }   /* bswap */
    if (op >= 0xd0) { *w = vec; return true; }
    return false;                                       /* 04 0a 0c 0f 24-27 36 39 3b-3f 7a 7b */
}

int hao_x86_decode(const uint8_t *p, size_t avail, hao_x86_insn *o)
{
    memset(o, 0, sizeof *o);
    o->base = o->index = -1;
    o->access = true;
    size_t i = 0, lim = avail < 15 ? avail : 15;
    bool p66 = false, p67 = false;
    int rep = 0, rex = 0;
    for (;; i++) {
        if (i >= lim) return 0;
        uint8_t b = p[i];
        if (b == 0x66) p66 = true;
        else if (b == 0x67) p67 = true;
        else if (b == 0xf2 || b == 0xf3) rep = b;
        else if (b != 0xf0 && b != 0x2e && b != 0x36 && b != 0x3e && b != 0x26 && b != 0x64 && b != 0x65) break;
    }
    if ((p[i] & 0xf0) == 0x40) {
        rex = p[i++];
        if (i >= lim) return 0;
    }
    uint8_t op = p[i++];
    int modrm, imm, w, rel, map = 0;
    bool vex = false, ok;
    int vl = 0;
    if (op == 0xc4 || op == 0xc5) {                     /* VEX (LES/LDS are invalid in 64-bit mode) */
        if (rex || p66 || rep) return 0;
        unsigned pp;
        if (op == 0xc5) {
            if (i + 1 >= lim) return 0;
            uint8_t v = p[i++];
            rex = (v & 0x80) ? 0 : 4;
            vl = (v >> 2) & 1, pp = v & 3, map = 1;
        } else {
            if (i + 2 >= lim) return 0;
            uint8_t v1 = p[i++], v2 = p[i++];
            map = v1 & 31;
            if (map < 1 || map > 3) return 0;
            rex = ((v1 & 0x80) ? 0 : 4) | ((v1 & 0x40) ? 0 : 2) | ((v1 & 0x20) ? 0 : 1) | ((v2 & 0x80) ? 8 : 0);
            vl = (v2 >> 2) & 1, pp = v2 & 3;
        }
        p66 = pp == 1;
        rep = pp == 2 ? 0xf3 : pp == 3 ? 0xf2 : 0;
        vex = true;
        op = p[i++];
    } else if (op == 0x0f) {
        if (i >= lim) return 0;
        op = p[i++];
        map = 1;
        if (op == 0x38 || op == 0x3a) {
            if (i >= lim) return 0;
            map = op == 0x38 ? 2 : 3;
            op = p[i++];
        }
    }
    if (map == 0) ok = map0(op, rex, p66, p67, &modrm, &imm, &w, o, &rel);
    else if (map == 1) ok = map1(op, rex, p66, rep, vex, vl, &modrm, &imm, &w, o, &rel);
    else {
        modrm = 1, imm = map == 3, rel = 0, ok = true;
        int vec = vl ? 32 : 16;
        w = vec;
        if (map == 2) {
            static const uint8_t pmov[6] = {8, 4, 2, 8, 4, 8};   /* pmovsx/zx bw bd bq wd wq dq */
            if ((op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35)) w = pmov[op & 7] << vl;
            else if (op == 0xf0 || op == 0xf1) w = rep == 0xf2 && op == 0xf0 ? 1 : opsz(rex, p66);   /* movbe, crc32 */
            else if (vex) w = 0;
        } else {
            switch (op) {
            case 0x0a: case 0x21: case 0x17: w = 4; break;   /* roundss, insertps, extractps */
            case 0x0b: w = 8; break;                    /* roundsd */
            case 0x14: case 0x20: w = 1; break;         /* pextrb, pinsrb */
            case 0x15: w = 2; break;                    /* pextrw */
            case 0x16: case 0x22: w = (rex & 8) ? 8 : 4; break;   /* pextrd/q, pinsrd/q */
            default: if (vex) w = 0;
            }
        }
    }
    if (!ok) return 0;
    if (modrm) {
        if (i >= lim) return 0;
        uint8_t m = p[i++];
        unsigned mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
        if (map == 0 && (op == 0xf6 || op == 0xf7) && reg < 2) imm = op == 0xf6 ? 1 : -1;   /* test r/m, imm */
        if (map == 0 && op == 0xff) {
            if (reg == 2) o->flow = HAO_X86_CALL_IND;
            else if (reg == 4) o->flow = HAO_X86_JUMP_IND;
            else if (reg == 3 || reg == 5) o->flow = HAO_X86_STOP;   /* far call or jump */
            else if (reg == 6) w = 8;                   /* push r/m */
            else if (reg == 7) return 0;
        }
        if (map == 0 && op == 0xfe && reg > 1) return 0;
        if (map == 0 && op == 0x8f && reg) return 0;
        if (map == 0 && (op == 0xc6 || op == 0xc7) && reg) {
            if (m != 0xf8) return 0;                    /* xabort, xbegin */
            if (op == 0xc7) { imm = 0; rel = 4; o->flow = HAO_X86_COND; }
        }
        if (mod != 3) {
            o->mem = true;
            o->base = (int8_t)(rm | (rex & 1) << 3);
            if (rm == 4) {
                if (i >= lim) return 0;
                uint8_t sib = p[i++];
                unsigned idx = ((sib >> 3) & 7) | (rex & 2) << 2, b = sib & 7;
                if (idx != 4) o->index = (int8_t)idx;
                o->base = (int8_t)(b | (rex & 1) << 3);
                if (b == 5 && mod == 0) {
                    o->base = -1;
                    if (i + 4 > lim) return 0;
                    memcpy(&o->disp, p + i, 4);
                    i += 4;
                }
            } else if (rm == 5 && mod == 0) {           /* rip-relative */
                o->base = -1;
                o->rip = true;
                if (i + 4 > lim) return 0;
                memcpy(&o->disp, p + i, 4);
                i += 4;
            }
            if (mod == 1) {
                if (i >= lim) return 0;
                o->disp = (int8_t)p[i++];
            } else if (mod == 2) {
                if (i + 4 > lim) return 0;
                memcpy(&o->disp, p + i, 4);
                i += 4;
            }
            o->width = (uint8_t)(w == W_V ? opsz(rex, p66) : w);
        } else o->access = false;
    } else o->access = false;
    if (imm < 0) imm = p66 ? 2 : 4;
    if (i + (size_t)imm + (size_t)rel > lim) return 0;
    i += (size_t)imm;
    if (rel == 1) o->rel = (int8_t)p[i];
    else if (rel == 4) { int32_t r; memcpy(&r, p + i, 4); o->rel = r; }
    i += (size_t)rel;
    o->len = (uint8_t)i;
    return (int)i;
}

bool hao_x86_stackargs(const uint8_t *code, size_t len, const uint32_t *skip, int nskip, hao_x86_stackuse *u)
{
    memset(u, 0, sizeof *u);
    hao_x86_insn o;
    size_t at = 0, table = 0;
    bool jreg = false;
    int lastlen = 0;
    if (len >= 4 && !memcmp(code, "\xf3\x0f\x1e\xfa", 4)) at = 4;   /* endbr64 */
    size_t first = at;
    if (len >= at + 5 && !memcmp(code + at, "\x55\x48\x89\xe5\x5d", 5)) first = at + 5;   /* push, mov, pop */
    int n = first < len ? hao_x86_decode(code + first, len - first, &o) : 0;
    if (n && o.flow == HAO_X86_JUMP) {                  /* a trampoline: the caller reads the target */
        u->jump = (uint64_t)(uintptr_t)(code + first + (size_t)n) + (uint64_t)o.rel;
        return true;
    }
    if (len < at + 4 || memcmp(code + at, "\x55\x48\x89\xe5", 4)) { u->why = "no frame-pointer prologue"; return false; }
    at += 4;
    while (at < len) {
        if (jreg && table && at >= table) break;        /* the jump tables */
        bool skipped = false;
        for (int k = 0; k < nskip; k++)
            if (at >= skip[2 * k] && at < skip[2 * k + 1]) { at = skip[2 * k + 1]; skipped = true; break; }
        if (skipped) continue;
        bool popped = at > 0 && code[at - 1] == 0x5d && lastlen == 1;   /* after `pop %rbp`: a tail call */
        n = hao_x86_decode(code + at, len - at, &o);
        lastlen = n;
        if (!n) {                                       /* alignment padding of zeros up to the next function */
            while (at < len && !code[at]) at++;
            if (at < len) { u->why = "an instruction not decoded"; return false; }
            break;
        }
        at += (size_t)n;
        if (o.mem && o.rip && !o.access && o.disp > 0 && at + (size_t)o.disp < len &&
            (!table || at + (size_t)o.disp < table))
            table = at + (size_t)o.disp;                /* a LEA of an address later in the function */
        if (o.flow == HAO_X86_JUMP_IND) {
            if (!o.mem && !popped) jreg = true;
            else if (o.index >= 0) { u->why = "an indexed jump through memory"; return false; }
        }
        if (!o.mem || o.base != 5) continue;            /* rbp only: r13 has REX.B */
        if (o.index >= 0) { u->why = "an indexed access through rbp"; return false; }
        if (o.disp < 16) continue;                      /* locals, saved rbp, return address */
        if (!o.access) { u->why = "the address of a stack argument taken"; return false; }
        if ((o.disp - 16) % 8) { u->why = "a stack argument read off its slot's start"; return false; }
        int slot = (o.disp - 16) / 8, wd = o.width ? o.width : HAO_X86_WIDTH_UNKNOWN;
        int cover = o.width > 8 ? o.width / 8 : 1;
        if (slot + cover > HAO_X86_MAX_SLOTS) { u->why = "too many stack slots"; return false; }
        for (int c = 0; c < cover; c++) {
            uint8_t *s = &u->width[slot + c], x = (uint8_t)(o.width > 8 ? 8 : wd);
            if (!*s || (*s != HAO_X86_WIDTH_UNKNOWN && (x == HAO_X86_WIDTH_UNKNOWN || x > *s))) *s = x;
        }
        if (slot + cover > u->nslots) u->nslots = slot + cover;
    }
    if (jreg && !table) { u->why = "a jump through a register and no jump table"; return false; }
    return true;
}
