/* Shared by tests/guest/uresume.cpp (the guest) and uresumehelper.cpp/.S
 * (the native helper, x86_64 + arm64e for the guest, arm64 for the
 * reference). The offsets are the assembler's (uresumehelper.S); the
 * helper checks them with static_asserts. */
#ifndef URESUME_H
#define URESUME_H
#include <stdint.h>
#include <string.h>

/* uh_call_full: one call of ctx->fn with every argument register and every
 * callee-saved register holding a known value, the FP control set */
typedef struct uh_ctx {
    uint8_t vec[8][16];       /* 0x000 v0-v7 / xmm0-xmm7 arguments */
    uint8_t yhi[16][16];      /* 0x080 x86_64 with AVX: ymm0-15 upper halves */
    uint64_t fn;              /* 0x180 */
    uint64_t gp[10];          /* 0x188 integer arguments a0..a9 */
    uint64_t keep[11];        /* 0x1d8 callee-saved: arm64 x19-x28 x29; x86_64 rbx r13 r12 r14 r15 (0-4), rbp (10) */
    uint64_t fpkeep[8];       /* 0x230 arm64 d8-d15 (callee-saved); x86_64 xmm8-15 low halves (not) */
    uint32_t fpctl;           /* 0x270 x86_64 MXCSR, arm64 FPCR */
    uint32_t fpstat;          /* 0x274 arm64 FPSR */
    uint32_t x87cw;           /* 0x278 */
    uint32_t avx;             /* 0x27c */
    uint64_t ret;             /* 0x280 */
    uint64_t keep_after[11];  /* 0x288 */
    uint64_t fpkeep_after[8]; /* 0x2e0 */
    uint64_t sp_before;       /* 0x320 */
    uint64_t sp_after;        /* 0x328 */
    uint32_t fpctl_after;     /* 0x330 */
    uint32_t fpstat_after;    /* 0x334 */
    uint32_t x87cw_after;     /* 0x338 */
    uint32_t flags_after;     /* 0x33c */
} uh_ctx;

/* uh_fault_full: a load from a PROT_NONE page with every register holding a
 * known value; a native SIGSEGV/SIGBUS handler makes the page readable and
 * returns, and the load runs again */
typedef struct uh_full {
    uint8_t vin[32][16];      /* 0x000 */
    uint8_t vout[32][16];     /* 0x200 */
    uint8_t yhin[16][16];     /* 0x400 */
    uint8_t yhout[16][16];    /* 0x500 */
    uint64_t gin[32];         /* 0x600 arm64 x0-x30 (x18 unused); x86_64 rax rcx rdx rbx (rsp) rbp rsi rdi r8-r15 */
    uint64_t gout[32];        /* 0x700 */
    uint64_t addr;            /* 0x800 the page */
    uint64_t sp_in;           /* 0x808 */
    uint64_t sp_out;          /* 0x810 */
    uint64_t flags_in;        /* 0x818 x86_64 RFLAGS, arm64 NZCV */
    uint64_t flags_out;       /* 0x820 */
    uint32_t fpctl_in;        /* 0x828 MXCSR / FPCR */
    uint32_t fpstat_in;       /* 0x82c FPSR */
    uint32_t x87_in;          /* 0x830 */
    uint32_t avx;             /* 0x834 */
    uint32_t fpctl_out;       /* 0x838 */
    uint32_t fpstat_out;      /* 0x83c */
    uint32_t x87_out;         /* 0x840 */
    uint32_t pad;
} uh_full;

static inline uint64_t uh_val(int i)
{
    return 0x1000000000000000ull * (uint64_t)(i % 15 + 1) + 0x0011223344556677ull * (uint64_t)(i + 3) + (uint64_t)i;
}

static inline uint64_t uh_mix(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= 0x100000001b3ull;
    return h ^ (h >> 29);
}

/* what the guest function returns for its arguments */
static inline uint64_t uh_sum(const uint64_t gp[10], const uint8_t vec[8][16])
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int k = 0; k < 10; k++) h = uh_mix(h, gp[k]);
    for (int k = 0; k < 8; k++) {
        uint64_t lo, hi;
        memcpy(&lo, vec[k], 8);
        memcpy(&hi, vec[k] + 8, 8);
        h = uh_mix(uh_mix(h, lo), hi);
    }
    return h;
}

#ifdef __cplusplus
extern "C" {
#endif
typedef long (*uh_fn)(long);
void uh_fill(uh_ctx *c, int salt);
int uh_check(uh_ctx *c, const char *tag, int print, uint64_t want_ret);
void uh_view(const char *tag);
void uh_mask_print(const char *tag);
long uh_call_slot(uh_fn *slot, long v);
long uh_catch_slot(uh_fn *slot, long v);
void uh_throw(int v);
int uh_loop(const uint64_t *fn, int n, int maskcheck);   /* every guest pointer goes through memory: */
int uh_nthreads(const uint64_t *fn, int nthreads, int n);  /* hao thunks one in x0-x7 */
void uh_mask_case(const uint64_t *fn);
void uh_trap_case(const uint64_t *dladdr_fn);
void uh_altstack_on(void);
void uh_altstack_check(void);
void uh_fwd(int n);
void uh_marker(void);
#ifdef __cplusplus
}
#endif
#endif
