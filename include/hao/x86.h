/*
 * hao — x86_64 machine-code emitter.
 *
 * tools/check_x86.sh cross-checks every encoder declared here, in every form
 * jit.c emits (register, base+index, RIP-relative and absolute addressing,
 * SSE prefix/REX combinations, short and long branches), against the system
 * assembler: tests/test_x86_emit.c generates the instructions, the script
 * compares the disassembly and the encoded lengths.
 *
 * Errors never produce a different instruction. A full buffer sets
 * `overflow`; an out-of-contract argument (operand size, SSE prefix,
 * register number) or a branch/RIP displacement that does not fit sets
 * `bad` and emits nothing for that instruction.
 */
#ifndef HAO_X86_H
#define HAO_X86_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
    XNONE = -1, XRIP = -2
};

/* x86 condition codes (low nibble of Jcc/SETcc/CMOVcc). */
enum {
    XCC_O = 0, XCC_NO, XCC_B, XCC_AE, XCC_E, XCC_NE, XCC_BE, XCC_A,
    XCC_S, XCC_NS, XCC_P, XCC_NP, XCC_L, XCC_GE, XCC_LE, XCC_G
};

/* Group-1 ALU ops: the /digit and the (op<<3) opcode base. */
enum { XADD = 0, XOR_ = 1, XADC = 2, XSBB = 3, XAND = 4, XSUB = 5, XXOR = 6, XCMP = 7 };
/* Group-2 shifts. */
enum { XROL = 0, XROR = 1, XSHL = 4, XSHR = 5, XSAR = 7 };
/* Group-3 unary (F7 /n). */
enum { XNOT = 2, XNEG = 3, XMUL = 4, XIMUL1 = 5, XDIV = 6, XIDIV = 7 };

/*
 * Memory operand. base = XRIP: disp is relative to the end of the whole
 * instruction (after any immediate), as the CPU computes it; x_mov_ld_rip and
 * x_lea_rip compute it from an absolute target instead. base = XNONE: absolute
 * [index*scale + disp32] (SIB form, no base).
 */
typedef struct {
    int base;   /* register, XNONE, or XRIP */
    int index;  /* register or XNONE (RSP cannot be an index) */
    int scale;  /* 1,2,4,8 */
    int32_t disp;
} xmem;

static inline xmem xm(int base, int32_t disp) { xmem m = { base, XNONE, 1, disp }; return m; }
static inline xmem xmi(int base, int index, int scale, int32_t disp)
{ xmem m = { base, index, scale, disp }; return m; }

typedef struct {
    uint8_t *start;
    uint8_t *p;
    uint8_t *end;
    bool overflow;  /* the buffer ran out */
    bool bad;       /* contract violation or displacement out of range (see top) */
} xbuf;

void xb_init(xbuf *b, void *mem, size_t size);
static inline size_t xb_len(const xbuf *b) { return (size_t)(b->p - b->start); }
void xb_u8(xbuf *b, uint8_t v);
void xb_u16(xbuf *b, uint16_t v);
void xb_u32(xbuf *b, uint32_t v);
void xb_u64(xbuf *b, uint64_t v);

/* Data movement. w = 1 for 64-bit operand size, 0 for 32-bit. */
void x_mov_rr(xbuf *b, int w, int dst, int src);
void x_mov_ri(xbuf *b, int dst, uint64_t imm);          /* shortest form, full 64-bit result */
void x_mov_ld(xbuf *b, int size, int dst, xmem m);      /* zero-extending load, size 1/2/4/8 */
void x_movs_ld(xbuf *b, int size, int w, int dst, xmem m); /* sign-extending load, size 1/2/4 into r32 (w=0) or
                                                            * r64 (w=1); size 4 with w=0 is a plain 32-bit load */
void x_mov_st(xbuf *b, int size, xmem m, int src);      /* store low size bytes, size 1/2/4/8 */
void x_mov_mi(xbuf *b, int size, xmem m, int32_t imm);  /* store imm, size 1/2/4/8 (8 => sign-extended imm32) */
void x_lea(xbuf *b, int w, int dst, xmem m);
/* RIP-relative forms with an absolute target; valid only while the buffer
 * sits at the address it will execute from, or when target lies in the same
 * buffer (the displacement is then position independent). */
void x_mov_ld_rip(xbuf *b, int size, int dst, const void *target);
void x_lea_rip(xbuf *b, int dst, const void *target);
void x_movzx8_rr(xbuf *b, int dst, int src);
void x_movzx16_rr(xbuf *b, int dst, int src);
void x_movsx_rr(xbuf *b, int srcsize, int w, int dst, int src); /* srcsize 1/2/4 */

/* ALU. */
void x_alu_rr(xbuf *b, int op, int w, int dst, int src);
void x_alu_rm(xbuf *b, int op, int w, int dst, xmem m);  /* dst op= [m] */
void x_alu_mr(xbuf *b, int op, int w, xmem m, int src);  /* [m] op= src */
void x_alu_ri(xbuf *b, int op, int w, int dst, int32_t imm);
void x_alu_mi(xbuf *b, int op, int w, xmem m, int32_t imm);
void x_test_rr(xbuf *b, int w, int a, int c);
void x_test_ri(xbuf *b, int w, int r, int32_t imm);
void x_test_mi8(xbuf *b, xmem m, uint8_t imm);
void x_shift_ri(xbuf *b, int op, int w, int r, uint8_t imm);
void x_shift_cl(xbuf *b, int op, int w, int r);
void x_unary(xbuf *b, int op, int w, int r);
void x_imul_rr(xbuf *b, int w, int dst, int src);
void x_bsr(xbuf *b, int w, int dst, int src);
void x_bsf(xbuf *b, int w, int dst, int src);
void x_bswap(xbuf *b, int w, int r);
void x_cmov_rr(xbuf *b, int cc, int w, int dst, int src);
void x_cmov_rm(xbuf *b, int cc, int w, int dst, xmem m);
void x_setcc(xbuf *b, int cc, int r);
void x_cqo(xbuf *b, int w);   /* cqo (w=1) / cdq (w=0) */
void x_xchg_rr(xbuf *b, int w, int a, int c);

/* Control flow. Branch helpers return the offset of their rel8/rel32 field;
 * x_patch8/x_patch32 point it at `target` (an offset in b) and return false,
 * setting `bad`, when the field is outside the buffer or rel8 does not reach. */
size_t x_jcc32(xbuf *b, int cc);
size_t x_jmp32(xbuf *b);
size_t x_jcc8(xbuf *b, int cc);
size_t x_jmp8(xbuf *b);
bool x_patch32(xbuf *b, size_t at, size_t target);
bool x_patch8(xbuf *b, size_t at, size_t target);
void x_call_r(xbuf *b, int r);
void x_call_m(xbuf *b, xmem m);
void x_jmp_r(xbuf *b, int r);
void x_jmp_m(xbuf *b, xmem m);
void x_ret(xbuf *b);
void x_push_r(xbuf *b, int r);
void x_pop_r(xbuf *b, int r);
void x_push_m(xbuf *b, xmem m);
void x_pop_m(xbuf *b, xmem m);
void x_push_i32(xbuf *b, int32_t imm);  /* push imm32 (sign-extended to 64 bits) */
void x_pushfq(xbuf *b);
void x_popfq(xbuf *b);
void x_cmc(xbuf *b);
void x_stc(xbuf *b);
void x_clc(xbuf *b);
void x_syscall(xbuf *b);
void x_ud2(xbuf *b);
void x_int3(xbuf *b);
void x_nop(xbuf *b);
void x_nopn(xbuf *b, int n);   /* one n-byte nop, n = 0..3 (0: nothing) */

/* Atomics (lock-prefixed), size 1/2/4/8. */
void x_lock_cmpxchg(xbuf *b, int size, xmem m, int src);  /* cmpxchg [m], src  (rax implicit) */
void x_lock_xadd(xbuf *b, int size, xmem m, int src);

/* SSE moves (xmm0-15). */
void x_movdqu_ld(xbuf *b, int xmm, xmem m);
void x_movdqu_st(xbuf *b, xmem m, int xmm);
void x_movq_ld(xbuf *b, int xmm, xmem m);   /* movq xmm, m64 (zero upper) */
void x_movd_ld(xbuf *b, int xmm, xmem m);   /* movd xmm, m32 (zero upper) */
void x_movq_st(xbuf *b, xmem m, int xmm);
void x_movd_st(xbuf *b, xmem m, int xmm);
/* movdqa: 16-byte aligned; single-copy atomic on every CPU that enumerates
 * AVX (Intel SDM vol. 3A 9.1.1), i.e. every Intel Mac hao targets. */
void x_movdqa_ld(xbuf *b, int xmm, xmem m);
void x_movdqa_st(xbuf *b, xmem m, int xmm);

size_t x_jrcxz8(xbuf *b);            /* jrcxz rel8: jump if rcx == 0, flags untouched */
void x_mfence(xbuf *b);
void x_xchg_mr(xbuf *b, int size, xmem m, int r); /* implicit lock, size 1/2/4/8 */
void x_shrd_ri(xbuf *b, int w, int dst, int src, uint8_t imm);

/* SSE ops: pfx is the legacy prefix byte 0, 0x66, 0xF2 or 0xF3 (anything
 * else sets `bad`); op is the 0F xx opcode byte; dst is the ModRM reg field,
 * src the r/m field. w sets REX.W (cvtsi2sd/cvttsd2si/movq gpr forms). */
void x_sse_rr(xbuf *b, int pfx, uint8_t op, int w, int dst, int src);
void x_sse_rm(xbuf *b, int pfx, uint8_t op, int w, int reg, xmem m);
void x_psllq_ri(xbuf *b, int xmm, uint8_t imm);   /* psllq xmm, imm8 (EFLAGS untouched) */
/* x_sse_rr followed by an immediate byte (pshufd, pshuflw, ...). */
void x_sse_rri(xbuf *b, int pfx, uint8_t op, int dst, int src, uint8_t imm);
/* Vector shift by an immediate: 66 0F op /digit ib with op 0x71 (words), 0x72
 * (doublewords) or 0x73 (quadwords) and digit 2 (psrl), 4 (psra; not for
 * quadwords) or 6 (psll). */
void x_sse_shift_ri(xbuf *b, uint8_t op, int digit, int xmm, uint8_t imm);

/* Inverse condition. */
static inline int xcc_not(int cc) { return cc ^ 1; }

#endif
