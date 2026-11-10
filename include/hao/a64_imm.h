/*
 * hao — A64 logical (bitmask) immediates.
 */
#ifndef HAO_A64_IMM_H
#define HAO_A64_IMM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Expand the N:immr:imms field of an A64 logical-immediate instruction
 * (AND/ORR/EOR/ANDS immediate) into its value, following the Arm ARM
 * pseudocode DecodeBitMasks(immN, imms, immr, immediate = TRUE, M) and
 * returning wmask.
 *
 * sf selects the register width (nonzero: 64-bit, zero: 32-bit). For a
 * 32-bit operation the result is the 32-bit pattern zero-extended to 64 bits.
 * Returns false, leaving *out untouched, for the encodings the architecture
 * makes UNDEFINED: N = 1 with sf = 0, an element size below 2 (N:NOT(imms)
 * has no set bit above bit 0), and S = all ones within the element (a
 * pattern of all ones is not encodable). Field values outside N <= 1,
 * immr <= 63, imms <= 63 are rejected too.
 */
bool a64_decode_bitmask_imm(unsigned sf, unsigned n, unsigned immr, unsigned imms, uint64_t *out);

/*
 * DecodeBitMasks(immN, imms, immr, immediate = FALSE, M) for the bitfield
 * instructions (SBFM/BFM/UBFM): *wmask is the rotated source mask and *tmask
 * the destination mask, both zero-extended from the register width. Returns
 * false for the encodings BFM's decode makes UNDEFINED: N != sf, and for
 * 32-bit forms immr or imms >= 32 (the opc = 11 check is the caller's).
 */
bool a64_decode_bitfield(unsigned sf, unsigned n, unsigned immr, unsigned imms, uint64_t *wmask, uint64_t *tmask);

#endif
