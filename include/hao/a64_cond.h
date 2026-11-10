/*
 * hao — A64 condition codes and their x86 equivalents.
 */
#ifndef HAO_A64_COND_H
#define HAO_A64_COND_H

/* The 4-bit cond field of B.cond, CSEL, CCMP, FCSEL, ... (Arm ARM order). */
typedef enum a64_cond {
    A64_COND_EQ = 0,  /* Z               */
    A64_COND_NE,      /* !Z              */
    A64_COND_CS,      /* C   (HS)        */
    A64_COND_CC,      /* !C  (LO)        */
    A64_COND_MI,      /* N               */
    A64_COND_PL,      /* !N              */
    A64_COND_VS,      /* V               */
    A64_COND_VC,      /* !V              */
    A64_COND_HI,      /* C && !Z         */
    A64_COND_LS,      /* !C || Z         */
    A64_COND_GE,      /* N == V          */
    A64_COND_LT,      /* N != V          */
    A64_COND_GT,      /* !Z && N == V    */
    A64_COND_LE,      /* Z || N != V     */
    A64_COND_AL,      /* always          */
    A64_COND_NV       /* always (AArch64 treats NV as AL) */
} a64_cond;

/*
 * The x86 condition (XCC_* from hao/x86.h, i.e. the low nibble of the
 * Jcc/SETcc/CMOVcc opcode) that tests ARM condition `cond` when the guest
 * NZCV sits in EFLAGS in hao's convention: N = SF, Z = ZF, V = OF and
 * C = NOT CF. With carry stored inverted, every ARM condition is a single
 * x86 condition (CS = AE, HI = A, ...), so no multi-instruction sequences
 * are needed.
 *
 * Returns -1 for AL and NV (and any cond > 15): they have no flag test, and
 * callers emit the unconditional form instead.
 */
int a64_cond_to_xcc(unsigned cond);

#endif
