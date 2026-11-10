/*
 * hao — guest (arm64/arm64e) CPU state shared by the interpreter, the JIT
 * and the runtime. Layout is ABI for emitted x86 code: keep offsets stable
 * and update the static asserts in cpu.c when changing it.
 */
#ifndef HAO_CPU_H
#define HAO_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef union {
    uint8_t  b[16];
    int8_t   sb[16];
    uint16_t h[8];
    int16_t  sh[8];
    uint32_t s[4];
    int32_t  ss[4];
    uint64_t d[2];
    int64_t  sd[2];
    float    f[4];
    double   df[2];
    unsigned __int128 q;
} __attribute__((aligned(16))) hao_vreg;

/*
 * Flags are kept in x86 RFLAGS form so translated code can `push [flags];
 * popfq` / `pushfq; pop [flags]` without conversion:
 *   N -> SF (bit 7), Z -> ZF (bit 6), V -> OF (bit 11), C -> NOT CF (bit 0).
 * Carry is stored inverted because x86 SUB/CMP set CF as a borrow while ARM
 * sets C as NOT borrow: with that polarity ARM `cmp; b.hi` maps straight
 * onto x86 `cmp; ja`, and every ARM condition is one x86 condition
 * (see a64_cond.c).
 */
#define HAO_RF_CF  (1u << 0)
#define HAO_RF_ZF  (1u << 6)
#define HAO_RF_SF  (1u << 7)
#define HAO_RF_OF  (1u << 11)
#define HAO_RF_BASE 0x202u   /* reserved bit 1 + IF, as pushfq reports */

typedef struct hao_cpu {
    uint64_t x[32];          /* 0x000: x0..x30, x[31] = SP */
    uint64_t pc;             /* 0x100 */
    uint64_t flags;          /* 0x108: RFLAGS image, see above */
    uint64_t tpidr_el0;      /* 0x110 */
    uint64_t tpidrro_el0;    /* 0x118 */
    uint32_t fpcr;           /* 0x120: FPCR bits other than RMode/FZ (see a64_fpcr_read) */
    uint32_t fpsr;           /* 0x124: FPSR.QC and IDC (see a64_fpsr_read) */
    uint32_t exit_reason;    /* 0x128: why translated code returned */
    uint32_t exit_insn;      /* 0x12c */
    uint64_t excl_addr;      /* 0x130: exclusive monitor */
    uint64_t excl_val[2];    /* 0x138 */
    uint32_t excl_size;      /* 0x148: bytes, 0 = monitor clear */
    uint32_t excl_pair;      /* 0x14c: the monitor was set by LDXP/LDAXP */
    uint64_t insn_count;     /* 0x150: interpreter/helper statistics */
    uint64_t host_scratch[5];/* 0x158 */
    /* runtime entry points, so translated code is position independent
     * (identical bytes for JIT and AOT): */
    uint64_t rt_helper;      /* 0x180: int helper(cpu, uint64_t descriptor, pc);
                              * low32 = original ARM word, high32 = internal tag */
    uint64_t rt_exit;        /* 0x188: exit stub */
    uint64_t rt_table;       /* 0x190: translation table */
    uint64_t fcmp_flags[8];  /* 0x198: x86 ucomis (PF<<2|ZF<<1|CF) -> guest RFLAGS image */
    /* hao_native_call bookkeeping (bridge_x86_64.S): */
    uint64_t bridge_rsp;     /* 0x1D8: rsp at the native call instruction */
    uint64_t bridge_rbp;     /* 0x1E0: rbp there (balance check) */
    uint64_t bridge_fn;      /* 0x1E8: native callee (balance check) */
    uint64_t native_pop;     /* 0x1F0: bytes the last native callee popped
                              * beyond its return address (x86_64 swifttailcc
                              * `retq $N`), if that call was a guest tail call
                              * (guest x30 = HAO_RETURN_MAGIC); else 0 */
    uint64_t rt_mask;        /* 0x1F8: rt_table's slot mask << 4 (jit.c: they name one table) */
    hao_vreg v[32];          /* 0x200 */
    void    *thread;         /* 0x400: owning hao_thread */
} hao_cpu;

#define CPU_OFF_X(n)   ((int32_t)(8 * (n)))
#define CPU_OFF_SP     CPU_OFF_X(31)
#define CPU_OFF_PC     0x100
#define CPU_OFF_FLAGS  0x108
#define CPU_OFF_EXIT   0x128
#define CPU_OFF_EXITI  0x12c
#define CPU_OFF_V(n)   ((int32_t)(0x200 + 16 * (n)))
#define CPU_OFF_RT_HELPER 0x180
#define CPU_OFF_RT_EXIT   0x188
#define CPU_OFF_RT_TABLE  0x190
#define CPU_OFF_FCMP      0x198
#define CPU_OFF_RT_MASK   0x1F8
#define CPU_OFF_SCRATCH0  0x158   /* host_scratch[0]: an exit's chain site (jit.c) */
/* Translated code (and AOT blobs, which freeze it) uses these offsets;
 * a layout change must update them and bump AOT_VERSION (jit.c). */
#define HAO_CPU_OFF(f, o) _Static_assert(__builtin_offsetof(hao_cpu, f) == (o), "cpu layout: " #f)
HAO_CPU_OFF(x, CPU_OFF_X(0));
HAO_CPU_OFF(pc, CPU_OFF_PC);
HAO_CPU_OFF(flags, CPU_OFF_FLAGS);
HAO_CPU_OFF(exit_reason, CPU_OFF_EXIT);
HAO_CPU_OFF(exit_insn, CPU_OFF_EXITI);
HAO_CPU_OFF(host_scratch, CPU_OFF_SCRATCH0);
HAO_CPU_OFF(rt_helper, CPU_OFF_RT_HELPER);
HAO_CPU_OFF(rt_exit, CPU_OFF_RT_EXIT);
HAO_CPU_OFF(rt_table, CPU_OFF_RT_TABLE);
HAO_CPU_OFF(fcmp_flags, CPU_OFF_FCMP);
HAO_CPU_OFF(rt_mask, CPU_OFF_RT_MASK);
HAO_CPU_OFF(v, CPU_OFF_V(0));
_Static_assert(__builtin_offsetof(hao_cpu, bridge_rsp) == 0x1D8, "cpu layout");
_Static_assert(__builtin_offsetof(hao_cpu, native_pop) == 0x1F0, "cpu layout");
#undef HAO_CPU_OFF

/* exit_reason values (also returned by the interpreter step). */
enum {
    HAO_EXIT_NONE = 0,
    HAO_EXIT_BRANCH = 1,     /* pc updated; continue dispatch */
    HAO_EXIT_SVC = 2,        /* pc = svc + 4; exit_insn = svc imm16 */
    HAO_EXIT_BRK = 3,        /* pc = brk; exit_insn = imm16 */
    HAO_EXIT_UNDEF = 4,      /* pc = insn; exit_insn = encoding */
    HAO_EXIT_UDF = 5,
    HAO_EXIT_HLT = 6,
};

static inline unsigned hao_nzcv(const hao_cpu *c)
{
    uint64_t f = c->flags;
    return (unsigned)(((f >> 7) & 1) << 3 | ((f >> 6) & 1) << 2 | ((~f) & 1) << 1 | ((f >> 11) & 1));
}

static inline void hao_set_nzcv(hao_cpu *c, unsigned nzcv)
{
    c->flags = HAO_RF_BASE | ((nzcv & 8) ? HAO_RF_SF : 0) | ((nzcv & 4) ? HAO_RF_ZF : 0) |
               ((nzcv & 2) ? 0 : HAO_RF_CF) | ((nzcv & 1) ? HAO_RF_OF : 0);
}

/* Evaluate an ARM condition (0..15) against NZCV. */
static inline bool hao_cond(unsigned nzcv, unsigned cond)
{
    bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1, r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return true;  /* AL, NV */
    }
    return (cond & 1) ? !r : r;
}

/* Interpreter: execute exactly one instruction at cpu->pc.
 * Returns HAO_EXIT_NONE/BRANCH on normal completion (pc updated) or an
 * exit reason that the runtime must service. */
int a64_step(hao_cpu *cpu);
/* Execute `insn` as if fetched from `pc` (used by the JIT for helpers). */
int a64_exec(hao_cpu *cpu, uint32_t insn, uint64_t pc);
/* Internal translated-helper ABI: tag zero is the original generic instruction.
 * Layout is unchanged; emitter and dispatcher changes bind to the libhao UUID.
 * Unknown tags and tag/word mismatches are UNDEF, retaining the original word. */
#define HAO_HELPER_TAG_VECTOR_FP 1u
bool a64_vector_fp_eligible(uint32_t insn);
uint64_t a64_helper_descriptor(uint32_t insn);
int a64_exec_helper(hao_cpu *cpu, uint64_t descriptor, uint64_t pc);
/* Checked subdecoder used by a64_exec_helper; returns 0/-1, does not set pc. */
int a64_exec_vector_fp(hao_cpu *cpu, uint32_t insn);
/* Decode rules shared by the interpreter and the JIT (a64_interp.c). */
bool a64_branch_reg_valid(uint32_t insn);
bool a64_pac_valid(uint32_t insn);
/* XPACI/XPACLRI (data = false) and XPACD (data = true) as Apple silicon EL0 strips. */
uint64_t a64_xpac(uint64_t v, bool data);
/* AdvSIMDExpandImm: the 64-bit pattern of a modified-immediate encoding
 * (shared with the translator). */
uint64_t a64_simd_expand_imm(unsigned op, unsigned cmode, unsigned imm8);
/* SIMD/FP sub-interpreter; returns -1 if not an FP/SIMD encoding it knows. */
int a64_exec_simd(hao_cpu *cpu, uint32_t insn, uint64_t pc);

/*
 * Guest FP environment. The guest and the native libraries it calls share
 * one thread, so they share one FP environment, and its home is the host
 * MXCSR: FPCR.RMode is MXCSR.RC, FPCR.FZ is MXCSR.FTZ|DAZ, and FPSR's
 * cumulative IOC/DZC/OFC/UFC/IXC are MXCSR's IE/ZE/OE/UE/PE. Translated SSE
 * code, the interpreter's C arithmetic and a native fesetround() therefore
 * all see the same rounding mode. Native long double (x87) code sees it too:
 * FPCR writes copy RMode to the x87 control word, FPSR reads include the x87
 * exception flags and FPSR writes clear them. The guest's <fenv.h> calls go
 * to traps that work on these registers with the arm64 FE_* values and
 * fenv_t (libm.c). cpu->fpcr keeps the other FPCR bits (DN,
 * AHP, FZ16, the trap enables, ...; only DN is modelled) and cpu->fpsr keeps
 * QC and IDC. MSR/MRS FPCR and FPSR go through these functions.
 */
uint32_t a64_fpcr_read(hao_cpu *cpu);
void a64_fpcr_write(hao_cpu *cpu, uint32_t v);
uint32_t a64_fpsr_read(hao_cpu *cpu);
void a64_fpsr_write(hao_cpu *cpu, uint32_t v);

/* Guest data addresses: TBI0 is set for data on Darwin arm64. */
static inline uint64_t hao_tbi(uint64_t a) { return a & 0x00FFFFFFFFFFFFFFull; }

/* System-register hooks implemented by the runtime. */
uint64_t hao_sysreg_read(hao_cpu *cpu, unsigned enc, bool *ok);
bool hao_sysreg_write(hao_cpu *cpu, unsigned enc, uint64_t v);

#endif
