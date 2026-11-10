/*
 * hao — A64 logical (bitmask) immediates, from the Arm ARM definition of
 * DecodeBitMasks (shared/functions/integer/DecodeBitMasks):
 *
 *   len    = HighestSetBit(immN:NOT(imms))       UNDEFINED if len < 1
 *   esize  = 1 << len,  levels = Ones(len)
 *   S      = imms AND levels                     UNDEFINED if S == levels
 *   R      = immr AND levels
 *   welem  = ZeroExtend(Ones(S + 1), esize)
 *   wmask  = Replicate(ROR(welem, R), M / esize)
 */
#include "hao/a64_imm.h"

static uint64_t ones(unsigned k) { return k >= 64 ? ~0ull : (1ull << k) - 1; }
static uint64_t replicate(uint64_t elem, unsigned esize, unsigned datasize)
{
    for (unsigned w = esize; w < datasize; w *= 2) elem |= elem << w;
    return datasize == 64 ? elem : (elem & 0xffffffffull);
}

/* Bitfield form (immediate = FALSE): no reserved all-ones element; the
 * tmask element is Ones(((S - R) AND levels) + 1). */
bool a64_decode_bitfield(unsigned sf, unsigned n, unsigned immr, unsigned imms, uint64_t *wmask, uint64_t *tmask)
{
    if (n > 1 || immr > 63 || imms > 63) return false;
    if (n != (sf ? 1u : 0u)) return false;
    if (!sf && (immr >= 32 || imms >= 32)) return false;
    unsigned datasize = sf ? 64 : 32;
    unsigned field = (n << 6) | (~imms & 63u);
    if (field < 2) return false;
    unsigned len = 31u - (unsigned)__builtin_clz(field);
    unsigned esize = 1u << len, levels = esize - 1;
    unsigned s = imms & levels, r = immr & levels, d = (s - r) & levels;
    uint64_t emask = ones(esize), w = ones(s + 1), t = ones(d + 1);
    if (r) w = ((w >> r) | (w << (esize - r))) & emask;
    *wmask = replicate(w, esize, datasize);
    *tmask = replicate(t, esize, datasize);
    return true;
}

bool a64_decode_bitmask_imm(unsigned sf, unsigned n, unsigned immr, unsigned imms, uint64_t *out)
{
    if (n > 1 || immr > 63 || imms > 63) return false;
    unsigned datasize = sf ? 64 : 32;
    if (!sf && n) return false;                 /* 32-bit forms require N = 0 */

    unsigned field = (n << 6) | (~imms & 63u);  /* immN:NOT(imms), 7 bits */
    if (field < 2) return false;                /* len < 1 */
    unsigned len = 31u - (unsigned)__builtin_clz(field);
    unsigned esize = 1u << len;                 /* 2, 4, ..., 64 */
    unsigned levels = esize - 1;
    unsigned s = imms & levels, r = immr & levels;
    if (s == levels) return false;              /* all-ones element: reserved */

    /* s + 1 <= 63 here, so the shift is defined. */
    uint64_t elem = (1ull << (s + 1)) - 1;
    if (r) {
        uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
        elem = ((elem >> r) | (elem << (esize - r))) & emask;
    }
    for (unsigned w = esize; w < datasize; w *= 2) elem |= elem << w;
    *out = datasize == 64 ? elem : (elem & 0xffffffffull);
    return true;
}
