/*
 * hao — x86_64 instruction lengths and the stack-argument slots a native
 * function's code reads (src/x86args.c; CORE: no OS dependencies).
 *
 * swiftcc.c checks a packed stack-argument layout it reads from an arm64 call
 * site against the native x86_64 callee: the callee's frame-pointer accesses
 * at rbp+16+8k say how many SysV stack slots it takes and how wide it reads
 * each one.
 */
#ifndef HAO_X86ARGS_H
#define HAO_X86ARGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HAO_X86_NEXT, HAO_X86_JUMP, HAO_X86_COND, HAO_X86_CALL, HAO_X86_RET, HAO_X86_JUMP_IND, HAO_X86_CALL_IND,
       HAO_X86_STOP };

typedef struct {
    uint8_t len;          /* bytes */
    uint8_t flow;         /* HAO_X86_* */
    bool mem;             /* a ModRM memory operand */
    bool rip;             /* ... rip-relative: at the next instruction's address + disp */
    bool access;          /* ... that is read or written (not LEA, prefetch or a hint NOP) */
    int8_t base, index;   /* registers 0-15 (4 rsp, 5 rbp); base -1: rip-relative or absolute; index -1: none */
    uint8_t width;        /* bytes the access reads or writes; 0: not known */
    int32_t disp;
    int64_t rel;          /* JUMP, COND, CALL: target - address of the next instruction */
} hao_x86_insn;

/* Decode one instruction of AVAIL bytes at P: its length, or 0 for an encoding the decoder does not
 * know (EVEX, 3DNow!, invalid in 64-bit mode) or one cut off. */
int hao_x86_decode(const uint8_t *p, size_t avail, hao_x86_insn *o);

#define HAO_X86_MAX_SLOTS 72
#define HAO_X86_WIDTH_UNKNOWN 0xff
typedef struct {
    int nslots;                          /* highest SysV stack-argument slot accessed, + 1 */
    uint8_t width[HAO_X86_MAX_SLOTS];    /* widest access of each slot in bytes (at most 8), 0 none,
                                          * HAO_X86_WIDTH_UNKNOWN an access of unknown width */
    uint64_t jump;                       /* the function is one direct jump to this address (then nslots 0) */
    const char *why;                     /* what hao_x86_stackargs refused */
} hao_x86_stackuse;

/* The stack-argument slots of the function whose code is the LEN bytes at CODE (its start up to the
 * next function's), with the NSKIP byte ranges [skip[2k], skip[2k+1]) of it (LC_DATA_IN_CODE) not
 * decoded. Slot k is rbp+16+8k after the prologue `push %rbp; mov %rsp, %rbp` (after an optional
 * endbr64), which the function must start with; a function that is one jump, or that prologue,
 * `pop %rbp` and a jump, instead sets u->jump. The code is decoded linearly; LLVM puts a function's
 * jump tables after its last block, often with no data-in-code entry, so decoding stops at the
 * lowest rip-relative LEA target past the LEA once a jump through a register was seen. Returns false
 * (u->why) when the code does not show its slots reliably: no such prologue, an instruction not
 * decoded, a jump through a register with no table found, an indexed jump through memory, an address
 * taken of or an indexed access to the argument area, or a slot past HAO_X86_MAX_SLOTS. Accesses
 * through rsp are not counted (with a frame pointer LLVM reaches the incoming arguments through
 * rbp), nor are tail calls' (a tail-called function's own reads). */
bool hao_x86_stackargs(const uint8_t *code, size_t len, const uint32_t *skip, int nskip, hao_x86_stackuse *u);

#endif
