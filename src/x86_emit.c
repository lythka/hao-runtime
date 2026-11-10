/* hao — x86_64 machine-code emitter. See include/hao/x86.h. */
#include "hao/x86.h"
#include <string.h>

void xb_init(xbuf *b, void *mem, size_t size)
{
    b->start = b->p = (uint8_t *)mem;
    b->end = b->start + size;
    b->overflow = false;
    b->bad = false;
}

void xb_u8(xbuf *b, uint8_t v)
{
    if (b->p >= b->end) { b->overflow = true; return; }
    *b->p++ = v;
}
void xb_u16(xbuf *b, uint16_t v) { xb_u8(b, (uint8_t)v); xb_u8(b, (uint8_t)(v >> 8)); }
void xb_u32(xbuf *b, uint32_t v) { xb_u16(b, (uint16_t)v); xb_u16(b, (uint16_t)(v >> 16)); }
void xb_u64(xbuf *b, uint64_t v) { xb_u32(b, (uint32_t)v); xb_u32(b, (uint32_t)(v >> 32)); }

static int fits8(int64_t v) { return v >= -128 && v <= 127; }
static int fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* Contract checks: a violation sets b->bad and the caller emits nothing. */
static bool bad(xbuf *b) { b->bad = true; return false; }
static bool okreg(xbuf *b, int r) { return (r >= 0 && r < 16) || bad(b); }
static bool okmem(xbuf *b, xmem m)
{
    if (m.base != XRIP && m.base != XNONE && (m.base < 0 || m.base > 15)) return bad(b);
    if (m.index != XNONE && (m.index < 0 || m.index > 15 || m.index == RSP || m.base == XRIP)) return bad(b);
    if (m.scale != 1 && m.scale != 2 && m.scale != 4 && m.scale != 8) return bad(b);
    return true;
}
static bool oksize(xbuf *b, int size) { return size == 1 || size == 2 || size == 4 || size == 8 || bad(b); }

/* Legacy prefixes. */
enum { P_NONE = 0, P_66 = 1, P_F2 = 2, P_F3 = 4, P_LOCK = 8 };

static void prefixes(xbuf *b, int pfx)
{
    if (pfx & P_LOCK) xb_u8(b, 0xF0);
    if (pfx & P_66) xb_u8(b, 0x66);
    if (pfx & P_F2) xb_u8(b, 0xF2);
    if (pfx & P_F3) xb_u8(b, 0xF3);
}

static void rex(xbuf *b, int w, int r, int x, int bb, int force)
{
    uint8_t v = (uint8_t)(0x40 | (w ? 8 : 0) | ((r >> 3) & 1) << 2 | ((x >> 3) & 1) << 1 | ((bb >> 3) & 1));
    if (v != 0x40 || force) xb_u8(b, v);
}

static void opcodes(xbuf *b, const uint8_t *op, int n)
{
    for (int i = 0; i < n; i++) xb_u8(b, op[i]);
}

/* reg-direct form: ModRM mod=11. */
static void enc_rr(xbuf *b, int pfx, int w, int force, const uint8_t *op, int n, int reg, int rm)
{
    if (!okreg(b, reg) || !okreg(b, rm)) return;
    prefixes(b, pfx);
    rex(b, w, reg, 0, rm, force);
    opcodes(b, op, n);
    xb_u8(b, (uint8_t)(0xC0 | (reg & 7) << 3 | (rm & 7)));
}

/* memory form; an XRIP disp is emitted as given (see xmem in x86.h). */
static void enc_rm(xbuf *b, int pfx, int w, int force, const uint8_t *op, int n, int reg, xmem m)
{
    if (!okreg(b, reg) || !okmem(b, m)) return;
    int base = m.base, index = m.index;
    int bidx = (base >= 0) ? base : 0;
    int iidx = (index >= 0) ? index : 0;
    prefixes(b, pfx);
    rex(b, w, reg, iidx, bidx, force);
    opcodes(b, op, n);
    int sc = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
    if (base == XRIP) {
        xb_u8(b, (uint8_t)(0x00 | (reg & 7) << 3 | 5));
        xb_u32(b, (uint32_t)m.disp);
        return;
    }
    if (base == XNONE) {
        xb_u8(b, (uint8_t)(0x00 | (reg & 7) << 3 | 4));
        xb_u8(b, (uint8_t)(sc << 6 | ((index >= 0 ? index : 4) & 7) << 3 | 5));
        xb_u32(b, (uint32_t)m.disp);
        return;
    }
    int mod;
    if (m.disp == 0 && (base & 7) != 5) mod = 0;
    else if (fits8(m.disp)) mod = 1;
    else mod = 2;
    int need_sib = index >= 0 || (base & 7) == 4;
    if (need_sib) {
        xb_u8(b, (uint8_t)(mod << 6 | (reg & 7) << 3 | 4));
        xb_u8(b, (uint8_t)(sc << 6 | ((index >= 0 ? index : 4) & 7) << 3 | (base & 7)));
    } else {
        xb_u8(b, (uint8_t)(mod << 6 | (reg & 7) << 3 | (base & 7)));
    }
    if (mod == 1) xb_u8(b, (uint8_t)(int8_t)m.disp);
    else if (mod == 2) xb_u32(b, (uint32_t)m.disp);
}

#define OP1(x) (const uint8_t[]){x}, 1
#define OP2(x, y) (const uint8_t[]){x, y}, 2

/* ---------------------------------------------------------------- moves */

void x_mov_rr(xbuf *b, int w, int dst, int src)
{
    enc_rr(b, 0, w, 0, OP1(0x89), src, dst);
}

void x_mov_ri(xbuf *b, int dst, uint64_t imm)
{
    if (!okreg(b, dst)) return;
    if (imm <= 0xFFFFFFFFull) {
        rex(b, 0, 0, 0, dst, 0);
        xb_u8(b, (uint8_t)(0xB8 + (dst & 7)));
        xb_u32(b, (uint32_t)imm);
    } else if ((int64_t)imm >= INT32_MIN && (int64_t)imm <= INT32_MAX) {
        enc_rr(b, 0, 1, 0, OP1(0xC7), 0, dst);
        xb_u32(b, (uint32_t)imm);
    } else {
        rex(b, 1, 0, 0, dst, 0);
        xb_u8(b, (uint8_t)(0xB8 + (dst & 7)));
        xb_u64(b, imm);
    }
}

void x_mov_ld(xbuf *b, int size, int dst, xmem m)
{
    switch (size) {
    case 1: enc_rm(b, 0, 0, 0, OP2(0x0F, 0xB6), dst, m); break;
    case 2: enc_rm(b, 0, 0, 0, OP2(0x0F, 0xB7), dst, m); break;
    case 4: enc_rm(b, 0, 0, 0, OP1(0x8B), dst, m); break;
    case 8: enc_rm(b, 0, 1, 0, OP1(0x8B), dst, m); break;
    default: bad(b); break;
    }
}

void x_movs_ld(xbuf *b, int size, int w, int dst, xmem m)
{
    switch (size) {
    case 1: enc_rm(b, 0, w, 0, OP2(0x0F, 0xBE), dst, m); break;
    case 2: enc_rm(b, 0, w, 0, OP2(0x0F, 0xBF), dst, m); break;
    case 4: if (w) enc_rm(b, 0, 1, 0, OP1(0x63), dst, m); else enc_rm(b, 0, 0, 0, OP1(0x8B), dst, m); break;
    default: bad(b); break;
    }
}

void x_mov_st(xbuf *b, int size, xmem m, int src)
{
    switch (size) {
    case 1: enc_rm(b, 0, 0, src >= 4, OP1(0x88), src, m); break;
    case 2: enc_rm(b, P_66, 0, 0, OP1(0x89), src, m); break;
    case 4: enc_rm(b, 0, 0, 0, OP1(0x89), src, m); break;
    case 8: enc_rm(b, 0, 1, 0, OP1(0x89), src, m); break;
    default: bad(b); break;
    }
}

void x_mov_mi(xbuf *b, int size, xmem m, int32_t imm)
{
    if (!oksize(b, size)) return;
    if (size == 1) { enc_rm(b, 0, 0, 0, OP1(0xC6), 0, m); xb_u8(b, (uint8_t)imm); return; }
    if (size == 2) { enc_rm(b, P_66, 0, 0, OP1(0xC7), 0, m); xb_u16(b, (uint16_t)imm); return; }
    enc_rm(b, 0, size == 8, 0, OP1(0xC7), 0, m);
    xb_u32(b, (uint32_t)imm);
}

void x_lea(xbuf *b, int w, int dst, xmem m) { enc_rm(b, 0, w, 0, OP1(0x8D), dst, m); }

/* Patch the disp32 that ends the instruction just emitted (it started at
 * `start`) so that it addresses `target`. */
static void rip_fix(xbuf *b, uint8_t *start, const void *target)
{
    if (b->overflow || b->p == start) return;   /* nothing (complete) was emitted */
    int64_t d = (int64_t)(intptr_t)target - (int64_t)(intptr_t)b->p;
    if (!fits32(d)) { b->p = start; bad(b); return; }
    int32_t v = (int32_t)d;
    memcpy(b->p - 4, &v, 4);
}
void x_mov_ld_rip(xbuf *b, int size, int dst, const void *target)
{
    uint8_t *start = b->p;
    x_mov_ld(b, size, dst, xm(XRIP, 0));
    rip_fix(b, start, target);
}
void x_lea_rip(xbuf *b, int dst, const void *target)
{
    uint8_t *start = b->p;
    x_lea(b, 1, dst, xm(XRIP, 0));
    rip_fix(b, start, target);
}
void x_movzx8_rr(xbuf *b, int dst, int src) { enc_rr(b, 0, 0, src >= 4, OP2(0x0F, 0xB6), dst, src); }
void x_movzx16_rr(xbuf *b, int dst, int src) { enc_rr(b, 0, 0, 0, OP2(0x0F, 0xB7), dst, src); }

void x_movsx_rr(xbuf *b, int srcsize, int w, int dst, int src)
{
    if (srcsize == 1) enc_rr(b, 0, w, src >= 4, OP2(0x0F, 0xBE), dst, src);
    else if (srcsize == 2) enc_rr(b, 0, w, 0, OP2(0x0F, 0xBF), dst, src);
    else enc_rr(b, 0, 1, 0, OP1(0x63), dst, src);
}

/* ------------------------------------------------------------------ ALU */

void x_alu_rr(xbuf *b, int op, int w, int dst, int src)
{
    enc_rr(b, 0, w, 0, OP1((uint8_t)(op << 3 | 1)), src, dst);
}
void x_alu_rm(xbuf *b, int op, int w, int dst, xmem m)
{
    enc_rm(b, 0, w, 0, OP1((uint8_t)(op << 3 | 3)), dst, m);
}
void x_alu_mr(xbuf *b, int op, int w, xmem m, int src)
{
    enc_rm(b, 0, w, 0, OP1((uint8_t)(op << 3 | 1)), src, m);
}
void x_alu_ri(xbuf *b, int op, int w, int dst, int32_t imm)
{
    if (fits8(imm)) { enc_rr(b, 0, w, 0, OP1(0x83), op, dst); xb_u8(b, (uint8_t)imm); }
    else { enc_rr(b, 0, w, 0, OP1(0x81), op, dst); xb_u32(b, (uint32_t)imm); }
}
void x_alu_mi(xbuf *b, int op, int w, xmem m, int32_t imm)
{
    if (fits8(imm)) { enc_rm(b, 0, w, 0, OP1(0x83), op, m); xb_u8(b, (uint8_t)imm); }
    else { enc_rm(b, 0, w, 0, OP1(0x81), op, m); xb_u32(b, (uint32_t)imm); }
}
void x_test_rr(xbuf *b, int w, int a, int c) { enc_rr(b, 0, w, 0, OP1(0x85), c, a); }
void x_test_ri(xbuf *b, int w, int r, int32_t imm)
{
    enc_rr(b, 0, w, 0, OP1(0xF7), 0, r);
    xb_u32(b, (uint32_t)imm);
}
void x_test_mi8(xbuf *b, xmem m, uint8_t imm)
{
    enc_rm(b, 0, 0, 0, OP1(0xF6), 0, m);
    xb_u8(b, imm);
}
void x_shift_ri(xbuf *b, int op, int w, int r, uint8_t imm)
{
    if (imm == 1) { enc_rr(b, 0, w, 0, OP1(0xD1), op, r); return; }
    enc_rr(b, 0, w, 0, OP1(0xC1), op, r);
    xb_u8(b, imm);
}
void x_shift_cl(xbuf *b, int op, int w, int r) { enc_rr(b, 0, w, 0, OP1(0xD3), op, r); }
void x_unary(xbuf *b, int op, int w, int r) { enc_rr(b, 0, w, 0, OP1(0xF7), op, r); }
void x_imul_rr(xbuf *b, int w, int dst, int src) { enc_rr(b, 0, w, 0, OP2(0x0F, 0xAF), dst, src); }
void x_bsr(xbuf *b, int w, int dst, int src) { enc_rr(b, 0, w, 0, OP2(0x0F, 0xBD), dst, src); }
void x_bsf(xbuf *b, int w, int dst, int src) { enc_rr(b, 0, w, 0, OP2(0x0F, 0xBC), dst, src); }
void x_bswap(xbuf *b, int w, int r)
{
    if (!okreg(b, r)) return;
    rex(b, w, 0, 0, r, 0);
    xb_u8(b, 0x0F);
    xb_u8(b, (uint8_t)(0xC8 + (r & 7)));
}
static bool okcc(xbuf *b, int cc) { return (cc >= 0 && cc < 16) || bad(b); }
void x_cmov_rr(xbuf *b, int cc, int w, int dst, int src)
{
    if (!okcc(b, cc)) return;
    enc_rr(b, 0, w, 0, OP2(0x0F, (uint8_t)(0x40 | cc)), dst, src);
}
void x_cmov_rm(xbuf *b, int cc, int w, int dst, xmem m)
{
    if (!okcc(b, cc)) return;
    enc_rm(b, 0, w, 0, OP2(0x0F, (uint8_t)(0x40 | cc)), dst, m);
}
void x_setcc(xbuf *b, int cc, int r) { if (!okcc(b, cc)) return; enc_rr(b, 0, 0, r >= 4, OP2(0x0F, (uint8_t)(0x90 | cc)), 0, r); }
void x_cqo(xbuf *b, int w) { if (w) xb_u8(b, 0x48); xb_u8(b, 0x99); }
void x_xchg_rr(xbuf *b, int w, int a, int c) { enc_rr(b, 0, w, 0, OP1(0x87), c, a); }

/* --------------------------------------------------------- control flow */

size_t x_jcc32(xbuf *b, int cc)
{
    if (!okcc(b, cc)) cc = XCC_O;
    xb_u8(b, 0x0F); xb_u8(b, (uint8_t)(0x80 | cc));
    size_t at = xb_len(b);
    xb_u32(b, 0);
    return at;
}
size_t x_jmp32(xbuf *b)
{
    xb_u8(b, 0xE9);
    size_t at = xb_len(b);
    xb_u32(b, 0);
    return at;
}
size_t x_jcc8(xbuf *b, int cc)
{
    if (!okcc(b, cc)) cc = XCC_O;
    xb_u8(b, (uint8_t)(0x70 | cc));
    size_t at = xb_len(b);
    xb_u8(b, 0);
    return at;
}
size_t x_jmp8(xbuf *b)
{
    xb_u8(b, 0xEB);
    size_t at = xb_len(b);
    xb_u8(b, 0);
    return at;
}
bool x_patch32(xbuf *b, size_t at, size_t target)
{
    int64_t rel = (int64_t)target - (int64_t)(at + 4);
    if (at + 4 > xb_len(b) || !fits32(rel)) return bad(b);
    int32_t v = (int32_t)rel;
    memcpy(b->start + at, &v, 4);
    return true;
}
bool x_patch8(xbuf *b, size_t at, size_t target)
{
    int64_t rel = (int64_t)target - (int64_t)(at + 1);
    if (at + 1 > xb_len(b) || !fits8(rel)) return bad(b);
    b->start[at] = (uint8_t)(int8_t)rel;
    return true;
}
void x_call_r(xbuf *b, int r) { enc_rr(b, 0, 0, 0, OP1(0xFF), 2, r); }
void x_call_m(xbuf *b, xmem m) { enc_rm(b, 0, 0, 0, OP1(0xFF), 2, m); }
void x_jmp_r(xbuf *b, int r) { enc_rr(b, 0, 0, 0, OP1(0xFF), 4, r); }
void x_jmp_m(xbuf *b, xmem m) { enc_rm(b, 0, 0, 0, OP1(0xFF), 4, m); }
void x_ret(xbuf *b) { xb_u8(b, 0xC3); }
void x_push_r(xbuf *b, int r) { if (!okreg(b, r)) return; rex(b, 0, 0, 0, r, 0); xb_u8(b, (uint8_t)(0x50 + (r & 7))); }
void x_pop_r(xbuf *b, int r) { if (!okreg(b, r)) return; rex(b, 0, 0, 0, r, 0); xb_u8(b, (uint8_t)(0x58 + (r & 7))); }
void x_push_m(xbuf *b, xmem m) { enc_rm(b, 0, 0, 0, OP1(0xFF), 6, m); }
void x_pop_m(xbuf *b, xmem m) { enc_rm(b, 0, 0, 0, OP1(0x8F), 0, m); }
void x_push_i32(xbuf *b, int32_t imm) { xb_u8(b, 0x68); xb_u32(b, (uint32_t)imm); }
void x_pushfq(xbuf *b) { xb_u8(b, 0x9C); }
void x_popfq(xbuf *b) { xb_u8(b, 0x9D); }
void x_lahf(xbuf *b) { xb_u8(b, 0x9F); }
void x_sahf(xbuf *b) { xb_u8(b, 0x9E); }
void x_add_al_i8(xbuf *b, uint8_t imm) { xb_u8(b, 0x04); xb_u8(b, imm); }
void x_cmc(xbuf *b) { xb_u8(b, 0xF5); }
void x_stc(xbuf *b) { xb_u8(b, 0xF9); }
void x_clc(xbuf *b) { xb_u8(b, 0xF8); }
void x_syscall(xbuf *b) { xb_u8(b, 0x0F); xb_u8(b, 0x05); }
void x_ud2(xbuf *b) { xb_u8(b, 0x0F); xb_u8(b, 0x0B); }
void x_int3(xbuf *b) { xb_u8(b, 0xCC); }
void x_nop(xbuf *b) { xb_u8(b, 0x90); }
void x_nopn(xbuf *b, int n)
{
    if (n < 0 || n > 3) { bad(b); return; }
    if (n == 1) xb_u8(b, 0x90);                                          /* nop */
    else if (n == 2) { xb_u8(b, 0x66); xb_u8(b, 0x90); }                /* xchg ax, ax */
    else if (n == 3) { xb_u8(b, 0x0F); xb_u8(b, 0x1F); xb_u8(b, 0x00); } /* nop dword [rax] */
}

/* -------------------------------------------------------------- atomics */

void x_lock_cmpxchg(xbuf *b, int size, xmem m, int src)
{
    if (!oksize(b, size)) return;
    int pfx = P_LOCK | (size == 2 ? P_66 : 0);
    enc_rm(b, pfx, size == 8, size == 1 && src >= 4, OP2(0x0F, (uint8_t)(size == 1 ? 0xB0 : 0xB1)), src, m);
}
void x_lock_xadd(xbuf *b, int size, xmem m, int src)
{
    if (!oksize(b, size)) return;
    int pfx = P_LOCK | (size == 2 ? P_66 : 0);
    enc_rm(b, pfx, size == 8, size == 1 && src >= 4, OP2(0x0F, (uint8_t)(size == 1 ? 0xC0 : 0xC1)), src, m);
}

/* ------------------------------------------------------------------ SSE */

void x_movdqu_ld(xbuf *b, int x, xmem m) { enc_rm(b, P_F3, 0, 0, OP2(0x0F, 0x6F), x, m); }
void x_movdqu_st(xbuf *b, xmem m, int x) { enc_rm(b, P_F3, 0, 0, OP2(0x0F, 0x7F), x, m); }
void x_movq_ld(xbuf *b, int x, xmem m) { enc_rm(b, P_F3, 0, 0, OP2(0x0F, 0x7E), x, m); }
void x_movd_ld(xbuf *b, int x, xmem m) { enc_rm(b, P_66, 0, 0, OP2(0x0F, 0x6E), x, m); }
void x_movq_st(xbuf *b, xmem m, int x) { enc_rm(b, P_66, 0, 0, OP2(0x0F, 0xD6), x, m); }
void x_movd_st(xbuf *b, xmem m, int x) { enc_rm(b, P_66, 0, 0, OP2(0x0F, 0x7E), x, m); }
void x_movdqa_ld(xbuf *b, int x, xmem m) { enc_rm(b, P_66, 0, 0, OP2(0x0F, 0x6F), x, m); }
void x_movdqa_st(xbuf *b, xmem m, int x) { enc_rm(b, P_66, 0, 0, OP2(0x0F, 0x7F), x, m); }

size_t x_jrcxz8(xbuf *b)
{
    xb_u8(b, 0xE3);
    size_t at = xb_len(b);
    xb_u8(b, 0);
    return at;
}
void x_mfence(xbuf *b) { xb_u8(b, 0x0F); xb_u8(b, 0xAE); xb_u8(b, 0xF0); }
void x_xchg_mr(xbuf *b, int size, xmem m, int r)
{
    if (!oksize(b, size)) return;
    enc_rm(b, size == 2 ? P_66 : 0, size == 8, size == 1 && r >= 4, OP1((uint8_t)(size == 1 ? 0x86 : 0x87)), r, m);
}
void x_shrd_ri(xbuf *b, int w, int dst, int src, uint8_t imm)
{
    enc_rr(b, 0, w, 0, OP2(0x0F, 0xAC), src, dst);
    xb_u8(b, imm);
}

static int sse_pfx(xbuf *b, int pfx)
{
    switch (pfx) {
    case 0: return P_NONE;
    case 0x66: return P_66;
    case 0xF2: return P_F2;
    case 0xF3: return P_F3;
    default: bad(b); return -1;
    }
}
void x_sse_rr(xbuf *b, int pfx, uint8_t op, int w, int dst, int src)
{
    int p = sse_pfx(b, pfx);
    if (p >= 0) enc_rr(b, p, w, 0, OP2(0x0F, op), dst, src);
}
void x_psllq_ri(xbuf *b, int x, uint8_t imm)
{
    uint8_t *start = b->p;
    enc_rr(b, P_66, 0, 0, OP2(0x0F, 0x73), 6, x);
    if (b->p != start) xb_u8(b, imm);
}
void x_sse_rri(xbuf *b, int pfx, uint8_t op, int dst, int src, uint8_t imm)
{
    uint8_t *start = b->p;
    x_sse_rr(b, pfx, op, 0, dst, src);
    if (b->p != start) xb_u8(b, imm);
}
void x_sse_shift_ri(xbuf *b, uint8_t op, int digit, int x, uint8_t imm)
{
    if (op < 0x71 || op > 0x73 || (digit != 2 && digit != 4 && digit != 6) || (op == 0x73 && digit == 4)) { bad(b); return; }
    uint8_t *start = b->p;
    enc_rr(b, P_66, 0, 0, OP2(0x0F, op), digit, x);
    if (b->p != start) xb_u8(b, imm);
}
void x_sse_rm(xbuf *b, int pfx, uint8_t op, int w, int reg, xmem m)
{
    int p = sse_pfx(b, pfx);
    if (p >= 0) enc_rm(b, p, w, 0, OP2(0x0F, op), reg, m);
}
