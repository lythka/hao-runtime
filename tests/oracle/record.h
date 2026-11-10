/* Shared record format between the native arm64 oracle and the x86_64 checker. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Scratch memory, mapped at the same address on both sides:
 *   [0, RO)          read/write, refilled from the record seed
 *   [RO, RO+RO_SIZE) read-only, a fixed pattern (loads must work, stores fault)
 *   [RO+RO_SIZE, FILL) read/write, refilled from the record seed
 * Stores anywhere in [0, FILL) are caught by a hash of the region. */
#define ORACLE_SCRATCH      0x400000000ull
#define ORACLE_SCRATCH_SIZE 0x40000ull
#define ORACLE_RO           0x10000ull
#define ORACLE_RO_SIZE      0x4000ull     /* one 16 KB arm64 page */
#define ORACLE_FILL         0x24000ull
#define ORACLE_MEMWIN       0x7f80ull     /* 256-byte window kept for diagnostics */

#define ORACLE_MAXI 8

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint32_t nzcv;     /* ARM NZCV in bits 31:28 */
    uint32_t fpsr;
    uint32_t fpcr;
    uint32_t pad;
    unsigned __int128 v[32];
} oracle_state;

/* Offsets used by the native harness generator. */
#define OS_SP   248
#define OS_NZCV 256
#define OS_FPSR 260
#define OS_FPCR 264
#define OS_V    272
_Static_assert(offsetof(oracle_state, sp) == OS_SP, "layout");
_Static_assert(offsetof(oracle_state, nzcv) == OS_NZCV, "layout");
_Static_assert(offsetof(oracle_state, fpcr) == OS_FPCR, "layout");
_Static_assert(offsetof(oracle_state, v) == OS_V, "layout");

enum { OM_DP, OM_SIMD, OM_MEM, OM_BLOCK, OM_PAC, OM_NMODES };
static const char *const oracle_mode_name[OM_NMODES] = {"dp", "simd", "mem", "block", "pac"};

typedef struct {
    uint32_t mode;
    uint32_t n;                 /* instructions in code[] */
    uint32_t code[ORACLE_MAXI];
    uint32_t status;            /* 0 = ran, 1 = SIGILL, 2 = fault */
    uint32_t sreg;              /* register the harness used: its output is its input */
    uint64_t pc;                /* address of code[0] */
    uint64_t seed;              /* scratch memory seed */
    uint64_t memhash;           /* hash of scratch [0, ORACLE_FILL) after execution */
    oracle_state in, out;
    uint8_t mem_after[256];     /* window at ORACLE_MEMWIN after execution */
} oracle_rec;

static inline uint64_t oracle_xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

/* Refill the writable scratch from `seed`; the read-only page is left alone. */
static inline void oracle_fill(uint64_t seed)
{
    uint64_t s = seed | 1, *p = (uint64_t *)ORACLE_SCRATCH;
    for (size_t k = 0; k < ORACLE_FILL / 8; k++) {
        uint64_t v = oracle_xs(&s);
        if (k * 8 < ORACLE_RO || k * 8 >= ORACLE_RO + ORACLE_RO_SIZE) p[k] = v;
    }
}

/* Fixed content of the read-only page (written once, before mprotect). */
static inline void oracle_fill_ro(void)
{
    uint64_t s = 0x5eed0f0ffeeull, *p = (uint64_t *)(ORACLE_SCRATCH + ORACLE_RO);
    for (size_t k = 0; k < ORACLE_RO_SIZE / 8; k++) p[k] = oracle_xs(&s);
}

static inline uint64_t oracle_memhash(void)
{
    const uint64_t *p = (const uint64_t *)ORACLE_SCRATCH;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t k = 0; k < ORACLE_FILL / 8; k++) h = (h ^ p[k]) * 0x100000001b3ull;
    return h;
}
