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
    {0xFFFFFC00, 0x1E634000, "BFCVT"},
    {0xBFFFFC00, 0x0EA16800, "BFCVTN/BFCVTN2"},
    {0xBFC0F400, 0x0F800000, "fmlal (indexed)"},
    {0xBFC0F400, 0x0F00F000, "sudot (indexed)"},
    {0xBFC0F400, 0x2F808000, "fmlal2 (indexed)"},
    {0xBFC0F400, 0x0F80F000, "usdot (indexed)"},
    {0xBFC0F400, 0x0F40F000, "bfdot (indexed)"},
    {0xBFC0FC00, 0x0F804000, "fmlsl (indexed)"},
    {0xBFC0F400, 0x0FC0F000, "bfmlalt (indexed)"},
    {0xBFC0FC00, 0x2F80C000, "fmlsl2 (indexed)"},
    {0xBFE0FC00, 0x2E40FC00, "bfdot"},
    {0xBFE0FC00, 0x0E809C00, "usdot"},
    {0xBFE0FC00, 0x2EA0CC00, "fmlsl2 (three-same)"},
    {0xBFE0FC00, 0x0EA0EC00, "fmlsl (three-same)"},
    {0xBFE0FC00, 0x2E20CC00, "fmlal2 (three-same)"},
    {0xBFE0FC00, 0x0E20EC00, "fmlal (three-same)"},
    {0xBFE0FC00, 0x2E40EC00, "bfmmla"},
    {0xBFE0FC00, 0x2E80A400, "ummla"},
    {0xBFE0FC00, 0x0E80A400, "smmla"},
    {0xBFE0FC00, 0x0E80AC00, "usmmla"},
    {0xBFE0FC00, 0x2EC0FC00, "bfmlalb"},
    {0xBF3FFC00, 0x3E203800, "usqadd (two-misc-scalar)"},
    {0xBF3FFC00, 0x0E207800, "sqabs (two-misc)"},
    {0xBF3FFC00, 0x2E203800, "usqadd (two-misc)"},
    {0xBF3FFC00, 0x0E203800, "suqadd (two-misc)"},
    {0xBF3FFC00, 0x2E207800, "sqneg (two-misc)"},
    {0xBF3FFC00, 0x3E207800, "sqneg (two-misc-scalar)"},
    {0xBF3FFC00, 0x1E207800, "sqabs (two-misc-scalar)"},
    {0xBFBFFC00, 0x0E21F800, "frint64z (two-misc)"},
    {0xBFFFFC00, 0x0E30F800, "fmaxv (across)"},
    {0xBFBFFC00, 0x1E28C000, "frint32x (fp-scalar)"},
    {0xBF3FFC00, 0x1E203800, "suqadd (two-misc-scalar)"},
    {0xBFFFFC00, 0x0EB0F800, "fminv (across)"},
    {0xBFBFFC00, 0x1E284000, "frint32z (fp-scalar)"},
    {0xBFBFFC00, 0x2E21E800, "frint32x (two-misc)"},
    {0xBFBFFC00, 0x1E294000, "frint64z (fp-scalar)"},
    {0xBFBFFC00, 0x2E21F800, "frint64x (two-misc)"},
    {0xBFFFFC00, 0x1E30D800, "faddp (pairwise-scalar)"},
    {0xBFBFFC00, 0x0E21E800, "frint32z (two-misc)"},
    {0xBFBFFC00, 0x1E29C000, "frint64x (fp-scalar)"},
    {0xBFFFFC00, 0x1EB0F800, "fminp (pairwise-scalar)"},
    {0xBFFFFC00, 0x1E30F800, "fmaxp (pairwise-scalar)"},
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
