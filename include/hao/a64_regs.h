/*
 * hao — A64 register use read from a function's own instructions.
 */
#ifndef HAO_A64_REGS_H
#define HAO_A64_REGS_H

#include <stdbool.h>
#include <stdint.h>

/* Reads the instruction word at pc into *insn; false when pc is not code the caller allows. */
typedef bool (*a64_fetch_fn)(void *ctx, uint64_t pc, uint32_t *insn);

/*
 * Does the function at entry read x8 before it writes it: is x8 an input, as it is for a function that
 * returns its result indirectly (AAPCS64 and Swift: x8 holds the result's buffer)? A bounded walk of at
 * most `budget` instructions over straight-line code, B to its target and B.cond, CBZ/CBNZ and
 * TBZ/TBNZ both ways (nested at most four deep). x8 read as a base register or source operand on some
 * path before any write to it answers true. A path ends without a read at a write to x8 (also w8),
 * a call (BL, BLR), an indirect branch or return, a system or exception instruction, an instruction
 * this walk does not classify that names register 8 in a register field, the edge of what fetch
 * allows, or the budget. Hints (NOP, PACIBSP, BTI, ...) and barriers are neutral.
 */
bool a64_x8_live_in(a64_fetch_fn fetch, void *ctx, uint64_t entry, int budget);

#endif
