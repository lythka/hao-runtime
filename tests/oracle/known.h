/*
 * Encodings whose outcome differs from hardware on purpose or by a known,
 * documented gap. The oracle checker passes a record when any of its
 * instructions matches (insn & mask) == value. Anything not listed here that
 * hao rejects while hardware runs it, or runs while hardware raises SIGILL,
 * fails `make oracle`.
 */
#pragma once
#include <stdint.h>
typedef struct { uint32_t mask, value; const char *what; } known_enc;

/* Valid on Apple silicon (M4), not implemented by hao: it reports UNDEF.
 * One entry per (mnemonic, decoder group), the bits common to every sample
 * of it in 2.4 million random simd-mode records, Rd/Rn/Q left free. */
static const known_enc known_unimpl[] = {
};

/* Reserved on Apple silicon (SIGILL), executed by hao. */
static const known_enc known_lenient[] = {
    /* Advanced SIMD groups whose decoders do not yet reject every reserved
     * size/opcode combination (the three-same, FP and load/store groups do). */
    {0x9F3E0C00, 0x0E200800, "two-reg misc (vector): reserved size/opcode combinations"},
    {0xDF3E0C00, 0x5E200800, "two-reg misc (scalar): reserved size/opcode combinations"},
    {0x9F3E0C00, 0x0E300800, "across lanes: reserved size/opcode combinations"},
    {0x9FE08400, 0x0E000400, "copy (DUP/INS/SMOV/UMOV): reserved imm5/imm4 combinations"},
    {0xDFE08400, 0x5E000400, "copy (scalar DUP): reserved imm5/op combinations"},
    {0x9F800400, 0x0F000400, "shift by immediate (vector): reserved immh/opcode combinations"},
    {0xDF800400, 0x5F000400, "shift by immediate (scalar): reserved immh/opcode combinations"},
    {0xBF208C00, 0x0E000800, "permute (UZP/TRN/ZIP): reserved size/Q combinations"},
    {0xDF000400, 0x5F000000, "by element (scalar): reserved size/opcode combinations"},
};
