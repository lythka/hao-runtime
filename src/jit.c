/*
 * hao — arm64 -> x86_64 block translator.
 *
 * Execution model
 *   rbx      = hao_cpu * (guest registers live in memory: x[n] at [rbx+8n])
 *   r12      = per-thread jump-cache base, fixed for one entry
 *   rax..r11 = scratch
 *   EFLAGS   = guest NZCV while `fl.in_eflags` (N=SF Z=ZF V=OF, C = NOT CF)
 *   [rbx+FLAGS] = canonical NZCV (the LAHF/SETO image of hao/cpu.h) when not dirty
 *
 * Every block is entered through hao_jit_enter (callee-saved registers
 * saved, rsp 16-byte aligned) and leaves through the exit stub with the
 * exit reason in eax. Direct branches are chained: their exit jmp is
 * patched to the successor block once it exists. Indirect branches and
 * returns look their target up in the thread's jump cache inline and, on a
 * miss, in the translation table from a shared stub.
 *
 * Instructions without a native translation call a64_exec (the oracle-
 * checked interpreter) as a helper on the in-memory state.
 */
#include "hao/runtime.h"
#include <errno.h>
#include <cpuid.h>
#include "hao/x86.h"
#include "hao/a64_cond.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hao/a64_imm.h"
#include "hao/aot.h"
#include "hao/gmap.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sys/acl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>

#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(x, n) (((x) >> (n)) & 1u)
static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

#define MAX_BLOCK_INSNS 96
#define MAX_BLOCK_BYTES (256u << 10)  /* includes 96 inline FP16 fused operations */
#define CODE_MB_DEFAULT 256           /* translated-code space; HAO_JIT_CACHE_MB (1..1024) overrides */
#define THUNK_AREA ((size_t)THUNK_SLOTS * 32)   /* 32-byte plain thunks */
#define SIG_SLOT 64
#define SIG_AREA (4u << 20)           /* 64-byte signature thunks: 65536 */
#define SCRATCH_SIZE MAX_BLOCK_BYTES /* hao_jit_exec_code (oracle) */
#define TABLE_MIN_BITS 12              /* first translation table: 4096 slots, 64 KB */
#define TABLE_BITS 22                  /* largest: large per-image AOT maps retain all entries */
#define TABLE_SIZE (1u << TABLE_BITS)
#define TABLE_MAX (TABLE_SIZE / 8 * 7) /* load factor limit of the largest table */
#define THUNK_SLOTS (1u << 18)        /* plain thunks (32 bytes each, own area) and their hash table */

typedef struct { uint64_t pc; void *code; } tentry;

/* The code cache is one mapping, laid out once by jit_init_once:
 *   stubs | thunk area | signature-thunk area | oracle scratch | code
 * Only cache_alloc and block_commit move cache_ptr, under jit_lock. */
static uint8_t *cache, *cache_ptr, *cache_end;
static unsigned cache_code_mb;
static uint8_t *thunk_area, *thunk_ptr, *thunk_end;        /* native-callable thunks (below) */
static uint8_t *sig_area, *sig_ptr, *sig_end;
static uint8_t *scratch;
/* The translation table, guest pc -> code. It starts at TABLE_MIN_BITS and
 * grows by replacement under jit_lock (a copy twice the size, published
 * with a release store). An entry, once published, keeps its pc for good
 * and always names valid code for it (the cache is never flushed; an AOT
 * blob may replace the code of the current table's entry with its own
 * block for the same pc). Old tables are never unmapped or written again.
 * The jump caches of translated code rely on both: their slots point at
 * entries of whichever table was current when they were filled. */
typedef struct jtab {
    struct jtab *prev;
    uint32_t bits, mask, used, limit;
    tentry e[];
} jtab;
static jtab *table;                   /* current table; atomic */
static uint32_t table_used;           /* under jit_lock (the current table's used, for messages) */
static uint64_t st_table_bytes;
static int cache_full;                /* atomic: no new blocks; misses run in the interpreter */
static void *exit_stub;               /* restores host state, returns eax */
static void *probe_stub;              /* jump-cache miss: probes the translation table (build_stubs) */
/* What an empty jump-cache slot points at: no branch target equals its pc
 * except a wild branch there, which goes to the probe stub like any miss. */
static tentry jc_none;
/* Counts the entries added to the translation table (insert): a jump
 * cache's record that a target has no translation holds until the next
 * one. Never 0, which is what an unused record holds. */
static uint64_t table_epoch = 1;
static int (*jit_enter)(hao_cpu *, void *code);
static uint64_t st_blocks, st_insns, st_native, st_helper, st_links, st_bytes, st_aot_blocks;
static uint64_t st_helper_calls;      /* dynamic calls, only with --stats */
/* Fixed decoder-class counters: no allocation or locks on a helper path.
 * Every update is relaxed and lock-free on the required x86-64 target. */
enum { HG_PERMUTE, HG_ADDV, HG_PAIR_LONG, HG_FP_PAIR, HG_FP_REDUCE,
       HG_FDIV, HG_FP_ZERO, HG_REG_SHIFT, HG_INDEXED, HG_SCALAR_INDEXED,
       HG_FP_VECTOR, HG_INT_VECTOR, HG_MISC, HG_ACROSS, HG_SHIFT,
       HG_COPY, HG_THREE_DIFF, HG_SCALAR, HG_CRYPTO, HG_LDST,
       HG_DP_IMM, HG_DP_REG, HG_SYSTEM, HG_OTHER, HG_COUNT };
static const char *const helper_group_names[HG_COUNT] = {
    "permute", "addv", "pair-long", "fp-pair", "fp-reduce", "fdiv",
    "fp-zero", "register-shift", "indexed-vector", "indexed-scalar",
    "fp-vector", "integer-simd", "simd-misc", "simd-across", "shift-immediate",
    "simd-copy", "three-different", "fp-scalar", "crypto", "load-store",
    "dp-immediate", "dp-register", "branch-system", "other"
};
static struct { uint64_t calls; uint32_t example; } helper_groups[HG_COUNT][2];
static unsigned helper_group(uint32_t i)
{
    if ((i & 0xBF208C00) == 0x0E000800) return HG_PERMUTE;
    if ((i & 0x9F3E0C00) == 0x0E300800) {
        unsigned op = BITS(i, 16, 12);
        return op == 0x1b ? HG_ADDV : op == 0x0c || op == 0x0f ? HG_FP_REDUCE : HG_ACROSS;
    }
    if ((i & 0xDF3E0C00) == 0x5E300800) return BIT(i, 29) ? HG_FP_PAIR : HG_INT_VECTOR;
    if ((i & 0x9F3E0C00) == 0x0E200800) {
        unsigned op = BITS(i, 16, 12);
        return op == 2 || op == 6 ? HG_PAIR_LONG : op >= 0xc && op <= 0xe ? HG_FP_ZERO : HG_MISC;
    }
    if ((i & 0x9F200400) == 0x0E200400) {
        unsigned op = BITS(i, 15, 11);
        if (op == 8) return HG_REG_SHIFT;
        if (op == 0x1f && BIT(i, 29)) return HG_FDIV;
        if (op == 0x1a && BIT(i, 29) && !BIT(i, 23)) return HG_FP_PAIR;
        return op >= 0x18 ? HG_FP_VECTOR : HG_INT_VECTOR;
    }
    if ((i & 0x9F000400) == 0x0F000000) return HG_INDEXED;
    if ((i & 0xDF000400) == 0x5F000000) return HG_SCALAR_INDEXED;
    if ((i & 0x9F800400) == 0x0F000400) return HG_SHIFT;
    if ((i & 0x9FE08400) == 0x0E000400) return HG_COPY;
    if ((i & 0x9F200C00) == 0x0E200000) return HG_THREE_DIFF;
    if ((i & 0xFF000000) == 0xCE000000 || (i & 0xFF3E0C00) == 0x4E280800) return HG_CRYPTO;
    if ((i & 0x5F000000) == 0x1E000000 || (i & 0x5F000000) == 0x1F000000) return HG_SCALAR;
    switch (BITS(i, 28, 25)) {
    case 8: case 9: return HG_DP_IMM;
    case 10: case 11: return HG_SYSTEM;
    case 4: case 6: case 12: case 14: return HG_LDST;
    case 5: case 13: return HG_DP_REG;
    default: return HG_OTHER;
    }
}

static uint64_t st_sites, st_sites_unaligned, st_link_skips;   /* chainable exits emitted / not 4-aligned; links refused */
static uint64_t st_fused;             /* instructions folded into the one before them (constant chains) */
static uint64_t st_fl_exits, st_fl_unsaved;   /* direct exits reached with NZCV in EFLAGS / of those, left unsaved */
static int jit_disabled_native;       /* HAO_JIT_HELPERS_ONLY=1: every insn via helper */

/* Optional translation features are immutable for the lifetime of a process,
 * including fault recovery and AOT re-emission. AES is outside x86-64-v2. */
static bool jit_aes;
static pthread_once_t jit_cpu_once = PTHREAD_ONCE_INIT;
static void jit_cpu_init(void)
{
    unsigned a, b, c, d;
    jit_aes = __get_cpuid(1, &a, &b, &c, &d) && (c & bit_AES);
}

/* ------------------------------------------------------------ tables */

/* The home slot, as exit_indirect's inline probe computes it: x ^ x >> 10 ^
 * x >> 20 of x = pc >> 2, so code aligned to pages (functions spread over
 * many images) still spreads over a small table. */
static inline uint32_t thash(uint64_t pc, uint32_t mask)
{
    uint64_t x = pc >> 2;
    return (uint32_t)(x ^ (x >> 10) ^ (x >> 20)) & mask;
}

/* The current table's entry for pc, or NULL */
static const tentry *lookup_entry(uint64_t pc)
{
    const jtab *t = __atomic_load_n(&table, __ATOMIC_ACQUIRE);
    if (!t) return NULL;
    uint32_t h = thash(pc, t->mask);
    for (uint32_t k = 0; k <= t->mask; k++) {
        const tentry *e = &t->e[(h + k) & t->mask];
        uint64_t p = __atomic_load_n(&e->pc, __ATOMIC_ACQUIRE);
        if (p == pc) return e;
        if (!p) return NULL;
    }
    return NULL;
}

static void *lookup(uint64_t pc)
{
    const tentry *e = lookup_entry(pc);
    return e ? __atomic_load_n(&e->code, __ATOMIC_ACQUIRE) : NULL;
}

static jtab *jtab_new(unsigned bits)
{
    size_t n = sizeof(jtab) + ((size_t)1 << bits) * sizeof(tentry);
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    jtab *t = p;
    t->bits = bits;
    t->mask = (1u << bits) - 1;
    t->limit = bits < TABLE_BITS ? (1u << bits) / 4 * 3 : TABLE_MAX;
    st_table_bytes += n;
    return t;
}

static tentry *jtab_find(jtab *t, uint64_t pc)
{
    for (uint32_t h = thash(pc, t->mask);; h = (h + 1) & t->mask)
        if (!t->e[h].pc || t->e[h].pc == pc) return &t->e[h];
}

/* jit_lock held: room for `n` more entries (growing the table), or false */
static bool table_room(uint32_t n)
{
    jtab *t = table;
    if (!t) return false;
    if (table_used + n <= t->limit) return true;
    if (table_used + n > TABLE_MAX) return false;
    unsigned bits = t->bits + 1;
    while (bits < TABLE_BITS && table_used + n > (1u << bits) / 4 * 3) bits++;
    jtab *g = jtab_new(bits);
    if (!g) return false;
    for (uint32_t k = 0; k <= t->mask; k++)
        if (t->e[k].pc) { *jtab_find(g, t->e[k].pc) = t->e[k]; g->used++; }
    g->prev = t;
    __atomic_store_n(&table, g, __ATOMIC_RELEASE);
    return true;
}

/* jit_lock held. False when the table is at its load-factor limit. An
 * existing pc gets the new code (an AOT blob loaded after the JIT
 * translated some of its blocks); the old code stays valid. */
static bool insert(uint64_t pc, void *code)
{
    tentry *e = table ? jtab_find(table, pc) : NULL;
    if (!e) return false;
    if (e->pc == pc) { __atomic_store_n(&e->code, code, __ATOMIC_RELEASE); return true; }
    if (!table_room(1)) return false;
    e = jtab_find(table, pc);       /* the table may have grown */
    table->used++;
    __atomic_store_n(&table_used, table->used, __ATOMIC_RELAXED);
    e->code = code;
    __atomic_store_n(&e->pc, pc, __ATOMIC_RELEASE);
    __atomic_store_n(&table_epoch, table_epoch + 1, __ATOMIC_RELEASE);   /* after the entry: a miss recorded now is rechecked */
    return true;
}

/* Fork. hao's mutexes are jit_lock and thunk_lock here, sig_lock (abi.c)
 * and invoke_lock (blocks.c). hao takes none of them in a fork prepare
 * handler: pthread_atfork prepare handlers run before libSystem's own
 * prepare, which takes the dyld, objc and malloc locks, so a forking
 * thread holding jit_lock would wait for dyld's lock while a thread that
 * runs new guest code under it (a dyld add-image callback, a guest -retain
 * under an objc lock) waits for jit_lock: the parent would hang in fork.
 * So fork may stop another thread anywhere inside a hao critical section,
 * and the child handler (runtime.c) only re-initialises the mutexes. Every
 * section therefore writes its entry first and publishes it with a final
 * release store; a cut-off section leaves at most an unused cache, thunk
 * or signature slot. A new hao mutex must be added to its file's
 * *_fork_child hook.
 *
 * Signals. Each of these mutexes is taken inside hao_crit_enter/leave
 * (runtime.c): a guest signal arriving inside is deferred and re-raised
 * when the outermost section ends (signals.c), so a guest handler never
 * runs on a thread that holds one (it would wait on itself). Under
 * jit_lock nothing else is locked, no guest code runs and no I/O is done.
 *
 * jit_lock owns cache_ptr, table[] inserts and table_used, im->jit_slot,
 * im->aot and the translation statistics. */
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------- code cache
 * All of these need jit_lock (and so hao_crit). */

/* n bytes, 16-aligned, or NULL if the code space cannot hold them */
static void *cache_alloc(size_t n)
{
    if (hao_crit_depth() <= 0) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "cache_alloc outside a hao critical section\n"); abort(); }
    uint8_t *p = (uint8_t *)(((uintptr_t)cache_ptr + 15) & ~(uintptr_t)15);
    if (p > cache_end || (size_t)(cache_end - p) < n) return NULL;
    cache_ptr = p + ((n + 15) & ~(size_t)15);
    if (cache_ptr > cache_end) cache_ptr = cache_end;
    return p;
}

/* Where the next block goes, or NULL if fewer than MAX_BLOCK_BYTES remain.
 * The block may use all the remaining space; block_commit claims it. */
static uint8_t *block_open(size_t *avail)
{
    uint8_t *p = (uint8_t *)(((uintptr_t)cache_ptr + 15) & ~(uintptr_t)15);
    if (p > cache_end || (size_t)(cache_end - p) < MAX_BLOCK_BYTES) return NULL;
    *avail = (size_t)(cache_end - p);
    return p;
}

static void block_commit(uint8_t *start, size_t len)
{
    cache_ptr = start + ((len + 15) & ~(size_t)15);
    if (cache_ptr > cache_end) cache_ptr = cache_end;
}

/* Warnings are noted under the lock and printed after it (no I/O inside). */
enum { WARN_FULL = 1, WARN_OVERFLOW = 2, WARN_SIGS = 4, WARN_RMAP = 8 };
static int warn_pending, warn_done;

static void cache_exhausted(void)
{
    __atomic_store_n(&cache_full, 1, __ATOMIC_RELEASE);
    __atomic_fetch_or(&warn_pending, WARN_FULL, __ATOMIC_RELAXED);
}

static void jit_warn(void)
{
    int w = __atomic_exchange_n(&warn_pending, 0, __ATOMIC_ACQ_REL);
    w &= ~__atomic_fetch_or(&warn_done, w, __ATOMIC_ACQ_REL);
    FILE *f = HAO_DIAG;
    if (w & WARN_FULL) {
        hao_diag_prefix(f);
        fprintf(f, "code cache full (%u MB, %u blocks): new blocks run in the interpreter\n", cache_code_mb,
                __atomic_load_n(&table_used, __ATOMIC_RELAXED));
    }
    if (w & WARN_OVERFLOW) { hao_diag_prefix(f); fprintf(f, "a block's translation overflowed: it runs in the interpreter\n"); }
    if (w & WARN_SIGS) { hao_diag_prefix(f); fprintf(f, "signature thunk area full: typed callbacks get no new signature thunks (signatures go to the callback path)\n"); }
    if (w & WARN_RMAP) { hao_diag_prefix(f); fprintf(f, "translated-block map incomplete: a signal in a later block reports its block's start, not the exact instruction\n"); }
}

/* Blocks by code address, for hao_jit_locate. The code cache is carved
 * upward only, so appending keeps the map sorted; writers hold jit_lock,
 * readers (signal handlers) take none. Every block starts 16-aligned, so
 * one entry per 16 bytes of cache (reserved, touched as used) never fills. */
typedef struct { uint64_t code, pc; } rment;
static rment *rmap;
static uint32_t rmap_n, rmap_cap;

static void rmap_add(const void *code, uint64_t pc)
{
    uint32_t n = rmap_n;
    if (!rmap || n >= rmap_cap || (n && rmap[n - 1].code >= (uint64_t)(uintptr_t)code)) {
        __atomic_fetch_or(&warn_pending, WARN_RMAP, __ATOMIC_RELAXED);
        return;
    }
    rmap[n].code = (uint64_t)(uintptr_t)code;
    rmap[n].pc = pc;
    __atomic_store_n(&rmap_n, n + 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------- translation */

typedef struct {
    xbuf b;
    uint64_t pc;
    uint64_t base;          /* guest image base the code is relative to */
    uint8_t *base_slot;     /* where the runtime stores `base` (rip-relative) */
    bool in_eflags, dirty;
    /* xmm5-7 hold t_vfp32's constants: set by a single-precision vector FP
     * translation (t_vfp32, t_vfp_rint), kept for the next instruction
     * only (vfp_prev while it is emitted) */
    bool vfp_consts, vfp_prev;
    bool pic;               /* AOT: every guest address must be base-relative */
    bool not_pic;           /* ... and one was not (the blob is not written) */
    bool live_out[MAX_BLOCK_INSNS + 1];
    int idx;
    /* the block being emitted, for translators that look at the next
     * instructions; `skip` of them were folded into the current one */
    const uint32_t *insns;
    int n, skip;
    /* where instructions beyond the block are read from, for the walk that
     * decides whether an exit must save NZCV (flags_dead_at): the block's
     * image, or the whole code of hao_jit_exec_code */
    hao_image *im;
    const uint32_t *win;
    uint64_t win_base;
    int win_n;
    /* AOT: direct-branch exits to resolve after all blocks are emitted */
    struct aot_link { uint32_t site; uint64_t target; } *links;
    int nlinks, caplinks;
    /* hao_jit_locate: re-emit without side effects, recording where each
     * guest instruction starts and whether NZCV lives in host RFLAGS there */
    bool dry;
    uint32_t *rec_off;
    bool *rec_fl;
    int nrec;
} tctx;
#define ST(x) do { if (!t->dry) x++; } while (0)

#define XR(n) xm(RBX, CPU_OFF_X(n))
#define FLAGS xm(RBX, CPU_OFF_FLAGS)

/* Load a guest address. Position independent when the target is within
 * +-2GB of the image base: mov reg,[rip+slot]; lea reg,[reg+off]. Else an
 * absolute immediate, which is right only for code that stays in this
 * process (the JIT, code outside any image); an AOT blob must not contain
 * one, since loading it patches only the base slot. */
static void gaddr(tctx *t, int reg, uint64_t addr)
{
    int64_t off = (int64_t)(addr - t->base);
    if (t->base_slot && off >= INT32_MIN && off <= INT32_MAX) {
        x_mov_ld_rip(&t->b, 8, reg, t->base_slot);
        if (off) x_lea(&t->b, 1, reg, xm(reg, (int32_t)off));
        return;
    }
    if (t->pic) t->not_pic = true;
    x_mov_ri(&t->b, reg, addr);
}

/* Flags bookkeeping. Saving and restoring NZCV go through AX (LAHF/SAHF
 * and SETO; see hao/cpu.h for the image): fl_materialize, fl_clobber and
 * fl_load may clobber rax, so a translator calls them before it loads
 * anything into rax. EFLAGS is left as it was by a save. */
static void fl_materialize(tctx *t)
{
    if (t->in_eflags && t->dirty) {
        x_setcc(&t->b, XCC_O, RAX);
        x_lahf(&t->b);
        x_mov_st(&t->b, 2, FLAGS, RAX);
        t->dirty = false;
    }
}
/* EFLAGS = the NZCV of a flags image in eax (rax scratch) */
static void fl_from_eax(tctx *t)
{
    x_add_al_i8(&t->b, 0x7f);                /* OF = (al == 1) */
    x_sahf(&t->b);
}
/* about to clobber EFLAGS inside an instruction that does not define flags */
static void fl_clobber(tctx *t)
{
    if (t->in_eflags && t->dirty && t->live_out[t->idx]) fl_materialize(t);
    t->in_eflags = false;
    t->dirty = false;
}
/* instruction defines flags via x86 EFLAGS now */
static void fl_defined(tctx *t) { t->in_eflags = true; t->dirty = true; }
/* need guest flags in EFLAGS */
static void fl_load(tctx *t)
{
    if (!t->in_eflags) {
        x_mov_ld(&t->b, 2, RAX, FLAGS);
        fl_from_eax(t);
        t->in_eflags = true;
        t->dirty = false;
    }
}

/* guest register access (ZR reads as 0; SP when sp_ok) */
static void ldx(tctx *t, int host, unsigned r, int sf, bool sp_ok)
{
    if (r == 31 && !sp_ok) { x_mov_ri(&t->b, host, 0); return; }
    x_mov_ld(&t->b, sf ? 8 : 4, host, XR(r));
}
static void stx(tctx *t, unsigned r, int host, bool sp_ok)
{
    if (r == 31 && !sp_ok) return;
    x_mov_st(&t->b, 8, XR(r), host);
}

static void exit_with(tctx *t, int reason)
{
    x_mov_ri(&t->b, RAX, (uint64_t)reason);
    x_jmp_m(&t->b, xm(RBX, CPU_OFF_RT_EXIT));
}

static bool exit_live(const tctx *t, uint64_t target);

/* direct branch exit: patchable jmp to a link stub. NZCV still in EFLAGS
 * is saved unless the code at the target replaces it before reading it. */
static void exit_direct(tctx *t, uint64_t target)
{
    if (t->in_eflags && t->dirty) {
        ST(st_fl_exits);
        if (exit_live(t, target)) fl_materialize(t);
        else ST(st_fl_unsaved);
    }
    /* layout: jmp rel32 -> stub ; stub: mov rax,target; mov [pc],rax; mov eax,BRANCH; jmp exit
     * hao_jit_run_block rewrites the rel32 while other threads may be
     * executing this jmp (cross-modifying code). x86 fetches it whole only
     * if the store does not cross an 8-byte or cache-line boundary, so the
     * field is 4-aligned: every block starts 16-aligned (JIT, AOT blob and
     * its load address, oracle scratch), so an offset in the buffer has
     * the alignment of the final address. */
    x_nopn(&t->b, (int)((3 - xb_len(&t->b)) & 3));
    size_t j = x_jmp32(&t->b);
    st_sites++;
    if (j & 3) st_sites_unaligned++;
    size_t stub = xb_len(&t->b);
    x_patch32(&t->b, j, stub);
    if (t->links) {
        if (t->nlinks == t->caplinks) {
            t->caplinks = t->caplinks ? 2 * t->caplinks : 1024;
            t->links = hao_alloc_check(realloc(t->links, (size_t)t->caplinks * sizeof *t->links), "AOT branch links");
        }
        t->links[t->nlinks].site = (uint32_t)j;
        t->links[t->nlinks].target = target;
        t->nlinks++;
    }
    gaddr(t, RAX, target);
    x_mov_st(&t->b, 8, xm(RBX, CPU_OFF_PC), RAX);
    /* record the patch site (address of rel32) for the block linker */
    x_lea_rip(&t->b, RCX, t->b.start + j);
    x_mov_st(&t->b, 8, xm(RBX, CPU_OFF_SCRATCH0), RCX);
    exit_with(t, HAO_EXIT_BRANCH);
}

/* indirect exit: target in RAX, NZCV already in memory (fl_materialize
 * before the target was loaded) */
static void exit_indirect(tctx *t)
{
    /*
     * The jump cache. Each thread has HAO_JIT_JC_SLOTS slots, indexed by
     * the low bits of the target's instruction index; a slot holds the
     * address of a translation-table entry {pc, code} or of jc_none. The
     * slot is read with one load and an entry never changes its pc, so the
     * compare and the jump agree even if a signal handler that runs guest
     * code on this thread refills the slot between them; a refill is one
     * store. Nothing here writes translated code.
     *   rdx = (pc & (SLOTS - 1) << 2), twice the slot's offset / 4: the
     *   probe stub stores to the same slot.
     */
    x_mov_st(&t->b, 8, xm(RBX, CPU_OFF_PC), RAX);
    x_mov_rr(&t->b, 0, RDX, RAX);
    x_alu_ri(&t->b, XAND, 0, RDX, (int32_t)((HAO_JIT_JC_SLOTS - 1) << 2));
    x_mov_ld(&t->b, 8, RCX, xmi(R12, RDX, 2, 0));
    x_alu_rm(&t->b, XCMP, 1, RAX, xm(RCX, 0));
    size_t miss = x_jcc8(&t->b, XCC_NE);
    x_jmp_m(&t->b, xm(RCX, 8));
    x_patch8(&t->b, miss, xb_len(&t->b));
    x_jmp_m(&t->b, xm(RBX, CPU_OFF_RT_PROBE));
}

static void helper(tctx *t, uint32_t insn, uint64_t pc, bool declined)
{
    fl_materialize(t);
    t->in_eflags = false;
    x_mov_rr(&t->b, 1, RDI, RBX);
    x_mov_ri(&t->b, RSI, a64_helper_descriptor(insn));
    gaddr(t, RDX, pc);
    x_mov_ri(&t->b, RCX, declined);  /* fourth ABI argument; descriptor unchanged */
    x_call_m(&t->b, xm(RBX, CPU_OFF_RT_HELPER));
    x_test_rr(&t->b, 0, RAX, RAX);
    size_t ok = x_jcc8(&t->b, XCC_E);
    /* non-zero: interpreter wants out (undef/brk/...), pc already set */
    x_jmp_m(&t->b, xm(RBX, CPU_OFF_RT_EXIT));
    x_patch8(&t->b, ok, xb_len(&t->b));
    ST(st_helper);
}

/* ---- flag effect classification for liveness */
enum { FL_NONE = 0, FL_READ = 1, FL_WRITE = 2 };

static int flag_effect(uint32_t i)
{
    unsigned op0 = BITS(i, 28, 25);
    if (op0 == 0x8 || op0 == 0x9) {  /* dp imm */
        unsigned g = BITS(i, 25, 23);
        if (g == 2 && BIT(i, 29)) return FL_WRITE;               /* ADDS/SUBS imm */
        if (g == 4 && BITS(i, 30, 29) == 3) return FL_WRITE;     /* ANDS imm */
        return FL_NONE;
    }
    if (op0 == 0x5 || op0 == 0xD) {  /* dp reg */
        if ((i & 0x1F000000) == 0x0A000000) return BITS(i, 30, 29) == 3 ? FL_WRITE : FL_NONE;
        if ((i & 0x1F000000) == 0x0B000000) return BIT(i, 29) ? FL_WRITE : FL_NONE;
        if ((i & 0x1FE00000) == 0x1A000000) return FL_READ | (BIT(i, 29) ? FL_WRITE : 0);  /* ADC/SBC */
        if ((i & 0x1FE00000) == 0x1A400000) return FL_READ | FL_WRITE;                     /* CCMP */
        if ((i & 0x1FE00000) == 0x1A800000) return FL_READ;                                /* CSEL */
        return FL_NONE;
    }
    if ((i & 0xFF000000) == 0x54000000) return FL_READ;                                   /* B.cond */
    if ((i & 0xFFC00000) == 0xD5000000) return FL_READ | FL_WRITE;                        /* MSR/MRS etc: conservative */
    if (op0 == 0x7 || op0 == 0xF) {
        if ((i & 0x5F203C00) == 0x1E202000) return FL_WRITE;                              /* FCMP */
        if ((i & 0x5F200C00) == 0x1E200400) return FL_READ | FL_WRITE;                    /* FCCMP */
        if ((i & 0x5F200C00) == 0x1E200C00) return FL_READ;                               /* FCSEL */
        if ((i & 0x7FFFFC00) == 0x1E7E0000) return FL_WRITE;                              /* FJCVTZS */
    }
    return FL_NONE;
}

static bool is_block_end(uint32_t i)
{
    if ((i & 0x7C000000) == 0x14000000) return true;   /* B/BL */
    if ((i & 0x7E000000) == 0x34000000) return true;   /* CBZ/CBNZ */
    if ((i & 0x7E000000) == 0x36000000) return true;   /* TBZ/TBNZ */
    if ((i & 0xFF000000) == 0x54000000) return true;   /* B.cond */
    if ((i & 0xFE000000) == 0xD6000000) return true;   /* BR/BLR/RET */
    if ((i & 0xFF000000) == 0xD4000000) return true;   /* SVC/BRK/HLT */
    if (i == 0) return true;
    return false;
}

/* Whether a lies in one of im's code ranges: hao_image_for_code(a) == im
 * (images do not overlap) without the walk of every image or the store to
 * the lookup's process-wide hint, for the instruction-by-instruction reads
 * of one image below. A released image has no ranges. */
static bool im_has_code(const hao_image *im, uint64_t a)
{
    int n = __atomic_load_n(&im->ncode, __ATOMIC_ACQUIRE);
    for (int k = 0; k < n; k++) if (a >= im->code_lo[k] && a < im->code_hi[k]) return true;
    return false;
}

/* The guest instruction at pc, if a block of this one's image (or of the
 * code hao_jit_exec_code runs) can continue there. */
static bool peek_insn(const tctx *t, uint64_t pc, uint32_t *insn)
{
    if (pc & 3) return false;
    if (t->win) {
        if (pc < t->win_base || pc >= t->win_base + 4u * (unsigned)t->win_n) return false;
        *insn = t->win[(pc - t->win_base) / 4];
        return true;
    }
    if (!t->im || !im_has_code(t->im, pc)) return false;
    memcpy(insn, hao_code_ptr_im(t->im, pc), 4);
    return true;
}

/*
 * True when the code at pc writes NZCV before it reads it on every path a
 * bounded walk can follow: straight-line code, B and BL to their target,
 * CBZ/CBNZ/TBZ/TBNZ both ways. Anything else ends the walk with "live": a
 * flag read, an indirect branch or return, a system instruction, a trap,
 * the edge of the image, the budget.
 *
 * An exit to such a target leaves NZCV unsaved. The answer depends only on
 * the image's code, so hao_jit_locate's re-emission and an AOT cache agree
 * with the first translation. What it gives up: the stale cpu.flags is what
 * a signal context shows for an interruption in the target before its
 * first flag write (a fault in a load there, say), where the processor
 * would show the old flags; guest code cannot read them, only a handler
 * that inspects the context can. The same holds inside a block, where
 * flags no later instruction reads are not saved before x86 clobbers them.
 */
static bool flags_dead_at(const tctx *t, uint64_t pc, int depth, int *budget)
{
    for (;;) {
        uint32_t i;
        if (--*budget < 0 || !peek_insn(t, pc, &i)) return false;
        int e = flag_effect(i);
        if (e & FL_READ) return false;
        if (e & FL_WRITE) return true;
        if ((i & 0x7C000000) == 0x14000000) { pc += (uint64_t)(sext(BITS(i, 25, 0), 26) * 4); continue; }
        if ((i & 0x7E000000) == 0x34000000 || (i & 0x7E000000) == 0x36000000) {
            uint64_t tg = pc + (uint64_t)(((i & 0x7E000000) == 0x34000000 ? sext(BITS(i, 23, 5), 19) : sext(BITS(i, 18, 5), 14)) * 4);
            if (depth >= 2 || !flags_dead_at(t, tg, depth + 1, budget)) return false;
        } else if (is_block_end(i))
            return false;
        pc += 4;
    }
}
static bool exit_live(const tctx *t, uint64_t target)
{
    int budget = 48;
    return !flags_dead_at(t, target, 0, &budget);
}

/* Whether NZCV is live after the last instruction of a block: at any of
 * its direct exits, or always when it leaves another way. */
static bool block_end_live(const tctx *t, uint64_t pc, const uint32_t *insns, int n)
{
    uint32_t i = insns[n - 1];
    uint64_t last = pc + 4u * (unsigned)(n - 1);
    if ((i & 0x7C000000) == 0x14000000) return exit_live(t, last + (uint64_t)(sext(BITS(i, 25, 0), 26) * 4));
    if ((i & 0x7E000000) == 0x34000000 || (i & 0xFF000000) == 0x54000000)
        return exit_live(t, last + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4)) || exit_live(t, last + 4);
    if ((i & 0x7E000000) == 0x36000000)
        return exit_live(t, last + (uint64_t)(sext(BITS(i, 18, 5), 14) * 4)) || exit_live(t, last + 4);
    if (!is_block_end(i)) return exit_live(t, last + 4);
    return true;
}

/* ---- native translators: return true if handled */

static bool t_addsub_imm(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31), op = BIT(i, 30), S = BIT(i, 29);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int32_t imm = (int32_t)BITS(i, 21, 10) << (BIT(i, 22) ? 12 : 0);
    if (BITS(i, 25, 23) != 2) return false;
    ldx(t, RAX, rn, sf, true);
    if (!S) {
        if (imm) x_lea(&t->b, sf, RAX, xm(RAX, op ? -imm : imm));
        else if (!sf) x_mov_rr(&t->b, 0, RAX, RAX);
        stx(t, rd, RAX, true);
        return true;
    }
    x_alu_ri(&t->b, op ? XSUB : XADD, sf, RAX, imm);
    if (!op) x_cmc(&t->b);
    fl_defined(t);
    stx(t, rd, RAX, false);
    return true;
}

static void shift_host(tctx *t, int host, int sf, unsigned type, unsigned amt)
{
    static const int ops[4] = {XSHL, XSHR, XSAR, XROR};
    if (amt) x_shift_ri(&t->b, ops[type], sf, host, (uint8_t)amt);
}

static bool t_addsub_shift(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31), op = BIT(i, 30), S = BIT(i, 29);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), type = BITS(i, 23, 22), amt = BITS(i, 15, 10);
    if (type == 3 || (!sf && amt >= 32)) return false;
    if (!S && !op && (amt == 0 || (type == 0 && amt <= 3))) {      /* flag-free ADD via lea */
        ldx(t, RAX, rn, sf, false);
        ldx(t, RCX, rm, sf, false);
        x_lea(&t->b, sf, RAX, xmi(RAX, RCX, 1 << amt, 0));
        stx(t, rd, RAX, false);
        return true;
    }
    if (!S) fl_clobber(t);
    ldx(t, RAX, rn, sf, false);
    ldx(t, RCX, rm, sf, false);
    shift_host(t, RCX, sf, type, amt);
    x_alu_rr(&t->b, op ? XSUB : XADD, sf, RAX, RCX);
    if (S) { if (!op) x_cmc(&t->b); fl_defined(t); }
    stx(t, rd, RAX, false);
    return true;
}

static bool t_addsub_ext(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31), op = BIT(i, 30), S = BIT(i, 29);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), opt = BITS(i, 15, 13), sh = BITS(i, 12, 10);
    if (sh > 4 || BITS(i, 23, 22)) return false;
    bool lea = !S && !op && sh <= 3;
    if (!S && !lea) fl_clobber(t);
    if (rm == 31) x_mov_ri(&t->b, RCX, 0);
    else switch (opt) {
    case 0: x_mov_ld(&t->b, 1, RCX, XR(rm)); break;
    case 1: x_mov_ld(&t->b, 2, RCX, XR(rm)); break;
    case 2: x_mov_ld(&t->b, 4, RCX, XR(rm)); break;
    case 3: case 7: x_mov_ld(&t->b, 8, RCX, XR(rm)); break;
    case 4: x_movs_ld(&t->b, 1, 1, RCX, XR(rm)); break;
    case 5: x_movs_ld(&t->b, 2, 1, RCX, XR(rm)); break;
    default: x_movs_ld(&t->b, 4, 1, RCX, XR(rm)); break;
    }
    ldx(t, RAX, rn, sf, true);
    if (lea) {
        x_lea(&t->b, sf, RAX, xmi(RAX, RCX, 1 << sh, 0));
        stx(t, rd, RAX, true);
        return true;
    }
    if (sh) x_shift_ri(&t->b, XSHL, 1, RCX, (uint8_t)sh);
    x_alu_rr(&t->b, op ? XSUB : XADD, sf, RAX, RCX);
    if (S) { if (!op) x_cmc(&t->b); fl_defined(t); stx(t, rd, RAX, false); }
    else stx(t, rd, RAX, true);
    return true;
}

static bool t_logical_imm(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned opc = BITS(i, 30, 29), rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), N = BIT(i, 22);
    uint64_t imm;
    if (!sf && N) return false;
    if (!a64_decode_bitmask_imm(sf, N, BITS(i, 21, 16), BITS(i, 15, 10), &imm)) return false;
    if (opc == 1 && rn == 31) {                                    /* MOV (bitmask imm) */
        x_mov_ri(&t->b, RAX, sf ? imm : (uint32_t)imm);
        stx(t, rd, RAX, true);
        return true;
    }
    if (opc != 3) fl_clobber(t);
    ldx(t, RAX, rn, sf, false);
    static const int ops[4] = {XAND, XOR_, XXOR, XAND};
    if (!sf || (int64_t)imm == (int32_t)imm) x_alu_ri(&t->b, ops[opc], sf, RAX, (int32_t)imm);
    else { x_mov_ri(&t->b, RCX, imm); x_alu_rr(&t->b, ops[opc], sf, RAX, RCX); }
    if (opc == 3) { x_stc(&t->b); fl_defined(t); stx(t, rd, RAX, false); }
    else stx(t, rd, RAX, true);
    return true;
}

static bool t_logical_shift(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned opc = BITS(i, 30, 29), N = BIT(i, 21), type = BITS(i, 23, 22), amt = BITS(i, 15, 10);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    if (!sf && amt >= 32) return false;
    if (opc == 1 && !N && rn == 31 && amt == 0) {                   /* MOV reg */
        ldx(t, RAX, rm, sf, false);
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc == 1 && N && rn == 31 && amt == 0) {                    /* MVN */
        ldx(t, RAX, rm, sf, false);
        x_unary(&t->b, XNOT, sf, RAX);
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc != 3) fl_clobber(t);
    else if (amt) fl_clobber(t);
    ldx(t, RCX, rm, sf, false);
    shift_host(t, RCX, sf, type, amt);
    if (N) x_unary(&t->b, XNOT, sf, RCX);
    ldx(t, RAX, rn, sf, false);
    static const int ops[4] = {XAND, XOR_, XXOR, XAND};
    x_alu_rr(&t->b, ops[opc], sf, RAX, RCX);
    if (opc == 3) { x_stc(&t->b); fl_defined(t); }
    stx(t, rd, RAX, false);
    return true;
}

/* and host, imm (rdx is scratch for a 64-bit mask that is no sign-extended imm32) */
static void and_imm(tctx *t, int sf, int host, uint64_t m)
{
    if (!sf) x_alu_ri(&t->b, XAND, 0, host, (int32_t)(uint32_t)m);
    else if ((int64_t)m == (int32_t)m) x_alu_ri(&t->b, XAND, 1, host, (int32_t)m);
    else { x_mov_ri(&t->b, RDX, m); x_alu_rr(&t->b, XAND, 1, host, RDX); }
}

/*
 * Constant chains. A constant wider than 16 bits is built by MOVZ or MOVN
 * and up to three MOVKs on one register, and an address by ADRP and ADD.
 * The first instruction of such a run is emitted with the run's final
 * effect and the others emit nothing (t->skip): none of them can fault, a
 * branch into the middle of a run starts another block, and
 * hao_jit_locate maps every host address of the one emitted sequence to
 * the first instruction (register not yet written) and the address after
 * it to the instruction after the run, so no intermediate value is ever
 * observable.
 */

/* The MOVKs on register rd (width sf) that follow instruction `from`:
 * the bits they replace and their values. Returns how many. */
static int movk_run(const tctx *t, int from, unsigned rd, int sf, uint64_t *mask, uint64_t *val)
{
    int cnt = 0;
    for (int k = from; k < t->n; k++, cnt++) {
        uint32_t j = t->insns[k];
        unsigned hw = BITS(j, 22, 21);
        if ((j & 0x7F800000) != 0x72800000 || BIT(j, 31) != (unsigned)sf || BITS(j, 4, 0) != rd || (!sf && hw >= 2)) break;
        *mask |= 0xffffull << (16 * hw);
        *val = (*val & ~(0xffffull << (16 * hw))) | (uint64_t)BITS(j, 20, 5) << (16 * hw);
    }
    return cnt;
}

static bool t_movewide(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned opc = BITS(i, 30, 29), hw = BITS(i, 22, 21), rd = BITS(i, 4, 0);
    uint64_t imm16 = BITS(i, 20, 5), all = sf ? ~0ull : 0xffffffffull;
    if (!sf && hw >= 2) return false;
    if (opc == 1) return false;
    if (rd == 31) return true;
    if (opc == 3) {                                                 /* MOVK */
        uint64_t mask = 0xffffull << (16 * hw), val = imm16 << (16 * hw);
        int run = movk_run(t, t->idx + 1, rd, sf, &mask, &val);
        if (mask == all) {                                          /* every part replaced: a constant */
            x_mov_ri(&t->b, RAX, val);
            stx(t, rd, RAX, false);
        } else if (t->in_eflags && t->dirty && t->live_out[t->idx]) {
            /* NZCV is live in EFLAGS: store the 16 bits and leave the flags
             * (from a register: an imm16 store has a length-changing prefix) */
            x_mov_ri(&t->b, RAX, imm16);
            x_mov_st(&t->b, 2, xm(RBX, CPU_OFF_X(rd) + 2 * (int)hw), RAX);
            if (!sf) x_mov_mi(&t->b, 4, xm(RBX, CPU_OFF_X(rd) + 4), 0);
            return true;
        } else {                                                    /* one read-modify-write of the register */
            fl_clobber(t);
            ldx(t, RAX, rd, sf, false);
            and_imm(t, sf, RAX, ~mask & all);
            if (!sf || (int64_t)val == (int32_t)val) { if (val) x_alu_ri(&t->b, XOR_, sf, RAX, (int32_t)(uint32_t)val); }
            else { x_mov_ri(&t->b, RDX, val); x_alu_rr(&t->b, XOR_, 1, RAX, RDX); }
            stx(t, rd, RAX, false);
        }
        t->skip = run;
        return true;
    }
    uint64_t v = imm16 << (16 * hw), mask = 0, val = 0;
    if (opc == 0) v = ~v;
    if (!sf) v = (uint32_t)v;
    t->skip = movk_run(t, t->idx + 1, rd, sf, &mask, &val);
    v = (v & ~mask) | val;
    x_mov_ri(&t->b, RAX, v);
    stx(t, rd, RAX, false);
    return true;
}

static bool t_adr(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned rd = BITS(i, 4, 0);
    int64_t imm = sext((uint64_t)BITS(i, 23, 5) << 2 | BITS(i, 30, 29), 21);
    uint64_t v = BIT(i, 31) ? (pc & ~0xfffull) + ((uint64_t)imm << 12) : pc + (uint64_t)imm;
    if (rd == 31) return true;
    if (BIT(i, 31) && t->idx + 1 < t->n) {                          /* ADRP Xd; ADD Xd, Xd, #imm: one address */
        uint32_t j = t->insns[t->idx + 1];
        if ((j & 0xFF800000) == 0x91000000 && BITS(j, 4, 0) == rd && BITS(j, 9, 5) == rd) {
            v += (uint64_t)BITS(j, 21, 10) << (BIT(j, 22) ? 12 : 0);
            t->skip = 1;
        }
    }
    gaddr(t, RAX, v);
    stx(t, rd, RAX, false);
    return true;
}

/* BFM (BFI, BFXIL, BFC): the destination field is cleared and the source
 * field, shifted into place, is or-ed in. Decode checks are t_bitfield's. */
static bool t_bfm(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned immr = BITS(i, 21, 16), imms = BITS(i, 15, 10), rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), W = sf ? 64 : 32;
    if (rd == 31) return true;
    unsigned width, lsb;
    if (imms >= immr) { width = imms - immr + 1; lsb = 0; }       /* BFXIL: Rn<imms:immr> -> Rd<width-1:0> */
    else { width = imms + 1; lsb = W - immr; }                    /* BFI: Rn<imms:0> -> Rd<lsb+imms:lsb> */
    uint64_t all = sf ? ~0ull : 0xffffffffull;
    uint64_t fm = (width == 64 ? ~0ull : ((1ull << width) - 1)) << lsb;
    fl_clobber(t);
    if (fm == all) { ldx(t, RAX, rn, sf, false); stx(t, rd, RAX, false); return true; }
    ldx(t, RAX, rd, sf, false);
    and_imm(t, sf, RAX, ~fm & all);
    if (rn != 31) {
        ldx(t, RCX, rn, sf, false);
        if (imms >= immr) { if (immr) x_shift_ri(&t->b, XSHR, sf, RCX, (uint8_t)immr); }
        else x_shift_ri(&t->b, XSHL, sf, RCX, (uint8_t)lsb);
        and_imm(t, sf, RCX, fm);
        x_alu_rr(&t->b, XOR_, sf, RAX, RCX);
    }
    stx(t, rd, RAX, false);
    return true;
}

static bool t_bitfield(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned opc = BITS(i, 30, 29), N = BIT(i, 22), immr = BITS(i, 21, 16), imms = BITS(i, 15, 10);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), W = sf ? 64 : 32;
    if (N != (unsigned)sf || opc == 3) return false;
    if (!sf && (immr >= 32 || imms >= 32)) return false;
    if (opc == 1) return t_bfm(t, i);
    /* flag-free fast paths */
    if (immr == 0 && opc == 2 && (imms == 7 || imms == 15)) {        /* UXTB/UXTH */
        x_mov_ld(&t->b, imms == 7 ? 1 : 2, RAX, XR(rn));
        if (rn == 31) x_mov_ri(&t->b, RAX, 0);
        stx(t, rd, RAX, false);
        return true;
    }
    if (immr == 0 && opc == 0 && (imms == 7 || imms == 15 || (imms == 31 && sf))) { /* SXTB/SXTH/SXTW */
        if (rn == 31) x_mov_ri(&t->b, RAX, 0);
        else x_movs_ld(&t->b, imms == 7 ? 1 : imms == 15 ? 2 : 4, sf, RAX, XR(rn));
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc == 2 && imms == W - 1 && immr == 0) {                    /* plain move */
        ldx(t, RAX, rn, sf, false);
        stx(t, rd, RAX, false);
        return true;
    }
    fl_clobber(t);
    ldx(t, RAX, rn, sf, false);
    if (imms >= immr) {
        unsigned width = imms - immr + 1;
        if (opc == 2) {  /* UBFX / LSR */
            if (immr) x_shift_ri(&t->b, XSHR, sf, RAX, (uint8_t)immr);
            if (width < W - immr) {
                x_shift_ri(&t->b, XSHL, sf, RAX, (uint8_t)(W - width));
                x_shift_ri(&t->b, XSHR, sf, RAX, (uint8_t)(W - width));
            }
        } else {         /* SBFX / ASR */
            if (W - 1 - imms) x_shift_ri(&t->b, XSHL, sf, RAX, (uint8_t)(W - 1 - imms));
            if (W - 1 - imms + immr) x_shift_ri(&t->b, XSAR, sf, RAX, (uint8_t)(W - 1 - imms + immr));
        }
    } else {
        unsigned width = imms + 1, shift = W - immr;
        x_shift_ri(&t->b, XSHL, sf, RAX, (uint8_t)(W - width));
        if (W - width - shift) x_shift_ri(&t->b, opc == 2 ? XSHR : XSAR, sf, RAX, (uint8_t)(W - width - shift));
    }
    stx(t, rd, RAX, false);
    return true;
}

static bool t_extr(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), lsb = BITS(i, 15, 10);
    if (BIT(i, 22) != (unsigned)sf || BITS(i, 30, 29) || BIT(i, 21) || (!sf && lsb >= 32)) return false;
    fl_clobber(t);
    ldx(t, RAX, rm, sf, false);
    if (lsb) {
        ldx(t, RCX, rn, sf, false);
        x_shrd_ri(&t->b, sf, RAX, RCX, (uint8_t)lsb);
    }
    stx(t, rd, RAX, false);
    return true;
}

static bool t_csel(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), cond = BITS(i, 15, 12), op2 = BITS(i, 11, 10);
    unsigned inv = BIT(i, 30);
    if (BIT(i, 29) || op2 > 1) return false;
    if (cond < 14) fl_load(t);
    /* alternative value (flag-free arithmetic) */
    ldx(t, RAX, rm, sf, false);
    if (inv) x_unary(&t->b, XNOT, sf, RAX);
    if (op2) x_lea(&t->b, sf, RAX, xm(RAX, 1));
    if (cond >= 14) { ldx(t, RAX, rn, sf, false); stx(t, rd, RAX, false); return true; }
    ldx(t, RCX, rn, sf, false);
    x_cmov_rr(&t->b, a64_cond_to_xcc(cond), sf, RAX, RCX);
    if (!sf) x_mov_rr(&t->b, 0, RAX, RAX);
    stx(t, rd, RAX, false);
    return true;
}

static bool t_ccmp(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31), cmn = !BIT(i, 30);
    unsigned rn = BITS(i, 9, 5), cond = BITS(i, 15, 12), nzcv = BITS(i, 3, 0), rm = BITS(i, 20, 16);
    if (BIT(i, 10) || BIT(i, 4) || !BIT(i, 29)) return false;
    if (cond < 14) fl_load(t);
    ldx(t, RAX, rn, sf, false);
    if (!BIT(i, 11)) ldx(t, RCX, rm, sf, false);
    size_t jfalse = 0, jend;
    if (cond < 14) jfalse = x_jcc32(&t->b, xcc_not(a64_cond_to_xcc(cond)));
    if (BIT(i, 11)) x_alu_ri(&t->b, cmn ? XADD : XSUB, sf, RAX, (int32_t)rm);
    else x_alu_rr(&t->b, cmn ? XADD : XSUB, sf, RAX, RCX);
    if (cmn) x_cmc(&t->b);
    if (cond < 14) {
        jend = x_jmp32(&t->b);
        x_patch32(&t->b, jfalse, xb_len(&t->b));
        uint32_t fl = HAO_FL_BASE | ((nzcv & 8) ? HAO_FL_SF : 0) | ((nzcv & 4) ? HAO_FL_ZF : 0) |
                      ((nzcv & 2) ? 0 : HAO_FL_CF) | ((nzcv & 1) ? HAO_FL_OF : 0);
        x_mov_ri(&t->b, RAX, fl);
        fl_from_eax(t);
        x_patch32(&t->b, jend, xb_len(&t->b));
    }
    fl_defined(t);
    return true;
}

/* ADC/SBC/ADCS/SBCS. Guest C is NOT CF: SBB computes a - b - !C as it
 * stands and leaves CF = borrow = !C; ADC needs CF = C on the way in and
 * leaves C on the way out, hence the CMCs. N, Z and V are SF, ZF and OF. */
static bool t_adc(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31), op = BIT(i, 30), S = BIT(i, 29);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    if (BITS(i, 15, 10)) return false;                            /* RMIF, SETF8/16: helper */
    fl_load(t);
    if (!S) fl_clobber(t);                                         /* EFLAGS still holds NZCV until the adc/sbb */
    ldx(t, RAX, rn, sf, false);
    ldx(t, RCX, rm, sf, false);
    if (!op) x_cmc(&t->b);
    x_alu_rr(&t->b, op ? XSBB : XADC, sf, RAX, RCX);
    if (S) { if (!op) x_cmc(&t->b); fl_defined(t); }
    stx(t, rd, RAX, false);
    return true;
}

static bool t_dp3(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), ra = BITS(i, 14, 10);
    unsigned op31 = BITS(i, 23, 21), o0 = BIT(i, 15);
    if (BITS(i, 30, 29) || ((op31 == 2 || op31 == 6) && o0)) return false;
    fl_clobber(t);
    switch (op31) {
    case 0:  /* MADD / MSUB */
        ldx(t, RAX, rn, sf, false);
        ldx(t, RCX, rm, sf, false);
        x_imul_rr(&t->b, sf, RAX, RCX);
        ldx(t, RCX, ra, sf, false);
        if (o0) { x_alu_rr(&t->b, XSUB, sf, RCX, RAX); stx(t, rd, RCX, false); }
        else { x_alu_rr(&t->b, XADD, sf, RAX, RCX); stx(t, rd, RAX, false); }
        return true;
    case 1: case 5:  /* SMADDL/SMSUBL, UMADDL/UMSUBL */
        if (!sf) return false;
        if (op31 == 1) {
            if (rn == 31) x_mov_ri(&t->b, RAX, 0); else x_movs_ld(&t->b, 4, 1, RAX, XR(rn));
            if (rm == 31) x_mov_ri(&t->b, RCX, 0); else x_movs_ld(&t->b, 4, 1, RCX, XR(rm));
        } else {
            ldx(t, RAX, rn, 0, false);
            ldx(t, RCX, rm, 0, false);
        }
        x_imul_rr(&t->b, 1, RAX, RCX);
        ldx(t, RCX, ra, 1, false);
        if (o0) { x_alu_rr(&t->b, XSUB, 1, RCX, RAX); stx(t, rd, RCX, false); }
        else { x_alu_rr(&t->b, XADD, 1, RAX, RCX); stx(t, rd, RAX, false); }
        return true;
    case 2: case 6:  /* SMULH / UMULH */
        if (!sf) return false;
        ldx(t, RAX, rn, 1, false);
        ldx(t, RCX, rm, 1, false);
        x_unary(&t->b, op31 == 2 ? XIMUL1 : XMUL, 1, RCX);
        stx(t, rd, RDX, false);
        return true;
    default: return false;
    }
}

static bool t_dp2(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), opc = BITS(i, 15, 10);
    if (BIT(i, 29)) return false;
    if (opc == 2 || opc == 3) {
        /* UDIV/SDIV. A64 defines what x86 traps on: x / 0 = 0, and the
         * most negative value / -1 is itself (every x / -1 is -x). */
        fl_clobber(t);
        ldx(t, RCX, rm, sf, false);
        ldx(t, RAX, rn, sf, false);
        size_t zero = x_jrcxz8(&t->b), neg = 0, d1, d2 = 0;      /* a 32-bit divisor is zero-extended in rcx */
        if (opc == 3) {
            x_alu_ri(&t->b, XCMP, sf, RCX, -1);
            neg = x_jcc8(&t->b, XCC_E);
            x_cqo(&t->b, sf);
            x_unary(&t->b, XIDIV, sf, RCX);
        } else {
            x_mov_ri(&t->b, RDX, 0);
            x_unary(&t->b, XDIV, sf, RCX);
        }
        d1 = x_jmp8(&t->b);
        if (opc == 3) {
            x_patch8(&t->b, neg, xb_len(&t->b));
            x_unary(&t->b, XNEG, sf, RAX);
            d2 = x_jmp8(&t->b);
        }
        x_patch8(&t->b, zero, xb_len(&t->b));
        x_mov_ri(&t->b, RAX, 0);
        x_patch8(&t->b, d1, xb_len(&t->b));
        if (opc == 3) x_patch8(&t->b, d2, xb_len(&t->b));
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc < 8 || opc > 11) return false;                 /* shifts; crc/pacga via helper */
    static const int ops[4] = {XSHL, XSHR, XSAR, XROR};
    fl_clobber(t);
    ldx(t, RCX, rm, sf, false);
    ldx(t, RAX, rn, sf, false);
    x_shift_cl(&t->b, ops[opc - 8], sf, RAX);
    stx(t, rd, RAX, false);
    return true;
}

/* rax = (rax & m) << sh | (rax >> sh) & m; rcx, rdx scratch */
static void swap_step(tctx *t, int sf, uint64_t m, unsigned sh)
{
    x_mov_rr(&t->b, sf, RCX, RAX);
    x_shift_ri(&t->b, XSHR, sf, RCX, (uint8_t)sh);
    if (sf) {
        x_mov_ri(&t->b, RDX, m);
        x_alu_rr(&t->b, XAND, 1, RAX, RDX);
        x_alu_rr(&t->b, XAND, 1, RCX, RDX);
    } else {
        x_alu_ri(&t->b, XAND, 0, RAX, (int32_t)(uint32_t)m);
        x_alu_ri(&t->b, XAND, 0, RCX, (int32_t)(uint32_t)m);
    }
    x_shift_ri(&t->b, XSHL, sf, RAX, (uint8_t)sh);
    x_alu_rr(&t->b, XOR_, sf, RAX, RCX);
}

static bool t_dp1(tctx *t, uint32_t i)
{
    int sf = BIT(i, 31);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), op2 = BITS(i, 20, 16), opc = BITS(i, 15, 10);
    if (BIT(i, 29)) return false;
    if (op2 == 1) return opc < 16 && a64_pac_valid(i);   /* PAC/AUT: identity; XPACI/XPACD: helper (a64_xpac) */
    if (op2) return false;
    if ((opc == 3 && sf) || (opc == 2 && !sf)) { /* REV */
        ldx(t, RAX, rn, sf, false);
        x_bswap(&t->b, sf, RAX);
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc == 0 || opc == 1) {                  /* RBIT: bytes, then nibbles, pairs, bits; REV16 */
        fl_clobber(t);
        ldx(t, RAX, rn, sf, false);
        if (opc == 0) {
            x_bswap(&t->b, sf, RAX);
            swap_step(t, sf, 0x0f0f0f0f0f0f0f0full, 4);
            swap_step(t, sf, 0x3333333333333333ull, 2);
            swap_step(t, sf, 0x5555555555555555ull, 1);
        } else
            swap_step(t, sf, 0x00ff00ff00ff00ffull, 8);
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc == 2) {                              /* REV32 (sf; REV Wd is above) */
        fl_clobber(t);
        ldx(t, RAX, rn, 1, false);
        x_bswap(&t->b, 1, RAX);
        x_shift_ri(&t->b, XROR, 1, RAX, 32);
        stx(t, rd, RAX, false);
        return true;
    }
    if (opc == 4) {                              /* CLZ */
        fl_clobber(t);
        ldx(t, RAX, rn, sf, false);
        x_mov_ri(&t->b, RDX, (uint64_t)-1);
        x_bsr(&t->b, sf, RCX, RAX);
        x_cmov_rr(&t->b, XCC_E, 1, RCX, RDX);
        x_mov_ri(&t->b, RAX, sf ? 63 : 31);
        x_alu_rr(&t->b, XSUB, 1, RAX, RCX);
        stx(t, rd, RAX, false);
        return true;
    }
    return false;
}

/* zero-extend the low `bytes` of a host register in place (no flags) */
static void zext(tctx *t, int host, int bytes)
{
    if (bytes == 1) x_movzx8_rr(&t->b, host, host);
    else if (bytes == 2) x_movzx16_rr(&t->b, host, host);
    else if (bytes == 4) x_mov_rr(&t->b, 0, host, host);
}

/*
 * LSE atomic memory operations (LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN,
 * LDUMAX, LDUMIN, SWP, each in four sizes and four orderings; ST<op> is
 * Rt = ZR) and LDAPR. x86 locked instructions are sequentially consistent,
 * which is at least every A64 ordering. SWP is xchg and LDADD is lock xadd;
 * the others have no x86 instruction that returns the old value, so they
 * retry lock cmpxchg, as the interpreter does. Rt gets the old value
 * zero-extended.
 */
static bool t_atomic(tctx *t, uint32_t i)
{
    int bytes = 1 << BITS(i, 31, 30);
    unsigned rs = BITS(i, 20, 16), o3 = BIT(i, 15), op = BITS(i, 14, 12), rn = BITS(i, 9, 5), rt = BITS(i, 4, 0);
    if (o3 && op == 4) {                                           /* LDAPR */
        if (!BIT(i, 23) || BIT(i, 22) || rs != 31) return false;
        ldx(t, RAX, rn, 1, true);
        x_mov_ld(&t->b, bytes, RDX, xm(RAX, 0));
        stx(t, rt, RDX, false);
        return true;
    }
    if (o3 && op) return false;
    if (o3) {                                                      /* SWP: xchg leaves EFLAGS alone */
        ldx(t, RDX, rs, 1, false);
        ldx(t, RAX, rn, 1, true);
        x_xchg_mr(&t->b, bytes, xm(RAX, 0), RDX);
        zext(t, RDX, bytes);
        stx(t, rt, RDX, false);
        return true;
    }
    fl_clobber(t);
    if (op == 0) {                                                 /* LDADD */
        ldx(t, RDX, rs, 1, false);
        ldx(t, RAX, rn, 1, true);
        x_lock_xadd(&t->b, bytes, xm(RAX, 0), RDX);
        zext(t, RDX, bytes);
        stx(t, rt, RDX, false);
        return true;
    }
    /* rcx = address, rsi = operand, rax = old value (zero-extended), rdx = new value */
    ldx(t, RCX, rn, 1, true);
    if (rs == 31) x_mov_ri(&t->b, RSI, 0);
    else if ((op == 4 || op == 5) && bytes < 8) x_movs_ld(&t->b, bytes, 1, RSI, XR(rs));   /* signed compare */
    else x_mov_ld(&t->b, op >= 6 ? bytes : 8, RSI, XR(rs));
    if (op == 1) x_unary(&t->b, XNOT, 1, RSI);                    /* LDCLR: and with the complement */
    x_mov_ld(&t->b, bytes, RAX, xm(RCX, 0));
    size_t loop = xb_len(&t->b);
    if ((op == 4 || op == 5) && bytes < 8) x_movsx_rr(&t->b, bytes, 1, RDX, RAX);
    else x_mov_rr(&t->b, 1, RDX, RAX);
    switch (op) {
    case 1: x_alu_rr(&t->b, XAND, 1, RDX, RSI); break;
    case 2: x_alu_rr(&t->b, XXOR, 1, RDX, RSI); break;
    case 3: x_alu_rr(&t->b, XOR_, 1, RDX, RSI); break;
    default: {                                                     /* new = old unless the operand wins */
        static const int win[4] = {XCC_L, XCC_G, XCC_B, XCC_A};   /* SMAX, SMIN, UMAX, UMIN */
        x_alu_rr(&t->b, XCMP, 1, RDX, RSI);
        x_cmov_rr(&t->b, win[op - 4], 1, RDX, RSI);
        break;
    }
    }
    x_lock_cmpxchg(&t->b, bytes, xm(RCX, 0), RDX);               /* a failure reloads al/ax/eax/rax */
    x_patch8(&t->b, x_jcc8(&t->b, XCC_NE), loop);
    stx(t, rt, RAX, false);
    return true;
}

/* LDRAA/LDRAB (arm64e): a 64-bit load at Xn + simm10 * 8, with optional
 * pre-index writeback. Pointers are unsigned here (see a64_interp.c), so
 * the authentication of the base is the identity. */
static bool t_ldraa(tctx *t, uint32_t i)
{
    unsigned rt = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int32_t off = (int32_t)sext((uint64_t)BIT(i, 22) << 9 | BITS(i, 20, 12), 10) * 8;
    if (BIT(i, 11) && rn != 31 && rn == rt) return false;         /* UNDEFINED */
    ldx(t, RAX, rn, 1, true);
    x_mov_ld(&t->b, 8, RDX, xm(RAX, off));
    stx(t, rt, RDX, false);
    if (BIT(i, 11)) { x_lea(&t->b, 1, RAX, xm(RAX, off)); stx(t, rn, RAX, true); }
    return true;
}

/* address = base(rn, SP ok) [+ offset] into RAX, returns xmem relative to RAX */
static bool t_ldst_reg(tctx *t, uint32_t i)
{
    unsigned size = BITS(i, 31, 30), opc = BITS(i, 23, 22), rt = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int V = BIT(i, 26), bytes, kind;   /* kind: 0 st, 1 ldzx, 2 ldsx64, 3 ldsx32 */
    if (!V && !BIT(i, 24) && BIT(i, 21) && BITS(i, 11, 10) != 2) {
        if (BITS(i, 11, 10) == 0) return t_atomic(t, i);
        return size == 3 && t_ldraa(t, i);
    }
    if (V) {
        if (BIT(opc, 1)) { if (size) return false; bytes = 16; }
        else bytes = 1 << size;
        kind = opc & 1;
    } else {
        bytes = 1 << size;
        switch (opc) {
        case 0: kind = 0; break;
        case 1: kind = 1; break;
        case 2: if (size == 3) /* PRFM/PRFUM: no-op; the writeback forms are UNDEFINED */
                    return BIT(i, 24) || (!BIT(i, 21) && BITS(i, 11, 10) == 0);
                kind = 2; break;
        default: if (size >= 2) return false; kind = 3; break;
        }
    }
    xmem m;
    int wb = 0;
    int32_t wboff = 0;
    ldx(t, RAX, rn, 1, true);
    if (BIT(i, 24)) {
        m = xm(RAX, (int32_t)(BITS(i, 21, 10) * (unsigned)bytes));
    } else if (!BIT(i, 21)) {
        unsigned mode = BITS(i, 11, 10);
        int32_t simm = (int32_t)sext(BITS(i, 20, 12), 9);
        if (mode == 2) return false;
        if ((mode == 1 || mode == 3) && !V && rn != 31 && rn == rt) return false;   /* UNDEFINED */
        m = xm(RAX, mode == 1 ? 0 : simm);
        if (mode == 1 || mode == 3) { wb = 1; wboff = simm; }
    } else if (BITS(i, 11, 10) == 2) {
        unsigned rm = BITS(i, 20, 16), opt = BITS(i, 15, 13), S = BIT(i, 12);
        if (!(opt & 2)) return false;
        if (rm == 31) x_mov_ri(&t->b, RCX, 0);
        else if (opt == 2) x_mov_ld(&t->b, 4, RCX, XR(rm));
        else if (opt == 6) x_movs_ld(&t->b, 4, 1, RCX, XR(rm));
        else x_mov_ld(&t->b, 8, RCX, XR(rm));
        int scale = S ? bytes : 1;
        if (scale == 16) { x_lea(&t->b, 1, RCX, xmi(XNONE, RCX, 8, 0)); scale = 2; }   /* index * 16, EFLAGS untouched */
        m = xmi(RAX, RCX, scale, 0);
    } else {
        return false;   /* SIMD encodings of the atomics/LDRAA space: UNDEFINED, helper */
    }
    if (V) {
        if (kind) {
            if (bytes == 16) x_movdqu_ld(&t->b, 0, m);
            else if (bytes == 8) x_movq_ld(&t->b, 0, m);
            else if (bytes == 4) x_movd_ld(&t->b, 0, m);
            else { x_mov_ld(&t->b, bytes, RDX, m); x_sse_rr(&t->b, 0x66, 0x6E, 0, 0, RDX); }
            x_movdqu_st(&t->b, xm(RBX, CPU_OFF_V(rt)), 0);
        } else {
            x_movdqu_ld(&t->b, 0, xm(RBX, CPU_OFF_V(rt)));
            if (bytes == 16) x_movdqu_st(&t->b, m, 0);
            else if (bytes == 8) x_movq_st(&t->b, m, 0);
            else if (bytes == 4) x_movd_st(&t->b, m, 0);
            else { x_sse_rr(&t->b, 0x66, 0x7E, 0, 0, RDX); x_mov_st(&t->b, bytes, m, RDX); }
        }
    } else if (kind == 0) {
        ldx(t, RDX, rt, 1, false);
        x_mov_st(&t->b, bytes, m, RDX);
    } else {
        if (kind == 1) x_mov_ld(&t->b, bytes, RDX, m);
        else x_movs_ld(&t->b, bytes, kind == 2, RDX, m);
        stx(t, rt, RDX, false);
    }
    if (wb) { x_lea(&t->b, 1, RAX, xm(RAX, wboff)); stx(t, rn, RAX, true); }
    return true;
}

/*
 * LDP/STP of two X registers at a 16-byte aligned address is one 16-byte
 * single-copy-atomic access (FEAT_LSE2), which arm64e code relies on for
 * 128-bit atomics and seqlocks. Emit it as movdqa (atomic on AVX CPUs, see
 * x86.h). SP is 16-byte aligned whenever it is a base (the architecture
 * checks it), so for SP-based pairs the alignment is known here; otherwise
 * it is tested at run time without touching EFLAGS: the address is copied
 * into xmm1, shifted left by 60 (psllq), copied back to rcx and tested with
 * jrcxz. Leaves the loaded pair in rdx/rsi; for stores rdx/rsi hold the data.
 */
static void pair128(tctx *t, bool load, int32_t disp, bool sp_base)
{
    xmem m = xm(RDI, 0);
    x_lea(&t->b, 1, RDI, xm(RAX, disp));
    size_t to_aligned = 0, to_done = 0;
    if (sp_base && (disp & 15)) {
        if (load) { x_mov_ld(&t->b, 8, RDX, m); x_mov_ld(&t->b, 8, RSI, xm(RDI, 8)); }
        else { x_mov_st(&t->b, 8, m, RDX); x_mov_st(&t->b, 8, xm(RDI, 8), RSI); }
        return;
    }
    if (!sp_base) {
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 1, RDI);                 /* movq xmm1, rdi */
        x_psllq_ri(&t->b, 1, 60);
        x_sse_rr(&t->b, 0x66, 0x7E, 1, 1, RCX);                 /* movq rcx, xmm1 */
        to_aligned = x_jrcxz8(&t->b);
        if (load) { x_mov_ld(&t->b, 8, RDX, m); x_mov_ld(&t->b, 8, RSI, xm(RDI, 8)); }
        else { x_mov_st(&t->b, 8, m, RDX); x_mov_st(&t->b, 8, xm(RDI, 8), RSI); }
        to_done = x_jmp8(&t->b);
        x_patch8(&t->b, to_aligned, xb_len(&t->b));
    }
    if (load) {
        x_movdqa_ld(&t->b, 0, m);
        x_sse_rr(&t->b, 0x66, 0x7E, 1, 0, RDX);                 /* movq rdx, xmm0 */
        x_sse_rr(&t->b, 0x66, 0x6D, 0, 0, 0);                   /* punpckhqdq xmm0, xmm0 */
        x_sse_rr(&t->b, 0x66, 0x7E, 1, 0, RSI);                 /* movq rsi, xmm0 */
    } else {
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 0, RDX);                 /* movq xmm0, rdx */
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 1, RSI);                 /* movq xmm1, rsi */
        x_sse_rr(&t->b, 0x66, 0x6C, 0, 0, 1);                   /* punpcklqdq xmm0, xmm1 */
        x_movdqa_st(&t->b, m, 0);
    }
    if (!sp_base) x_patch8(&t->b, to_done, xb_len(&t->b));
}

static bool t_ldst_pair(tctx *t, uint32_t i)
{
    unsigned opc = BITS(i, 31, 30), type = BITS(i, 24, 23), L = BIT(i, 22);
    unsigned rt = BITS(i, 4, 0), rt2 = BITS(i, 14, 10), rn = BITS(i, 9, 5);
    int V = BIT(i, 26), bytes, sx = 0;
    if (V) { if (opc == 3) return false; bytes = 4 << opc; }
    else if (opc == 0) bytes = 4;
    else if (opc == 2) bytes = 8;
    else if (opc == 1 && L && type != 0) { bytes = 4; sx = 1; }
    else return false;
    /* UNDEFINED on Apple silicon: the interpreter helper reports it */
    if (L && rt == rt2) return false;
    if (!V && (type == 1 || type == 3) && rn != 31 && (rn == rt || rn == rt2)) return false;
    int32_t off = (int32_t)sext(BITS(i, 21, 15), 7) * bytes;
    ldx(t, RAX, rn, 1, true);
    int32_t base = type == 1 ? 0 : off;
    if (!V && bytes == 8) {
        if (!L) { ldx(t, RDX, rt, 1, false); ldx(t, RSI, rt2, 1, false); }
        pair128(t, L, base, rn == 31);
        if (L) { stx(t, rt, RDX, false); stx(t, rt2, RSI, false); }
        if (type == 1 || type == 3) { x_lea(&t->b, 1, RAX, xm(RAX, off)); stx(t, rn, RAX, true); }
        return true;
    }
    for (int k = 0; k < 2; k++) {
        unsigned r = k ? rt2 : rt;
        xmem m = xm(RAX, base + k * bytes);
        if (V) {
            if (L) {
                if (bytes == 16) x_movdqu_ld(&t->b, k, m); else if (bytes == 8) x_movq_ld(&t->b, k, m); else x_movd_ld(&t->b, k, m);
            } else {
                x_movdqu_ld(&t->b, k, xm(RBX, CPU_OFF_V(r)));
                if (bytes == 16) x_movdqu_st(&t->b, m, k); else if (bytes == 8) x_movq_st(&t->b, m, k); else x_movd_st(&t->b, m, k);
            }
        } else if (L) {
            int h = k ? RSI : RDX;
            if (sx) x_movs_ld(&t->b, 4, 1, h, m); else x_mov_ld(&t->b, bytes, h, m);
        } else {
            ldx(t, RDX, r, 1, false);
            x_mov_st(&t->b, bytes, m, RDX);
        }
    }
    if (L) {   /* write back after both loads (rt may alias rn) */
        if (V) { x_movdqu_st(&t->b, xm(RBX, CPU_OFF_V(rt)), 0); x_movdqu_st(&t->b, xm(RBX, CPU_OFF_V(rt2)), 1); }
        else { stx(t, rt, RDX, false); stx(t, rt2, RSI, false); }
    }
    if (type == 1 || type == 3) { x_lea(&t->b, 1, RAX, xm(RAX, off)); stx(t, rn, RAX, true); }
    return true;
}

static bool t_ldr_lit(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned opc = BITS(i, 31, 30), rt = BITS(i, 4, 0);
    uint64_t addr = pc + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4);
    if (BIT(i, 26)) {
        if (opc == 3) return false;
        gaddr(t, RAX, addr);
        if (opc == 2) x_movdqu_ld(&t->b, 0, xm(RAX, 0)); else if (opc == 1) x_movq_ld(&t->b, 0, xm(RAX, 0)); else x_movd_ld(&t->b, 0, xm(RAX, 0));
        x_movdqu_st(&t->b, xm(RBX, CPU_OFF_V(rt)), 0);
        return true;
    }
    if (opc == 3) return true;  /* PRFM */
    gaddr(t, RAX, addr);
    if (opc == 0) x_mov_ld(&t->b, 4, RDX, xm(RAX, 0));
    else if (opc == 1) x_mov_ld(&t->b, 8, RDX, xm(RAX, 0));
    else x_movs_ld(&t->b, 4, 1, RDX, xm(RAX, 0));
    stx(t, rt, RDX, false);
    return true;
}

static bool t_ordered(tctx *t, uint32_t i)
{
    /* LDAR/STLR (o2=1, o1=0): x86 TSO loads are acquire; release-store via xchg */
    unsigned size = BITS(i, 31, 30), L = BIT(i, 22), rt = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int bytes = 1 << size;
    if ((i & 0x3FA00000) == 0x08A00000) {
        /* CAS{,A,L,AL}{B,H,,}: lock cmpxchg compares al/ax/eax/rax with
         * memory and loads it on a mismatch; Rs gets the old value
         * zero-extended either way. CASP and the exclusives: helper. */
        unsigned rs = BITS(i, 20, 16);
        if (BITS(i, 14, 10) != 31) return false;
        fl_clobber(t);
        ldx(t, RAX, rs, 1, false);
        ldx(t, RDX, rt, 1, false);
        ldx(t, RCX, rn, 1, true);
        x_lock_cmpxchg(&t->b, bytes, xm(RCX, 0), RDX);
        zext(t, RAX, bytes);
        stx(t, rs, RAX, false);
        return true;
    }
    if ((i & 0x3FA00000) != 0x08800000) return false;
    ldx(t, RAX, rn, 1, true);
    if (L) { x_mov_ld(&t->b, bytes, RDX, xm(RAX, 0)); stx(t, rt, RDX, false); }
    else { ldx(t, RDX, rt, 1, false); x_xchg_mr(&t->b, bytes, xm(RAX, 0), RDX); }
    return true;
}

/* Match a64_fpcr/fpsr_{read,write}, including shared native MXCSR state
 * and the x87 rounding/exception state. Save guest NZCV before integer bit
 * mapping; the stack entry stub reserves eight scratch bytes. */
static bool t_fp_system(tctx *t, uint32_t i)
{
    uint32_t key = i & 0xffffffe0u;
    bool read = BIT(i, 21), status = BIT(i, 5);
    if (key != 0xd53b4400u && key != 0xd51b4400u && key != 0xd53b4420u && key != 0xd51b4420u) return false;
    unsigned rt = BITS(i, 4, 0);
    xmem env = xm(RSP, 0), cw = xm(RSP, 4);
    fl_clobber(t);
    x_stmxcsr(&t->b, env);
    if (read && !status) {
        x_mov_ld(&t->b, 4, RAX, xm(RBX, offsetof(hao_cpu, fpcr)));
        x_alu_ri(&t->b, XAND, 0, RAX, ~(7u << 22));
        x_mov_ld(&t->b, 4, RCX, env);
        for (unsigned bit = 13; bit <= 15; bit++) {
            x_mov_rr(&t->b, 0, RDX, RCX); x_alu_ri(&t->b, XAND, 0, RDX, 1u << bit);
            x_shift_ri(&t->b, XSHL, 0, RDX, (uint8_t)(bit == 13 ? 10 : bit == 14 ? 8 : 9));
            x_alu_rr(&t->b, XOR_, 0, RAX, RDX);
        }
        stx(t, rt, RAX, false);
    } else if (!read && !status) {
        ldx(t, RAX, rt, 0, false); x_alu_ri(&t->b, XAND, 0, RAX, 0x07ff9f07); /* FPCR_WRITABLE */
        x_mov_st(&t->b, 4, xm(RBX, offsetof(hao_cpu, fpcr)), RAX);
        x_mov_ld(&t->b, 4, RCX, env); x_alu_ri(&t->b, XAND, 0, RCX, ~0xe040u);
        /* Swapped RMode in edx; retain it for the x87 control word. */
        x_mov_rr(&t->b, 0, RDX, RAX); x_shift_ri(&t->b, XSHR, 0, RDX, 8); x_alu_ri(&t->b, XAND, 0, RDX, 1u << 14);
        x_mov_rr(&t->b, 0, RSI, RAX); x_shift_ri(&t->b, XSHR, 0, RSI, 10); x_alu_ri(&t->b, XAND, 0, RSI, 1u << 13);
        x_alu_rr(&t->b, XOR_, 0, RDX, RSI); x_alu_rr(&t->b, XOR_, 0, RCX, RDX);
        x_alu_ri(&t->b, XAND, 0, RAX, 1u << 24);
        size_t nofz = x_jcc32(&t->b, XCC_E);
        x_alu_ri(&t->b, XOR_, 0, RCX, 0x8040); /* FTZ and DAZ */
        x_patch32(&t->b, nofz, xb_len(&t->b));
        x_mov_st(&t->b, 4, env, RCX); x_ldmxcsr(&t->b, env);
        x_fnstcw(&t->b, cw); x_mov_ld(&t->b, 2, RCX, cw); x_mov_rr(&t->b, 0, RSI, RCX);
        x_alu_ri(&t->b, XAND, 0, RCX, ~0x0c00u); x_shift_ri(&t->b, XSHR, 0, RDX, 3);
        x_alu_rr(&t->b, XOR_, 0, RCX, RDX);
        /* Like x87_rc_set: avoid a waiting FLDCW when nothing changed. */
        x_alu_rr(&t->b, XCMP, 0, RCX, RSI);
        size_t unchanged = x_jcc8(&t->b, XCC_E);
        x_mov_st(&t->b, 2, cw, RCX); x_fldcw(&t->b, cw);
        x_patch8(&t->b, unchanged, xb_len(&t->b));
    } else if (read) {
        x_fnstsw(&t->b, cw); x_mov_ld(&t->b, 2, RAX, cw); x_alu_ri(&t->b, XAND, 0, RAX, 0x3d);
        x_mov_ld(&t->b, 4, RCX, env); x_alu_rr(&t->b, XOR_, 0, RAX, RCX);
        x_mov_rr(&t->b, 0, RDX, RAX); x_alu_ri(&t->b, XAND, 0, RDX, 1);
        x_shift_ri(&t->b, XSHR, 0, RAX, 1); x_alu_ri(&t->b, XAND, 0, RAX, 0x1e); x_alu_rr(&t->b, XOR_, 0, RAX, RDX);
        x_mov_ld(&t->b, 4, RCX, xm(RBX, offsetof(hao_cpu, fpsr))); x_alu_ri(&t->b, XAND, 0, RCX, 0x08000080);
        x_alu_rr(&t->b, XOR_, 0, RAX, RCX); stx(t, rt, RAX, false);
    } else {
        ldx(t, RAX, rt, 0, false); x_mov_rr(&t->b, 0, RDX, RAX); x_alu_ri(&t->b, XAND, 0, RDX, 0x08000080);
        x_mov_st(&t->b, 4, xm(RBX, offsetof(hao_cpu, fpsr)), RDX);
        x_mov_rr(&t->b, 0, RDX, RAX); x_alu_ri(&t->b, XAND, 0, RDX, 1);
        x_alu_ri(&t->b, XAND, 0, RAX, 0x1e); x_shift_ri(&t->b, XSHL, 0, RAX, 1); x_alu_rr(&t->b, XOR_, 0, RAX, RDX);
        x_mov_ld(&t->b, 4, RCX, env); x_alu_ri(&t->b, XAND, 0, RCX, ~0x3fu); x_alu_rr(&t->b, XOR_, 0, RAX, RCX);
        x_mov_st(&t->b, 4, env, RAX); x_ldmxcsr(&t->b, env);
        x_fnstsw(&t->b, cw); x_mov_ld(&t->b, 2, RAX, cw); x_alu_ri(&t->b, XAND, 0, RAX, 0x3d);
        size_t clean = x_jcc8(&t->b, XCC_E); x_fnclex(&t->b); x_patch8(&t->b, clean, xb_len(&t->b));
    }
    return true;
}

static bool t_system(tctx *t, uint32_t i)
{
    if (t_fp_system(t, i)) return true;
    if (i == 0xD50320FF || i == 0xD503207F) return false;            /* XPACLRI (a64_xpac), WFI (UNDEF): helper */
    if ((i & 0xFFFFF01F) == 0xD503201F) return true;                 /* HINT space: NOP, PAC*SP, BTI, ... */
    if ((i & 0xFFFFF0FF) == 0xD50330BF) {                            /* DMB */
        if ((BITS(i, 11, 8) & 3) == 3) x_mfence(&t->b);
        return true;
    }
    if ((i & 0xFFFFF0FF) == 0xD503309F) { if ((BITS(i, 11, 8) & 3) == 3) x_mfence(&t->b); return true; } /* DSB */
    if ((i & 0xFFFFF0FF) == 0xD50330DF) return true;                 /* ISB */
    if ((i & 0xFFFFFFE0) == 0xD53BD060) {                            /* MRS TPIDRRO_EL0 */
        x_mov_ld(&t->b, 8, RAX, xm(RBX, 0x118));
        stx(t, BITS(i, 4, 0), RAX, false);
        return true;
    }
    if ((i & 0xFFFFFFE0) == 0xD53BD040) {                            /* MRS TPIDR_EL0 */
        x_mov_ld(&t->b, 8, RAX, xm(RBX, 0x110));
        stx(t, BITS(i, 4, 0), RAX, false);
        return true;
    }
    return false;
}

/* ---- scalar floating point (SSE2) */
#define VR(n) xm(RBX, CPU_OFF_V(n))
static void fp_load(tctx *t, int xmm, unsigned r, int dbl)
{
    if (dbl) x_movq_ld(&t->b, xmm, VR(r)); else x_movd_ld(&t->b, xmm, VR(r));
}
static void fp_store(tctx *t, unsigned r, int xmm) { x_movdqu_st(&t->b, VR(r), xmm); }

/* Admit scalar SSE before it can alter FPSR. Normal/zero inputs are safe
 * for comparisons, sqrt and widening. Arithmetic also excludes magnitudes
 * that could produce a tiny nonzero result: FZ's UFC-only rule differs from
 * SSE's UFC|IXC. The remaining cases use the shared interpreter unchanged.
 * This deliberately does not consult cpu->fpcr's cached FZ bit: native
 * callees share MXCSR and may change its controls directly. */
static unsigned fp_begin_checked(tctx *t, int dbl, unsigned rn, int rm,
                                 uint32_t insn, size_t decline[2])
{
    fl_clobber(t);
    uint64_t low = dbl ? 0x0010000000000000ull : 0x00800000u, high = 0;
    if ((insn & 0xFF200C00) == 0x1E200800) {
        unsigned op = BITS(insn, 15, 12);
        if (op < 2) {  /* products/quotients in a safe exponent interval */
            low = dbl ? 0x20B0000000000000ull : 0x26800000u;
            high = dbl ? 0x5F30000000000000ull : 0x58800000u;
        } else {       /* cancellation remains normal or exactly zero */
            low = dbl ? 0x0360000000000000ull : 0x0C800000u;
        }
    } else if ((insn & 0xFFBFFC00) == 0x1E224000) {
        /* FCVT S,D: no result below the minimum normal single. */
        low = 0x3810000000000000ull;
    }
    unsigned n = rm < 0 ? 1 : 2;
    for (unsigned k = 0; k < n; k++) {
        x_mov_ld(&t->b, dbl ? 8 : 4, RAX, VR(k ? (unsigned)rm : rn));
        x_alu_rr(&t->b, XADD, dbl, RAX, RAX);             /* 2*abs, sign discarded */
        if (!high) {
            /* 2*abs-1 admits zero by unsigned wrap, with one branch. */
            x_alu_ri(&t->b, XSUB, dbl, RAX, 1);
            if (dbl) { x_mov_ri(&t->b, RCX, 2*low-1); x_alu_rr(&t->b, XCMP, 1, RAX, RCX); }
            else x_alu_ri(&t->b, XCMP, 0, RAX, (uint32_t)(2*low-1));
            decline[k] = x_jcc32(&t->b, XCC_B);
            continue;
        }
        size_t zero = x_jcc32(&t->b, XCC_E);
        size_t special = 0;
        if (high) {
            uint64_t inf = dbl ? 0xFFE0000000000000ull : 0xFF000000u;
            if (dbl) { x_mov_ri(&t->b, RCX, inf); x_alu_rr(&t->b, XCMP, 1, RAX, RCX); }
            else x_alu_ri(&t->b, XCMP, 0, RAX, (uint32_t)inf);
            special = x_jcc32(&t->b, XCC_AE);           /* infinities cannot underflow */
        }
        if (dbl) { x_mov_ri(&t->b, RCX, 2*low); x_alu_rr(&t->b, XSUB, 1, RAX, RCX); }
        else x_alu_ri(&t->b, XSUB, 0, RAX, (uint32_t)(2*low));
        uint64_t span = high ? 2*(high-low) : (dbl ? ~0ull : 0xffffffffu) - 2*low;
        if (dbl) { x_mov_ri(&t->b, RCX, span); x_alu_rr(&t->b, XCMP, 1, RAX, RCX); }
        else x_alu_ri(&t->b, XCMP, 0, RAX, (uint32_t)span);
        decline[k] = x_jcc32(&t->b, XCC_A);
        x_patch32(&t->b, zero, xb_len(&t->b));
        if (special) x_patch32(&t->b, special, xb_len(&t->b));
    }
    return n;
}
static void fp_store_checked(tctx *t, unsigned rd, int dbl, uint32_t insn, uint64_t pc,
                             const size_t *decline, unsigned ndecline)
{
    x_sse_rr(&t->b, dbl ? 0x66 : 0, 0x2E, 0, 0, 0);
    size_t nan = x_jcc32(&t->b, XCC_P);
    fp_store(t, rd, 0);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, nan, xb_len(&t->b));
    for (unsigned k = 0; k < ndecline; k++) x_patch32(&t->b, decline[k], xb_len(&t->b));
    helper(t, insn, pc, true);
    st_helper--;
    x_patch32(&t->b, done, xb_len(&t->b));
}

/*
 * FMAX / FMIN / FMAXNM / FMINNM (scalar single or double). For two ordered
 * operands that FZ cannot flush, all four are the same selection, raise
 * nothing and return one operand unchanged: MAXSS/MINSS when the operands
 * differ, and for equal operands (identical, or zeros of either sign) the
 * AND (max: +0) or OR (min: -0) of the two. Two cases take the helper with
 * the operands untouched: a denormal operand (under FZ it is flushed and
 * raises IDC; found on the bits, 0 < 2|x| < 2 * minimum normal, before any
 * SSE instruction sees it) and a NaN (UCOMIS unordered; it raises IE only
 * for a signalling NaN, as FPProcessNaNs then does).
 */
static void fp_maxmin(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    int dbl = BITS(i, 23, 22) == 1, min = BIT(i, 12);
    size_t den[2];
    fl_clobber(t);
    if (dbl) x_mov_ri(&t->b, RCX, 0x001FFFFFFFFFFFFFull);
    for (int k = 0; k < 2; k++) {
        x_mov_ld(&t->b, dbl ? 8 : 4, RAX, VR(k ? rm : rn));
        x_alu_rr(&t->b, XADD, dbl, RAX, RAX);                  /* 2|x|: the sign leaves */
        x_alu_ri(&t->b, XSUB, dbl, RAX, 1);                    /* zero wraps to the top */
        if (dbl) x_alu_rr(&t->b, XCMP, 1, RAX, RCX);
        else x_alu_ri(&t->b, XCMP, 0, RAX, 0x00FFFFFF);
        den[k] = x_jcc32(&t->b, XCC_B);
    }
    fp_load(t, 0, rn, dbl);
    fp_load(t, 1, rm, dbl);
    x_sse_rr(&t->b, dbl ? 0x66 : 0, 0x2E, 0, 0, 1);            /* ucomis xmm0, xmm1 */
    size_t nan = x_jcc32(&t->b, XCC_P);
    size_t differ = x_jcc8(&t->b, XCC_NE);
    x_sse_rr(&t->b, 0, min ? 0x56 : 0x54, 0, 0, 1);            /* orps / andps xmm0, xmm1 */
    size_t same = x_jmp8(&t->b);
    x_patch8(&t->b, differ, xb_len(&t->b));
    x_sse_rr(&t->b, dbl ? 0xF2 : 0xF3, min ? 0x5D : 0x5F, 0, 0, 1);   /* mins / maxs xmm0, xmm1 */
    x_patch8(&t->b, same, xb_len(&t->b));
    fp_store(t, rd, 0);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, den[0], xb_len(&t->b));
    x_patch32(&t->b, den[1], xb_len(&t->b));
    x_patch32(&t->b, nan, xb_len(&t->b));
    helper(t, i, pc, true);
    st_helper--;                                               /* counted as native */
    x_patch32(&t->b, done, xb_len(&t->b));
}

/*
 * FCVTZS / FCVTZU Wd|Xd, Sn|Dn. CVTTSS2SI/CVTTSD2SI truncate as FPToFixed
 * does and raise PE for IXC, but answer the integer indefinite (the minimum
 * integer, with IE) where A64 saturates. The inline path is therefore taken
 * only for results it can vouch for; everything else re-executes in the
 * helper, and what the declined attempt left in MXCSR is what the helper
 * raises again:
 *  - signed: a result equal to the minimum integer (CMP result, 1 overflows
 *    for that value only) is NaN or out of range (IE; A64 IOC) or the true
 *    minimum (nothing, or PE and IXC alike for a double just below it);
 *  - unsigned: the operand's bits are admitted first, 0 <= x < 2^32 or 2^63
 *    (one unsigned compare: negative values, NaNs and infinities are
 *    above), because a negative fraction or a double beyond 32 bits would
 *    raise PE where A64 raises only IOC. The admitted value converts as a
 *    signed 64-bit one;
 *  - a zero result from a denormal operand: A64 under FZ raises IDC and not
 *    IXC, so a zero result checks the operand for a zero exponent with a
 *    nonzero fraction.
 */
static void fp_to_int_z(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int dbl = BITS(i, 23, 22) == 1, sf = BIT(i, 31), uns = BIT(i, 16), w = sf || uns;
    size_t out = 0, slow;
    fl_clobber(t);
    if (uns) {
        x_mov_ld(&t->b, dbl ? 8 : 4, RAX, VR(rn));
        if (dbl) {
            x_mov_ri(&t->b, RCX, sf ? 0x43E0000000000000ull : 0x41F0000000000000ull);
            x_alu_rr(&t->b, XCMP, 1, RAX, RCX);
        } else x_alu_ri(&t->b, XCMP, 0, RAX, sf ? 0x5F000000 : 0x4F800000);
        out = x_jcc32(&t->b, XCC_AE);
    }
    fp_load(t, 0, rn, dbl);
    x_sse_rr(&t->b, dbl ? 0xF2 : 0xF3, 0x2C, w, RAX, 0);       /* cvttss2si / cvttsd2si */
    if (!uns) {
        x_alu_ri(&t->b, XCMP, sf, RAX, 1);
        out = x_jcc32(&t->b, XCC_O);
    }
    x_test_rr(&t->b, w, RAX, RAX);
    size_t nonzero = x_jcc8(&t->b, XCC_NE);
    x_mov_ld(&t->b, dbl ? 8 : 4, RCX, VR(rn));
    x_alu_rr(&t->b, XADD, dbl, RCX, RCX);                      /* drop the sign */
    size_t zero = x_jcc8(&t->b, XCC_E);
    x_shift_ri(&t->b, XSHR, dbl, RCX, dbl ? 53 : 24);          /* the exponent */
    slow = x_jcc32(&t->b, XCC_E);
    x_patch8(&t->b, nonzero, xb_len(&t->b));
    x_patch8(&t->b, zero, xb_len(&t->b));
    stx(t, rd, RAX, false);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, out, xb_len(&t->b));
    x_patch32(&t->b, slow, xb_len(&t->b));
    helper(t, i, pc, true);
    st_helper--;                                               /* counted as native */
    x_patch32(&t->b, done, xb_len(&t->b));
}

/*
 * Packed single conversions (.4S and .2S; a .2S
 * operand is loaded with its upper half zero, which every path below maps to
 * zero and no exception):
 *   SCVTF      CVTDQ2PS: one rounding per lane in the MXCSR mode, PE for IXC.
 *   UCVTF      the same when no lane has bit 31 set; otherwise the helper.
 *   FCVTZS     CVTTPS2DQ. A lane answering the integer indefinite was a NaN
 *              or out of range (IE; A64 raises IOC and saturates) or the
 *              true minimum: the helper redoes the instruction and raises
 *              the same flags again.
 * FCVTZS first sends every operand with a denormal lane to the
 * helper (FZ flushes it and raises IDC). The lane
 * test 2|x| - 1 < 2^24 also catches the minimum normal number, which the
 * helper handles as exactly.
 */
static bool t_simd_fpconv(tctx *t, uint32_t i, uint64_t pc)
{
#define PI(op, d, s_) x_sse_rr(&t->b, 0x66, op, 0, d, s_)
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    uint32_t key = i & 0xBFFFFC00;
    enum { SCVTF = 0x0E21D800, UCVTF = 0x2E21D800, FCVTZS = 0x0EA1B800 };
    if (key != SCVTF && key != UCVTF && key != FCVTZS) return false;
    size_t slow[2];
    int nslow = 0;
    if (key != SCVTF) fl_clobber(t);
    if (BIT(i, 30)) x_movdqu_ld(&t->b, 0, VR(rn)); else x_movq_ld(&t->b, 0, VR(rn));
    if (key == SCVTF) x_sse_rr(&t->b, 0, 0x5B, 0, 0, 0);       /* cvtdq2ps xmm0, xmm0 */
    else if (key == UCVTF) {
        x_sse_rr(&t->b, 0, 0x50, 0, RAX, 0);                   /* movmskps eax, xmm0 */
        x_test_rr(&t->b, 0, RAX, RAX);
        slow[nslow++] = x_jcc32(&t->b, XCC_NE);
        x_sse_rr(&t->b, 0, 0x5B, 0, 0, 0);
    } else {
        PI(0x6F, 1, 0);                                         /* movdqa xmm1, xmm0 */
        PI(0xFE, 1, 1);                                         /* paddd: 2|x| */
        PI(0x76, 2, 2);                                         /* pcmpeqd xmm2: all ones */
        PI(0xFE, 1, 2);                                         /* - 1 */
        x_sse_shift_ri(&t->b, 0x72, 2, 1, 24);                 /* psrld: zero for a tiny lane */
        PI(0xEF, 3, 3);                                         /* pxor xmm3: zero */
        PI(0x76, 1, 3);                                         /* pcmpeqd xmm1, xmm3 */
        PI(0xD7, RAX, 1);                                       /* pmovmskb eax, xmm1 */
        x_test_rr(&t->b, 0, RAX, RAX);
        slow[nslow++] = x_jcc32(&t->b, XCC_NE);
        x_sse_rr(&t->b, 0xF3, 0x5B, 0, 0, 0);                  /* cvttps2dq xmm0, xmm0 */
        x_sse_shift_ri(&t->b, 0x72, 6, 2, 31);                   /* xmm2 = 0x80000000 */
        PI(0x76, 2, 0);
        PI(0xD7, RAX, 2);
        x_test_rr(&t->b, 0, RAX, RAX);
        slow[nslow++] = x_jcc32(&t->b, XCC_NE);
    }
    fp_store(t, rd, 0);
    if (nslow) {
        size_t done = x_jmp32(&t->b);
        for (int k = 0; k < nslow; k++) x_patch32(&t->b, slow[k], xb_len(&t->b));
        helper(t, i, pc, true);
        st_helper--;                                           /* counted as native */
        x_patch32(&t->b, done, xb_len(&t->b));
    }
    return true;
#undef PI
}

static void t_vfp_fma(tctx *t, bool sub, unsigned rd, unsigned rn, unsigned rm, int idx, int Q, uint32_t insn, uint64_t pc);
static void t_scalar_indexed_mul(tctx *t, uint32_t i, uint64_t pc, int dbl, int idx);
enum { RI_N, RI_M, RI_P, RI_Z, RI_A, RI_X, RI_I };
static void t_vfp_rint(tctx *t, int mode, int dbl, int width, unsigned rd, unsigned rn, uint32_t insn, uint64_t pc);

static bool t_tail_conversion(tctx *t, uint32_t i, uint64_t pc);
static bool t_half_scalar(tctx *t, uint32_t i, uint64_t pc);

static void t_fccmp(tctx *t, uint32_t i, uint64_t pc);

static bool t_fp(tctx *t, uint32_t i, uint64_t pc)
{
    if (t_tail_conversion(t, i, pc)) return true;
    unsigned type = BITS(i, 23, 22), rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    if (BIT(i, 31) && (i & 0x5F20FC00) != 0x1E200000) return false;
    if (type == 3) return t_half_scalar(t, i, pc);
    if (type > 1) return false;
    int dbl = type == 1, pfx = dbl ? 0xF2 : 0xF3;
    if ((i & 0xFFC00000) == 0x1F000000) {
        t_vfp_fma(t, BIT(i, 15) != BIT(i, 21), rd, rn, rm, -1, 0, i, pc);
        return true;
    }

    if ((i & 0xff200c00) == 0x1e200400) { t_fccmp(t, i, pc); return true; }
    if ((i & 0xFF200C00) == 0x1E200800) {            /* 2-source: FMUL FDIV FADD FSUB (FNMUL) */
        static const uint8_t ops[4] = {0x59, 0x5E, 0x58, 0x5C};
        unsigned op = BITS(i, 15, 12);
        if (op >= 4 && op <= 7) { fp_maxmin(t, i, pc); return true; }
        if (op == 8 && !dbl) { t_scalar_indexed_mul(t, i, pc, 0, 0); return true; }
        if (op > 3) return false;
        size_t decline[2];
        unsigned nd = fp_begin_checked(t, dbl, rn, (int)rm, i, decline);
        fp_load(t, 0, rn, dbl);
        fp_load(t, 1, rm, dbl);
        x_sse_rr(&t->b, pfx, ops[op], 0, 0, 1);
        fp_store_checked(t, rd, dbl, i, pc, decline, nd);
        return true;
    }
    if ((i & 0xFF207C00) == 0x1E204000) {            /* 1-source */
        unsigned op = BITS(i, 20, 15);
        switch (op) {
        case 0:  /* FMOV */
            fp_load(t, 0, rn, dbl); fp_store(t, rd, 0); return true;
        case 1: case 2: { /* FABS / FNEG via GPR, flag-free (btr/btc would clobber) */
            x_mov_ld(&t->b, dbl ? 8 : 4, RAX, VR(rn));
            x_mov_ri(&t->b, RCX, dbl ? 0x8000000000000000ull : 0x80000000ull);
            if (op == 1) {
                x_unary(&t->b, XNOT, 1, RCX);
                /* and/xor clobber flags: do it in xmm instead */
                x_sse_rr(&t->b, 0x66, 0x6E, 1, 0, RAX);           /* movq xmm0, rax */
                x_sse_rr(&t->b, 0x66, 0x6E, 1, 1, RCX);           /* movq xmm1, rcx */
                x_sse_rr(&t->b, 0x66, 0x54, 0, 0, 1);             /* andpd */
            } else {
                x_sse_rr(&t->b, 0x66, 0x6E, 1, 0, RAX);
                x_sse_rr(&t->b, 0x66, 0x6E, 1, 1, RCX);
                x_sse_rr(&t->b, 0x66, 0x57, 0, 0, 1);             /* xorpd */
            }
            if (!dbl) { x_sse_rr(&t->b, 0x66, 0x7E, 0, 0, RAX); x_sse_rr(&t->b, 0x66, 0x6E, 0, 0, RAX); } /* zero bits 32..63 */
            fp_store(t, rd, 0);
            return true;
        }
        case 3: { /* FSQRT */
            size_t decline[2];
            unsigned nd = fp_begin_checked(t, dbl, rn, -1, i, decline);
            fp_load(t, 0, rn, dbl);
            x_sse_rr(&t->b, pfx, 0x51, 0, 0, 0);
            fp_store_checked(t, rd, dbl, i, pc, decline, nd);
            return true;
        }
        case 4: case 5: { /* FCVT to single / double */
            if ((op == 4) == !dbl) return false;
            size_t decline[2];
            unsigned nd = fp_begin_checked(t, dbl, rn, -1, i, decline);
            fp_load(t, 0, rn, dbl);
            x_sse_rr(&t->b, pfx, 0x5A, 0, 1, 0);                  /* cvtsd2ss / cvtss2sd xmm1, xmm0 */
            x_sse_rr(&t->b, 0x66, 0xEF, 0, 0, 0);                 /* pxor xmm0 */
            if (op == 4) x_sse_rr(&t->b, 0xF3, 0x10, 0, 0, 1);    /* movss xmm0, xmm1 */
            else x_sse_rr(&t->b, 0xF2, 0x10, 0, 0, 1);            /* movsd xmm0, xmm1 */
            fp_store_checked(t, rd, op == 5, i, pc, decline, nd);
            return true;
        }
        case 8: case 9: case 10: case 11: case 12: case 14: case 15: {   /* FRINTN P M Z A X I */
            static const int mode[8] = {RI_N, RI_P, RI_M, RI_Z, RI_A, -1, RI_X, RI_I};
            t_vfp_rint(t, mode[op - 8], dbl, dbl ? 8 : 4, rd, rn, i, pc);
            return true;
        }
        default: return false;
        }
    }
    if ((i & 0xFF20FC07) == 0x1E202000) {           /* FCMP / FCMPE (Rm or #0.0) */
        size_t decline[2];
        unsigned nd = fp_begin_checked(t, dbl, rn, BIT(i, 3) ? -1 : (int)rm, i, decline);
        fp_load(t, 0, rn, dbl);
        if (BIT(i, 3)) x_sse_rr(&t->b, 0x66, 0xEF, 0, 1, 1);     /* pxor xmm1 (compare with zero) */
        else fp_load(t, 1, rm, dbl);
        /* FCMP: ucomis (IOC for signalling NaNs only); FCMPE: comis (any NaN) */
        x_sse_rr(&t->b, dbl ? 0x66 : 0, BIT(i, 4) ? 0x2F : 0x2E, 0, 0, 1);
        x_setcc(&t->b, XCC_P, RAX);
        x_setcc(&t->b, XCC_E, RCX);
        x_setcc(&t->b, XCC_B, RDX);
        x_movzx8_rr(&t->b, RAX, RAX);
        x_movzx8_rr(&t->b, RCX, RCX);
        x_movzx8_rr(&t->b, RDX, RDX);
        x_lea(&t->b, 0, RAX, xmi(RDX, RAX, 4, 0));
        x_lea(&t->b, 0, RAX, xmi(RAX, RCX, 2, 0));
        x_mov_ld(&t->b, 8, RAX, xmi(RBX, RAX, 8, CPU_OFF_FCMP));
        x_mov_st(&t->b, 8, FLAGS, RAX);
        t->in_eflags = false;
        t->dirty = false;
        size_t done = x_jmp32(&t->b);
        for (unsigned k = 0; k < nd; k++) x_patch32(&t->b, decline[k], xb_len(&t->b));
        helper(t, i, pc, true);
        st_helper--;
        x_patch32(&t->b, done, xb_len(&t->b));
        return true;
    }
    if ((i & 0xFF200C00) == 0x1E200C00) {            /* FCSEL */
        unsigned cond = BITS(i, 15, 12);
        if (cond < 14) fl_load(t);
        x_mov_ld(&t->b, dbl ? 8 : 4, RAX, VR(rm));
        x_mov_ld(&t->b, dbl ? 8 : 4, RCX, VR(rn));
        if (cond < 14) x_cmov_rr(&t->b, a64_cond_to_xcc(cond), 1, RAX, RCX);
        else x_mov_rr(&t->b, 1, RAX, RCX);
        x_mov_st(&t->b, 8, VR(rd), RAX);
        x_mov_mi(&t->b, 8, xm(RBX, CPU_OFF_V(rd) + 8), 0);
        if (!dbl) x_mov_mi(&t->b, 4, xm(RBX, CPU_OFF_V(rd) + 4), 0);
        return true;
    }
    if ((i & 0x7F3FFC00) == 0x1E220000) {            /* SCVTF (scalar, integer) */
        int sf = BIT(i, 31);
        if (rn == 31) x_mov_ri(&t->b, RAX, 0);
        else if (sf) x_mov_ld(&t->b, 8, RAX, XR(rn));
        else x_movs_ld(&t->b, 4, 1, RAX, XR(rn));
        x_sse_rr(&t->b, 0x66, 0xEF, 0, 0, 0);                    /* pxor xmm0 */
        x_sse_rr(&t->b, pfx, 0x2A, 1, 0, RAX);                   /* cvtsi2sd/ss xmm0, rax */
        fp_store(t, rd, 0);
        return true;
    }
    if ((i & 0x7F3FFC00) == 0x1E230000) {            /* UCVTF (scalar, integer) */
        /* Zero-extended to a non-negative int64, the signed host conversion
         * is the unsigned one: one rounding in the MXCSR mode, PE for IXC.
         * An Xn with bit 63 set is beyond it and takes the helper. */
        int sf = BIT(i, 31);
        size_t big = 0;
        if (sf) fl_clobber(t);
        ldx(t, RAX, rn, sf, false);
        if (sf) {
            x_test_rr(&t->b, 1, RAX, RAX);
            big = x_jcc32(&t->b, XCC_S);
        }
        x_sse_rr(&t->b, 0x66, 0xEF, 0, 0, 0);                    /* pxor xmm0 */
        x_sse_rr(&t->b, pfx, 0x2A, 1, 0, RAX);                   /* cvtsi2sd/ss xmm0, rax */
        fp_store(t, rd, 0);
        if (sf) {
            size_t done = x_jmp32(&t->b);
            x_patch32(&t->b, big, xb_len(&t->b));
            helper(t, i, pc, true);
            st_helper--;                                         /* counted as native */
            x_patch32(&t->b, done, xb_len(&t->b));
        }
        return true;
    }
    if ((i & 0x7F3EFC00) == 0x1E380000) {            /* FCVTZS / FCVTZU (scalar, integer) */
        fp_to_int_z(t, i, pc);
        return true;
    }
    if ((i & 0xFF201FE0) == 0x1E201000) {            /* FMOV (scalar, immediate): VFPExpandImm */
        uint64_t imm8 = BITS(i, 20, 13), sgn = imm8 >> 7, b6 = (imm8 >> 6) & 1, frac = imm8 & 0x3f;
        uint64_t v = dbl ? sgn << 63 | (uint64_t)!b6 << 62 | (b6 ? 0xffull : 0) << 54 | frac << 48
                         : sgn << 31 | (uint64_t)!b6 << 30 | (b6 ? 0x1full : 0) << 25 | frac << 19;
        x_mov_ri(&t->b, RAX, v);
        x_mov_st(&t->b, 8, VR(rd), RAX);
        x_mov_mi(&t->b, 8, xm(RBX, CPU_OFF_V(rd) + 8), 0);
        return true;
    }
    if ((i & 0x7F3FFC00) == 0x1E260000) {            /* FMOV Rd, Fn */
        int sf = BIT(i, 31);
        if ((sf != 0) != (dbl != 0)) return false;
        x_mov_ld(&t->b, dbl ? 8 : 4, RAX, VR(rn));
        stx(t, rd, RAX, false);
        return true;
    }
    if ((i & 0x7F3FFC00) == 0x1E270000) {            /* FMOV Fd, Rn */
        int sf = BIT(i, 31);
        if ((sf != 0) != (dbl != 0)) return false;
        ldx(t, RAX, rn, sf, false);
        x_mov_st(&t->b, 8, VR(rd), RAX);
        x_mov_mi(&t->b, 8, xm(RBX, CPU_OFF_V(rd) + 8), 0);
        return true;
    }
    return false;
}

/* EOR3 / BCAX (FEAT_SHA3; 1100 1110 00 op Rm 0 Ra Rn Rd): obfuscated code
 * runs EOR3 through its hot loops, so the two bitwise forms are native;
 * PXOR/PANDN leave EFLAGS alone. SHA*, RAX1 and XAR retain their helper;
 * the separate AES class uses t_aes when CPUID advertises AES-NI. */
static bool t_eor3_bcax(tctx *t, uint32_t i)
{
    unsigned rm = BITS(i, 20, 16), ra = BITS(i, 14, 10), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!BIT(i, 21)) {                                       /* EOR3: Vn ^ Vm ^ Va */
        x_movdqu_ld(&t->b, 0, VR(rn));
        x_movdqu_ld(&t->b, 1, VR(rm));
        x_sse_rr(&t->b, 0x66, 0xEF, 0, 0, 1);                /* pxor */
        x_movdqu_ld(&t->b, 1, VR(ra));
    } else {                                                 /* BCAX: Vn ^ (Vm & ~Va) */
        x_movdqu_ld(&t->b, 1, VR(ra));
        x_movdqu_ld(&t->b, 0, VR(rm));
        x_sse_rr(&t->b, 0x66, 0xDF, 0, 1, 0);                /* pandn: xmm1 = ~xmm1 & xmm0 */
        x_movdqu_ld(&t->b, 0, VR(rn));
    }
    x_sse_rr(&t->b, 0x66, 0xEF, 0, 0, 1);                    /* pxor */
    x_movdqu_st(&t->b, VR(rd), 0);
    return true;
}

/* ---- Advanced SIMD integer forms on SSE2 and the x86-64-v2 additions
 * (SSSE3 PSHUFB, PALIGNR, PABSB/W/D; SSE4.1 PMULLD, PMINSB/SD/UW/UD,
 * PMAXSB/SD/UW/UD, PCMPEQQ; SSE4.2 PCMPGTQ). V registers live in memory; an
 * instruction loads its operands into xmm0-xmm4 and xmm8-xmm9 (unaligned
 * moves: nothing here relies on hao_cpu's alignment), computes, and stores
 * all 16 bytes of Vd, the upper half zero for a 64-bit vector or a scalar.
 * SSE integer instructions leave EFLAGS alone. Only allocated encodings are
 * taken; saturating forms publish FPSR.QC without touching EFLAGS, and
 * the remaining forms stay with the helper. */
#define P(op, dst, src) x_sse_rr(&t->b, 0x66, op, 0, dst, src)
static void vld(tctx *t, int xmm, unsigned r) { x_movdqu_ld(&t->b, xmm, VR(r)); }
static void vst(tctx *t, unsigned rd, int xmm, int Q)
{
    if (!Q) x_sse_rr(&t->b, 0xF3, 0x7E, 0, xmm, xmm);            /* movq xmm, xmm: clear the upper half */
    x_movdqu_st(&t->b, VR(rd), xmm);
}
static void vzero(tctx *t, int xmm) { P(0xEF, xmm, xmm); }
static void vones(tctx *t, int xmm) { P(0x76, xmm, xmm); }
static void vnot(tctx *t, int xmm, int tmp) { vones(t, tmp); P(0xEF, xmm, tmp); }
/* xmm = v in the low half and, if both, in the upper half too (rax scratch) */
static void vconst(tctx *t, int xmm, uint64_t v, bool both)
{
    if (!v) { vzero(t, xmm); return; }
    if (v == ~0ull && both) { vones(t, xmm); return; }
    x_mov_ri(&t->b, RAX, v);
    x_sse_rr(&t->b, 0x66, 0x6E, 1, xmm, RAX);                    /* movq xmm, rax */
    if (both) P(0x6C, xmm, xmm);                                 /* punpcklqdq */
}
static xmem velem(unsigned r, int es, int idx) { return xm(RBX, CPU_OFF_V(r) + idx * es); }

static const uint8_t padd[4] = {0xFC, 0xFD, 0xFE, 0xD4}, psub[4] = {0xF8, 0xF9, 0xFA, 0xFB};
static const uint8_t pcmpeq[3] = {0x74, 0x75, 0x76}, pcmpgt[3] = {0x64, 0x65, 0x66};

/* The x86-64-v2 forms (SSSE3, SSE4.1, SSE4.2), which hao requires of the
 * CPU (src/main.c refuses to start without them); never VEX. */
#define P38(op, dst, src) x_sse38_rr(&t->b, op, dst, src)
/* ARM's AddRoundKey precedes SubBytes/ShiftRows. x86's last-round
 * instructions put it afterwards, so XOR first and pass a zero round key.
 * AESMC cancels InvSubBytes/InvShiftRows with AESENC's forward transforms,
 * leaving exactly MixColumns. No instruction changes EFLAGS or FPSR. */
static bool t_aes(tctx *t, uint32_t i)
{
    unsigned size = BITS(i, 23, 22), op = BITS(i, 16, 12), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!jit_aes || size || op < 4 || op > 7) return false;
    if (op < 6) {
        vld(t, 0, rd); vld(t, 1, rn); P(0xEF, 0, 1);          /* ARM AddRoundKey */
        vzero(t, 1); P38(op == 4 ? 0xDD : 0xDF, 0, 1);       /* aesenclast / aesdeclast */
    } else {
        vld(t, 0, rn);
        if (op == 7) P38(0xDB, 0, 0);                         /* aesimc */
        else {
            vzero(t, 1); P38(0xDF, 0, 1); P38(0xDC, 0, 1);  /* aesdeclast, aesenc: MixColumns */
        }
    }
    vst(t, rd, 0, 1);
    return true;
}

/* lane compares in every size: pcmpeqq (SSE4.1), pcmpgtq (SSE4.2) */
static void vcmpeq(tctx *t, unsigned size, int d, int s) { if (size == 3) P38(0x29, d, s); else P(pcmpeq[size], d, s); }
static void vcmpgt(tctx *t, unsigned size, int d, int s) { if (size == 3) P38(0x37, d, s); else P(pcmpgt[size], d, s); }
/* xmm d and s with the sign bit of each 32- or 64-bit lane flipped: the
 * signed order of the results is the unsigned order of the inputs */
static void vbias(tctx *t, unsigned size, int d, int s, int tmp)
{
    vconst(t, tmp, size == 3 ? 0x8000000000000000ull : 0x8000000080000000ull, true);
    P(0xEF, d, tmp);
    P(0xEF, s, tmp);
}
/* xmm0 = xmm0 * xmm1 in each lane, modulo the lane size (MUL); xmm2-xmm3
 * scratch. Bytes: the even bytes are the low bytes of pmullw's words, the
 * odd ones the high bytes of (a >> 8) * (b & 0xff00). */
static void vmul(tctx *t, unsigned size)
{
    if (size == 2) { P38(0x40, 0, 1); return; }                  /* pmulld */
    if (size == 1) { P(0xD5, 0, 1); return; }                   /* pmullw */
    P(0x6F, 2, 0); P(0xD5, 2, 1);
    vconst(t, 3, 0x00ff00ff00ff00ffull, true);
    P(0xDB, 2, 3);                                               /* the even bytes' products */
    x_sse_shift_ri(&t->b, 0x71, 2, 0, 8);                        /* psrlw 8: the odd bytes of a */
    P(0xDF, 3, 1);                                               /* pandn: the odd bytes of b, in place */
    P(0xD5, 0, 3);
    P(0xEB, 0, 2);
}

/* Widen exactly the selected low/upper half. The wide-input forms never
 * shift Vn: Q selects only the narrow operand. */
static void vwiden(tctx *t, int r, unsigned size, unsigned U, unsigned Q)
{
    static const uint8_t ops[] = {0x20, 0x23, 0x25};
    if (Q) x_sse_shift_ri(&t->b, 0x73, 3, r, 8);
    P38((uint8_t)(ops[size] + (U ? 0x10 : 0)), r, r);
}

static bool t_simd_three_diff(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 12);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    if (size == 3 || (op > 3 && op != 8 && op != 10 && op != 12)) return false;
    vld(t, 0, rn); vld(t, 1, rm);
    if (!(op == 1 || op == 3)) vwiden(t, 0, size, U, Q);
    vwiden(t, 1, size, U, Q);
    if (op <= 3) P(op < 2 ? padd[size + 1] : psub[size + 1], 0, 1);
    else {
        if (size == 0) P(0xD5, 0, 1);
        else if (size == 1) P38(0x40, 0, 1);
        else if (U) P(0xF4, 0, 1); else P38(0x28, 0, 1); /* low dwords of widened qwords */
        if (op != 12) { vld(t, 1, rd); P(op == 8 ? padd[size + 1] : psub[size + 1], 1, 0); P(0x6F, 0, 1); }
    }
    vst(t, rd, 0, 1);
    return true;
}

/* Sticky FPSR.QC from a byte mask, without an EFLAGS write. Only active
 * lanes may be present in mask. The branch is short and its store preserves
 * every other software FPSR bit as well as all MXCSR exception flags. */
static void vset_qc(tctx *t, int mask)
{
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, mask);
    size_t clean = x_jrcxz8(&t->b);
    x_movd_ld(&t->b, 3, xm(RBX, offsetof(hao_cpu, fpsr)));
    vconst(t, 4, 1u << 27, false); P(0xEB, 3, 4);
    x_movd_st(&t->b, xm(RBX, offsetof(hao_cpu, fpsr)), 3);
    x_patch8(&t->b, clean, xb_len(&t->b));
}

/* SQ(RD)MULH: doubled signed products, optionally rounded, saturate only
 * for min * min. PMULHRSW gives the rounded halfword result except that
 * one overflow; full signed dword products fit PMULDQ's qword lanes. */
static void t_simd_mulhigh(tctx *t, unsigned rd, unsigned rn, unsigned rm, unsigned size, unsigned Q, bool round, int idx)
{
    vld(t, 0, rn); vld(t, 1, rm);
    if (idx >= 0) {
        if (size == 1) {
            if (idx >= 4) x_sse_shift_ri(&t->b, 0x73, 3, 1, 8);
            x_sse_rri(&t->b, 0xF2, 0x70, 1, 1, (uint8_t)(0x55 * (idx & 3)));
            P(0x6C, 1, 1);
        } else x_sse_rri(&t->b, 0x66, 0x70, 1, 1, (uint8_t)(0x55 * idx));
    }
    vconst(t, 2, size == 1 ? 0x8000800080008000ull : 0x8000000080000000ull, true);
    P(0x6F, 8, 0); P(pcmpeq[size], 8, 2);
    P(0x6F, 9, 1); P(pcmpeq[size], 9, 2); P(0xDB, 8, 9); /* overflow */
    if (size == 1 && round) P38(0x0B, 0, 1);
    else if (size == 1) {
        P(0x6F, 2, 0); P(0xD5, 2, 1); P(0xE5, 0, 1); /* low/high signed product */
        x_sse_shift_ri(&t->b, 0x71, 6, 0, 1);
        x_sse_shift_ri(&t->b, 0x71, 2, 2, 15); P(0xEB, 0, 2);
    } else {
        P(0x6F, 2, 0); P(0x6F, 3, 1);
        x_sse_shift_ri(&t->b, 0x73, 2, 2, 32); x_sse_shift_ri(&t->b, 0x73, 2, 3, 32);
        P38(0x28, 0, 1); P38(0x28, 2, 3);
        if (round) { vconst(t, 4, 1ull << 30, true); P(0xD4, 0, 4); P(0xD4, 2, 4); }
        /* Logical shifts suffice: only the low 32 bits survive narrowing. */
        x_sse_shift_ri(&t->b, 0x73, 2, 0, 31); x_sse_shift_ri(&t->b, 0x73, 2, 2, 31);
        x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0x88);
        x_sse_rri(&t->b, 0x66, 0x70, 2, 2, 0x88); P(0x62, 0, 2);
    }
    P(0xEF, 0, 8); /* min * min wrapped to min: complement to max */
    if (!Q) x_sse_rr(&t->b, 0xF3, 0x7E, 0, 8, 8);
    vset_qc(t, 8);
    vst(t, rd, 0, (int)Q);
}

/* Per-lane signed low-byte shifts, without a host flag write. SSE's
 * oversized logical shift gives zero, including a negative count encoded
 * as an unsigned 64-bit value. A separate right candidate uses -count and
 * is selected only for a negative count. Stage every lane before Vd writes
 * so aliases read the original source and shift vector throughout. */
static void t_simd_register_shift(tctx *t, unsigned rd, unsigned rn, unsigned rm, unsigned size, unsigned Q, unsigned U)
{
    int es = 1 << size, lanes = (Q ? 16 : 8) / es;
    vzero(t, 8);
    for (int lane = 0; lane < lanes; lane++) {
        if (size == 3) x_movq_ld(&t->b, 0, velem(rn, es, lane));
        else x_movd_ld(&t->b, 0, velem(rn, es, lane));
        x_movs_ld(&t->b, 1, 1, RAX, velem(rm, es, lane));
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 1, RAX);                 /* signed low byte */
        P(0x6F, 4, 0); P(size == 3 ? 0xF3 : 0xF2, 4, 1);      /* left candidate */
        vzero(t, 2); P(0xFB, 2, 1);                             /* register shift: -count */
        vzero(t, 3); P38(0x37, 3, 1);                           /* count < 0 */
        if (U) P(size == 3 ? 0xD3 : 0xD2, 0, 2);               /* logical right */
        else if (size == 2) P(0xE2, 0, 2);                      /* arithmetic right dword */
        else {
            vzero(t, 1); P38(0x37, 1, 0);                       /* sign mask */
            P(0xEF, 0, 1); P(0xD3, 0, 2); P(0xEF, 0, 1);      /* arithmetic right qword */
        }
        P(0xDB, 0, 3); P(0xEB, 0, 4);
        if (lane) x_sse_shift_ri(&t->b, 0x73, 7, 0, (uint8_t)(lane * es));
        P(0xEB, 8, 0);
    }
    vst(t, rd, 8, (int)Q);
}

static bool t_simd_three_same(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 11);
    unsigned rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (op == 3) {                                               /* logical: size selects the operation */
        if (!U) switch (size) {
        case 0: vld(t, 0, rn); vld(t, 1, rm); P(0xDB, 0, 1); break;                             /* AND */
        case 1: vld(t, 0, rm); vld(t, 1, rn); P(0xDF, 0, 1); break;                             /* BIC: ~m & n */
        case 2: vld(t, 0, rn); if (rm != rn) { vld(t, 1, rm); P(0xEB, 0, 1); } break;           /* ORR, MOV */
        default: vld(t, 0, rm); vnot(t, 0, 1); vld(t, 1, rn); P(0xEB, 0, 1); break;             /* ORN */
        } else switch (size) {
        case 0: vld(t, 0, rn); vld(t, 1, rm); P(0xEF, 0, 1); break;                             /* EOR */
        case 1: vld(t, 0, rm); vld(t, 1, rn); P(0xEF, 1, 0); vld(t, 2, rd); P(0xDB, 1, 2); P(0xEF, 0, 1); break; /* BSL: m ^ ((m ^ n) & d) */
        case 2: vld(t, 0, rd); vld(t, 1, rn); P(0xEF, 1, 0); vld(t, 2, rm); P(0xDB, 1, 2); P(0xEF, 0, 1); break; /* BIT: d ^ ((d ^ n) & m) */
        default: vld(t, 0, rd); vld(t, 1, rn); P(0xEF, 1, 0); vld(t, 2, rm); P(0xDF, 2, 1); P(0xEF, 0, 2); break; /* BIF: d ^ ((d ^ n) & ~m) */
        }
        vst(t, rd, 0, Q);
        return true;
    }
    if (size == 3 && !Q) return false;
    if (op == 8 && size >= 2) {
        t_simd_register_shift(t, rd, rn, rm, size, Q, U);
        return true;
    }
    if (op == 0x16) {
        if (size != 1 && size != 2) return false;
        t_simd_mulhigh(t, rd, rn, rm, size, Q, U, -1);
        return true;
    }
    switch (op) {
    case 0x10:                                                   /* ADD / SUB */
        vld(t, 0, rn); vld(t, 1, rm);
        P(U ? psub[size] : padd[size], 0, 1);
        break;
    case 0x11:                                                   /* CMTST / CMEQ */
        vld(t, 0, rn); vld(t, 1, rm);
        if (U) vcmpeq(t, size, 0, 1);
        else { P(0xDB, 0, 1); vzero(t, 1); vcmpeq(t, size, 0, 1); vnot(t, 0, 1); }
        break;
    case 0x06:                                                   /* CMGT / CMHI */
        vld(t, 0, rn); vld(t, 1, rm);
        if (!U) vcmpgt(t, size, 0, 1);
        else if (size < 2) { P(size ? 0xD9 : 0xD8, 0, 1); vzero(t, 1); P(pcmpeq[size], 0, 1); vnot(t, 0, 1); }   /* n -sat m != 0 */
        else { vbias(t, size, 0, 1, 2); vcmpgt(t, size, 0, 1); }
        break;
    case 0x07:                                                   /* CMGE: !(m > n) / CMHS */
        vld(t, 0, rm); vld(t, 1, rn);
        if (!U) { vcmpgt(t, size, 0, 1); vnot(t, 0, 1); }
        else if (size < 2) { P(size ? 0xD9 : 0xD8, 0, 1); vzero(t, 1); P(pcmpeq[size], 0, 1); }                 /* m -sat n == 0 */
        else { vbias(t, size, 0, 1, 2); vcmpgt(t, size, 0, 1); vnot(t, 0, 1); }
        break;
    case 0x0C: case 0x0D: {                                      /* SMAX/UMAX, SMIN/UMIN */
        /* [max/min][U][size]: pmaxsb pmaxsw pmaxsd / pmaxub pmaxuw pmaxud, and the
         * minima; the SSE2 ones (pmaxsw, pmaxub, ...) are two-byte opcodes */
        static const uint8_t o[2][2][3] = {{{0x3C, 0xEE, 0x3D}, {0xDE, 0x3E, 0x3F}}, {{0x38, 0xEA, 0x39}, {0xDA, 0x3A, 0x3B}}};
        if (size == 3) return false;                             /* reserved */
        uint8_t x = o[op & 1][U][size];
        vld(t, 0, rn); vld(t, 1, rm);
        if (size == (U ? 0u : 1u)) P(x, 0, 1); else P38(x, 0, 1);
        break;
    }
    case 0x12: case 0x13:                                        /* MLA / MLS, MUL (PMUL: helper) */
        if (size == 3 || (op == 0x13 && U)) return false;
        vld(t, 0, rn); vld(t, 1, rm);
        vmul(t, size);
        if (op == 0x12) {
            vld(t, 1, rd);
            P(U ? psub[size] : padd[size], 1, 0);
            vst(t, rd, 1, Q);
            return true;
        }
        break;
    default: return false;
    }
    vst(t, rd, 0, Q);
    return true;
}

static void vnarrow(tctx *t, int oes);
static void vst_narrow(tctx *t, unsigned rd, int Q);
static bool t_vfp_absneg(tctx *t, uint32_t i);
static bool t_vfp_rint_vec(tctx *t, uint32_t i, uint64_t pc);
static bool t_vfp_estimate(tctx *t, uint32_t i, uint64_t pc, bool scalar);

static bool t_simd_two_misc(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 16, 12);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (op == 0x1d && (size & 2)) return t_vfp_estimate(t, i, pc, false);
    if (op <= 1) { /* REV64 / REV32 / REV16 */
        unsigned cont = op == 1 ? 2 : U ? 4 : 8, es = 1u << size;
        if (es >= cont || (op == 1 && U)) return false;
        uint64_t lo = 0, hi = 0;
        for (unsigned b = 0; b < 16; b++) {
            unsigned source = (b & ~(cont - 1)) | ((b ^ (cont - es)) & (cont - 1));
            if (b < 8) lo |= (uint64_t)source << (8 * b);
            else hi |= (uint64_t)source << (8 * (b - 8));
        }
        vld(t, 0, rn); vconst(t, 1, lo, false); vconst(t, 2, hi, false); P(0x6C, 1, 2);
        P38(0x00, 0, 1); vst(t, rd, 0, Q);
        return true;
    }
    if (op == 0x13) { /* SHLL */
        if (!U || size == 3) return false;
        vld(t, 0, rn); vwiden(t, 0, size, 1, Q);
        x_sse_shift_ri(&t->b, (uint8_t)(0x71 + size), 6, 0, (uint8_t)(8u << size));
        vst(t, rd, 0, 1); return true;
    }
    if (op == 0x14 || (op == 0x12 && U)) { /* SQXTN / UQXTN / SQXTUN */
        if (size == 3) return false;
        unsigned bits = 8u << size, wide = size + 1;
        bool unsrc = op == 0x14 && U, undst = U;
        uint64_t max = (1ull << (bits - !undst)) - 1;
        uint64_t min = undst ? 0 : ~max;
        if (wide == 1) { max *= 0x0001000100010001ull; min = (min & 0xffff) * 0x0001000100010001ull; }
        if (wide == 2) { max *= 0x0000000100000001ull; min = (min & 0xffffffff) * 0x0000000100000001ull; }
        vld(t, 0, rn); P(0x6F, 8, 0);
        vconst(t, 1, max, true); P(0x6F, 2, 0);
        if (unsrc) {
            vconst(t, 3, wide == 1 ? 0x8000800080008000ull : wide == 2 ? 0x8000000080000000ull : 0x8000000000000000ull, true);
            P(0xEF, 2, 3); P(0xEF, 1, 3); vcmpgt(t, wide, 2, 1); P(0xEF, 1, 3);
        } else vcmpgt(t, wide, 2, 1);
        P(0xDB, 1, 2); P(0xDF, 2, 0); P(0xEB, 1, 2); P(0x6F, 0, 1); /* min(x,max) */
        if (!unsrc) {
            vconst(t, 1, min, true); P(0x6F, 2, 1); vcmpgt(t, wide, 2, 0);
            P(0xDB, 1, 2); P(0xDF, 2, 0); P(0xEB, 1, 2); P(0x6F, 0, 1); /* max(x,min) */
        }
        vcmpeq(t, wide, 8, 0); vnot(t, 8, 2); vset_qc(t, 8);
        vnarrow(t, 1 << size); vst_narrow(t, rd, Q);
        return true;
    }
    if (op == 2 || op == 6) { /* [SU]ADDLP / [SU]ADALP */
        if (size == 3) return false;
        static const uint8_t widen[3] = {0x20, 0x23, 0x25};
        vld(t, 0, rn); P(0x6F, 1, 0);
        /* Compact even/odd source lanes into the low half before widening. */
        for (int r = 0; r < 2; r++) {
            uint64_t mask = 0;
            for (unsigned b = 0; b < 8; b++)
                mask |= (uint64_t)((b >> size) * (2u << size) + (r << size) + (b & ((1u << size) - 1))) << (8 * b);
            vconst(t, 2, mask, false); P38(0x00, r, 2);
            P38((uint8_t)(widen[size] + (U ? 0x10 : 0)), r, r);
        }
        P(padd[size + 1], 0, 1);
        if (op == 6) { vld(t, 1, rd); P(padd[size + 1], 0, 1); }
        vst(t, rd, 0, (int)Q);
        return true;
    }
    if (op == 0x0F && (size & 2)) return t_vfp_absneg(t, i);    /* FABS / FNEG */
    if (op == 0x12 && !U) {                                      /* XTN, XTN2 (SQXTUN: helper) */
        if (size == 3) return false;                             /* unallocated */
        vld(t, 0, rn);
        vnarrow(t, 1 << size);
        vst_narrow(t, rd, (int)Q);
        return true;
    }
    if (op == 0x05) {
        if (size) return false;                                  /* RBIT: helper */
        vld(t, 0, rn);
        if (U) vnot(t, 0, 1);                                    /* NOT */
        else {
            /* CNT, per byte: x -= x >> 1 & 0x55; x = (x & 0x33) + (x >> 2 & 0x33);
             * x = x + (x >> 4) & 0x0f. The shifts are 16-bit; the masks drop
             * the bits that crossed a byte boundary. */
            vconst(t, 2, 0x5555555555555555ull, true);
            P(0x6F, 1, 0); x_sse_shift_ri(&t->b, 0x71, 2, 1, 1); P(0xDB, 1, 2); P(0xF8, 0, 1);
            vconst(t, 2, 0x3333333333333333ull, true);
            P(0x6F, 1, 0); x_sse_shift_ri(&t->b, 0x71, 2, 1, 2); P(0xDB, 1, 2); P(0xDB, 0, 2); P(0xFC, 0, 1);
            vconst(t, 2, 0x0f0f0f0f0f0f0f0full, true);
            P(0x6F, 1, 0); x_sse_shift_ri(&t->b, 0x71, 2, 1, 4); P(0xFC, 0, 1); P(0xDB, 0, 2);
        }
        vst(t, rd, 0, Q);
        return true;
    }
    if (op == 0x18 || op == 0x19) return t_vfp_rint_vec(t, i, pc);   /* FRINT* */
    if (size == 3 && (!Q || op < 0x08 || op > 0x0B)) return false;  /* 64-bit lanes: compares with zero, ABS, NEG */
    switch (op) {
    case 0x08:                                                   /* CMGT #0 / CMGE #0: !(0 > x) */
        if (!U) { vld(t, 0, rn); vzero(t, 1); vcmpgt(t, size, 0, 1); }
        else { vzero(t, 0); vld(t, 1, rn); vcmpgt(t, size, 0, 1); vnot(t, 0, 1); }
        break;
    case 0x09:                                                   /* CMEQ #0 / CMLE #0: !(x > 0) */
        vld(t, 0, rn); vzero(t, 1);
        if (!U) vcmpeq(t, size, 0, 1);
        else { vcmpgt(t, size, 0, 1); vnot(t, 0, 1); }
        break;
    case 0x0A:                                                   /* CMLT #0: 0 > x */
        if (U) return false;
        vzero(t, 0); vld(t, 1, rn); vcmpgt(t, size, 0, 1);
        break;
    case 0x0B:                                                   /* NEG, ABS */
        if (U) { vzero(t, 0); vld(t, 1, rn); P(psub[size], 0, 1); }
        else if (size < 3) { vld(t, 0, rn); P38((uint8_t)(0x1C + size), 0, 0); }   /* pabsb / pabsw / pabsd */
        else { vld(t, 0, rn); vzero(t, 1); vcmpgt(t, 3, 1, 0); P(0xEF, 0, 1); P(0xFB, 0, 1); }   /* (x ^ s) - s, s = 0 > x */
        break;
    default: return false;
    }
    vst(t, rd, 0, Q);
    return true;
}

/* ADDV wraps in its source lane width. UADDLV over bytes uses PSADBW
 * against zero to sum each half, then combines the two wide sums. */
static bool t_simd_across(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    unsigned size = BITS(i, 23, 22), op = BITS(i, 16, 12);
    if (op == 0x1b && !BIT(i, 29)) { /* ADDV, modulo the source lane width */
        if (size == 3 || (size == 2 && !Q)) return false;
        if (Q) vld(t, 0, rn); else x_movq_ld(&t->b, 0, VR(rn));
        for (unsigned bytes = Q ? 8 : 4; bytes >= (1u << size); bytes >>= 1) {
            P(0x6F, 1, 0); x_sse_shift_ri(&t->b, 0x73, 3, 1, (uint8_t)bytes);
            P(padd[size], 0, 1);                                  /* ADDV lane width */
        }
        vconst(t, 1, (1ull << (8u << size)) - 1, false); P(0xDB, 0, 1);
        vst(t, rd, 0, 1);
        return true;
    }
    if (!BIT(i, 29) || BITS(i, 23, 22) || BITS(i, 16, 12) != 0x03) return false;
    vld(t, 0, rn);
    if (!Q) x_sse_rr(&t->b, 0xF3, 0x7E, 0, 0, 0);
    vzero(t, 1);
    P(0xF6, 0, 1);                                               /* psadbw */
    if (Q) { x_sse_rri(&t->b, 0x66, 0x70, 1, 0, 0xEE); P(0xD4, 0, 1); }   /* add the upper half's sum */
    vst(t, rd, 0, 0);
    return true;
}

/* MOVI, MVNI, FMOV (vector, immediate), ORR and BIC (vector, immediate) */
static bool t_simd_modimm(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), op = BIT(i, 29), cmode = BITS(i, 15, 12), rd = BITS(i, 4, 0);
    unsigned imm8 = BITS(i, 18, 16) << 5 | BITS(i, 9, 5);
    if (BIT(i, 11) || (op && cmode == 15 && !Q)) return false;   /* half-precision FMOV, reserved */
    uint64_t imm = a64_simd_expand_imm(op, cmode, imm8);
    if (cmode < 12 && (cmode & 1)) {
        vconst(t, 1, imm, true);
        vld(t, 0, rd);
        if (op) { P(0xDF, 1, 0); vst(t, rd, 1, Q); }             /* BIC: ~imm & d */
        else { P(0xEB, 0, 1); vst(t, rd, 0, Q); }                /* ORR */
        return true;
    }
    if (op && cmode != 14 && cmode != 15) imm = ~imm;            /* MVNI */
    vconst(t, 0, imm, Q);
    vst(t, rd, 0, 1);
    return true;
}

/* xmm0 = the low es bytes of rax in every lane of a 64-bit (upper half
 * zero) or 128-bit vector */
static void vdup_rax(tctx *t, int es, int Q)
{
    x_sse_rr(&t->b, 0x66, 0x6E, es == 8, 0, RAX);                /* movd / movq xmm0, rax */
    if (es == 1) P(0x60, 0, 0);                                  /* punpcklbw: b0 b0 b1 b1 ... */
    if (es <= 2) x_sse_rri(&t->b, 0xF2, 0x70, 0, 0, 0);          /* pshuflw: word 0 into words 0-3 */
    if (es == 4) P(0x62, 0, 0);                                  /* punpckldq: d0 d0 0 0 */
    if (Q) P(0x6C, 0, 0);                                        /* punpcklqdq */
}

/* DUP, INS, UMOV, SMOV: element accesses are plain loads and stores of the
 * register's slot. */
static bool t_simd_copy(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), op = BIT(i, 29), imm5 = BITS(i, 20, 16), imm4 = BITS(i, 14, 11);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!(imm5 & 15)) return false;
    int sz = __builtin_ctz(imm5), es = 1 << sz, idx = (int)(imm5 >> (sz + 1));
    if (op) {                                                    /* INS (element) */
        if (!Q) return false;
        x_mov_ld(&t->b, es, RAX, velem(rn, es, (int)(imm4 >> sz)));
        x_mov_st(&t->b, es, velem(rd, es, idx), RAX);
        return true;
    }
    switch (imm4) {
    case 0: case 1:                                              /* DUP (element), DUP (general) */
        if (es == 8 && !Q) return false;
        if (imm4) ldx(t, RAX, rn, 1, false);
        else x_mov_ld(&t->b, es, RAX, velem(rn, es, idx));
        vdup_rax(t, es, (int)Q);
        vst(t, rd, 0, 1);
        return true;
    case 3:                                                      /* INS (general) */
        if (!Q) return false;
        ldx(t, RAX, rn, 1, false);
        x_mov_st(&t->b, es, velem(rd, es, idx), RAX);
        return true;
    case 5:                                                      /* SMOV */
        if (es > (Q ? 4 : 2)) return false;
        x_movs_ld(&t->b, es, (int)Q, RAX, velem(rn, es, idx));
        stx(t, rd, RAX, false);
        return true;
    case 7:                                                      /* UMOV */
        if (Q ? es != 8 : es == 8) return false;
        x_mov_ld(&t->b, es, RAX, velem(rn, es, idx));
        stx(t, rd, RAX, false);
        return true;
    default: return false;
    }
}

/* xmm0 = the low half of each 2*oes-byte lane of xmm0, packed into its low
 * 64 bits (XTN): bytes by packuswb of the masked words, halfwords by packssdw
 * of the sign-extended low halves (both exact: no lane saturates),
 * words by pshufd. */
static void vnarrow(tctx *t, int oes)
{
    if (oes == 1) { vconst(t, 2, 0x00ff00ff00ff00ffull, true); P(0xDB, 0, 2); P(0x67, 0, 0); }
    else if (oes == 2) { x_sse_shift_ri(&t->b, 0x72, 6, 0, 16); x_sse_shift_ri(&t->b, 0x72, 4, 0, 16); P(0x6B, 0, 0); }
    else x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0x08);
}
/* a narrowed result in the low half of xmm0: Vd = result (upper half zero),
 * or for the "2" forms Vd's lower half and the result above it */
static void vst_narrow(tctx *t, unsigned rd, int Q)
{
    if (!Q) { vst(t, rd, 0, 0); return; }
    vld(t, 1, rd);
    P(0x6C, 1, 0);                                               /* punpcklqdq */
    x_movdqu_st(&t->b, VR(rd), 1);
}

/* SHL, USHR, SSHR by an immediate, and the right shifts' accumulating
 * (USRA, SSRA) and rounding (URSHR, SRSHR, URSRA, SRSRA) forms. Words,
 * doublewords and quadwords map onto the SSE2 shifts; bytes shift as words
 * and drop the bits that crossed into the neighbouring byte, and the
 * arithmetic forms without an SSE2 shift (bytes, quadwords) sign-extend the
 * logical result. A right shift by the lane size is in range on both sides:
 * zero, or the sign in every bit. Rounding adds bit rsh - 1 of the operand
 * to the shifted value, which is (x + 2^(rsh-1)) >> rsh without the carry
 * out of the lane.
 * Also SHRN (shift, then XTN) and SSHLL/USHLL (UXTL/SXTL: widen the lower or
 * upper half, then shift left). */
static bool t_simd_shift(tctx *t, uint32_t i, bool scalar)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), immh = BITS(i, 22, 19), op = BITS(i, 15, 11);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!immh) return false;
    int es = 1 << (31 - __builtin_clz(immh)), bits = es * 8, immhb = (int)BITS(i, 22, 16);
    int rsh = 2 * bits - immhb, lsh = immhb - bits;
    if (!scalar && op == 0x14) {                                 /* SSHLL / USHLL: es is the source lane size */
        if (es == 8) return false;                               /* unallocated */
        vld(t, 0, rn);
        if (Q) x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0xEE);         /* the upper half to the lower */
        vzero(t, 1);
        if (!U) P(pcmpgt[es >> 1], 1, 0);                        /* the sign of each lane */
        P(es == 1 ? 0x60 : es == 2 ? 0x61 : 0x62, 0, 1);         /* punpckl{bw,wd,dq}: widened */
        if (lsh) x_sse_shift_ri(&t->b, es == 1 ? 0x71 : es == 2 ? 0x72 : 0x73, 6, 0, (uint8_t)lsh);
        vst(t, rd, 0, 1);
        return true;
    }
    if (!scalar && op == 0x10 && !U) {                           /* SHRN: es is the destination lane size */
        if (es == 8) return false;                               /* unallocated */
        vld(t, 0, rn);
        x_sse_shift_ri(&t->b, es == 1 ? 0x71 : es == 2 ? 0x72 : 0x73, 2, 0, (uint8_t)rsh);
        vnarrow(t, es);
        vst_narrow(t, rd, (int)Q);
        return true;
    }
    if (scalar ? es != 8 : (es == 8 && !Q)) return false;
    uint8_t grp = es == 2 ? 0x71 : es == 4 ? 0x72 : 0x73;
    if (U && (op == 8 || op == 10)) { /* SRI / SLI */
        bool right = op == 8;
        int sh = right ? rsh : lsh;
        uint64_t lane_mask = es == 8 ? ~0ull : (1ull << bits) - 1;
        uint64_t mask = sh == bits ? 0 : right ? lane_mask >> sh : (lane_mask << sh) & lane_mask;
        for (int shift = bits; shift < 64; shift *= 2) mask |= mask << shift;
        vld(t, 0, rn); vld(t, 1, rd);
        x_sse_shift_ri(&t->b, es == 1 ? 0x71 : grp, right ? 2 : 6, 0, (uint8_t)sh);
        vconst(t, 2, mask, true); P(0xDB, 0, 2); P(0xDF, 2, 1); P(0xEB, 0, 2);
        vst(t, rd, 0, scalar ? 0 : (int)Q); return true;
    }
    if (!(op & ~6u)) {                                           /* [SU]SHR, [SU]SRA, [SU]RSHR, [SU]RSRA */
        bool rnd = op & 4, acc = op & 2;
        vld(t, 0, rn);
        if (rnd) P(0x6F, 3, 0);
        if (es == 1) {                                           /* bytes */
            if (rsh == 8) {
                if (!U) P(0x6F, 1, 0);
                vzero(t, 0);
                if (!U) P(pcmpgt[0], 0, 1);                      /* 0 > x: the sign in every bit */
            } else {
                x_sse_shift_ri(&t->b, 0x71, 2, 0, (uint8_t)rsh);
                vconst(t, 1, 0x0101010101010101ull * (0xffu >> rsh), true);
                P(0xDB, 0, 1);
                if (!U) {                                        /* (x ^ m) - m sign-extends from bit 7 - rsh */
                    vconst(t, 1, 0x0101010101010101ull * (0x80u >> rsh), true);
                    P(0xEF, 0, 1);
                    P(0xF8, 0, 1);
                }
            }
        } else if (es == 8 && !U) {                              /* arithmetic, quadwords */
            x_sse_rri(&t->b, 0x66, 0x70, 1, 0, 0xF5);            /* each quadword's high doubleword, twice */
            x_sse_shift_ri(&t->b, 0x72, 4, 1, 31);               /* the sign in every bit */
            x_sse_shift_ri(&t->b, 0x73, 2, 0, (uint8_t)rsh);     /* rsh = 64 leaves 0 */
            x_sse_shift_ri(&t->b, 0x73, 6, 1, (uint8_t)(64 - rsh));
            P(0xEB, 0, 1);
        } else
            x_sse_shift_ri(&t->b, grp, U ? 2 : 4, 0, (uint8_t)rsh);
        if (rnd) {                                               /* + bit rsh - 1 of x: the rounding, without overflow */
            if (es == 1) {
                x_sse_shift_ri(&t->b, 0x71, 2, 3, (uint8_t)(rsh - 1));
                vconst(t, 2, 0x0101010101010101ull, true);
                P(0xDB, 3, 2);
            } else {
                x_sse_shift_ri(&t->b, grp, 2, 3, (uint8_t)(rsh - 1));
                x_sse_shift_ri(&t->b, grp, 6, 3, (uint8_t)(bits - 1));
                x_sse_shift_ri(&t->b, grp, 2, 3, (uint8_t)(bits - 1));
            }
            P(padd[es == 8 ? 3 : es >> 1], 0, 3);
        }
        if (acc) { vld(t, 1, rd); P(padd[es == 8 ? 3 : es >> 1], 0, 1); }
    } else if (op == 0x0A && !U) {                               /* SHL */
        vld(t, 0, rn);
        if (es == 1) {
            if (lsh) {
                x_sse_shift_ri(&t->b, 0x71, 6, 0, (uint8_t)lsh);
                vconst(t, 1, 0x0101010101010101ull * ((0xffu << lsh) & 0xffu), true);
                P(0xDB, 0, 1);
            }
        } else
            x_sse_shift_ri(&t->b, grp, 6, 0, (uint8_t)lsh);
    } else
        return false;
    vst(t, rd, 0, scalar ? 0 : (int)Q);
    return true;
}

/* ---- vector floating point, single precision, on SSE2.
 *
 * The forms the interpreter computes with fp_vec32 / fp_vec32_select
 * (a64_fp.c): FADD, FSUB, FMUL (vector and by element), FMAX(NM), FMIN(NM),
 * FCMEQ, FCMGE, FCMGT, .2S and .4S. The emitted code is that function's
 * fast path, instruction for instruction: the same integer admission of the
 * operands (every lane zero or normal, before any FP instruction), the same
 * SSE operation under the guest's MXCSR (cpu.h), and for arithmetic the same
 * check of the result (normal and above the minimum normal, or an exact zero
 * of the operands) with the MXCSR saved around the operation and restored
 * when the result is declined. Whatever fp_vec32 would decline goes to the
 * interpreter helper, which declines it again and takes the per-lane
 * FPProcessNaNs/FPRound path: results and FPSR are the interpreter's.
 *
 * Nothing here touches EFLAGS (SSE, pmovmskb, jrcxz, stmxcsr, ldmxcsr), so
 * NZCV stays where it is on the fast path; the declining path saves it
 * around the helper call when it is live (helper_keep_flags). The MXCSR is
 * saved in the 8 bytes at [rsp], the padding the entry stub reserves in
 * every frame translated code runs in (build_stubs).
 *
 * xmm0 = Vn, xmm1 = Vm (or the broadcast element), xmm5 = 0,
 * xmm6 = 0x7fffffff, xmm7 = 0x7f800000 in every lane; xmm2-4, xmm8, xmm9
 * scratch. The three constants stay for a vector FP instruction that
 * directly follows in the block (tctx.vfp_consts; nothing else uses
 * xmm5-7, and the declining path loads them again after its call). A .2S
 * form clears both operands' upper lanes first: zero lanes are admitted,
 * raise nothing and give exact zeros, so they never decline, and the store
 * clears them in Vd. */
#define MXCSR_SLOT xm(RSP, 0)

/* jmp rel32 when rcx != 0, EFLAGS untouched; returns the rel32 to patch */
static size_t jrcxnz32(tctx *t)
{
    size_t z = x_jrcxz8(&t->b);
    size_t j = x_jmp32(&t->b);
    x_patch8(&t->b, z, xb_len(&t->b));
    return j;
}

/* The interpreter helper for an instruction a native translation declined
 * at run time. NZCV that is live after it stays in EFLAGS: it is stored
 * before the call (the helper and a signal inside it read it from memory)
 * and loaded back after; the path that did not call keeps EFLAGS as it was. */
static void helper_keep_flags(tctx *t, uint32_t insn, uint64_t pc)
{
    bool keep = t->in_eflags && t->live_out[t->idx], dirty = t->dirty;
    helper(t, insn, pc, true);
    st_helper -= !t->dry;                                        /* counted as native */
    if (keep) { x_mov_ld(&t->b, 2, RAX, FLAGS); fl_from_eax(t); }
    t->in_eflags = keep;
    t->dirty = keep && dirty;
}

/* xmm4 |= the lanes of xmm `s` that are not zero or normal: exponent all
 * ones (infinity, NaN), or zero with a nonzero fraction (subnormal) */
static void vfp_special(tctx *t, int s)
{
    P(0x6F, 2, s); P(0xDB, 2, 7);                                /* e = s & exp */
    P(0x6F, 3, 2); P(0x76, 3, 5);                                /* e == 0 */
    P(0x76, 2, 7); P(0xEB, 4, 2);                                /* e == exp */
    P(0x6F, 2, s); P(0xDB, 2, 6); P(0x76, 2, 5);                 /* |s| == 0 */
    P(0xDF, 2, 3); P(0xEB, 4, 2);                                /* e == 0 && |s| != 0 */
}
/* the same for double lanes, with xmm12 = 0, xmm10 = 0x7fff...ffff and
 * xmm11 = 0x7ff0...0000 in both lanes (vfp_constants64) */
static void vfp_special64(tctx *t, int s)
{
    P(0x6F, 2, s); P(0xDB, 2, 11);
    P(0x6F, 3, 2); P38(0x29, 3, 12);
    P38(0x29, 2, 11); P(0xEB, 4, 2);
    P(0x6F, 2, s); P(0xDB, 2, 10); P38(0x29, 2, 12);
    P(0xDF, 2, 3); P(0xEB, 4, 2);
}
static void vfp_constants64(tctx *t)
{
    vzero(t, 12);
    vones(t, 10); x_sse_shift_ri(&t->b, 0x73, 2, 10, 1);                                     /* 0x7fffffffffffffff */
    vones(t, 11); x_sse_shift_ri(&t->b, 0x73, 2, 11, 53); x_sse_shift_ri(&t->b, 0x73, 6, 11, 52); /* 0x7ff0000000000000 */
}

enum { VFP_ADD, VFP_SUB, VFP_MUL, VFP_DIV, VFP_MAX, VFP_MIN, VFP_EQ, VFP_GE, VFP_GT };

static void vfp_constants(tctx *t)
{
    vzero(t, 5);
    vones(t, 6); x_sse_shift_ri(&t->b, 0x72, 2, 6, 1);                                     /* 0x7fffffff */
    vones(t, 7); x_sse_shift_ri(&t->b, 0x72, 2, 7, 24); x_sse_shift_ri(&t->b, 0x72, 6, 7, 23); /* 0x7f800000 */
}

/* The ordinary estimate path is entirely integer. Admit every active
 * operand before publishing Vd: reciprocal excludes output-denormal input
 * exponents; reciprocal square root excludes negative and nonnormal inputs.
 * All remaining cases retain the interpreter's NaN/zero/FZ/exception rules. */
static bool t_vfp_estimate(tctx *t, uint32_t i, uint64_t pc, bool scalar)
{
    unsigned size = BITS(i, 23, 22), Q = BIT(i, 30), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    bool root = BIT(i, 29), dbl = size & 1;
    if (BITS(i, 16, 12) != 0x1d || !(size & 2) || (!scalar && dbl && !Q)) return false;
    unsigned es = dbl ? 8 : 4, mbits = dbl ? 52 : 23, expmax = dbl ? 2047 : 255, bias = dbl ? 1023 : 127;
    unsigned n = scalar ? 1 : (Q ? 16 : 8) / es;
    size_t decline[12]; unsigned nd = 0;
    fl_clobber(t);
    x_mov_ld(&t->b, 8, RDI, xm(RBX, CPU_OFF_FP_ESTIMATES));
    vzero(t, 8);
    for (unsigned lane = 0; lane < n; lane++) {
        x_mov_ld(&t->b, (int)es, RAX, velem(rn, (int)es, (int)lane));
        if (root) { x_test_rr(&t->b, dbl, RAX, RAX); decline[nd++] = x_jcc32(&t->b, XCC_S); }
        x_mov_rr(&t->b, 1, RDX, RAX); x_shift_ri(&t->b, XSHR, 1, RDX, (uint8_t)mbits);
        x_alu_ri(&t->b, XAND, 0, RDX, expmax); decline[nd++] = x_jcc32(&t->b, XCC_E);
        x_alu_ri(&t->b, XCMP, 0, RDX, root ? expmax : 2 * bias - 1); decline[nd++] = x_jcc32(&t->b, XCC_AE);
        x_mov_rr(&t->b, 1, RCX, RAX); x_shift_ri(&t->b, XSHR, 1, RCX, (uint8_t)(mbits - 8));
        x_alu_ri(&t->b, XAND, 0, RCX, 255);
        if (root) {
            x_alu_ri(&t->b, XOR_, 0, RCX, 256);
            x_mov_rr(&t->b, 0, RSI, RDX); x_alu_ri(&t->b, XAND, 0, RSI, 1);
            size_t even = x_jcc8(&t->b, XCC_E);
            x_shift_ri(&t->b, XSHR, 0, RCX, 1); /* odd exponent: 128 | fraction[7:1] */
            x_patch8(&t->b, even, xb_len(&t->b));
        }
        x_mov_ld(&t->b, 1, RCX, xmi(RDI, RCX, 1, root ? 256 : 0));
        x_shift_ri(&t->b, XSHL, 1, RCX, (uint8_t)(mbits - 8));
        x_mov_ri(&t->b, RSI, root ? 3 * bias - 1 : 2 * bias - 1); x_alu_rr(&t->b, XSUB, 0, RSI, RDX);
        if (root) x_shift_ri(&t->b, XSHR, 0, RSI, 1);
        x_shift_ri(&t->b, XSHL, 1, RSI, (uint8_t)mbits); x_alu_rr(&t->b, XOR_, 1, RCX, RSI);
        if (!root) {
            x_mov_ri(&t->b, RDX, 1ull << (8 * es - 1)); x_alu_rr(&t->b, XAND, 1, RAX, RDX);
            x_alu_rr(&t->b, XOR_, 1, RCX, RAX);
        }
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 0, RCX);
        if (lane) x_sse_shift_ri(&t->b, 0x73, 7, 0, (uint8_t)(lane * es));
        P(0xEB, 8, 0);
    }
    vst(t, rd, 8, scalar ? 0 : (int)Q);
    size_t done = x_jmp32(&t->b);
    for (unsigned k = 0; k < nd; k++) x_patch32(&t->b, decline[k], xb_len(&t->b));
    helper(t, i, pc, true); st_helper -= !t->dry;
    x_patch32(&t->b, done, xb_len(&t->b));
    return true;
}

/* FCCMP evaluates its condition before reading either FP operand. A
 * false condition cannot raise exceptions. Ordinary active inputs take
 * COMIS/UCOMIS; NaNs, infinities and subnormals use the unchanged helper. */
static void t_fccmp(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned cond = BITS(i, 15, 12), nzcv = BITS(i, 3, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    int dbl = BIT(i, 22);
    fl_load(t); fl_materialize(t);
    size_t skip = cond < 14 ? x_jcc32(&t->b, a64_cond_to_xcc(cond) ^ 1) : 0;
    t->in_eflags = false; t->dirty = false;
    fp_load(t, 0, rn, dbl); fp_load(t, 1, rm, dbl);
    if (dbl) vfp_constants64(t); else vfp_constants(t);
    vzero(t, 4);
    if (dbl) { vfp_special64(t, 0); vfp_special64(t, 1); } else { vfp_special(t, 0); vfp_special(t, 1); }
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t special = jrcxnz32(t);
    x_sse_rr(&t->b, dbl ? 0x66 : 0, BIT(i, 4) ? 0x2F : 0x2E, 0, 0, 1);
    x_setcc(&t->b, XCC_P, RAX); x_setcc(&t->b, XCC_E, RCX); x_setcc(&t->b, XCC_B, RDX);
    x_movzx8_rr(&t->b, RAX, RAX); x_movzx8_rr(&t->b, RCX, RCX); x_movzx8_rr(&t->b, RDX, RDX);
    x_lea(&t->b, 0, RAX, xmi(RDX, RAX, 4, 0)); x_lea(&t->b, 0, RAX, xmi(RAX, RCX, 2, 0));
    x_mov_ld(&t->b, 8, RAX, xmi(RBX, RAX, 8, CPU_OFF_FCMP)); x_mov_st(&t->b, 8, FLAGS, RAX);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, special, xb_len(&t->b)); helper(t, i, pc, true); st_helper -= !t->dry;
    size_t helper_done = skip ? x_jmp32(&t->b) : 0;
    if (skip) {
        x_patch32(&t->b, skip, xb_len(&t->b));
        uint32_t fl = HAO_FL_BASE | ((nzcv & 8) ? HAO_FL_SF : 0) | ((nzcv & 4) ? HAO_FL_ZF : 0) |
            ((nzcv & 2) ? 0 : HAO_FL_CF) | ((nzcv & 1) ? HAO_FL_OF : 0);
        x_mov_mi(&t->b, 8, FLAGS, (int32_t)fl);
        x_patch32(&t->b, helper_done, xb_len(&t->b));
    }
    x_patch32(&t->b, done, xb_len(&t->b));
}

/* Vd = Vn op Vm (idx < 0) or Vn op Vm.S[idx] */
static void t_vfp32(tctx *t, int op, unsigned rd, unsigned rn, unsigned rm, int idx, int Q, uint32_t insn, uint64_t pc)
{
    static const uint8_t arith[4] = {0x58, 0x5C, 0x59, 0x5E};
    vld(t, 0, rn);
    vld(t, 1, rm);
    if (idx >= 0) x_sse_rri(&t->b, 0x66, 0x70, 1, 1, (uint8_t)(0x55 * idx));
    if (!Q) { x_sse_rr(&t->b, 0xF3, 0x7E, 0, 0, 0); x_sse_rr(&t->b, 0xF3, 0x7E, 0, 1, 1); }
    if (!Q && op == VFP_DIV) {
        /* Duplicate active lanes: padded 0/0 must not raise IOC. */
        x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0x44);
        x_sse_rri(&t->b, 0x66, 0x70, 1, 1, 0x44);
    }
    if (!t->vfp_prev) vfp_constants(t);
    vzero(t, 4);
    vfp_special(t, 0);
    vfp_special(t, 1);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);                      /* pmovmskb ecx, xmm4 */
    size_t decline = jrcxnz32(t), done;
    if (op <= VFP_DIV) {
        x_stmxcsr(&t->b, MXCSR_SLOT);
        P(0x6F, 2, 0);
        x_sse_rr(&t->b, 0, arith[op], 0, 2, 1);                  /* addps / subps / mulps / divps */
        /* declined result lanes: exponent all ones, zero or minimum normal
         * (exponent & 0xfe == 0) */
        P(0x6F, 3, 2); P(0xDB, 3, 7); P(0x76, 3, 7);
        vones(t, 4); x_sse_shift_ri(&t->b, 0x72, 2, 4, 25); x_sse_shift_ri(&t->b, 0x72, 6, 4, 24);   /* 0x7f000000 */
        P(0xDB, 4, 2); P(0x76, 4, 5); P(0xEB, 4, 3);
        x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
        size_t exact = jrcxnz32(t); /* packed arithmetic result admission */
        size_t store = xb_len(&t->b);
        vst(t, rd, 2, Q);
        done = x_jmp32(&t->b);
        /* a zero result is kept when it is exact: a zero operand of a
         * multiplication, two zeros or an exact cancellation (x == -y for
         * FADD, x == y for FSUB) of a sum */
        x_patch32(&t->b, exact, xb_len(&t->b));
        P(0x6F, 3, 0); P(0xDB, 3, 6); P(0x76, 3, 5);             /* |x| == 0 */
        P(0x6F, 8, 1); P(0xDB, 8, 6); P(0x76, 8, 5);             /* |y| == 0 */
        if (op == VFP_MUL) P(0xEB, 3, 8);
        else if (op == VFP_DIV) { P(0xDF, 8, 3); P(0x6F, 3, 8); } /* zero numerator, nonzero divisor */
        else {
            P(0xDB, 3, 8);
            if (op == VFP_ADD) { vones(t, 9); x_sse_shift_ri(&t->b, 0x72, 6, 9, 31); P(0xEF, 9, 1); }  /* y ^ sign */
            else P(0x6F, 9, 1);
            P(0x76, 9, 0); P(0xEB, 3, 9);
        }
        P(0x6F, 8, 2); P(0xDB, 8, 6); P(0x76, 8, 5); P(0xDB, 3, 8);   /* exact && the result is zero */
        P(0xDF, 3, 4);                                           /* declined lanes that are not */
        x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 3);
        size_t restore = jrcxnz32(t);
        x_patch32(&t->b, x_jmp32(&t->b), store);
        x_patch32(&t->b, restore, xb_len(&t->b));
        x_ldmxcsr(&t->b, MXCSR_SLOT);
    } else {
        if (op == VFP_MAX || op == VFP_MIN) {
            /* maxps/minps give their second operand for two zeros; A64
             * gives +0 for FMAX and -0 for FMIN of opposite-signed zeros */
            P(0x6F, 2, 0);
            x_sse_rr(&t->b, 0, op == VFP_MAX ? 0x5F : 0x5D, 0, 2, 1);
            P(0x6F, 3, 0); P(0xDB, 3, 6); P(0x76, 3, 5);
            P(0x6F, 8, 1); P(0xDB, 8, 6); P(0x76, 8, 5); P(0xDB, 3, 8);   /* both zero */
            P(0x6F, 8, 0); P(op == VFP_MAX ? 0xDB : 0xEB, 8, 1);          /* x & y / x | y */
            P(0xDB, 8, 3); P(0xDF, 3, 2); P(0xEB, 3, 8);
            vst(t, rd, 3, Q);
        } else {
            /* cmpeqps x, y; cmpleps y, x (x >= y); cmpltps y, x (x > y) */
            P(0x6F, 2, op == VFP_EQ ? 0 : 1);
            x_sse_rri(&t->b, 0, 0xC2, 2, op == VFP_EQ ? 1 : 0, op == VFP_EQ ? 0 : op == VFP_GE ? 2 : 1);
            vst(t, rd, 2, Q);
        }
        done = x_jmp32(&t->b);
    }
    x_patch32(&t->b, decline, xb_len(&t->b));
    helper_keep_flags(t, insn, pc);
    vfp_constants(t);                                            /* the call clobbered them */
    x_patch32(&t->b, done, xb_len(&t->b));
    t->vfp_consts = true;
}

/* Packed double arithmetic and packed single/double square roots. The
 * same admission as host2/fp_sqrt: all inputs zero or normal, then reject
 * nonfinite/tiny results with MXCSR restored before the descriptor helper.
 * Integer lane comparisons preserve live NZCV throughout. */
static void t_vfp_wide(tctx *t, int op, bool dbl, unsigned rd, unsigned rn, unsigned rm,
                       int Q, uint32_t insn, uint64_t pc)
{
    bool sqrt = op == 4;
    unsigned size = dbl ? 3 : 2;
    int zero = dbl ? 12 : 5, mag = dbl ? 10 : 6, exp = dbl ? 11 : 7;
    vld(t, 0, rn);
    if (!Q) x_sse_rr(&t->b, 0xF3, 0x7E, 0, 0, 0);
    if (!sqrt) vld(t, 1, rm); else vzero(t, 1);
    if (dbl) vfp_constants64(t); else vfp_constants(t);
    vzero(t, 4);
    if (dbl) { vfp_special64(t, 0); vfp_special64(t, 1); }
    else vfp_special(t, 0);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    x_stmxcsr(&t->b, MXCSR_SLOT);
    P(0x6F, 2, 0);
    static const uint8_t ops[] = {0x58, 0x5C, 0x59, 0x5E, 0x51};
    x_sse_rr(&t->b, dbl ? 0x66 : 0, ops[op], 0, 2, sqrt ? 0 : 1);
    P(0x6F, 3, 2); P(0xDB, 3, exp); vcmpeq(t, size, 3, exp);
    vconst(t, 4, dbl ? 0x7fe0000000000000ull : 0x7f0000007f000000ull, true);
    P(0xDB, 4, 2); vcmpeq(t, size, 4, zero); P(0xEB, 4, 3);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t exact = jrcxnz32(t);                                  /* wide result admission */
    size_t store = xb_len(&t->b);
    vst(t, rd, 2, Q);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, exact, xb_len(&t->b));
    P(0x6F, 3, 0); P(0xDB, 3, mag); vcmpeq(t, size, 3, zero);
    if (!sqrt) {
        P(0x6F, 8, 1); P(0xDB, 8, mag); vcmpeq(t, size, 8, zero);
        if (op == VFP_MUL) P(0xEB, 3, 8);
        else if (op == VFP_DIV) { P(0xDF, 8, 3); P(0x6F, 3, 8); }
        else {
            P(0xDB, 3, 8);
            if (op == VFP_ADD) {
                vconst(t, 9, 0x8000000000000000ull, true); P(0xEF, 9, 1);
            } else P(0x6F, 9, 1);
            vcmpeq(t, size, 9, 0); P(0xEB, 3, 9);
        }
    }
    P(0x6F, 8, 2); P(0xDB, 8, mag); vcmpeq(t, size, 8, zero); P(0xDB, 3, 8);
    P(0xDF, 3, 4);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 3);
    size_t restore = jrcxnz32(t);
    x_patch32(&t->b, x_jmp32(&t->b), store);
    x_patch32(&t->b, restore, xb_len(&t->b)); x_ldmxcsr(&t->b, MXCSR_SLOT);
    x_patch32(&t->b, decline, xb_len(&t->b)); helper_keep_flags(t, insn, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
}

/* fma32_odd (a64_fp.c), two low single lanes -> two doubles in s.
 * a/b/c are destroyed; e is scratch. All inputs have passed integer
 * admission and MXCSR is RN. The product is exact; TwoSum recovers the
 * sum's error, then the integer step rounds to odd without double rounding. */
static void vfp_fma_odd(tctx *t, int a, int b, int c, int s, int e)
{
    x_sse_rr(&t->b, 0, 0x5A, 0, a, a);                          /* cvtps2pd */
    x_sse_rr(&t->b, 0, 0x5A, 0, b, b);
    x_sse_rr(&t->b, 0, 0x5A, 0, c, c);
    P(0x59, a, b);                                               /* exact product */
    P(0x6F, s, a); P(0x58, s, c);                               /* RN sum */
    P(0x6F, b, s); P(0x5C, b, a);                               /* c' */
    P(0x6F, e, s); P(0x5C, e, b);                               /* p' */
    P(0x5C, a, e); P(0x5C, c, b); P(0x58, a, c);               /* error */
    vzero(t, c); P(0x6F, e, a);
    x_sse_rri(&t->b, 0x66, 0xC2, a, c, 4);                      /* error != 0 */
    P(0x6F, b, s); P(0xEF, b, e);
    x_sse_shift_ri(&t->b, 0x72, 4, b, 31);
    x_sse_rri(&t->b, 0x66, 0x70, b, b, 0xF5);                   /* signs differ */
    vones(t, e); x_sse_shift_ri(&t->b, 0x73, 2, e, 63);
    P(0xEB, b, e);                                               /* -1 or +1 */
    P(0x6F, c, s); x_sse_shift_ri(&t->b, 0x73, 6, c, 63);
    x_sse_shift_ri(&t->b, 0x72, 4, c, 31);
    x_sse_rri(&t->b, 0x66, 0x70, c, c, 0xF5);                   /* sum odd */
    P(0xDF, c, a); P(0xDB, b, c); P(0xD4, s, b);               /* round to odd */
}

/* fp_vec32_muladd's admission, arithmetic and rollback, with no EFLAGS
 * writes or absolute addresses. xmm5-7 retain vfp_constants; xmm10-12
 * hold the upper input pair while xmm0/1/8 compute the lower one. */
static void t_vfp_fma(tctx *t, bool sub, unsigned rd, unsigned rn, unsigned rm, int idx, int Q, uint32_t insn, uint64_t pc)
{
    bool scalar = BIT(insn, 28);
    bool three = (insn & 0x5F000000) == 0x1F000000;
    bool step = !three && (insn & 0x8F20FC00) == 0x0E20FC00;
    bool rsqrt = step && BIT(insn, 23);
    if (scalar) {
        x_movd_ld(&t->b, 0, VR(rn)); x_movd_ld(&t->b, 1, velem(rm, 4, idx < 0 ? 0 : idx));
        x_movd_ld(&t->b, 8, VR(three ? BITS(insn, 14, 10) : rd));
    } else { vld(t, 0, rn); vld(t, 1, rm); vld(t, 8, rd); }
    if (step) vconst(t, 8, rsqrt ? 0x4040000040400000ull : 0x4000000040000000ull, Q);
    if (!scalar && idx >= 0) x_sse_rri(&t->b, 0x66, 0x70, 1, 1, (uint8_t)(0x55 * idx));
    if (!Q) {
        x_sse_rr(&t->b, 0xF3, 0x7E, 0, 0, 0);
        x_sse_rr(&t->b, 0xF3, 0x7E, 0, 1, 1);
        x_sse_rr(&t->b, 0xF3, 0x7E, 0, 8, 8);
    }
    if (!t->vfp_prev) vfp_constants(t);
    vzero(t, 4);
    vfp_special(t, 0); vfp_special(t, 1); vfp_special(t, 8);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    /* The same FPCR contract as the helper, including future/unsupported
     * controls. SSE integer masks leave live guest NZCV untouched. */
    x_movd_ld(&t->b, 2, xm(RBX, offsetof(hao_cpu, fpcr)));
    x_mov_ri(&t->b, RAX, 0x03c00000);
    x_sse_rr(&t->b, 0x66, 0x6E, 0, 3, RAX);
    P(0xDF, 3, 2);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 3, RCX);
    size_t control = jrcxnz32(t);
    if (sub || (three && BIT(insn, 21))) {
        vones(t, 2); x_sse_shift_ri(&t->b, 0x72, 6, 2, 31);
        if (sub) P(0xEF, 0, 2);                                  /* negate product exactly */
        if (three && BIT(insn, 21)) P(0xEF, 8, 2);                /* scalar fused addend sign */
    }
    x_stmxcsr(&t->b, MXCSR_SLOT);
    x_movd_ld(&t->b, 2, MXCSR_SLOT);
    x_mov_ri(&t->b, RAX, 0x6000);
    x_sse_rr(&t->b, 0x66, 0x6E, 0, 3, RAX);
    P(0xDB, 3, 2);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 3, RCX);                    /* guest RC, kept through TwoSum */
    size_t nearest = x_jrcxz8(&t->b);
    P(0xDF, 3, 2);
    x_movd_st(&t->b, xm(RSP, 4), 3);
    x_ldmxcsr(&t->b, xm(RSP, 4));                                 /* RN for both pairs */
    x_patch8(&t->b, nearest, xb_len(&t->b));
    if (Q) {
        P(0x6F, 10, 0); P(0x6D, 10, 10);
        P(0x6F, 11, 1); P(0x6D, 11, 11);
        P(0x6F, 12, 8); P(0x6D, 12, 12);
    }
    vfp_fma_odd(t, 0, 1, 8, 9, 4);
    if (Q) vfp_fma_odd(t, 10, 11, 12, 13, 4);
    if (rsqrt) {
        /* Exact scaling preserves the odd low bit; round to single only after it. */
        vconst(t, 2, 0x3fe0000000000000ull, true);
        P(0x59, 9, 2); if (Q) P(0x59, 13, 2);
    }
    /* In RN, retain PE from an inexact sum (the conversion raises it too).
     * Otherwise restore guest RC before converting, as fma32 does. */
    nearest = x_jrcxz8(&t->b);
    x_ldmxcsr(&t->b, MXCSR_SLOT);
    x_patch8(&t->b, nearest, xb_len(&t->b));
    P(0x5A, 9, 9);                                               /* cvtpd2ps, guest RC/FZ */
    if (Q) { P(0x5A, 13, 13); P(0x6C, 9, 13); }
    /* Decline exponents 0 and 1 (including zero); overflow is admitted.
     * .2S checks only the active pair and cvtpd2ps clears the upper half. */
    vones(t, 2); x_sse_shift_ri(&t->b, 0x72, 2, 2, 25); x_sse_shift_ri(&t->b, 0x72, 6, 2, 24);
    P(0xDB, 2, 9); P(0x76, 2, 5);
    if (scalar) {
        x_sse_rr(&t->b, 0x66, 0x7E, 0, 2, RAX);
        x_sse_rr(&t->b, 0x66, 0x6E, 0, 2, RAX);                 /* only the scalar result is active */
    } else if (!Q) x_sse_rr(&t->b, 0xF3, 0x7E, 0, 2, 2);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 2);
    size_t restore = jrcxnz32(t);
    if (scalar) {
        x_sse_rr(&t->b, 0x66, 0x7E, 0, 9, RAX);
        x_sse_rr(&t->b, 0x66, 0x6E, 0, 9, RAX);                 /* clear all 96 upper bits */
    }
    vst(t, rd, 9, Q);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, restore, xb_len(&t->b));
    x_ldmxcsr(&t->b, MXCSR_SLOT);
    x_patch32(&t->b, decline, xb_len(&t->b));
    x_patch32(&t->b, control, xb_len(&t->b));
    helper_keep_flags(t, insn, pc);
    vfp_constants(t);
    x_patch32(&t->b, done, xb_len(&t->b));
    t->vfp_consts = true;
}

/* The half path keeps input vectors in xmm8-10, the uncommitted result in
 * xmm11, cumulative MXCSR flags in xmm12 and the ARM rounding mode in xmm13.
 * xmm7 is zero. No instruction here changes EFLAGS. */
static void vfp_half_widen(tctx *t, int dst, int src)
{
    P38(0x34, dst, src);                                         /* pmovzxwq: two half lanes */
    vconst(t, 4, 0x7fff, true); P(0xDB, 4, dst);
    P(0x6F, 5, 4); P38(0x29, 5, 7);                            /* zero magnitude */
    x_sse_shift_ri(&t->b, 0x73, 2, dst, 15);
    x_sse_shift_ri(&t->b, 0x73, 6, dst, 63);                     /* sign */
    x_sse_shift_ri(&t->b, 0x73, 6, 4, 42);
    vconst(t, 6, 1008ull << 52, true); P(0xD4, 4, 6);
    P(0xDF, 5, 4); P(0xEB, dst, 5);                            /* signed zero or exact double */
}

/* q=xmm3, remainder=xmm4, midpoint=xmm5, sign=xmm1, tiny flag=xmm15.
 * Accumulate MXCSR flags and increment q in the selected ARM mode. */
static void vfp_half_increment(tctx *t, bool both)
{
    vconst(t, 6, 0x20, both); P(0xEB, 15, 6);                  /* IXC and optional UFC */
    P(0x6F, 6, 4); P38(0x29, 6, 7); P(0xDF, 6, 15); P(0xEB, 12, 6);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 13, RCX);
    size_t directed = jrcxnz32(t);
    P(0x6F, 6, 4); P38(0x37, 6, 5);                           /* remainder > midpoint */
    P38(0x29, 4, 5);
    P(0x6F, 5, 3); x_sse_shift_ri(&t->b, 0x73, 6, 5, 63);
    P38(0x37, 7, 5); P(0xDB, 4, 7); vzero(t, 7);             /* odd q && tie */
    P(0xEB, 6, 4);
    size_t increment = x_jmp32(&t->b);
    x_patch32(&t->b, directed, xb_len(&t->b));
    P(0x6F, 6, 1); x_sse_shift_ri(&t->b, 0x73, 2, 6, 15);
    vconst(t, 5, 1, both); P(0xD4, 6, 5);                     /* positive -> RP, negative -> RM */
    P38(0x29, 6, 13); P38(0x29, 4, 7); P(0xDF, 4, 6); P(0x6F, 6, 4);
    x_patch32(&t->b, increment, xb_len(&t->b));
    P(0xFB, 3, 6);                                             /* half rounding increment */
}

/* Both exponents are in [1009,1038]. Round the pair together, including
 * overflow caused by the increment. Tiny/zero/large results use the scalar
 * integer path below; neither path converts through binary32. */
static void vfp_half_normal(tctx *t)
{
    P(0x6F, 1, 0); x_sse_shift_ri(&t->b, 0x73, 2, 1, 63);
    x_sse_shift_ri(&t->b, 0x73, 6, 1, 15);
    vconst(t, 2, 0x7fffffffffffffffull, true); P(0xDB, 2, 0);
    P(0x6F, 3, 2); x_sse_shift_ri(&t->b, 0x73, 2, 3, 42);
    vconst(t, 6, 1008u << 10, true); P(0xFB, 3, 6);
    vconst(t, 4, (1ull << 42) - 1, true); P(0xDB, 4, 2);
    vconst(t, 5, 1ull << 41, true); vzero(t, 15);
    vfp_half_increment(t, true);
    vconst(t, 4, 0x7bff, true); P(0x6F, 5, 3); P38(0x37, 5, 4); /* overflow mask */
    vconst(t, 6, 0x28, true); P(0xDB, 6, 5); P(0xEB, 12, 6);
    P(0x6F, 6, 1); x_sse_shift_ri(&t->b, 0x73, 2, 6, 15);
    vconst(t, 2, 1, true); P(0xD4, 6, 2); P38(0x29, 6, 13);
    P(0x6F, 0, 13); P38(0x29, 0, 7); P(0xEB, 6, 0); P(0xDB, 6, 2);
    P(0xD4, 4, 6); P(0xDB, 4, 5); P(0xDF, 5, 3); P(0xEB, 4, 5);
    P(0x6F, 0, 4); P(0xEB, 0, 1);
    x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0x88);                /* two half results in dwords */
    P38(0x2B, 0, 7);                                          /* packusdw */
    x_sse_shift_ri(&t->b, 0x73, 7, 0, 12);
    x_sse_shift_ri(&t->b, 0x73, 3, 0, 12);                    /* keep just the two halfwords */
}

/* Low binary64 lane in xmm0 -> low half in xmm0, integer-only. Mirrors
 * fp_round16_exact; xmm1-6/15 scratch, xmm7 zero, xmm12 flags, xmm13 mode.
 * The high lane is irrelevant. Ordinary half products/sums cannot be
 * binary64 subnormal or infinite, but all finite exponent ranges work. */
static void vfp_half_round(tctx *t)
{
    P(0x6F, 1, 0); x_sse_shift_ri(&t->b, 0x73, 2, 1, 63);
    x_sse_shift_ri(&t->b, 0x73, 6, 1, 15);                       /* half sign */
    vconst(t, 2, 0x7fffffffffffffffull, false); P(0xDB, 2, 0);
    vzero(t, 3);
    P(0x6F, 6, 2); P38(0x29, 6, 7);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 6, RCX);
    size_t zero = jrcxnz32(t);
    P(0x6F, 0, 2); x_sse_shift_ri(&t->b, 0x73, 2, 0, 52);      /* exponent */
    vconst(t, 6, 1038, false); P(0x6F, 4, 0); P38(0x37, 4, 6);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 4, RCX);
    size_t over = jrcxnz32(t);
    vconst(t, 6, 1009, false); P38(0x37, 6, 0);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 6, RCX);
    size_t tiny = jrcxnz32(t);
    P(0x6F, 3, 2); x_sse_shift_ri(&t->b, 0x73, 2, 3, 42);
    vconst(t, 6, 1008u << 10, false); P(0xFB, 3, 6);
    vconst(t, 4, (1ull << 42) - 1, false); P(0xDB, 4, 2);
    vconst(t, 5, 1ull << 41, false);
    vzero(t, 15);
    size_t normal = x_jmp32(&t->b);
    x_patch32(&t->b, tiny, xb_len(&t->b));
    vconst(t, 15, 0x10, false);                                 /* tininess BEFORE rounding */
    vconst(t, 6, 998, false); P38(0x37, 6, 0);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 6, RCX);
    size_t verytiny = jrcxnz32(t);
    vconst(t, 6, 1051, false); P(0xFB, 6, 0);                    /* half subnormal shift */
    vconst(t, 4, (1ull << 52) - 1, false); P(0xDB, 4, 2);
    vconst(t, 5, 1ull << 52, false); P(0xEB, 4, 5);
    P(0x6F, 3, 4); P(0xD3, 3, 6);                             /* q = mantissa >> shift */
    P(0x6F, 5, 3); P(0xF3, 5, 6); P(0xFB, 4, 5);             /* remainder */
    vconst(t, 5, 1, false); P(0xFB, 6, 5); P(0xF3, 5, 6);     /* midpoint */
    size_t subnormal = x_jmp32(&t->b);
    x_patch32(&t->b, verytiny, xb_len(&t->b));
    vzero(t, 3); vconst(t, 4, 1, false); vconst(t, 5, 2, false);
    x_patch32(&t->b, normal, xb_len(&t->b));
    x_patch32(&t->b, subnormal, xb_len(&t->b));
    vfp_half_increment(t, false);
    vconst(t, 6, 0x7bff, false); P(0x6F, 4, 3); P38(0x37, 4, 6);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 4, RCX);
    size_t rounded_over = jrcxnz32(t);
    size_t rounded = x_jmp32(&t->b);
    x_patch32(&t->b, over, xb_len(&t->b));
    x_patch32(&t->b, rounded_over, xb_len(&t->b));
    vconst(t, 6, 0x28, false); P(0xEB, 12, 6);                 /* OFC | IXC */
    P(0x6F, 6, 1); x_sse_shift_ri(&t->b, 0x73, 2, 6, 15);
    vconst(t, 5, 1, false); P(0xD4, 6, 5); P38(0x29, 6, 13);
    P(0x6F, 4, 13); P38(0x29, 4, 7); P(0xEB, 6, 4); P(0xDB, 6, 5);
    vconst(t, 3, 0x7bff, false); P(0xD4, 3, 6);                /* infinity or max finite */
    x_patch32(&t->b, rounded, xb_len(&t->b));
    x_patch32(&t->b, zero, xb_len(&t->b));
    P(0x6F, 0, 3); P(0xEB, 0, 1);
}

#include "jit-conversions.inc"

/* Round two exact binary64 results to H and append them to xmm11.
 * Shared by fused and ordinary half arithmetic. */
static void vfp_half_pair_round(tctx *t, int Q)
{
    P(0x6F, 14, 0); P(0x6D, 14, 14);
    vconst(t, 2, 0x7fffffffffffffffull, true); P(0xDB, 2, 0);
    x_sse_shift_ri(&t->b, 0x73, 2, 2, 52);
    vconst(t, 3, 1009, true); P38(0x37, 3, 2);
    vconst(t, 4, 1038, true); P38(0x37, 2, 4); P(0xEB, 3, 2);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 3);
    size_t scalar_round = jrcxnz32(t);
    vfp_half_normal(t);
    x_sse_shift_ri(&t->b, 0x73, 3, 11, 4);
    x_sse_shift_ri(&t->b, 0x73, 7, 0, Q ? 12 : 4); P(0xEB, 11, 0);
    size_t pair_done = x_jmp32(&t->b);
    x_patch32(&t->b, scalar_round, xb_len(&t->b));
    for (int lane = 0; lane < 2; lane++) {
        if (lane) P(0x6F, 0, 14);
        vfp_half_round(t);
        x_sse_shift_ri(&t->b, 0x73, 3, 11, 2);
        x_sse_shift_ri(&t->b, 0x73, 7, 0, Q ? 14 : 6);
        P(0xEB, 11, 0);
    }
    x_patch32(&t->b, pair_done, xb_len(&t->b));
}

static void t_vfp_fma16(tctx *t, bool sub, unsigned rd, unsigned rn, unsigned rm, int idx, int Q, uint32_t insn, uint64_t pc)
{
    vld(t, 8, rn); vld(t, 9, rm); vld(t, 10, rd);
    if (idx >= 0) {
        /* H:L:M selects H[0..7]; Rm has only four bits. */
        x_sse_shift_ri(&t->b, 0x73, 3, 9, (uint8_t)(2 * idx));
        x_sse_rri(&t->b, 0xF2, 0x70, 9, 9, 0);                 /* pshuflw */
        x_sse_rri(&t->b, 0x66, 0x70, 9, 9, 0);                 /* broadcast low dword */
    }
    if (!Q) for (int r = 8; r <= 10; r++) x_sse_rr(&t->b, 0xF3, 0x7E, 0, r, r);
    vzero(t, 7); vzero(t, 4);
    vconst(t, 5, 0x7fff7fff7fff7fffull, true);
    vconst(t, 6, 0x7c007c007c007c00ull, true);
    for (int r = 8; r <= 10; r++) {
        P(0x6F, 0, r); P(0xDB, 0, 6);
        P(0x6F, 1, 0); P(0x75, 1, 7); P(0x75, 0, 6); P(0xEB, 4, 0);
        P(0x6F, 0, r); P(0xDB, 0, 5); P(0x75, 0, 7); P(0xDF, 0, 1); P(0xEB, 4, 0);
    }
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    x_movd_ld(&t->b, 13, xm(RBX, offsetof(hao_cpu, fpcr)));
    vconst(t, 0, 0x03c00000, false); P(0xDF, 0, 13);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 0, RCX);
    size_t control = jrcxnz32(t);
    x_sse_shift_ri(&t->b, 0x73, 6, 13, 40);
    x_sse_shift_ri(&t->b, 0x73, 2, 13, 62);                     /* ARM RMode */
    P(0x6C, 13, 13);
    if (sub) { vconst(t, 0, 0x8000800080008000ull, true); P(0xEF, 8, 0); } /* half FMLS negate */
    x_stmxcsr(&t->b, MXCSR_SLOT);
    x_movd_ld(&t->b, 0, MXCSR_SLOT);
    vconst(t, 1, 0x6000, false); P(0xDF, 1, 0);
    x_movd_st(&t->b, xm(RSP, 4), 1); x_ldmxcsr(&t->b, xm(RSP, 4)); /* RN for TwoSum */
    vzero(t, 11); vzero(t, 12);
    x_mov_ri(&t->b, R8, Q ? 4 : 2);
    size_t loop = xb_len(&t->b);
    vfp_half_widen(t, 0, 8); vfp_half_widen(t, 1, 9); vfp_half_widen(t, 2, 10);
    P(0x59, 0, 1);                                            /* exact product p */
    P(0x6F, 3, 0); P(0x58, 3, 2);                            /* RN sum s */
    P(0x6F, 4, 3); P(0x5C, 4, 0);                            /* c' = s - p */
    P(0x6F, 5, 3); P(0x5C, 5, 4);                            /* p' = s - c' */
    P(0x6F, 6, 0); P(0x5C, 6, 5);
    P(0x6F, 5, 2); P(0x5C, 5, 4); P(0x58, 6, 5);            /* TwoSum error */
    x_sse_rri(&t->b, 0x66, 0xC2, 6, 7, 4);                    /* half exactness: error != 0 */
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 6);
    size_t restore = jrcxnz32(t);
    /* RN supplies the right zero sign except opposite signs under RM.
     * For an exact zero, RM adds the sign of either term; +0 + +0 stays +0. */
    P(0x6F, 14, 3);
    P(0xEB, 0, 2); x_sse_shift_ri(&t->b, 0x73, 2, 0, 63);
    x_sse_shift_ri(&t->b, 0x73, 6, 0, 63);
    x_sse_rri(&t->b, 0x66, 0xC2, 3, 7, 0);                    /* exact zero, including -0 */
    vconst(t, 4, 2, false); P38(0x29, 4, 13); P(0x6C, 4, 4);
    P(0xDB, 0, 3); P(0xDB, 0, 4);
    P(0xEB, 0, 14);
    vfp_half_pair_round(t, Q);
    for (int r = 8; r <= 10; r++) x_sse_shift_ri(&t->b, 0x73, 3, r, 4);
    x_lea(&t->b, 1, R8, xm(R8, -1)); x_mov_rr(&t->b, 1, RCX, R8);
    size_t again = jrcxnz32(t); x_patch32(&t->b, again, loop);
    P(0x6F, 0, 12); P(0x6D, 0, 0); P(0xEB, 12, 0);          /* combine pair flags */
    x_movd_ld(&t->b, 0, MXCSR_SLOT); P(0xEB, 0, 12);
    x_movd_st(&t->b, xm(RSP, 4), 0); x_ldmxcsr(&t->b, xm(RSP, 4));
    vst(t, rd, 11, Q);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, restore, xb_len(&t->b)); x_ldmxcsr(&t->b, MXCSR_SLOT);
    x_patch32(&t->b, decline, xb_len(&t->b)); x_patch32(&t->b, control, xb_len(&t->b));
    helper_keep_flags(t, insn, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
}

enum { HALF_MUL, HALF_ADD, HALF_SUB, HALF_MAX, HALF_MIN };

/* Every finite binary16 product and sum fits binary64 exactly (at most
 * 22 product bits, or 40 bits spanning a sum). SSE computes the exact
 * result in the guest mode, including cancellation's zero sign; integer
 * rounding supplies the single binary16 rounding and its FPSR flags. */
static void t_vfp_arith16(tctx *t, int op, unsigned rd, unsigned rn, unsigned rm,
                          int idx, int Q, bool scalar, uint32_t insn, uint64_t pc)
{
    if (scalar) {
        x_mov_ld(&t->b, 2, RAX, VR(rn)); x_sse_rr(&t->b, 0x66, 0x6E, 0, 8, RAX);
        x_mov_ld(&t->b, 2, RAX, VR(rm)); x_sse_rr(&t->b, 0x66, 0x6E, 0, 9, RAX);
    } else { vld(t, 8, rn); vld(t, 9, rm); }
    if (idx >= 0) {
        x_sse_shift_ri(&t->b, 0x73, 3, 9, 2 * idx);
        x_sse_rri(&t->b, 0xF2, 0x70, 9, 9, 0);
        x_sse_rri(&t->b, 0x66, 0x70, 9, 9, 0);
    }
    if (!Q) for (int r = 8; r <= 9; r++) x_sse_rr(&t->b, 0xF3, 0x7E, 0, r, r);
    vzero(t, 7); vzero(t, 4);
    vconst(t, 5, 0x7fff7fff7fff7fffull, true);
    vconst(t, 6, 0x7c007c007c007c00ull, true);
    for (int r = 8; r <= 9; r++) {
        P(0x6F, 0, r); P(0xDB, 0, 6);
        P(0x6F, 1, 0); P(0x75, 1, 7); P(0x75, 0, 6); P(0xEB, 4, 0);
        P(0x6F, 0, r); P(0xDB, 0, 5); P(0x75, 0, 7); P(0xDF, 0, 1); P(0xEB, 4, 0);
    }
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    x_movd_ld(&t->b, 13, xm(RBX, offsetof(hao_cpu, fpcr)));
    vconst(t, 0, 0x03c00000, false); P(0xDF, 0, 13);
    x_sse_rr(&t->b, 0x66, 0x7E, 0, 0, RCX);
    size_t control = jrcxnz32(t);
    if (op == HALF_MAX || op == HALF_MIN) {
        /* A signed comparison of the monotonic half keys orders finite
         * values and -0 before +0. Selection preserves operand bits and
         * raises no flags; min/max do not need widening or rounding. */
        P(0x6F, 0, 8); P(0x6F, 1, 9);
        P(0x6F, 2, 0); P(0x6F, 3, 1);
        for (int r = 2; r <= 3; r++) {
            x_sse_shift_ri(&t->b, 0x71, 4, r, 15);
            x_sse_shift_ri(&t->b, 0x71, 2, r, 1);
        }
        P(0xEF, 2, 0); P(0xEF, 3, 1); P(0x65, 2, 3);
        P(0x6F, 3, 2);
        P(0xDB, 2, op == HALF_MAX ? 0 : 1);
        P(0xDF, 3, op == HALF_MAX ? 1 : 0); P(0xEB, 2, 3);
        vst(t, rd, 2, Q);
        size_t done = x_jmp32(&t->b);
        x_patch32(&t->b, decline, xb_len(&t->b)); x_patch32(&t->b, control, xb_len(&t->b));
        helper_keep_flags(t, insn, pc);
        x_patch32(&t->b, done, xb_len(&t->b));
        return;
    }
    x_sse_shift_ri(&t->b, 0x73, 6, 13, 40);
    x_sse_shift_ri(&t->b, 0x73, 2, 13, 62); P(0x6C, 13, 13);
    x_stmxcsr(&t->b, MXCSR_SLOT);
    vzero(t, 11); vzero(t, 12);
    if (!scalar) x_mov_ri(&t->b, R8, Q ? 4 : 2);
    size_t loop = xb_len(&t->b);
    vfp_half_widen(t, 0, 8); vfp_half_widen(t, 1, 9);
    static const uint8_t arithmetic[] = {0x59, 0x58, 0x5C};
    P(arithmetic[op], 0, 1); /* exact half arithmetic */
    if (scalar) {
        vfp_half_round(t); P(0x6F, 11, 0);
        x_sse_shift_ri(&t->b, 0x73, 7, 11, 14);
        x_sse_shift_ri(&t->b, 0x73, 3, 11, 14);
    } else {
        vfp_half_pair_round(t, Q);
        for (int r = 8; r <= 9; r++) x_sse_shift_ri(&t->b, 0x73, 3, r, 4);
        x_lea(&t->b, 1, R8, xm(R8, -1)); x_mov_rr(&t->b, 1, RCX, R8);
        size_t again = jrcxnz32(t); x_patch32(&t->b, again, loop);
    }
    P(0x6F, 0, 12); P(0x6D, 0, 0); P(0xEB, 12, 0);
    x_movd_ld(&t->b, 0, MXCSR_SLOT); P(0xEB, 0, 12);
    x_movd_st(&t->b, xm(RSP, 4), 0); x_ldmxcsr(&t->b, xm(RSP, 4));
    vst(t, rd, 11, Q);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, decline, xb_len(&t->b)); x_patch32(&t->b, control, xb_len(&t->b));
    helper_keep_flags(t, insn, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
}

static bool t_half_scalar(tctx *t, uint32_t i, uint64_t pc)
{
    if ((i & 0xFFE00C00) != 0x1EE00800) return false;
    unsigned op = BITS(i, 15, 12);
    int kind = op == 0 ? HALF_MUL : op == 2 ? HALF_ADD : op == 3 ? HALF_SUB :
               op == 4 || op == 6 ? HALF_MAX : op == 5 || op == 7 ? HALF_MIN : -1;
    if (kind < 0) return false;
    t_vfp_arith16(t, kind, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, 0, true, i, pc);
    return true;
}

/* Trim inactive lanes without touching EFLAGS. */
static void vfp_trim(tctx *t, int r, int bytes)
{
    if (bytes == 4) {
        x_sse_shift_ri(&t->b, 0x73, 7, r, 12);
        x_sse_shift_ri(&t->b, 0x73, 3, r, 12);
    } else if (bytes == 8) x_sse_rr(&t->b, 0xF3, 0x7E, 0, r, r);
}

/* Pairwise ADDPS/ADDPD, one guest rounding per pair. Integer admission
 * precedes arithmetic; a result near underflow or overflow restores the
 * pre-attempt MXCSR and invokes the original instruction helper. */
static void t_vfp_pair_add(tctx *t, uint32_t i, uint64_t pc, bool scalar)
{
    unsigned Q = BIT(i, 30), dbl = BIT(i, 22), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16), rd = BITS(i, 4, 0);
    int bytes = scalar ? (dbl ? 8 : 4) : (Q ? 16 : 8);
    vld(t, 0, rn);
    if (scalar) {
        if (!dbl) vfp_trim(t, 0, 8);
        P(0x6F, 1, 0);
        x_sse_shift_ri(&t->b, 0x73, 3, 1, dbl ? 8 : 4);
        vfp_trim(t, 0, bytes); vfp_trim(t, 1, bytes);
    } else {
        vld(t, 1, rm);
        if (dbl) {
            P(0x6F, 2, 0); P(0x6C, 0, 1); P(0x6D, 2, 1); P(0x6F, 1, 2);
        } else if (Q) {
            P(0x6F, 2, 0);
            x_sse_rri(&t->b, 0, 0xC6, 0, 1, 0x88); /* pair-add even lanes */
            x_sse_rri(&t->b, 0, 0xC6, 2, 1, 0xDD);
            P(0x6F, 1, 2);
        } else {
            P(0x6C, 0, 1); P(0x6F, 1, 0);
            x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0x88);
            x_sse_rri(&t->b, 0x66, 0x70, 1, 1, 0xDD);
            vfp_trim(t, 0, 8); vfp_trim(t, 1, 8);
        }
    }
    if (dbl) vfp_constants64(t); else vfp_constants(t);
    vzero(t, 4);
    if (dbl) { vfp_special64(t, 0); vfp_special64(t, 1); }
    else { vfp_special(t, 0); vfp_special(t, 1); }
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    x_stmxcsr(&t->b, MXCSR_SLOT);
    x_sse_rr(&t->b, dbl ? 0x66 : 0, 0x58, 0, 0, 1);
    /* Conservative result admission includes rejecting exact zero and
     * minimum-normal results. Inactive lanes never force a decline. */
    P(0x6F, 2, 0); P(0xDB, 2, dbl ? 11 : 7);
    if (dbl) P38(0x29, 2, 11); else P(0x76, 2, 7);
    vconst(t, 3, dbl ? 0x7fe0000000000000ull : 0x7f0000007f000000ull, true);
    P(0xDB, 3, 0);
    if (dbl) P38(0x29, 3, 12); else P(0x76, 3, 5);
    P(0xEB, 2, 3); vfp_trim(t, 2, bytes);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 2);
    size_t restore = jrcxnz32(t);
    vfp_trim(t, 0, bytes); vst(t, rd, 0, 1);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, restore, xb_len(&t->b)); x_ldmxcsr(&t->b, MXCSR_SLOT);
    x_patch32(&t->b, decline, xb_len(&t->b)); helper_keep_flags(t, i, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
}

/* xmm0 = min/max(xmm0, xmm1), admitted ordinary singles. Signed zeros
 * need ARM's AND (max) or OR (min), rather than SSE's second operand. */
static void vfp_reduce_pair(tctx *t, bool minimum)
{
    P(0x6F, 8, 0); x_sse_rr(&t->b, 0, minimum ? 0x5D : 0x5F, 0, 8, 1);
    P(0x6F, 2, 0); P(0xDB, 2, 6); P(0x76, 2, 5);
    P(0x6F, 3, 1); P(0xDB, 3, 6); P(0x76, 3, 5); P(0xDB, 2, 3);
    P(minimum ? 0xEB : 0xDB, 0, 1); P(0xDB, 0, 2);
    P(0xDF, 2, 8); P(0xEB, 0, 2);
}
static bool t_vfp_reduce(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned op = BITS(i, 16, 12), size = BITS(i, 23, 22), rd = BITS(i, 4, 0);
    if ((op != 0x0c && op != 0x0f) || !BIT(i, 29) || !BIT(i, 30) || (size & 1)) return false;
    bool minimum = size & 2;
    vld(t, 0, BITS(i, 9, 5)); vfp_constants(t); vzero(t, 4); vfp_special(t, 0);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    P(0x6F, 1, 0);
    x_sse_rri(&t->b, 0x66, 0x70, 0, 0, 0x88);
    x_sse_rri(&t->b, 0x66, 0x70, 1, 1, 0xDD);
    vfp_reduce_pair(t, minimum); /* (lane0 op lane1), (lane2 op lane3) */
    x_sse_rri(&t->b, 0x66, 0x70, 1, 0, 0x55);
    vfp_reduce_pair(t, minimum);
    vfp_trim(t, 0, 4); vst(t, rd, 0, 1);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, decline, xb_len(&t->b)); helper_keep_flags(t, i, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
    return true;
}

/* Comparisons with zero: integer admission before CMPPS, including all
 * signed-zero cases. Special lanes retain the interpreter's NaN/IDC rules. */
static bool t_vfp_zero(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned key = i & 0xBFFFFC00, Q = BIT(i, 30), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    int pred, swap;
    switch (key) {
    case 0x0EA0C800: pred = 1; swap = 1; break; /* FCMGT */
    case 0x2EA0C800: pred = 2; swap = 1; break; /* FCMGE */
    case 0x0EA0D800: pred = 0; swap = 0; break; /* FCMEQ */
    case 0x2EA0D800: pred = 2; swap = 0; break; /* FCMLE */
    case 0x0EA0E800: pred = 1; swap = 0; break; /* FCMLT */
    default: return false;
    }
    if (Q) vld(t, 0, rn); else x_movq_ld(&t->b, 0, VR(rn));
    vfp_constants(t); vzero(t, 4); vfp_special(t, 0);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    if (swap) { vzero(t, 1); x_sse_rri(&t->b, 0, 0xC2, 1, 0, (uint8_t)pred); }
    else { P(0x6F, 1, 0); x_sse_rri(&t->b, 0, 0xC2, 1, 5, (uint8_t)pred); }
    vst(t, rd, 1, (int)Q);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, decline, xb_len(&t->b)); helper_keep_flags(t, i, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
    return true;
}

/* three-same, floating point: the single-precision forms of t_vfp32 */
static bool t_vfp_three_same(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 11);
    if (U && op == 0x1a && size < 2 && (!(size & 1) || Q)) {
        t_vfp_pair_add(t, i, pc, false); return true;
    }
    if (size & 1) {
        int wide_op;
        if (!Q) return false;                                   /* .1D reserved */
        switch (U << 6 | (size >> 1) << 5 | (op & 7)) {
        case 0x02: wide_op = VFP_ADD; break;
        case 0x22: wide_op = VFP_SUB; break;
        case 0x43: wide_op = VFP_MUL; break;
        case 0x47: wide_op = VFP_DIV; break;
        default: return false;
        }
        t_vfp_wide(t, wide_op, true, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), 1, i, pc);
        return true;
    }
    if (!U && op == 0x1f) {
        t_vfp_fma(t, true, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, (int)Q, i, pc);
        return true;
    }
    if (!U && op == 0x19) {
        t_vfp_fma(t, size >> 1, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, (int)Q, i, pc);
        return true;
    }
    int o;
    switch (U << 6 | (size >> 1) << 5 | (op & 7)) {
    case 0x02: o = VFP_ADD; break;
    case 0x22: o = VFP_SUB; break;
    case 0x43: o = VFP_MUL; break;
    case 0x47: o = VFP_DIV; break;
    case 0x00: case 0x06: o = VFP_MAX; break;                    /* FMAXNM, FMAX: no NaN is admitted */
    case 0x20: case 0x26: o = VFP_MIN; break;
    case 0x04: o = VFP_EQ; break;
    case 0x44: o = VFP_GE; break;
    case 0x64: o = VFP_GT; break;
    default: return false;
    }
    t_vfp32(t, o, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, (int)Q, i, pc);
    return true;
}

/* MUL, MLA, MLS (by element), halfword and word lanes:
 * 0 Q U 01111 size L M Rm opcode H 0 Rn Rd, opcode 1000 (U = 0) or 0x00 (U = 1) */
static bool t_simd_indexed_int(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), size = BITS(i, 23, 22), op = BITS(i, 15, 12);
    unsigned L = BIT(i, 21), M = BIT(i, 20), H = BIT(i, 11), rm = BITS(i, 19, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!(U ? op == 0 || op == 4 : op == 8) || (size != 1 && size != 2)) return false;
    int es = 1 << size, idx = (int)(size == 1 ? H << 2 | L << 1 | M : H << 1 | L);
    if (size == 2) rm |= M << 4;
    x_mov_ld(&t->b, es, RAX, velem(rm, es, idx));
    vdup_rax(t, es, 1);
    P(0x6F, 1, 0);
    vld(t, 0, rn);
    vmul(t, size);
    if (op != 8) {
        vld(t, 1, rd);
        P(op == 4 ? psub[size] : padd[size], 1, 0);
        vst(t, rd, 1, (int)Q);
    } else
        vst(t, rd, 0, (int)Q);
    return true;
}

/* FMUL/FMLA/FMLS by element, single precision. H:L selects S[0..3];
 * M extends Rm to V0..V31 (only half precision restricts Vm to V0..V15). */
static bool t_vfp_indexed(tctx *t, uint32_t i, uint64_t pc)
{
    if (t_simd_indexed_int(t, i)) return true;
    unsigned op = BITS(i, 15, 12);
    if (!BIT(i, 29) && BITS(i, 23, 22) == 0 && (op == 1 || op == 5 || op == 9)) {
        unsigned half_idx = BIT(i, 11) << 2 | BIT(i, 21) << 1 | BIT(i, 20);
        if (op == 9) t_vfp_arith16(t, HALF_MUL, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 19, 16), (int)half_idx, BIT(i, 30), false, i, pc);
        else t_vfp_fma16(t, op == 5, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 19, 16), (int)half_idx, BIT(i, 30), i, pc);
        return true;
    }
    if (BIT(i, 29) || BITS(i, 23, 22) != 2 || (op != 9 && op != 1 && op != 5)) return false;
    unsigned rm = BITS(i, 20, 16), idx = BIT(i, 11) << 1 | BIT(i, 21);
    if (op == 9) t_vfp32(t, VFP_MUL, BITS(i, 4, 0), BITS(i, 9, 5), rm, (int)idx, (int)BIT(i, 30), i, pc);
    else t_vfp_fma(t, op == 5, BITS(i, 4, 0), BITS(i, 9, 5), rm, (int)idx, (int)BIT(i, 30), i, pc);
    return true;
}

/* FABS, FNEG (vector): sign-bit operations, no exceptions */
static bool t_vfp_absneg(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), U = BIT(i, 29), dbl = BIT(i, 22);
    if (dbl && !Q) return false;                                 /* .1D: reserved */
    vld(t, 0, BITS(i, 9, 5));
    vones(t, 1);
    uint8_t grp = dbl ? 0x73 : 0x72;
    if (U) x_sse_shift_ri(&t->b, grp, 6, 1, dbl ? 63 : 31);      /* the sign bit */
    else x_sse_shift_ri(&t->b, grp, 2, 1, 1);                    /* everything else */
    P(U ? 0xEF : 0xDB, 0, 1);
    vst(t, BITS(i, 4, 0), 0, (int)Q);
    return true;
}

/*
 * FRINTN, FRINTM, FRINTP, FRINTZ, FRINTA, FRINTX, FRINTI: vector .2S/.4S/.2D
 * and scalar S/D. These are fp_rint's fast path (rint_fast, a64_fp.c): only
 * operands whose lanes are all zero or normal are taken, found on the bits
 * before any FP instruction runs; a subnormal (FZ flushes it and raises
 * IDC), infinite or NaN lane (FPProcessNaN, DN) sends the instruction to the
 * helper with nothing raised. For admitted lanes ROUNDPS/ROUNDPD is
 * FPRoundInt exactly: the result is integral, never subnormal, keeps the
 * operand's sign (a zero result included), and the immediate picks the mode
 * (0-3: nearest-even, down, up, toward zero; 4: MXCSR.RC, the guest's FPCR
 * RMode, for FRINTX and FRINTI) and whether PE (FPSR.IXC) is raised for an
 * inexact result: only FRINTX raises it. A scalar or .2S operand is loaded
 * with the lanes above it zero, which round to +0 and raise nothing, so the
 * whole register is stored.
 *
 * FRINTA (ties away from zero) has no ROUND mode: r = trunc(x) (ROUND mode
 * 3), d = x - r, and r + copysign(1, x) when |d| >= 1/2, else r + x's signed
 * zero (so a zero r keeps its sign in every rounding mode). Every step is
 * exact for an admitted x, so nothing is raised: d is 0, x itself (|x| < 1)
 * or x's fraction (Sterbenz: r <= |x| <= 2r), a normal number or zero; the
 * comparison is an integer one on |d|'s bits; |r| < 2^23 (2^52) when the
 * sum is not with a zero.
 */
static void t_vfp_rint(tctx *t, int mode, int dbl, int width, unsigned rd, unsigned rn, uint32_t insn, uint64_t pc)
{
    static const uint8_t imm[7] = {0x08, 0x09, 0x0A, 0x0B, 0, 0x04, 0x0C};
    uint8_t rop = dbl ? 0x09 : 0x08, fpfx = dbl ? 0x66 : 0;       /* roundpd / roundps; addpd / addps prefix */
    int abs = dbl ? 10 : 6;
    if (width == 16) vld(t, 0, rn);
    else if (width == 8) x_movq_ld(&t->b, 0, VR(rn));
    else x_movd_ld(&t->b, 0, VR(rn));
    if (dbl) vfp_constants64(t);
    else if (!t->vfp_prev) vfp_constants(t);
    vzero(t, 4);
    if (dbl) vfp_special64(t, 0); else vfp_special(t, 0);
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);                      /* pmovmskb ecx, xmm4 */
    size_t decline = jrcxnz32(t);
    if (mode != RI_A) x_sse3a_rri(&t->b, rop, 0, 0, imm[mode]);
    else {
        x_sse3a_rri(&t->b, rop, 1, 0, 0x0B);                     /* r = trunc(x), no PE */
        P(0x6F, 2, 0);
        x_sse_rr(&t->b, fpfx, 0x5C, 0, 2, 1);                    /* d = x - r */
        P(0xDB, 2, abs);                                         /* |d| */
        vconst(t, 3, dbl ? 0x3FDFFFFFFFFFFFFFull : 0x3EFFFFFF3EFFFFFFull, true);
        vcmpgt(t, dbl ? 3 : 2, 2, 3);                            /* |d| >= 1/2 */
        vconst(t, 3, dbl ? 0x3FF0000000000000ull : 0x3F8000003F800000ull, true);
        P(0xDB, 2, 3);                                           /* 1.0 there */
        P(0x6F, 3, abs); P(0xDF, 3, 0);                          /* the sign of x */
        P(0xEB, 2, 3);
        x_sse_rr(&t->b, fpfx, 0x58, 0, 1, 2);                    /* r + (+-1.0 or +-0) */
        P(0x6F, 0, 1);
    }
    x_movdqu_st(&t->b, VR(rd), 0);
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, decline, xb_len(&t->b));
    helper_keep_flags(t, insn, pc);
    if (!dbl) vfp_constants(t);                                  /* the call clobbered them */
    x_patch32(&t->b, done, xb_len(&t->b));
    t->vfp_consts = !dbl;
}

/* FRINT* (vector): 0 Q U 01110 o2 sz 10000 1100 o1 10 Rn Rd */
static bool t_vfp_rint_vec(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned Q = BIT(i, 30), dbl = BIT(i, 22), key = BIT(i, 29) << 6 | BIT(i, 23) << 5 | BITS(i, 16, 12);
    int mode;
    switch (key) {
    case 0x18: mode = RI_N; break;
    case 0x19: mode = RI_M; break;
    case 0x38: mode = RI_P; break;
    case 0x39: mode = RI_Z; break;
    case 0x58: mode = RI_A; break;
    case 0x59: mode = RI_X; break;
    case 0x79: mode = RI_I; break;
    default: return false;                                       /* unallocated */
    }
    if (dbl && !Q) return false;                                 /* .1D: reserved */
    t_vfp_rint(t, mode, (int)dbl, Q ? 16 : 8, BITS(i, 4, 0), BITS(i, 9, 5), i, pc);
    return true;
}

/*
 * Advanced SIMD structure loads and stores: LD1-ST4 multiple structures,
 * LD1R, LD1/ST1 of one lane; no
 * offset, or post-indexed by the transfer size (Rm = 31) or by Xm. As in
 * the interpreter (a64_ldst_simd), registers are transferred one at a time
 * in order and the base is written back after the last access, so a fault
 * part way leaves the earlier registers loaded (or stored) and Xn unchanged;
 * accesses are the plain unaligned moves of t_ldst_reg. Interleaved loads
 * instead commit every destination only after all element reads succeed,
 * matching the interpreter's temporary vectors. Interleaved stores write
 * in element-major order. Reserved encodings stay with the helper.
 */
static bool t_ldst_simd(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned Q = BIT(i, 30), L = BIT(i, 22), post = BIT(i, 23), rm = BITS(i, 20, 16), rn = BITS(i, 9, 5), rt = BITS(i, 4, 0);
    unsigned size = BITS(i, 11, 10);
    int32_t total;
    size_t store_decline = SIZE_MAX;
    if (!post && rm) return false;
    if (!BIT(i, 24)) {                                           /* multiple structures */
        int regs, interleave = 0, bytes = Q ? 16 : 8;
        if (BIT(i, 21)) return false;
        switch (BITS(i, 15, 12)) {
        case 0x7: regs = 1; break;
        case 0xA: regs = 2; break;
        case 0x6: regs = 3; break;
        case 0x2: regs = 4; break;
        case 0x8: regs = 2; interleave = 1; break;
        case 0x4: regs = 3; interleave = 1; break;
        case 0x0: regs = 4; interleave = 1; break;
        default: return false;                                   /* unallocated */
        }
        if (interleave && size == 3 && !Q) return false;          /* .1D reserved */
        ldx(t, RAX, rn, 1, true);
        if (interleave) {
            int es = 1 << size;
            if (!L && es > 1) {
                /* Preserve memcpy's partial writes for elements crossing a
                 * protection boundary. Every supported VM page is a multiple
                 * of 4 KiB: decline a crossing transfer before any store. */
                x_sse_rr(&t->b, 0x66, 0x6E, 1, 0, RAX);
                vconst(t, 1, 4095, false); P(0xDB, 0, 1);
                vconst(t, 1, (uint64_t)(4096 - regs * bytes), false);
                P(0x66, 0, 1);                                  /* offset > 4096 - total */
                x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 0);
                store_decline = jrcxnz32(t);
                ldx(t, RAX, rn, 1, true);                        /* vconst used RAX */
            }
            if (L) for (int r = 0; r < regs; r++) vzero(t, r);
            for (int e = 0; e < bytes / es; e++)
                for (int r = 0; r < regs; r++) {
                    xmem m = xm(RAX, (e * regs + r) * es);
                    if (L) {
                        x_mov_ld(&t->b, es, RCX, m);
                        x_sse_rr(&t->b, 0x66, 0x6E, 1, 8, RCX); /* movq xmm8, rcx */
                        if (e) x_sse_shift_ri(&t->b, 0x73, 7, 8, (uint8_t)(e * es));
                        P(0xEB, r, 8);                          /* accumulate without publishing */
                    } else {
                        x_mov_ld(&t->b, es, RCX, velem((rt + (unsigned)r) & 31, es, e));
                        x_mov_st(&t->b, es, m, RCX);
                    }
                }
            if (L) for (int r = 0; r < regs; r++) x_movdqu_st(&t->b, VR((rt + (unsigned)r) & 31), r);
        } else for (int r = 0; r < regs; r++) {
            xmem m = xm(RAX, r * bytes), v = VR((rt + (unsigned)r) & 31);
            if (L) {
                if (Q) x_movdqu_ld(&t->b, 0, m); else x_movq_ld(&t->b, 0, m);
                x_movdqu_st(&t->b, v, 0);
            } else if (Q) {
                x_movdqu_ld(&t->b, 0, v); x_movdqu_st(&t->b, m, 0);
            } else {
                x_movq_ld(&t->b, 0, v); x_movq_st(&t->b, m, 0);
            }
        }
        total = regs * bytes;
    } else {                                                     /* single structure */
        unsigned opc = BITS(i, 15, 13), S = BIT(i, 12);
        int es, idx;
        if (BIT(i, 21) || (opc & 1)) return false;               /* LD2-LD4 / ST2-ST4 (and LDnR) */
        if (opc == 6) {                                          /* LD1R */
            if (!L || S) return false;
            es = 1 << size;
            ldx(t, RDX, rn, 1, true);
            x_mov_ld(&t->b, es, RAX, xm(RDX, 0));
            vdup_rax(t, es, (int)Q);
            x_movdqu_st(&t->b, VR(rt), 0);
            x_mov_rr(&t->b, 1, RAX, RDX);
        } else {
            switch (opc >> 1) {
            case 0: es = 1; idx = (int)(Q << 3 | S << 2 | size); break;
            case 1: if (size & 1) return false; es = 2; idx = (int)(Q << 2 | S << 1 | size >> 1); break;
            case 2:
                if (size == 0) { es = 4; idx = (int)(Q << 1 | S); }
                else if (size == 1 && !S) { es = 8; idx = (int)Q; }
                else return false;
                break;
            default: return false;
            }
            ldx(t, RAX, rn, 1, true);
            if (L) { x_mov_ld(&t->b, es, RCX, xm(RAX, 0)); x_mov_st(&t->b, es, velem(rt, es, idx), RCX); }
            else { x_mov_ld(&t->b, es, RCX, velem(rt, es, idx)); x_mov_st(&t->b, es, xm(RAX, 0), RCX); }
        }
        total = es;
    }
    if (post) {
        if (rm == 31) x_lea(&t->b, 1, RAX, xm(RAX, total));
        else { x_mov_ld(&t->b, 8, RCX, XR(rm)); x_lea(&t->b, 1, RAX, xmi(RAX, RCX, 1, 0)); }
        stx(t, rn, RAX, true);
    }
    if (store_decline != SIZE_MAX) {
        size_t done = x_jmp32(&t->b);
        x_patch32(&t->b, store_decline, xb_len(&t->b));
        helper_keep_flags(t, i, pc);
        x_patch32(&t->b, done, xb_len(&t->b));
    }
    return true;
}

/* TBL / TBX with one to four table registers (Vn ... Vn+len-1, modulo 32).
 * PSHUFB takes index bits 0-3 and gives 0 where bit 7 is set: for table
 * register k the indices are i - 16k (modulo 256), and a saturating add of
 * 0x70 sets bit 7 exactly for those outside 0..15. The OR of the per-register
 * results is TBL; TBX then keeps Vd's byte where i >= 16 * len. */
static bool t_simd_tbl(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), rm = BITS(i, 20, 16), len = BITS(i, 14, 13) + 1, tbx = BIT(i, 12);
    unsigned rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    vld(t, 1, rm);
    vconst(t, 3, 0x7070707070707070ull, true);
    if (len > 1) { vconst(t, 4, 0x1010101010101010ull, true); P(0x6F, 2, 1); }
    for (unsigned k = 0; k < len; k++) {
        int dst = k ? 9 : 0, sel = len > 1 ? 8 : 2;
        if (k) P(0xF8, 2, 4);                                    /* psubb: the indices into register k */
        P(0x6F, sel, len > 1 ? 2 : 1);
        P(0xDC, sel, 3);                                         /* paddusb 0x70 */
        vld(t, dst, (rn + k) & 31);
        P38(0x00, dst, sel);                                     /* pshufb */
        if (k) P(0xEB, 0, 9);
    }
    if (tbx) {
        vconst(t, 3, 0x0101010101010101ull * (16 * len - 1), true);
        P(0xD8, 1, 3);                                           /* psubusb: nonzero beyond the table */
        vzero(t, 3);
        P(0x74, 1, 3);                                           /* in range */
        vld(t, 2, rd);
        P(0xDF, 1, 2);                                           /* Vd where out of range */
        P(0xEB, 0, 1);
    }
    vst(t, rd, 0, (int)Q);
    return true;
}

/* EXT: bytes pos.. of Vm:Vn (PALIGNR); the 64-bit form rotates Vm<63:0>:Vn<63:0> */
static bool t_simd_ext(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), rm = BITS(i, 20, 16), pos = BITS(i, 14, 11), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
    if (!Q && (pos & 8)) return false;                           /* reserved */
    if (Q) {
        vld(t, 0, pos ? rm : rn);
        if (pos) { vld(t, 1, rn); x_sse3a_rri(&t->b, 0x0F, 0, 1, (uint8_t)pos); }   /* palignr */
    } else {
        x_movq_ld(&t->b, 0, VR(rn));
        x_movq_ld(&t->b, 1, VR(rm));
        P(0x6C, 0, 1);                                           /* punpcklqdq */
        if (pos) x_sse3a_rri(&t->b, 0x0F, 0, 0, (uint8_t)pos);
    }
    vst(t, rd, 0, (int)Q);
    return true;
}

/* Exact byte selection for all allocated ZIP/UZP/TRN arrangements. */
static bool t_simd_permute(tctx *t, uint32_t i)
{
    unsigned Q = BIT(i, 30), size = BITS(i, 23, 22), op = BITS(i, 14, 12);
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    if (!(op & 3) || (size == 3 && !Q)) return false;
    int es = 1 << size, n = (Q ? 16 : 8) / es;
    vld(t, 0, rn); vld(t, 1, rm);
    if ((op & 3) == 3) {
        static const uint8_t unpack[4] = {0x60, 0x61, 0x62, 0x6C};
        if (op == 7 && !Q) {
            x_sse_shift_ri(&t->b, 0x73, 3, 0, 4);
            x_sse_shift_ri(&t->b, 0x73, 3, 1, 4);
        }
        P((uint8_t)(unpack[size] + (op == 7 && Q ? (size == 3 ? 1 : 8) : 0)), 0, 1);
    } else {
        uint8_t masks[2][16];
        memset(masks, 0x80, sizeof masks);
        for (int e = 0; e < n; e++) {
            int source, lane;
            if ((op & 3) == 1) { /* UZP */
                int j = 2 * e + (op == 5);
                source = j >= n; lane = j % n;
            } else { /* TRN */
                source = e & 1; lane = (e & ~1) + (op == 6);
            }
            for (int b = 0; b < es; b++) masks[source][e * es + b] = (uint8_t)(lane * es + b);
        }
        for (int r = 0; r < 2; r++) {
            uint64_t lo, hi;
            memcpy(&lo, masks[r], 8); memcpy(&hi, masks[r] + 8, 8);
            vconst(t, 2, lo, false); vconst(t, 3, hi, false); P(0x6C, 2, 3);
            P38(0x00, r, 2); /* pshufb */
        }
        P(0xEB, 0, 1);
    }
    vst(t, rd, 0, (int)Q);
    return true;
}

static bool t_simd_integer_indexed(tctx *t, uint32_t i)
{
    unsigned size = BITS(i, 23, 22), op = BITS(i, 15, 12), U = BIT(i, 29);
    if (U || (size != 1 && size != 2) || (op != 12 && op != 13)) return false;
    unsigned rm = BITS(i, 19, 16), idx = (BIT(i, 11) << 1) | BIT(i, 21);
    if (size == 1) idx = idx << 1 | BIT(i, 20); else rm |= BIT(i, 20) << 4;
    t_simd_mulhigh(t, BITS(i, 4, 0), BITS(i, 9, 5), rm, size, BIT(i, 30), op == 13, (int)idx);
    return true;
}

/* The groups in a64_exec_simd's order (none of the masks overlap an
 * earlier group of that decoder). */
static bool t_simd(tctx *t, uint32_t i, uint64_t pc)
{
    if ((i & 0x9F200C00) == 0x0E200000) return t_simd_three_diff(t, i);

    if ((i & 0xFF3E0C00) == 0x4E280800) return t_aes(t, i);
    if ((i & 0x9F200400) == 0x0E200400) return BITS(i, 15, 11) >= 0x18 ? t_vfp_three_same(t, i, pc) : t_simd_three_same(t, i);
    if ((i & 0xBFBFFC00) == 0x2EA1F800) {                     /* FSQRT .2S/.4S/.2D */
        bool dbl = BIT(i, 22); int Q = BIT(i, 30);
        if (dbl && !Q) return false;
        t_vfp_wide(t, 4, dbl, BITS(i, 4, 0), BITS(i, 9, 5), 0, Q, i, pc);
        return true;
    }
    if ((i & 0x9F3E0C00) == 0x0E200800) return t_simd_two_misc(t, i, pc);
    if ((i & 0x9F3E0C00) == 0x0E300800) return t_vfp_reduce(t, i, pc) || t_simd_across(t, i);
    if ((i & 0x9FF80400) == 0x0F000400) return t_simd_modimm(t, i);
    if ((i & 0x9FE08400) == 0x0E000400) return t_simd_copy(t, i);
    if ((i & 0x9F800400) == 0x0F000400) return t_simd_shift(t, i, false);
    if ((i & 0xBFE08400) == 0x2E000000) return t_simd_ext(t, i);
    if ((i & 0xBFE08C00) == 0x0E000000) return t_simd_tbl(t, i);
    switch (i & 0xBFE0FC00) {
    case 0x0E401400: case 0x0EC01400: case 0x2E401C00:
    case 0x0E403400: case 0x0EC03400: case 0x0E400400: case 0x0EC00400: {
        uint32_t key = i & 0xBFE0FC00;
        int op = key == 0x2E401C00 ? HALF_MUL : key == 0x0E401400 ? HALF_ADD :
                 key == 0x0EC01400 ? HALF_SUB : BIT(i, 23) ? HALF_MIN : HALF_MAX;
        t_vfp_arith16(t, op, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, BIT(i, 30), false, i, pc);
        return true;
    }
    default: break;
    }
    if ((i & 0xBF60FC00) == 0x0E400C00) {
        t_vfp_fma16(t, BIT(i, 23), BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, BIT(i, 30), i, pc);
        return true;
    }
    if ((i & 0xBF208C00) == 0x0E000800) return t_simd_permute(t, i);
    if ((i & 0x9F000400) == 0x0F000000) return t_simd_integer_indexed(t, i) || t_vfp_indexed(t, i, pc);
    return false;
}

/* Scalar FMUL by element, S and D. Admit ordinary operands on integer
 * bits before SSE; reject tiny, minimum-normal or nonfinite results and
 * restore MXCSR before the unchanged descriptor helper. Conservative zero
 * declines keep the result check shared with underflow cases. */
static void t_scalar_indexed_mul(tctx *t, uint32_t i, uint64_t pc, int dbl, int idx)
{
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5), rm = BITS(i, 20, 16);
    fp_load(t, 0, rn, dbl);
    if (dbl) x_movq_ld(&t->b, 1, velem(rm, 8, idx));
    else x_movd_ld(&t->b, 1, velem(rm, 4, idx));
    if (dbl) vfp_constants64(t); else vfp_constants(t);
    vzero(t, 4);
    if (dbl) { vfp_special64(t, 0); vfp_special64(t, 1); }
    else { vfp_special(t, 0); vfp_special(t, 1); }
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t decline = jrcxnz32(t);
    x_stmxcsr(&t->b, MXCSR_SLOT);
    x_sse_rr(&t->b, dbl ? 0xF2 : 0xF3, 0x59, 0, 0, 1);        /* mulsd / mulss */
    vzero(t, 4);
    if (dbl) vfp_special64(t, 0); else vfp_special(t, 0);
    vconst(t, 3, dbl ? 0x7fe0000000000000ull : 0x7f000000ull, false);
    P(0xDB, 3, 0);
    if (dbl) P38(0x29, 3, 12); else P(0x76, 3, 5);
    P(0xEB, 4, 3);
    if (dbl) x_sse_rr(&t->b, 0xF3, 0x7E, 0, 4, 4);
    else {
        x_sse_rr(&t->b, 0x66, 0x7E, 0, 4, RAX);
        x_sse_rr(&t->b, 0x66, 0x6E, 0, 4, RAX);
    }
    x_sse_rr(&t->b, 0x66, 0xD7, 0, RCX, 4);
    size_t restore = jrcxnz32(t);
    if ((i & 0xFFE0FC00) == 0x1E208800) {
        vconst(t, 2, 0x80000000, false); P(0xEF, 0, 2);           /* FNMUL: negate after rounding */
    }
    fp_store(t, rd, 0);                                         /* scalar loads cleared inactive lanes */
    size_t done = x_jmp32(&t->b);
    x_patch32(&t->b, restore, xb_len(&t->b)); x_ldmxcsr(&t->b, MXCSR_SLOT);
    x_patch32(&t->b, decline, xb_len(&t->b));
    helper_keep_flags(t, i, pc);
    x_patch32(&t->b, done, xb_len(&t->b));
}

static bool t_scalar_indexed(tctx *t, uint32_t i, uint64_t pc)
{
    unsigned size = BITS(i, 23, 22), op = BITS(i, 15, 12);
    if (BIT(i, 29) || (size != 2 && size != 3) || (op != 1 && op != 5 && op != 9)) return false;
    if (size == 3 && (BIT(i, 21) || op != 9)) return false;       /* reserved L; double FMA remains a helper */
    int idx = size == 3 ? (int)BIT(i, 11) : (int)(BIT(i, 11) << 1 | BIT(i, 21));
    if (op == 9) t_scalar_indexed_mul(t, i, pc, size == 3, idx);
    else t_vfp_fma(t, op == 5, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), idx, 0, i, pc);
    return true;
}

static bool t_simd_scalar(tctx *t, uint32_t i, uint64_t pc)
{
    if ((i & 0xFF60FC00) == 0x5E20FC00) {
        t_vfp_fma(t, true, BITS(i, 4, 0), BITS(i, 9, 5), BITS(i, 20, 16), -1, 0, i, pc);
        return true;
    }

    if ((i & 0xdf3e0c00) == 0x5e200800 && t_vfp_estimate(t, i, pc, true)) return true;
    if ((i & 0xFFFFFC00) == 0x5EF1B800) { /* ADDP Dd, Vn.2D */
        vld(t, 0, BITS(i, 9, 5)); x_sse_rri(&t->b, 0x66, 0x70, 1, 0, 0xEE);
        P(0xD4, 0, 1); vst(t, BITS(i, 4, 0), 0, 0); return true;
    }

    if (t_tail_conversion(t, i, pc)) return true;
    if ((i & 0xFFBFFC00) == 0x7E30D800) {
        t_vfp_pair_add(t, i, pc, true); return true;
    }
    if ((i & 0xFFE0FC00) == 0x5E000400) {                        /* DUP (element), scalar: MOV Dd, Vn.D[1] ... */
        unsigned imm5 = BITS(i, 20, 16), rn = BITS(i, 9, 5), rd = BITS(i, 4, 0);
        if (!(imm5 & 15)) return false;
        int sz = __builtin_ctz(imm5), es = 1 << sz, idx = (int)(imm5 >> (sz + 1));
        if (es == 8) x_movq_ld(&t->b, 0, velem(rn, es, idx));
        else if (es == 4) x_movd_ld(&t->b, 0, velem(rn, es, idx));
        else { x_mov_ld(&t->b, es, RAX, velem(rn, es, idx)); x_sse_rr(&t->b, 0x66, 0x6E, 0, 0, RAX); }
        x_movdqu_st(&t->b, VR(rd), 0);
        return true;
    }
    if ((i & 0xDF000400) == 0x5F000000) return t_scalar_indexed(t, i, pc);
    if ((i & 0xDF800400) == 0x5F000400) return t_simd_shift(t, i, true);
    return false;
}
#undef P
#undef P38

/* returns: 0 = continue block, 1 = block ended */
static int translate_insn(tctx *t, uint32_t i, uint64_t pc)
{
    bool ok = false;
    t->vfp_prev = t->vfp_consts;
    t->vfp_consts = false;
    if (!jit_disabled_native) switch (BITS(i, 28, 25)) {
    case 0x8: case 0x9:
        switch (BITS(i, 25, 23)) {
        case 0: case 1: ok = t_adr(t, i, pc); break;
        case 2: ok = t_addsub_imm(t, i); break;
        case 4: ok = t_logical_imm(t, i); break;
        case 5: ok = t_movewide(t, i); break;
        case 6: ok = t_bitfield(t, i); break;
        case 7: ok = t_extr(t, i); break;
        default: break;
        }
        break;
    case 0x5: case 0xD:
        if ((i & 0x1F000000) == 0x0A000000) ok = t_logical_shift(t, i);
        else if ((i & 0x1F200000) == 0x0B000000) ok = t_addsub_shift(t, i);
        else if ((i & 0x1F200000) == 0x0B200000) ok = t_addsub_ext(t, i);
        else if ((i & 0x1FE00000) == 0x1A000000) ok = t_adc(t, i);
        else if ((i & 0x1FE00000) == 0x1A800000) ok = t_csel(t, i);
        else if ((i & 0x1FE00000) == 0x1A400000) ok = t_ccmp(t, i);
        else if ((i & 0x1F000000) == 0x1B000000) ok = t_dp3(t, i);
        else if ((i & 0x5FE00000) == 0x1AC00000) ok = t_dp2(t, i);
        else if ((i & 0x5FE00000) == 0x5AC00000) ok = t_dp1(t, i);
        break;
    case 0x4: case 0x6: case 0xC: case 0xE:
        if ((i & 0x3A000000) == 0x38000000) ok = t_ldst_reg(t, i);
        else if ((i & 0x3A000000) == 0x28000000) ok = t_ldst_pair(t, i);
        else if ((i & 0x3B000000) == 0x18000000) ok = t_ldr_lit(t, i, pc);
        else if ((i & 0x3F000000) == 0x08000000) ok = t_ordered(t, i);
        else if ((i & 0xBE000000) == 0x0C000000) ok = t_ldst_simd(t, i, pc);
        break;
    case 0xA: case 0xB:
        if ((i & 0xFFC00000) == 0xD5000000) ok = t_system(t, i);
        break;
    case 0xF:
        if ((i & 0x5E000000) == 0x1E000000) ok = t_fp(t, i, pc);
        else ok = t_simd_scalar(t, i, pc);
        break;
    case 0x7:
        if ((i & 0xFFC08000) == 0xCE000000) ok = t_eor3_bcax(t, i);
        else ok = t_vfp_zero(t, i, pc) || t_simd_fpconv(t, i, pc) || t_tail_conversion(t, i, pc) || t_simd(t, i, pc);
        break;
    default: break;
    }
    /* control flow is always native */
    if ((i & 0x7C000000) == 0x14000000) {                       /* B / BL */
        uint64_t target = pc + (uint64_t)(sext(BITS(i, 25, 0), 26) * 4);
        if (BIT(i, 31)) { gaddr(t, RAX, pc + 4); stx(t, 30, RAX, false); }
        exit_direct(t, target);
        ST(st_native);
        return 1;
    }
    if ((i & 0x7E000000) == 0x34000000) {                       /* CBZ / CBNZ: jrcxz keeps flags */
        uint64_t target = pc + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4);
        ldx(t, RCX, BITS(i, 4, 0), BIT(i, 31), false);
        size_t jz = x_jrcxz8(&t->b);
        size_t skip = x_jmp8(&t->b);         /* rcx != 0 */
        size_t zero = xb_len(&t->b);
        /* layout: [jrcxz Z][jmp NZ] Z: jmp to (cbz ? taken : fall) ; NZ: (cbz ? fall : taken) */
        x_patch8(&t->b, jz, zero);
        size_t zj = x_jmp32(&t->b);
        x_patch8(&t->b, skip, xb_len(&t->b));
        bool cbnz = BIT(i, 24);
        /* NZ path; each exit saves NZCV if its target needs it */
        bool saved_in = t->in_eflags, saved_dirty = t->dirty;
        exit_direct(t, cbnz ? target : pc + 4);
        t->in_eflags = saved_in; t->dirty = saved_dirty;
        x_patch32(&t->b, zj, xb_len(&t->b));
        exit_direct(t, cbnz ? pc + 4 : target);
        ST(st_native);
        return 1;
    }
    if ((i & 0x7E000000) == 0x36000000) {                       /* TBZ / TBNZ */
        uint64_t target = pc + (uint64_t)(sext(BITS(i, 18, 5), 14) * 4);
        unsigned bit = BIT(i, 31) << 5 | BITS(i, 23, 19);
        if (t->in_eflags && t->dirty && (exit_live(t, pc + 4) || exit_live(t, target))) fl_materialize(t);
        t->in_eflags = t->dirty = false;                        /* the test below replaces EFLAGS */
        x_mov_ld(&t->b, 1, RCX, xm(RBX, CPU_OFF_X(BITS(i, 4, 0)) + (int32_t)(bit / 8)));
        if (BITS(i, 4, 0) == 31) x_mov_ri(&t->b, RCX, 0);
        x_test_ri(&t->b, 0, RCX, 1 << (bit % 8));
        size_t j = x_jcc32(&t->b, BIT(i, 24) ? XCC_NE : XCC_E);
        exit_direct(t, pc + 4);
        x_patch32(&t->b, j, xb_len(&t->b));
        exit_direct(t, target);
        ST(st_native);
        return 1;
    }
    if ((i & 0xFF000000) == 0x54000000) {                       /* B.cond */
        uint64_t target = pc + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4);
        unsigned cond = BITS(i, 3, 0);
        if (cond >= 14) { exit_direct(t, target); ST(st_native); return 1; }
        fl_load(t);
        size_t j = x_jcc32(&t->b, a64_cond_to_xcc(cond));
        bool saved_in = t->in_eflags, saved_dirty = t->dirty;
        exit_direct(t, pc + 4);
        t->in_eflags = saved_in; t->dirty = saved_dirty;
        x_patch32(&t->b, j, xb_len(&t->b));
        exit_direct(t, target);
        ST(st_native);
        return 1;
    }
    if ((i & 0xFE000000) == 0xD6000000) {                       /* BR / BLR / RET (+PAC) */
        unsigned opc = BITS(i, 24, 21), op3 = BITS(i, 15, 10), rn = BITS(i, 9, 5);
        if (a64_branch_reg_valid(i)) {
            if (opc == 2 && op3) rn = 30;
            fl_materialize(t);
            ldx(t, RAX, rn, 1, false);
            if (opc & 1) { gaddr(t, RCX, pc + 4); stx(t, 30, RCX, false); }
            exit_indirect(t);
            ST(st_native);
            return 1;
        }
    }
    if ((i & 0xFFE0001F) == 0xD4000001) {                       /* SVC */
        fl_materialize(t);
        gaddr(t, RAX, pc + 4);
        x_mov_st(&t->b, 8, xm(RBX, CPU_OFF_PC), RAX);
        x_mov_mi(&t->b, 4, xm(RBX, CPU_OFF_EXITI), (int32_t)BITS(i, 20, 5));
        exit_with(t, HAO_EXIT_SVC);
        ST(st_native);
        return 1;
    }
    if (ok) { ST(st_native); return 0; }
    helper(t, i, pc, false);
    if (is_block_end(i)) {
        /* brk/hlt/udf etc.: helper already exited on failure; otherwise continue at pc */
        fl_materialize(t);
        x_mov_ld(&t->b, 8, RAX, xm(RBX, CPU_OFF_PC));
        exit_indirect(t);
        return 1;
    }
    return 0;
}

/* Fetch a block's instructions; returns count. */
static int fetch_block(uint64_t pc, hao_image *im, uint32_t *insns)
{
    int n = 0;
    while (n < MAX_BLOCK_INSNS) {
        uint64_t a = pc + 4u * (unsigned)n;
        if (im ? !im_has_code(im, a) : hao_image_for_code(a) != NULL) break;
        memcpy(&insns[n], hao_code_ptr_im(im, a), 4);   /* hao/code.h */
        if (is_block_end(insns[n++])) break;
    }
    return n;
}

/* Emit one block at the current position of t->b. */
static void emit_block(tctx *t, uint64_t pc, const uint32_t *insns, int n)
{
    t->im = t->win ? NULL : hao_image_for_code(pc);
    bool live = n > 0 ? block_end_live(t, pc, insns, n) : true;   /* n == 0: the image was released under the fetch */
    for (int k = n - 1; k >= 0; k--) {
        t->live_out[k] = live;
        int e = flag_effect(insns[k]);
        if (e & FL_WRITE) live = false;
        if (e & FL_READ) live = true;
    }
    t->in_eflags = t->dirty = false;
    t->vfp_consts = false;
    t->insns = insns;
    t->n = n;
    t->skip = 0;
    int k;
    for (k = 0; k < n; k++) {
        t->idx = k;
        if (t->rec_off) { t->rec_off[k] = (uint32_t)xb_len(&t->b); t->rec_fl[k] = t->in_eflags && t->dirty; t->nrec = k + 1; }
        if (t->skip) { t->skip--; ST(st_native); ST(st_fused); continue; }   /* folded into an earlier instruction */
        if (translate_insn(t, insns[k], pc + 4u * (unsigned)k)) break;
    }
    if (k == n) {
        /* the fall-through exit belongs to the next instruction */
        if (t->rec_off) { t->rec_off[n] = (uint32_t)xb_len(&t->b); t->rec_fl[n] = t->in_eflags && t->dirty; t->nrec = n + 1; }
        exit_direct(t, pc + 4u * (unsigned)n);
    }
    if (t->dry) return;
    st_blocks++;
    st_insns += (uint64_t)n;
}

/* jit_lock held. NULL when the block cannot be translated (code cache or
 * table full, or its translation overflowed): the caller interprets it. */
static void *translate(uint64_t pc)
{
    if (__atomic_load_n(&cache_full, __ATOMIC_ACQUIRE)) return NULL;
    if (table_used >= TABLE_MAX) { cache_exhausted(); return NULL; }
    uint32_t insns[MAX_BLOCK_INSNS];
    hao_image *im = hao_image_for_code(pc);
    int n = fetch_block(pc, im, insns);
    tctx t;
    memset(&t, 0, sizeof t);
    if (im) {
        if (!im->jit_slot) {
            /* fill and claim the slot before publishing it (see jit_lock) */
            uint8_t *slot = cache_alloc(16);
            if (!slot) { cache_exhausted(); return NULL; }
            memcpy(slot, &im->base, 8);
            __atomic_store_n(&im->jit_slot, slot, __ATOMIC_RELEASE);
        }
        t.base = im->base;
        t.base_slot = im->jit_slot;
    }
    size_t avail;
    uint8_t *start = block_open(&avail);
    if (!start) { cache_exhausted(); return NULL; }
    xb_init(&t.b, start, avail);
    emit_block(&t, pc, insns, n);
    if (t.b.overflow) {
        /* nothing was claimed. At the end of the cache the block did not
         * fit; otherwise an internal rel8 was out of range (a hao bug) */
        if (t.b.p >= t.b.end) cache_exhausted();
        else __atomic_fetch_or(&warn_pending, WARN_OVERFLOW, __ATOMIC_RELAXED);
        return NULL;
    }
    if (t.b.bad) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "x86 encoder rejected an operand or displacement in the block at %#llx\n", (unsigned long long)pc);
        abort();
    }
    block_commit(start, xb_len(&t.b));
    st_bytes += xb_len(&t.b);
    if (!insert(pc, start)) { cache_exhausted(); return NULL; }
    rmap_add(start, pc);
    return start;
}

/* Which guest instruction a host pc inside translated code belongs to.
 * Translated code updates cpu.pc only at block exits (and not at all
 * between chained blocks), so a signal handler gets the precise pc by
 * re-emitting the block: emission is deterministic and its length does not
 * depend on where the bytes go. Async-signal-safe: no locks, no allocation;
 * one locate at a time (a concurrent one reports false). */
static uint8_t loc_buf[MAX_BLOCK_BYTES];
static int loc_busy;

bool hao_jit_locate(uint64_t a, uint64_t *guest_pc, bool *flags_in_host)
{
    if (!cache || a < (uint64_t)(uintptr_t)cache || a >= (uint64_t)(uintptr_t)cache_end) return false;
    uint32_t n = __atomic_load_n(&rmap_n, __ATOMIC_ACQUIRE);
    if (!n || rmap[0].code > a) return false;
    uint32_t lo = 0, hi = n - 1;               /* last entry with code <= a */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (rmap[mid].code <= a) lo = mid; else hi = mid - 1;
    }
    uint64_t code = rmap[lo].code, pc = rmap[lo].pc;
    hao_image *im = hao_image_for_code(pc);
    if (!im || __atomic_exchange_n(&loc_busy, 1, __ATOMIC_ACQUIRE)) return false;
    static uint32_t insns[MAX_BLOCK_INSNS], off[MAX_BLOCK_INSNS + 1];
    static bool fl[MAX_BLOCK_INSNS + 1];
    int ni = fetch_block(pc, im, insns);
    tctx t;
    memset(&t, 0, sizeof t);
    t.dry = true;
    t.rec_off = off;
    t.rec_fl = fl;
    t.base = im->base;
    t.base_slot = im->jit_slot;
    xb_init(&t.b, loc_buf, sizeof loc_buf);
    bool ok = false;
    if (ni > 0) {
        emit_block(&t, pc, insns, ni);
        uint64_t rel = a - code;
        if (!t.b.overflow && rel < xb_len(&t.b) && t.nrec) {
            int k = t.nrec - 1;
            while (k > 0 && off[k] > rel) k--;
            *guest_pc = pc + 4u * (unsigned)k;
            *flags_in_host = fl[k];
            ok = true;
        }
    }
    __atomic_store_n(&loc_busy, 0, __ATOMIC_RELEASE);
    return ok;
}

/* ---------------------------------------------------------------- AOT */

/*
 * A cache is loaded only from a directory that hao_aot_dirs_for() lists for
 * this process, only if the file and every directory above it pass the
 * owner/mode rules of hao/aot.h, and only if hao_aot_check() accepts every
 * header field, entry and link. The file is read into memory, checked and
 * hashed there, and those same bytes are copied into the code cache, so a
 * file changed after the checks cannot slip in other code.
 *
 * Ownership says who wrote the cache, not what it was translated from: the
 * LC_UUID, vmsize and load commands in its header are whatever the file
 * given to `hao --aot` said. So the header also carries the code identity
 * of that file (hao/aot.h HAO_AOT_ID_*), and hao_aot_check() compares it
 * with the identity of the image being loaded (aot_image_id below):
 *  - a signed image: the cdhash of the signature in its mapped __LINKEDIT.
 *    `hao --aot` writes a cdhash only after guest_sig.c has checked every
 *    page of the slice against that CodeDirectory, on a private copy of the
 *    file that it also maps and translates (hao_aot_generate_file), so equal
 *    cdhashes mean the cache was translated from the bytes this image's
 *    signature covers: header, load commands and code;
 *  - an unsigned image: a SHA-256 over its header, load commands and
 *    executable segments as mapped, computed when a candidate cache names
 *    this guest and this build. No system directory holds or serves such a
 *    cache.
 */

static uint64_t st_aot_rejected;

static void block_successors(const uint32_t *insns, int n, uint64_t pc, uint64_t *out, int *nout)
{
    uint64_t last = pc + 4u * (unsigned)(n - 1);
    uint32_t i = insns[n - 1];
    *nout = 0;
    if ((i & 0x7C000000) == 0x14000000) {
        out[(*nout)++] = last + (uint64_t)(sext(BITS(i, 25, 0), 26) * 4);
        if (BIT(i, 31)) out[(*nout)++] = last + 4;       /* return address */
    } else if ((i & 0x7E000000) == 0x34000000 || (i & 0xFF000000) == 0x54000000) {
        out[(*nout)++] = last + (uint64_t)(sext(BITS(i, 23, 5), 19) * 4);
        out[(*nout)++] = last + 4;
    } else if ((i & 0x7E000000) == 0x36000000) {
        out[(*nout)++] = last + (uint64_t)(sext(BITS(i, 18, 5), 14) * 4);
        out[(*nout)++] = last + 4;
    } else if ((i & 0xFE000000) == 0xD6000000 && (BITS(i, 24, 21) & 7) == 1) {
        out[(*nout)++] = last + 4;                         /* after BLR */
    } else if ((i & 0xFFE0001F) == 0xD4000001) {
        out[(*nout)++] = last + 4;                         /* after SVC */
    } else if (!is_block_end(i)) {
        out[(*nout)++] = last + 4;                         /* size-limited block */
    }
}

/* The build identity: LC_UUID of the image holding this code (libhao.dylib
 * for both hao and the embedded stubs). ld64 derives it from the content,
 * so every changed build rejects every older cache. Bit zero of its last
 * byte additionally names the AES translation profile. Only the base UUID
 * and this explicit XOR-derived identity are accepted by packaging tools;
 * the runtime requires its own exact profile, so optional AES instructions
 * can never load on a CPU lacking AES-NI. */
static uint8_t gen_uuid[16];
static bool gen_ok;
static pthread_once_t gen_once = PTHREAD_ONCE_INIT;
static void gen_init(void)
{
    Dl_info di;
    if (!dladdr((const void *)(uintptr_t)gen_init, &di) || !di.dli_fbase) return;
    const struct mach_header_64 *mh = di.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return;
    const uint8_t *p = (const uint8_t *)(mh + 1), *e = p + mh->sizeofcmds;
    for (uint32_t k = 0; k < mh->ncmds && p + sizeof(struct load_command) <= e; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || p + lc->cmdsize > e) return;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            memcpy(gen_uuid, ((const struct uuid_command *)lc)->uuid, 16);
            static const uint8_t zero[16];
            gen_ok = memcmp(gen_uuid, zero, 16) != 0;
            pthread_once(&jit_cpu_once, jit_cpu_init);
            gen_uuid[15] ^= jit_aes ? 1 : 0;
            return;
        }
        p += lc->cmdsize;
    }
}
static const uint8_t *aot_gen(void)
{
    pthread_once(&gen_once, gen_init);
    return gen_ok ? gen_uuid : NULL;
}

/* The effective user's home from the user database (never $HOME), looked
 * up only for ordinary users and only again when the euid changes. */
static pthread_mutex_t pw_lock = PTHREAD_MUTEX_INITIALIZER;
static uid_t pw_uid = (uid_t)-1;
static bool pw_have;
static char pw_home[1024];
static const char *aot_pw_dir(uid_t euid, char *out, size_t n)
{
    if (euid < 501 || euid >= 0x80000000u) return NULL;
    hao_crit_enter();
    pthread_mutex_lock(&pw_lock);
    if (pw_uid != euid) {
        struct passwd pw, *r = NULL;
        char b[4096];
        pw_have = getpwuid_r(euid, &pw, b, sizeof b, &r) == 0 && r && r->pw_dir && strlen(r->pw_dir) < sizeof pw_home;
        if (pw_have) snprintf(pw_home, sizeof pw_home, "%s", r->pw_dir);
        pw_uid = euid;
    }
    bool ok = pw_have;
    if (ok) snprintf(out, n, "%s", pw_home);
    pthread_mutex_unlock(&pw_lock);
    hao_crit_leave();
    return ok ? out : NULL;
}

static int aot_dirs(hao_aot_dir *d, int *w, char *why, size_t n)
{
    int tier = hao_env_tier();
    uid_t eu = geteuid();
    char home[1024];
    const char *pw = tier == HAO_ENV_OPEN ? aot_pw_dir(eu, home, sizeof home) : NULL;
    return hao_aot_dirs_for(tier, eu, pw, hao_opt.cache, d, w, why, n);
}

static void aot_name(hao_image *im, char *out, size_t n, const char *ext)
{
    char u[33];
    for (int k = 0; k < 16; k++) snprintf(u + 2 * k, 3, "%02x", im->uuid[k]);
    snprintf(out, n, "%s.%s", u, ext);
}

static bool fd_acl_allows(int fd)
{
    acl_t a = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    bool r = hao_aot_acl_allows(a);
    if (a) acl_free(a);
    return r;
}

/* Open dir/name for reading if it and every directory above it are trusted.
 * Returns -1 with why empty when there is no such file, or set when the file
 * exists but is rejected. */
int hao_aot_open_trusted(const hao_aot_dir *d, const char *name, struct stat *st, char *why, size_t n)
{
    char canon[PATH_MAX], p[PATH_MAX + 80], w[400];
    why[0] = 0;
    if (!realpath(d->path, canon)) return -1;
    if (!hao_aot_literal_ok(d->path, why, n)) return -1;
    if (snprintf(p, sizeof p, "%s/%s", canon, name) >= (int)sizeof p) return -1;
    struct stat l;
    if (lstat(p, &l) != 0) {
        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES) snprintf(why, n, "%s", strerror(errno));
        return -1;
    }
    if (!hao_aot_dir_ok(canon, d->kind, geteuid(), why, n)) return -1;
    /* O_NOFOLLOW_ANY: no symlink anywhere; O_NONBLOCK: a FIFO cannot hang us */
    int fd = open(p, O_RDONLY | O_NOFOLLOW_ANY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) { snprintf(why, n, "%s", errno == ELOOP ? "is a symlink" : strerror(errno)); return -1; }
    if (fstat(fd, st) != 0 || st->st_dev != l.st_dev || st->st_ino != l.st_ino) snprintf(why, n, "changed while it was opened");
    else if (!hao_aot_stat_ok(st, d->kind, HAO_AOT_FILE, geteuid(), w, sizeof w)) snprintf(why, n, "%s", w);
    else if (fd_acl_allows(fd)) snprintf(why, n, "its ACL grants write access");
    if (why[0]) { close(fd); return -1; }
    return fd;
}

/* The whole file, in its own mapping: aot_free_all returns it to the
 * system at once (a freed malloc block this large stays in the process's
 * footprint, in malloc's cache of large blocks). */
static uint8_t *aot_read_all(int fd, uint64_t size)
{
    if (size > HAO_AOT_MAX_FILE) return NULL;
    void *m = mmap(NULL, size ? (size_t)size : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return NULL;
    uint8_t *buf = m;
    for (uint64_t off = 0; off < size;) {
        ssize_t r = pread(fd, buf + off, (size_t)(size - off), (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { munmap(buf, size ? (size_t)size : 1); return NULL; }
        off += (uint64_t)r;
    }
    return buf;
}

static void aot_free_all(uint8_t *buf, uint64_t size) { if (buf) munmap(buf, size ? (size_t)size : 1); }

/* ---- the code identity of a mapped image */

/* [a, a+len) lies in one mapped segment of the image */
static bool aot_mapped(const hao_image *im, uint64_t a, uint64_t len)
{
    for (uint32_t k = 0; k < im->n_identity_ranges; k++)
        if (a >= im->identity_ranges[k][0] && a <= im->identity_ranges[k][1] && len <= im->identity_ranges[k][1] - a) return true;
    return false;
}

/* bytes mapped at im->base in the segment that holds the mach header */
static uint64_t aot_hdr_avail(const hao_image *im)
{
    for (uint32_t k = 0; k < im->n_identity_ranges; k++)
        if (im->identity_ranges[k][0] <= im->base && im->base < im->identity_ranges[k][1]) return im->identity_ranges[k][1] - im->base;
    return 0;
}

typedef struct {
    uint32_t off, size;         /* LC_CODE_SIGNATURE: slice offset and size (size 0: unsigned) */
    const uint8_t *at;          /* where the image maps the signature (NULL: in no mapped segment) */
    uint64_t exec_end;          /* the highest slice offset an executable segment maps */
    uint64_t cmds_len;          /* mach header and load commands: bytes at im->base */
} aot_sig;

/* What the image's own (mapped) load commands say about its signature.
 * False when they are not mapped at im->base or are malformed. */
static bool aot_image_sig(const hao_image *im, aot_sig *s)
{
    memset(s, 0, sizeof *s);
    uint64_t avail = aot_hdr_avail(im);
    struct mach_header_64 mh;
    if (avail < sizeof mh) return false;
    memcpy(&mh, (const void *)(uintptr_t)im->base, sizeof mh);
    if (mh.magic != MH_MAGIC_64 || mh.sizeofcmds > avail - sizeof mh) return false;
    s->cmds_len = sizeof mh + mh.sizeofcmds;
    const uint8_t *cmds = (const uint8_t *)(uintptr_t)im->base + sizeof mh, *end = cmds + mh.sizeofcmds, *p = cmds;
    const struct linkedit_data_command *cs = NULL;
    for (uint32_t k = 0; k < mh.ncmds; k++) {
        const struct load_command *lc = (const void *)p;
        if ((size_t)(end - p) < sizeof *lc || lc->cmdsize < sizeof *lc || lc->cmdsize > (size_t)(end - p)) return false;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize < sizeof(struct segment_command_64)) return false;
        if (lc->cmd == LC_CODE_SIGNATURE) {
            if (cs || lc->cmdsize < sizeof *cs) return false;      /* two signatures: which one counts is not ours to pick */
            cs = (const void *)p;
        }
        p += lc->cmdsize;
    }
    if (cs && cs->datasize) { s->off = cs->dataoff; s->size = cs->datasize; }
    p = cmds;
    for (uint32_t k = 0; k < mh.ncmds; k++, p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *sg = (const void *)p;
        if (sg->cmd != LC_SEGMENT_64 || !sg->filesize || sg->filesize > UINT64_MAX - sg->fileoff) continue;
        if ((sg->initprot & VM_PROT_EXECUTE) && sg->fileoff + sg->filesize > s->exec_end) s->exec_end = sg->fileoff + sg->filesize;
        if (s->size && !s->at && s->off >= sg->fileoff && (uint64_t)s->off + s->size <= sg->fileoff + sg->filesize) {
            uint64_t a = sg->vmaddr + im->slide + (s->off - sg->fileoff);
            if (aot_mapped(im, a, s->size)) s->at = (const uint8_t *)(uintptr_t)a;
        }
    }
    return true;
}

typedef struct { uint32_t kind, hash; uint8_t id[HAO_AOT_ID_LEN]; } aot_id;
#define AOT_SIG_MAX (40u << 20)     /* xnu's CS_MAX_BLOB_SIZE, as guest_sig.c */

/* The code identity of a mapped image, from the image alone: nothing here
 * comes from a cache file or from what an earlier check reported.
 * Signed: the cdhash of the signature the image maps (guest_sig.c parses a
 * private copy; no page is hashed here: see the comment at the top of this
 * section for why the cdhash is enough). Unsigned: SHA-256 over the mapped
 * header, load commands and executable segments (hao_aot_code_id), read
 * where hao reads guest code (hao/code.h). */
static bool aot_image_id(const hao_image *im, aot_id *out, char *why, size_t n)
{
    aot_sig s;
    memset(out, 0, sizeof *out);
    if (!aot_image_sig(im, &s)) { snprintf(why, n, "this image's load commands are not mapped with its header, or are malformed"); return false; }
    if (s.size) {
        if (!s.at || s.size > AOT_SIG_MAX) { snprintf(why, n, "this image's code signature is not in a mapped segment"); return false; }
        char w[300] = "out of memory";
        uint8_t *blob = malloc(s.size), ht = 0;
        bool ok = blob != NULL;
        if (ok) {
            memcpy(blob, s.at, s.size);
            ok = hao_guest_sig_cdhash(blob, s.size, out->id, &ht, w, sizeof w);
        }
        free(blob);
        if (!ok) { snprintf(why, n, "this image's code signature is invalid (%s)", w); return false; }
        out->kind = HAO_AOT_ID_CDHASH;
        out->hash = ht;
        return true;
    }
    uint64_t lo[HAO_MAX_RANGES], hi[HAO_MAX_RANGES];
    const uint8_t *code[HAO_MAX_RANGES];
    int nc = im->ncode;
    for (int k = 0; k < nc; k++) {
        lo[k] = im->code_lo[k] - im->base;
        hi[k] = im->code_hi[k] - im->base;
        code[k] = hao_code_ptr_im(im, im->code_lo[k]);
    }
    hao_aot_code_id((const void *)(uintptr_t)im->base, s.cmds_len, lo, hi, code, nc, out->id);
    out->kind = HAO_AOT_ID_CODE;
    return true;
}

/* What the image expects of a cache. The kind of identity is known from the
 * load commands; the identity itself (x->code_id) is left for the caller to
 * compute when a file gets that far (hao_aot_check's HAO_AOT_NEED_ID). */
static void aot_expect(hao_image *im, const uint8_t *gen, uint64_t *lo, uint64_t *hi, hao_aot_expect *x)
{
    for (int k = 0; k < im->ncode; k++) { lo[k] = im->code_lo[k] - im->base; hi[k] = im->code_hi[k] - im->base; }
    aot_sig s;
    uint32_t kind = !aot_image_sig(im, &s) ? 0 : s.size ? HAO_AOT_ID_CDHASH : HAO_AOT_ID_CODE;
    *x = (hao_aot_expect){im->uuid, gen, im->vmsize,
                          hao_aot_guest_id((const void *)(uintptr_t)im->base, aot_hdr_avail(im), im->slice_size),
                          lo, hi, im->ncode, kind, 0, NULL};
}

/* A directory every process trusts (hao/aot.h): by kind or canonical path,
 * and, for a directory being written (dfd >= 0), by being the same directory
 * as a system one under another name (a firmlink, another spelling). */
static bool aot_dir_system(const hao_aot_dir *d, const char *canon, int dfd)
{
    static const char *const sys[] = {HAO_AOT_SYS_DIR, HAO_AOT_SYS_RO_DIR};
    if (hao_aot_dir_is_system(d->kind, canon)) return true;
    struct stat a, b;
    if (dfd < 0 || fstat(dfd, &a) != 0) return false;
    for (int k = 0; k < 2; k++)
        if (stat(sys[k], &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) return true;
    return false;
}

static void aot_reject(const char *dir, const char *name, const char *why)
{
    __atomic_fetch_add(&st_aot_rejected, 1, __ATOMIC_RELAXED);
    if (hao_verbose) fprintf(HAO_DIAG, "hao: AOT cache %s/%s rejected: %s\n", dir, name, why);
}

static int aot_cmp_code(void *ents, const void *a, const void *b)
{
    const hao_aot_ent *e = ents;
    uint32_t x = e[*(const uint32_t *)a].code_off, y = e[*(const uint32_t *)b].code_off;
    return x < y ? -1 : x > y;
}

/* Publish a checked cache. Other threads may be translating (an earlier
 * image's initializer started them), so its space comes from the
 * code-cache allocator, and the copy and the inserts are made under
 * jit_lock (no I/O there; the reverse map needs code added in address
 * order). A full cache or table refuses the blob. Blocks the JIT already
 * made for this image keep using their old base slot. */
static bool aot_install(hao_image *im, const hao_aot_view *v, char *why, size_t n)
{
    uint64_t cs = v->hdr->code_size;
    uint32_t nb = v->hdr->nblocks;
    /* blocks by code offset, for hao_jit_locate */
    uint32_t *order = malloc((size_t)nb * sizeof *order + 4);
    if (order) {
        for (uint32_t k = 0; k < nb; k++) order[k] = k;
        qsort_r(order, nb, sizeof *order, (void *)v->ents, aot_cmp_code);
    }
    size_t avail = 0;
    uint32_t k = 0;
    hao_crit_enter();
    pthread_mutex_lock(&jit_lock);
    uint8_t *code = NULL;
    if (!__atomic_load_n(&cache_full, __ATOMIC_ACQUIRE) && table_room(nb)) code = cache_alloc((size_t)cs);
    if (code) {
        memcpy(code, v->code, (size_t)cs);
        memcpy(code, &im->base, 8);       /* the base slot */
        __atomic_store_n(&im->jit_slot, code, __ATOMIC_RELEASE);
        for (; k < nb; k++)
            if (!insert(im->base + v->ents[k].guest_off, code + v->ents[k].code_off)) { cache_exhausted(); break; }
        for (uint32_t j = 0; order && j < nb; j++) rmap_add(code + v->ents[order[j]].code_off, im->base + v->ents[order[j]].guest_off);
        im->aot = code;
        st_aot_blocks += k;
    } else
        avail = cache_ptr < cache_end ? (size_t)(cache_end - cache_ptr) : 0;
    pthread_mutex_unlock(&jit_lock);
    hao_crit_leave();
    free(order);
    if (__atomic_load_n(&warn_pending, __ATOMIC_RELAXED)) jit_warn();
    if (!code) snprintf(why, n, "needs %llu KB and %u table entries, code cache has %zu KB free", (unsigned long long)(cs >> 10), nb, avail >> 10);
    return code != NULL;
}

/* Load the AOT cache for `im` from the first trusted directory that has a
 * valid one. Never called with jit_lock held (image loads, not translate). */
/* The process's footprint and its memory limit, as the kernel's
 * phys_footprint ledger has them: launchd's jetsam limit (InactiveHard 15
 * MB for modelmanagerd and findmylocateagent on 27.0) is that ledger's
 * limit. From the ledger system call on this process (xnu bsd/kern/
 * sys_generic.c ledger(): LEDGER_TEMPLATE_INFO names the entries,
 * LEDGER_ENTRY_INFO gives their balance and limit; layouts from
 * osfmk/kern/ledger.h). False when there is no limit or it cannot be read
 * (a sandbox may refuse the call). */
int ledger(int cmd, caddr_t arg1, caddr_t arg2, caddr_t arg3);
static bool footprint_ledger(uint64_t *footprint, uint64_t *limit)
{
    enum { LEDGER_ENTRY_INFO = 1, LEDGER_TEMPLATE_INFO = 2, NMAX = 160 };
    struct { char name[32], group[32], units[32]; } tmpl[NMAX];
    struct { int64_t balance, credit, debit; uint64_t limit, refill_period, last_refill; } ent[NMAX];
    int nt = NMAX, ne = NMAX;
    if (ledger(LEDGER_TEMPLATE_INFO, (caddr_t)tmpl, (caddr_t)&nt, NULL) != 0 || nt <= 0 || nt > NMAX) return false;
    if (ledger(LEDGER_ENTRY_INFO, (caddr_t)(intptr_t)getpid(), (caddr_t)ent, (caddr_t)&ne) != 0 || ne <= 0 || ne > NMAX) return false;
    for (int k = 0; k < nt && k < ne; k++) {
        if (strncmp(tmpl[k].name, "phys_footprint", sizeof tmpl[k].name)) continue;
        if (ent[k].balance < 0) return false;
        *footprint = (uint64_t)ent[k].balance;
        *limit = ent[k].limit;      /* LEDGER_LIMIT_INFINITY (INT64_MAX) when there is none */
        return true;
    }
    return false;
}

bool hao_footprint_limit(uint64_t *footprint, uint64_t *limit)
{
    return footprint_ledger(footprint, limit) && *limit < (uint64_t)INT64_MAX;
}

/* An AOT cache is installed whole: the file is read (transiently), its code
 * copied into the code cache and every block entered in the translation
 * table and the reverse map, about twice the file's size however little of
 * the program runs. modelmanagerd's 27.0 cache is 19 MB, past its 15 MB
 * jetsam limit before main. Under a memory limit a cache is loaded only
 * when that cost fits in half of what the limit leaves; otherwise the
 * program is translated as it runs (the JIT's memory grows with use).
 * HAO_AOT_LIMIT_KB (tests, open tier) stands in for the kernel's limit. */
static bool aot_fits(uint64_t file_size, char *why, size_t n)
{
    uint64_t fp, limit;
    if (!footprint_ledger(&fp, &limit)) return true;
    if (hao_opt.aot_limit_kb) limit = (uint64_t)hao_opt.aot_limit_kb << 10;
    if (limit >= (uint64_t)INT64_MAX) return true;
    uint64_t cost = 2 * file_size + file_size / 4, left = limit > fp ? limit - fp : 0;
    if (cost <= left / 2) return true;
    snprintf(why, n, "not loaded under the process's memory limit: it needs about %llu KB, the limit is %llu KB, "
             "the footprint %llu KB; translated as it runs",
             (unsigned long long)(cost >> 10), (unsigned long long)(limit >> 10), (unsigned long long)(fp >> 10));
    return false;
}

bool hao_aot_load(hao_image *im)
{
    /* no code cache (no executable memory): nothing to install, run interpreted */
    if (!im->has_uuid || hao_opt.no_aot || !hao_jit_init()) return false;
    const uint8_t *gen = aot_gen();
    if (!gen) { if (hao_verbose) fprintf(HAO_DIAG, "hao: AOT off: this hao build has no LC_UUID\n"); return false; }
    hao_aot_dir dirs[HAO_AOT_MAX_DIRS];
    int w;
    char why[512], name[64];
    int nd = aot_dirs(dirs, &w, why, sizeof why);
    if (!nd && why[0] && hao_verbose) fprintf(HAO_DIAG, "hao: %s\n", why);
    aot_name(im, name, sizeof name, "aot");
    uint64_t lo[HAO_MAX_RANGES], hi[HAO_MAX_RANGES];
    hao_aot_expect x;
    aot_expect(im, gen, lo, hi, &x);
    aot_id id;
    for (int d = 0; d < nd; d++) {
        struct stat st;
        int fd = hao_aot_open_trusted(&dirs[d], name, &st, why, sizeof why);
        if (fd < 0) { if (why[0]) aot_reject(dirs[d].path, name, why); continue; }
        if (x.id_kind == HAO_AOT_ID_CODE && aot_dir_system(&dirs[d], dirs[d].path, -1)) {
            close(fd);
            aot_reject(dirs[d].path, name, "this image is not code signed; a system cache directory serves signed code only");
            continue;
        }
        if (!aot_fits((uint64_t)st.st_size, why, sizeof why)) {
            close(fd);
            __atomic_fetch_add(&st_aot_rejected, 1, __ATOMIC_RELAXED);
            hao_diag_prefix(HAO_DIAG);
            fprintf(HAO_DIAG, "AOT cache %s/%s %s\n", dirs[d].path, name, why);
            return false;
        }
        uint8_t *buf = aot_read_all(fd, (uint64_t)st.st_size);
        close(fd);
        if (!buf) { aot_reject(dirs[d].path, name, st.st_size > (off_t)HAO_AOT_MAX_FILE ? "too large" : "cannot read it"); continue; }
        hao_aot_view v;
        int r = hao_aot_check(buf, (uint64_t)st.st_size, &x, &v, why, sizeof why);
        if (r == HAO_AOT_NEED_ID) {
            /* the file names this guest and this build: now the image's code
             * identity, which every later directory's file is checked against too */
            r = -1;
            if (aot_image_id(im, &id, why, sizeof why)) {
                if (id.kind != x.id_kind) snprintf(why, sizeof why, "this image's load commands changed while it was loaded");
                else {
                    x.id_hash = id.hash;
                    x.code_id = id.id;
                    r = hao_aot_check(buf, (uint64_t)st.st_size, &x, &v, why, sizeof why);
                }
            }
        }
        bool ok = r == 0 && aot_install(im, &v, why, sizeof why);
        uint32_t nb = ok ? v.hdr->nblocks : 0;
        uint64_t cs = ok ? v.hdr->code_size : 0;
        aot_free_all(buf, (uint64_t)st.st_size);
        if (!ok) { aot_reject(dirs[d].path, name, why); continue; }
        if (hao_verbose)
            fprintf(HAO_DIAG, "hao: AOT cache %s/%s loaded: %u blocks, %llu KB\n", dirs[d].path, name, nb, (unsigned long long)(cs >> 10));
        return true;
    }
    return false;
}

/* Open (creating as needed) a cache directory to write into. The existing
 * part is canonicalised, then the path is walked from "/" with O_NOFOLLOW
 * and each element is checked with fstat, so nothing can be redirected
 * between the check and the use. canon gets the canonical path. */
static int aot_open_dir_create(const hao_aot_dir *d, char *canon, size_t cn, char *why, size_t n)
{
    char lit[PATH_MAX], probe[PATH_MAX], full[PATH_MAX], w[400];
    if (snprintf(lit, sizeof lit, "%s", d->path) >= (int)sizeof lit || lit[0] != '/') { snprintf(why, n, "not an absolute path"); return -1; }
    size_t L = strlen(lit);
    while (L > 1 && lit[L - 1] == '/') lit[--L] = 0;
    if (!hao_aot_literal_ok(lit, why, n)) return -1;
    memcpy(probe, lit, L + 1);
    while (!realpath(probe, canon)) {
        if (errno != ENOENT) { snprintf(why, n, "%s: %s", probe, strerror(errno)); return -1; }
        char *s = strrchr(probe, '/');
        if (s == probe) { probe[1] = 0; continue; }
        *s = 0;
    }
    const char *rest = lit + (strcmp(probe, "/") ? strlen(probe) : 0);   /* missing part: "" or "/a/b" */
    if (snprintf(full, sizeof full, "%s%s", strcmp(canon, "/") ? canon : (rest[0] ? "" : "/"), rest) >= (int)sizeof full) {
        snprintf(why, n, "path too long");
        return -1;
    }
    mode_t mode = d->kind == HAO_AOT_USER ? 0700 : 0755;
    uid_t eu = geteuid();
    int dfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st;
    if (dfd < 0 || fstat(dfd, &st) != 0) { snprintf(why, n, "/: %s", strerror(errno)); if (dfd >= 0) close(dfd); return -1; }
    bool last = !strcmp(full, "/");
    if (!hao_aot_stat_ok(&st, d->kind, last ? HAO_AOT_DIR : HAO_AOT_ANCESTOR, eu, w, sizeof w) || fd_acl_allows(dfd)) {
        snprintf(why, n, "/: %s", w[0] ? w : "its ACL grants write access");
        close(dfd);
        return -1;
    }
    for (const char *c = full + 1; !last;) {
        const char *e = strchr(c, '/');
        size_t cl = e ? (size_t)(e - c) : strlen(c);
        char comp[NAME_MAX + 1];
        if (cl == 0 || cl > NAME_MAX) { snprintf(why, n, "bad path element"); close(dfd); return -1; }
        memcpy(comp, c, cl);
        comp[cl] = 0;
        if (!strcmp(comp, ".") || !strcmp(comp, "..")) { snprintf(why, n, "'%s' in path", comp); close(dfd); return -1; }
        last = !e;
        int nfd = openat(dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0 && errno == ENOENT) {
            if (mkdirat(dfd, comp, mode) != 0 && errno != EEXIST) { snprintf(why, n, "mkdir %s: %s", comp, strerror(errno)); close(dfd); return -1; }
            nfd = openat(dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        close(dfd);
        if (nfd < 0) { snprintf(why, n, "%s: %s", comp, errno == ELOOP || errno == ENOTDIR ? "a symlink or not a directory" : strerror(errno)); return -1; }
        dfd = nfd;
        w[0] = 0;
        if (fstat(dfd, &st) != 0 || !hao_aot_stat_ok(&st, d->kind, last ? HAO_AOT_DIR : HAO_AOT_ANCESTOR, eu, w, sizeof w) ||
            fd_acl_allows(dfd)) {
            snprintf(why, n, "%.*s: %s", (int)(c + cl - full), full, w[0] ? w : "its ACL grants write access");
            close(dfd);
            return -1;
        }
        c = e ? e + 1 : c + cl;
    }
    snprintf(canon, cn, "%s", full);
    return dfd;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/* Translate every reachable block of `im` into one position-independent
 * blob and write it to dfd/<uuid>.aot. */
static int aot_generate(hao_image *im, const uint8_t *gen, int dfd, const hao_aot_dir *d, const char *dir, const aot_id *id,
                        bool verbose)
{
    int rc = -1, ofd = -1;
    bool oom = false;
    char name[64], tmp[96] = "", why[512];
    aot_name(im, name, sizeof name, "aot");
    hao_jit_init();   /* translator options; the output goes to a heap buffer, so a refused cache is fine */
    /* leaders: function starts, entry, initializers, hints */
    size_t cap = 4096, nwork = 0, nents = 0, capents = 0, nl = 0;
    uint64_t *work = malloc(cap * 8), *sorted = NULL;
    uint8_t *seen = calloc((size_t)(im->vmsize / 4) + 1, 1), *buf = NULL, *file = NULL;
    hao_aot_ent *ents = NULL;
    hao_aot_link *links = NULL;
    tctx t;
    memset(&t, 0, sizeof t);
    if (!work || !seen) goto oom;
#define PUSH(a) do { uint64_t _a = (a); if (hao_image_for_code(_a) == im && !(_a & 3) && !seen[(_a - im->base) / 4]) { \
        if (nwork == cap) { uint64_t *_n = realloc(work, cap * 2 * 8); if (!_n) { oom = true; break; } work = _n; cap *= 2; } \
        seen[(_a - im->base) / 4] = 1; work[nwork++] = _a; } } while (0)
    /* LC_FUNCTION_STARTS as macho.c decoded it at load */
    for (size_t k = 0; k < im->nfstarts; k++) PUSH(im->fstarts[k]);
    if (im->entry) PUSH(im->entry);
    char hn[64];
    aot_name(im, hn, sizeof hn, "hints");
    int hfd = openat(dfd, hn, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (hfd >= 0) {
        struct stat hs;
        char w[256] = "its ACL grants write access";
        FILE *hf = NULL;
        if (fstat(hfd, &hs) == 0 && hao_aot_stat_ok(&hs, d->kind, HAO_AOT_FILE, geteuid(), w, sizeof w) && !fd_acl_allows(hfd))
            hf = fdopen(hfd, "r");
        else
            fprintf(stderr, "hao-aot: %s/%s ignored: %s\n", dir, hn, w);
        if (hf) {
            unsigned long long off;
            while (fscanf(hf, "%llx", &off) == 1) PUSH(im->base + off);
            fclose(hf);
        } else close(hfd);
    }
    if (oom) goto oom;

    /* Keep the emitter arena fixed: its base slot and RIP-relative operands
     * retain pointers into it. Large images can exceed the old 64-MiB arena
     * before all reachable blocks are emitted. Budget sixteen output bytes
     * per executable input byte, saturating at 1 GiB (below rel32's reach),
     * with the existing 64-MiB floor. This is a bounded estimate, not a proof
     * of expansion size: the existing checked overflow still refuses a cache
     * rather than relocating live emitter pointers or writing partial output. */
    const size_t maxbuf = (size_t)1 << 30;
    uint64_t code_bytes = 0;
    for (int k = 0; k < im->ncode; k++) {
        uint64_t n = im->code_hi[k] - im->code_lo[k];
        if (n >= maxbuf / 16 - code_bytes) { code_bytes = maxbuf / 16; break; }
        code_bytes += n;
    }
    size_t bufsz = (size_t)code_bytes * 16;
    if (bufsz < (64u << 20)) bufsz = 64u << 20;
    buf = malloc(bufsz);
    t.links = malloc(1024 * sizeof *t.links);
    t.caplinks = 1024;
    if (!buf || !t.links) goto oom;
    xb_init(&t.b, buf, bufsz);
    xb_u64(&t.b, im->base);   /* base slot at offset 0 (patched at load) */
    xb_u64(&t.b, 0);
    t.base = im->base;
    t.base_slot = buf;
    t.pic = true;
    uint64_t ninsns = 0;
    for (size_t wi = 0; wi < nwork; wi++) {
        uint64_t pc = work[wi];
        uint32_t insns[MAX_BLOCK_INSNS];
        int n = fetch_block(pc, im, insns);
        if (n <= 0) continue;
        while (xb_len(&t.b) & 15) x_int3(&t.b);
        if (nents == capents) {
            size_t nc = capents ? capents * 2 : 4096;
            hao_aot_ent *ne = realloc(ents, nc * sizeof *ents);
            if (!ne) goto oom;
            ents = ne;
            capents = nc;
        }
        ents[nents].guest_off = (uint32_t)(pc - im->base);
        ents[nents].code_off = (uint32_t)xb_len(&t.b);
        nents++;
        emit_block(&t, pc, insns, n);
        if (!t.links) goto oom;
        ninsns += (uint64_t)n;
        if (t.b.overflow) { fprintf(stderr, "hao-aot: output buffer overflow\n"); goto out; }
        if (t.b.bad) {
            fprintf(stderr, "hao-aot: x86 encoder rejected an operand or displacement at %#llx: no cache written\n", (unsigned long long)pc);
            goto out;
        }
        if (t.not_pic) {
            fprintf(stderr, "hao-aot: block at %#llx needs an absolute guest address (more than 2 GB from the image base): no cache written\n",
                    (unsigned long long)pc);
            goto out;
        }
        uint64_t succ[3];
        int ns;
        block_successors(insns, n, pc, succ, &ns);
        for (int k = 0; k < ns; k++) PUSH(succ[k]);
        if (oom) goto oom;
    }
    /* resolve direct links between AOT blocks; record each for the loader */
    sorted = malloc((nents ? nents : 1) * 8);
    links = malloc((t.nlinks ? (size_t)t.nlinks : 1) * sizeof *links);
    if (!sorted || !links) goto oom;
    for (size_t e = 0; e < nents; e++) sorted[e] = (uint64_t)ents[e].guest_off << 32 | e;
    qsort(sorted, nents, 8, cmp_u64);
    for (int k = 0; k < t.nlinks; k++) {
        uint64_t tg = t.links[k].target;
        if (hao_image_for_code(tg) != im) continue;
        uint32_t go = (uint32_t)(tg - im->base);
        size_t lo = 0, hi = nents;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if ((uint32_t)(sorted[mid] >> 32) < go) lo = mid + 1; else hi = mid; }
        if (lo == nents || (uint32_t)(sorted[lo] >> 32) != go) continue;
        uint32_t idx = (uint32_t)sorted[lo];
        if (!x_patch32(&t.b, t.links[k].site, ents[idx].code_off)) {
            fprintf(stderr, "hao-aot: bad link site %#x: no cache written\n", t.links[k].site);
            goto out;
        }
        links[nl++] = (hao_aot_link){t.links[k].site, idx};
    }
    hao_aot_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, HAO_AOT_MAGIC, 8);
    h.version = HAO_AOT_VERSION;
    h.hdr_size = sizeof h;
    memcpy(h.uuid, im->uuid, 16);
    memcpy(h.gen, gen, 16);
    h.vmsize = im->vmsize;
    uint64_t lo_r[HAO_MAX_RANGES], hi_r[HAO_MAX_RANGES];
    hao_aot_expect x;
    aot_expect(im, gen, lo_r, hi_r, &x);
    if (x.id_kind != id->kind) { fprintf(stderr, "hao-aot: %s: code identity of the wrong kind: no cache written\n", im->path); goto out; }
    x.id_hash = id->hash;
    x.code_id = id->id;
    h.guest_id = x.guest_id;
    h.id_kind = id->kind;
    h.id_hash = id->hash;
    memcpy(h.code_id, id->id, sizeof h.code_id);
    h.nblocks = (uint32_t)nents;
    h.nlinks = (uint32_t)nl;
    h.code_size = xb_len(&t.b);
    h.file_size = sizeof h + nents * sizeof *ents + nl * sizeof *links + h.code_size;
    file = malloc((size_t)h.file_size);
    if (!file) goto oom;
    uint8_t *q = file + sizeof h;
    memcpy(q, ents, nents * sizeof *ents); q += nents * sizeof *ents;
    memcpy(q, links, nl * sizeof *links); q += nl * sizeof *links;
    memcpy(q, buf, (size_t)h.code_size);
    /* The base slot holds this run's (ASLR) image base, which aot_install
     * overwrites with the loading image's: zero in the file, so one guest and
     * one libhao always give the same bytes (reproducible payloads). */
    memset(q, 0, 8);
    h.payload_hash = hao_aot_hash64(file + sizeof h, (size_t)(h.file_size - sizeof h), 0);
    memcpy(file, &h, sizeof h);
    /* the loader's own checks, so generation and loading cannot drift apart */
    if (hao_aot_check(file, h.file_size, &x, NULL, why, sizeof why) != 0) {
        fprintf(stderr, "hao-aot: %s: generated cache fails the loader's check: %s\n", im->path, why);
        goto out;
    }
    snprintf(tmp, sizeof tmp, ".%s.%d.%08x", name, (int)getpid(), arc4random());
    ofd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (ofd < 0) { fprintf(stderr, "hao-aot: %s/%s: %s\n", dir, tmp, strerror(errno)); tmp[0] = 0; goto out; }
    int cr = 0;
    if (fchmod(ofd, 0644) != 0 || !write_all(ofd, file, (size_t)h.file_size) || fsync(ofd) != 0) cr = -1;
    if (close(ofd) != 0) cr = -1;
    ofd = -1;
    if (cr != 0 || renameat(dfd, tmp, dfd, name) != 0) { fprintf(stderr, "hao-aot: %s/%s: %s\n", dir, name, strerror(errno)); goto out; }
    tmp[0] = 0;
    if (verbose) {
        char idx[2 * HAO_AOT_ID_LEN + 1];
        int nb = id->kind == HAO_AOT_ID_CDHASH ? HAO_AOT_CDHASH_LEN : HAO_AOT_ID_LEN;
        for (int k = 0; k < nb; k++) snprintf(idx + 2 * k, 3, "%02x", id->id[k]);
        fprintf(stderr, "hao-aot: %s: %zu blocks, %llu insns (%.4f%% translated natively, rest via interpreter helper), %zu/%d links, "
                "%zu KB code, bound to %s %s -> %s/%s\n",
                im->path, nents, (unsigned long long)ninsns, st_native + st_helper ? 100.0 * st_native / (st_native + st_helper) : 0.0,
                nl, t.nlinks, (size_t)(h.code_size >> 10), id->kind == HAO_AOT_ID_CDHASH ? "cdhash" : "code hash (unsigned)", idx, dir, name);
    }
    rc = 0;
    goto out;
oom:
    fprintf(stderr, "hao-aot: %s: out of memory\n", im->path);
out:
    if (ofd >= 0) close(ofd);
    if (tmp[0]) unlinkat(dfd, tmp, 0);
    free(buf); free(file); free(ents); free(links); free(sorted); free(work); free(seen); free(t.links);
    return rc;
#undef PUSH
}

/* A private copy of the guest file, in the directory the cache goes to
 * (aot_open_dir_create checked that only this user, or root, can write
 * there): created exclusively with mode 0600, so from its first byte nobody
 * else can change it. Returns its descriptor, and its name in tmp. */
static int aot_private_copy(const char *path, int dfd, char *tmp, size_t tn, char *why, size_t n)
{
    enum { CHUNK = 1 << 20 };
    int sfd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC), tfd = -1;
    struct stat st;
    uint8_t *buf = NULL;
    tmp[0] = 0;
    if (sfd < 0) { snprintf(why, n, "open %s: %s", path, strerror(errno)); return -1; }
    if (fstat(sfd, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(why, n, "%s: not a regular file", path); goto fail; }
    snprintf(tmp, tn, ".guest.%d.%08x", (int)getpid(), arc4random());
    tfd = openat(dfd, tmp, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (tfd < 0) { snprintf(why, n, "%s: cannot create a private copy: %s", path, strerror(errno)); tmp[0] = 0; goto fail; }
    buf = malloc(CHUNK);
    if (!buf || fchmod(tfd, 0600) != 0) { snprintf(why, n, "%s: cannot create a private copy", path); goto fail; }
    /* Not a Mach-O or fat file (a script, a disk image named by mistake): its first page is
     * all the loader needs to say so. */
    uint64_t size = (uint64_t)st.st_size;
    uint32_t magic = 0;
    if (pread(sfd, &magic, 4, 0) != 4 || (magic != MH_MAGIC_64 && magic != FAT_CIGAM && magic != FAT_CIGAM_64)) size = size < 4096 ? size : 4096;
    for (uint64_t off = 0; off < size;) {
        uint64_t want = size - off;
        ssize_t r = pread(sfd, buf, want < CHUNK ? (size_t)want : CHUNK, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) break;                                /* it shrank: what is there is what gets checked */
        if (r < 0 || !write_all(tfd, buf, (size_t)r)) { snprintf(why, n, "%s: cannot copy it: %s", path, strerror(errno)); goto fail; }
        off += (uint64_t)r;
    }
    free(buf);
    close(sfd);
    return tfd;
fail:
    free(buf);
    close(sfd);
    if (tfd >= 0) close(tfd);
    if (tmp[0]) unlinkat(dfd, tmp, 0);
    return -1;
}

/*
 * `hao --aot FILE`: translate FILE into dfd/<uuid>.aot.
 *
 * The cache will be run in place of FILE's code by every process that
 * trusts this directory and loads an image with FILE's identity, so the
 * identity written must be that of the very bytes translated, whoever owns
 * FILE and whatever they do to it meanwhile. hao therefore copies FILE
 * first and works on the copy alone: the copy is mapped (the loader reports
 * which file it mapped), its signature is checked in full on the copy's
 * descriptor (guest_sig.c: structure, CodeDirectory, every page hash), and
 * the translator reads the mapping.
 *
 *  - Signed, and the signature validates: the identity is its cdhash. Who
 *    signed it and what it is entitled to decide whether hao runs a
 *    program, not what its code is; only an image with this cdhash ever
 *    loads the cache.
 *  - Signed, and it does not validate: no cache, anywhere. A cdhash would
 *    name code the file does not hold.
 *  - Unsigned: the identity is the code hash. Not into a system directory:
 *    those serve every user and guarded processes, which run signed code
 *    only, and the loader would have to hash the code on every load. In the
 *    user's own or an explicit directory it is allowed: the user can write
 *    any file there already, and only that user's ordinary processes read it.
 *
 * cache_dir is `--cache DIR`, already absolute, or NULL for the directory
 * this process writes to by default. It is checked as HAO_CACHE's directory
 * is (ownership, modes, ACLs and symlinks of every element; the rule for
 * unsigned guests if it is, or lies in, a system directory).
 */
int hao_aot_generate_file(const char *path, bool verbose, const char *cache_dir)
{
    const uint8_t *gen = aot_gen();
    if (!gen) { fprintf(stderr, "hao-aot: this hao build has no LC_UUID\n"); return -1; }
    if (hao_env_tier() == HAO_ENV_SETUGID) { fprintf(stderr, "hao-aot: refused in a setuid/setgid process\n"); return -1; }
    hao_aot_dir dirs[HAO_AOT_MAX_DIRS];
    int w, rc = -1;
    char why[700], canon[PATH_MAX], tmp[64], tpath[PATH_MAX + 80], err[512];
    if (cache_dir) {
        /* --cache: named on the command line, so taken whatever the tier ignores of the environment */
        if (!hao_aot_dir_explicit(hao_env_tier(), cache_dir, &dirs[0], why, sizeof why)) { fprintf(stderr, "hao-aot: %s\n", why); return -1; }
        w = 0;
    } else {
        aot_dirs(dirs, &w, why, sizeof why);
    }
    if (w < 0) { fprintf(stderr, "hao-aot: no trusted AOT cache directory%s%s\n", why[0] ? ": " : "", why); return -1; }
    int dfd = aot_open_dir_create(&dirs[w], canon, sizeof canon, why, sizeof why);
    if (dfd < 0) { fprintf(stderr, "hao-aot: %s rejected: %s\n", dirs[w].path, why); return -1; }
    int tfd = aot_private_copy(path, dfd, tmp, sizeof tmp, why, sizeof why);
    if (tfd < 0) { fprintf(stderr, "hao-aot: %s\n", why); close(dfd); return -1; }
    snprintf(tpath, sizeof tpath, "%s/%s", canon, tmp);
    hao_image *im = hao_map_image_only(tpath, err, sizeof err);
    struct stat ts;
    bool same = im && fstat(tfd, &ts) == 0 && im->dev == (uint64_t)ts.st_dev && im->ino == (uint64_t)ts.st_ino;
    unlinkat(dfd, tmp, 0);
    if (!im) {
        /* the loader names the file it was given: say the user's */
        const char *t = strstr(err, tpath);
        if (t) fprintf(stderr, "hao-aot: %.*s%s%s\n", (int)(t - err), err, path, t + strlen(tpath));
        else fprintf(stderr, "hao-aot: %s: %s\n", path, err);
        goto out;
    }
    if (!same) { fprintf(stderr, "hao-aot: %s: its private copy was replaced before it was mapped: no cache written\n", path); goto out; }
    snprintf(im->path, sizeof im->path, "%s", path);
    if (!im->has_uuid) { fprintf(stderr, "hao-aot: %s has no LC_UUID\n", path); goto out; }
    aot_sig s;
    aot_id id;
    if (!aot_image_sig(im, &s)) {
        fprintf(stderr, "hao-aot: %s: its load commands are not mapped with its header, or are malformed: no cache written\n", path);
        goto out;
    }
    if (s.size) {
        /* the private copy's slice, checked on its bytes: structure and every page hash (libval.c) */
        struct { uint8_t hash_type, cdhash[20]; } g;
        why[0] = 0;
        hao_slice *sl = hao_slice_open(tfd, why, sizeof why);
        bool good = sl && hao_slice_verify(sl, NULL, why, sizeof why) == 0 && sl->sig.cd;
        if (good) { g.hash_type = sl->sig.hash_type; memcpy(g.cdhash, sl->sig.cdhash, sizeof g.cdhash); }
        else if (sl && !why[0]) snprintf(why, sizeof why, "its code signature could not be verified");
        hao_slice_close(sl);
        if (!good) {
            fprintf(stderr, "hao-aot: %s: code signature invalid (%s): no cache written\n", path, why);
            goto out;
        }
        /* every byte below the signature is page-hashed; the signature's own bytes are not all sealed */
        if (s.exec_end > s.off) {
            fprintf(stderr, "hao-aot: %s: an executable segment reaches into the code signature: no cache written\n", path);
            goto out;
        }
        /* the identity as the loader will compute it, which must be the one just checked */
        if (!aot_image_id(im, &id, why, sizeof why)) { fprintf(stderr, "hao-aot: %s: %s: no cache written\n", path, why); goto out; }
        if (id.kind != HAO_AOT_ID_CDHASH || id.hash != g.hash_type || memcmp(id.id, g.cdhash, sizeof g.cdhash) != 0) {
            fprintf(stderr, "hao-aot: %s: the mapped signature is not the one checked: no cache written\n", path);
            goto out;
        }
    } else {
        if (aot_dir_system(&dirs[w], canon, dfd)) {
            fprintf(stderr, "hao-aot: %s: not code signed: %s is a system cache directory, which holds caches for signed code only; "
                    "no cache written\n", path, canon);
            goto out;
        }
        if (!aot_image_id(im, &id, why, sizeof why)) { fprintf(stderr, "hao-aot: %s: %s: no cache written\n", path, why); goto out; }
    }
    rc = aot_generate(im, gen, dfd, &dirs[w], canon, &id, verbose);
out:
    if (im) hao_unload_image(im);     /* and with it the last reference to the private copy */
    close(tfd);
    close(dfd);
    return rc;
}

/* HAO_AOT_LEARN (ordinary processes only): blocks the AOT cache missed
 * (indirect targets), appended to <uuid>.hints in the directory `hao --aot`
 * writes to, for the next generation. Called after jit_lock is released
 * (it does I/O). */
static pthread_once_t learn_once = PTHREAD_ONCE_INIT;
static char learn_dir[PATH_MAX];
static bool learn_ok;
static void learn_init(void)
{
    hao_aot_dir dirs[HAO_AOT_MAX_DIRS];
    int w;
    char why[512];
    aot_dirs(dirs, &w, why, sizeof why);
    if (w < 0) return;
    int dfd = aot_open_dir_create(&dirs[w], learn_dir, sizeof learn_dir, why, sizeof why);
    if (dfd < 0) { if (hao_verbose) fprintf(HAO_DIAG, "hao: HAO_AOT_LEARN: %s rejected: %s\n", dirs[w].path, why); return; }
    close(dfd);
    learn_ok = true;
}

void hao_aot_note_jit_block(hao_image *im, uint64_t pc)
{
    pthread_once(&learn_once, learn_init);
    if (!learn_ok) return;
    char name[64], p[PATH_MAX + 80];
    aot_name(im, name, sizeof name, "hints");
    snprintf(p, sizeof p, "%s/%s", learn_dir, name);
    int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW_ANY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC, 0644);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & (S_IWGRP | S_IWOTH))) {
        char line[32];
        int l = snprintf(line, sizeof line, "%llx\n", (unsigned long long)(pc - im->base));
        if (write(fd, line, (size_t)l) != l) { /* best effort */ }
    }
    close(fd);
}

/* ------------------------------------------------------------- glue */

static void build_stubs(void)
{
    xbuf b;
    xb_init(&b, cache_ptr, 1024);
    /* int enter(hao_cpu *cpu, void *code) */
    void *enter = cache_ptr;
    x_push_r(&b, RBP); x_push_r(&b, RBX); x_push_r(&b, R12); x_push_r(&b, R13); x_push_r(&b, R14); x_push_r(&b, R15);
    x_alu_ri(&b, XSUB, 1, RSP, 8);     /* alignment; translated code keeps the MXCSR there (MXCSR_SLOT) */
    x_mov_rr(&b, 1, RBX, RDI);
    /* rt_bind has installed this entry's per-thread cache. Its mapping
     * outlives every chain; helpers preserve r12 by the host ABI. Nested
     * entries (callbacks/signals) save and restore it here. Guest integer
     * and vector registers stay in hao_cpu; recovery needs no new locations. */
    x_mov_ld(&b, 8, R12, xm(RBX, CPU_OFF_RT_JC));
    x_jmp_r(&b, RSI);
    while (xb_len(&b) & 15) x_int3(&b);
    exit_stub = b.p;
    x_alu_ri(&b, XADD, 1, RSP, 8);
    x_pop_r(&b, R15); x_pop_r(&b, R14); x_pop_r(&b, R13); x_pop_r(&b, R12); x_pop_r(&b, RBX); x_pop_r(&b, RBP);
    x_ret(&b);
    while (xb_len(&b) & 15) x_int3(&b);
    /*
     * The probe stub: an indirect exit (exit_indirect) whose jump-cache slot
     * did not hold its target. rax = target (already in cpu.pc), rdx = the
     * slot index * 4, NZCV in memory.
     *
     * It probes the current translation table as lookup() does (thash,
     * linear probing to an empty slot; the table is grown at 3/4). A hit
     * stores the entry's address in the slot and jumps to its code. A miss
     * leaves through the exit stub with no chain site: the dispatcher
     * translates the target, or makes the native call it is.
     *
     * Most misses are native functions and the return to native code,
     * which never get a translation. Each slot therefore has a miss record
     * {pc, table_epoch} behind the slots: a miss writes its target and the
     * epoch read before its probe, and the stub leaves without probing
     * while the record names its target and the table has not changed. A
     * record that is wrong or torn (a signal handler on this thread
     * rewrote it) costs a probe or an unneeded trip to the dispatcher,
     * never a wrong jump.
     */
    probe_stub = b.p;
    x_mov_ld(&b, 8, RCX, xm(RBX, CPU_OFF_RT_JC));
    x_lea(&b, 1, RDI, xmi(RCX, RDX, 4, 8 * HAO_JIT_JC_SLOTS));  /* the slot's miss record (16 bytes each) */
    x_mov_ri(&b, R8, (uint64_t)(uintptr_t)&table_epoch);
    x_mov_ld(&b, 8, RSI, xm(R8, 0));                             /* before the probe: an insert during it is noticed */
    x_alu_rm(&b, XCMP, 1, RAX, xm(RDI, 0));
    size_t probe = x_jcc8(&b, XCC_NE);
    x_alu_rm(&b, XCMP, 1, RSI, xm(RDI, 8));
    size_t known = x_jcc32(&b, XCC_E);
    x_patch8(&b, probe, xb_len(&b));
    x_mov_ri(&b, R8, (uint64_t)(uintptr_t)&table);
    x_mov_ld(&b, 8, R8, xm(R8, 0));
    x_mov_rr(&b, 1, R9, RAX);
    x_shift_ri(&b, XSHR, 1, R9, 2);                              /* x = pc >> 2 */
    x_mov_rr(&b, 1, R10, R9);
    x_shift_ri(&b, XSHR, 1, R10, 10);
    x_alu_rr(&b, XXOR, 1, R9, R10);
    x_shift_ri(&b, XSHR, 1, R10, 10);
    x_alu_rr(&b, XXOR, 1, R9, R10);                              /* x ^ x >> 10 ^ x >> 20 */
    x_mov_ld(&b, 4, R10, xm(R8, (int32_t)offsetof(jtab, mask)));
    x_alu_rr(&b, XAND, 1, R9, R10);
    size_t loop = xb_len(&b);
    x_mov_rr(&b, 1, R11, R9);
    x_shift_ri(&b, XSHL, 1, R11, 4);
    x_lea(&b, 1, R11, xmi(R8, R11, 1, (int32_t)offsetof(jtab, e)));
    x_alu_rm(&b, XCMP, 1, RAX, xm(R11, 0));
    size_t hit = x_jcc8(&b, XCC_E);
    x_alu_mi(&b, XCMP, 1, xm(R11, 0), 0);
    size_t miss = x_jcc8(&b, XCC_E);
    x_lea(&b, 1, R9, xm(R9, 1));
    x_alu_rr(&b, XAND, 1, R9, R10);
    x_patch8(&b, x_jmp8(&b), loop);
    x_patch8(&b, hit, xb_len(&b));
    x_mov_st(&b, 8, xmi(RCX, RDX, 2, 0), R11);
    x_jmp_m(&b, xm(R11, 8));
    x_patch8(&b, miss, xb_len(&b));
    x_mov_st(&b, 8, xm(RDI, 0), RAX);
    x_mov_st(&b, 8, xm(RDI, 8), RSI);
    x_patch32(&b, known, xb_len(&b));
    x_mov_mi(&b, 8, xm(RBX, CPU_OFF_SCRATCH0), 0);
    x_mov_ri(&b, RAX, HAO_EXIT_BRANCH);
    x_jmp_m(&b, xm(RBX, CPU_OFF_RT_EXIT));
    while (xb_len(&b) & 15) x_int3(&b);
    if (b.overflow || b.bad) abort();
    cache_ptr += xb_len(&b);
    jit_enter = (int (*)(hao_cpu *, void *))enter;
    jc_none.pc = ~0ull;
    jc_none.code = probe_stub;
}

/* 1: the code cache is mapped; -1: no executable memory (interpreter only,
 * no thunks); 0: not tried yet. */
static int jit_state;

/*
 * The code cache is one MAP_JIT region. When the kernel refuses it there is
 * no other source of executable memory worth having: a MAC policy refused it
 * (the process's sandbox denies dynamic-code-generation, or AMFI found no
 * allow-jit entitlement in a hardened process), and plain RWX memory either
 * fails as well (any process with CS_ENFORCEMENT: "curprot cannot be
 * write+execute") or would sidestep the sandbox's denial. The program then
 * runs in the interpreter, and native->guest calls take the NX-fault path
 * (hao_thunk_for returns the guest address; signatures go through the side
 * table in abi.c). HAO_NO_JIT_MEMORY=1 (hao_opt.no_jit_memory) makes the
 * mapping fail, for tests.
 *
 * Every fixed-size area is carved here, once: afterwards only translate()
 * and aot_install() move cache_ptr, through cache_alloc/block_commit under
 * jit_lock.
 */
/* Low eight estimate bits, generated once from the same integer
 * RecipEstimate/RecipSqrtEstimate definitions as a64_fp.c. The hidden
 * leading bit is implicit in the emitted result exponent. Publication is
 * covered by jit_once, and translated code reaches this through hao_cpu. */
static uint8_t fp_estimates[768];
static void init_estimates(void)
{
    for (unsigned a = 256; a < 512; a++)
        fp_estimates[a - 256] = (uint8_t)(((1u << 19) / (2 * a + 1) + 1) / 2);
    for (unsigned a = 128; a < 512; a++) {
        unsigned scaled = a < 256 ? 2 * a + 1 : 2 * ((a & ~1u) + 1);
        unsigned b = 512;
        while (scaled * (b + 1) * (b + 1) < (1u << 28)) b++;
        fp_estimates[256 + a] = (uint8_t)((b + 1) / 2);
    }
}

static void jit_init_once(void)
{
    init_estimates();
    hao_opts_init(NULL);   /* already done by the front ends; host tests call this first */
    pthread_once(&jit_cpu_once, jit_cpu_init);
    if (hao_opt.jit_helpers_only) jit_disabled_native = 1;
    cache_code_mb = hao_opt.jit_cache_mb ? hao_opt.jit_cache_mb : CODE_MB_DEFAULT;
    if (cache_code_mb > 1024) cache_code_mb = 1024;   /* chained links are rel32 */
    size_t fixed = 4096 + THUNK_AREA + SIG_AREA + SCRATCH_SIZE;
    size_t size = fixed + ((size_t)cache_code_mb << 20);
    errno = EPERM;
    cache = hao_opt.no_jit_memory ? MAP_FAILED :
            mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (cache == MAP_FAILED) {
        int e = errno;
        cache = NULL;
        hao_use_jit = 0;
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "no executable memory for translated code (mmap MAP_JIT: %s); running interpreted, "
                "native->guest calls take the fault path\n", strerror(e));
        __atomic_store_n(&jit_state, -1, __ATOMIC_RELEASE);
        return;
    }
    cache_ptr = cache;
    cache_end = cache + size;
    table = jtab_new(TABLE_MIN_BITS);
    if (!table) { perror("hao: translation table"); abort(); }
    rmap_cap = (uint32_t)(size / 16);
    rmap = mmap(NULL, (size_t)rmap_cap * sizeof *rmap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rmap == MAP_FAILED) rmap = NULL;   /* rmap_add warns once */
    build_stubs();
    cache_ptr = cache + 4096;
    thunk_area = thunk_ptr = cache_ptr; cache_ptr += THUNK_AREA; thunk_end = cache_ptr;
    sig_area = sig_ptr = cache_ptr; cache_ptr += SIG_AREA; sig_end = cache_ptr;
    scratch = cache_ptr; cache_ptr += SCRATCH_SIZE;
    __atomic_store_n(&jit_state, 1, __ATOMIC_RELEASE);
}

/* Returns true when the code cache exists. */
static pthread_once_t jit_once = PTHREAD_ONCE_INIT;
bool hao_jit_init(void)
{
    pthread_once(&jit_once, jit_init_once);
    return __atomic_load_n(&jit_state, __ATOMIC_ACQUIRE) > 0;
}

/* native-callable thunks can be made (the code cache exists) */
bool hao_have_thunks(void) { return hao_jit_init(); }
bool hao_thunk_range(uint64_t *lo, uint64_t *hi)
{
    if (!hao_jit_init()) return false;
    *lo = (uint64_t)(uintptr_t)thunk_area;
    *hi = (uint64_t)(uintptr_t)thunk_end;
    return true;
}

/* ucomis: greater 000, less 001 (CF), equal 010 (ZF), unordered 111 */
static void init_fcmp(hao_cpu *c)
{
    for (int k = 0; k < 8; k++) c->fcmp_flags[k] = HAO_FL_BASE | HAO_FL_OF;  /* unordered: C=1 V=1 */
    c->fcmp_flags[0] = HAO_FL_BASE;                                     /* C=1 */
    c->fcmp_flags[1] = HAO_FL_BASE | HAO_FL_SF | HAO_FL_CF;             /* N=1, C=0 */
    c->fcmp_flags[2] = HAO_FL_BASE | HAO_FL_ZF;                         /* Z=1, C=1 */
}

/* The interpreter helper of translated code. It records the guest pc of
 * the instruction it runs, for a signal that interrupts it. */
static int jit_helper(hao_cpu *c, uint64_t descriptor, uint64_t pc, unsigned declined)
{
    c->host_scratch[HAO_SCRATCH_HELPER] = pc;
    if (hao_opt.stats) {
        unsigned group = helper_group((uint32_t)descriptor), kind = !!declined;
        uint32_t empty = 0;
        __atomic_fetch_add(&st_helper_calls, 1, __ATOMIC_RELAXED);
        __atomic_compare_exchange_n(&helper_groups[group][kind].example, &empty,
                                    (uint32_t)descriptor, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        __atomic_fetch_add(&helper_groups[group][kind].calls, 1, __ATOMIC_RELAXED);
    }
    int r = a64_exec_helper(c, descriptor, pc);
    c->host_scratch[HAO_SCRATCH_HELPER] = 0;
    return r;
}

/* Translate the block at pc unless another thread just did; NULL if it
 * has to be interpreted. */
static void *find_or_translate(uint64_t pc)
{
    bool fresh = false;
    hao_crit_enter();
    pthread_mutex_lock(&jit_lock);
    void *code = lookup(pc);
    if (!code) { code = translate(pc); fresh = code != NULL; }
    pthread_mutex_unlock(&jit_lock);
    hao_crit_leave();
    if (__atomic_load_n(&warn_pending, __ATOMIC_RELAXED)) jit_warn();
    if (fresh && hao_opt.aot_learn) {
        hao_image *im = hao_image_for_code(pc);
        if (im && im->aot) {
            extern void hao_aot_note_jit_block(hao_image *, uint64_t);
            hao_aot_note_jit_block(im, pc);
        }
    }
    return code;
}

/* Give a hao_cpu what translated code reaches through it: the entry
 * points, the FCMP table and a jump cache (HAO_JIT_JC_SLOTS slots and as
 * many two-word miss records, zero when new: every slot then starts at
 * jc_none). */
static void rt_bind(hao_cpu *c, uint64_t *jc, void *helper)
{
    if (!jc[0]) for (int k = 0; k < HAO_JIT_JC_SLOTS; k++) jc[k] = (uint64_t)(uintptr_t)&jc_none;
    c->rt_helper = (uint64_t)(uintptr_t)helper;
    c->rt_exit = (uint64_t)(uintptr_t)exit_stub;
    c->rt_probe = (uint64_t)(uintptr_t)probe_stub;
    init_fcmp(c);
    c->fp_estimates = fp_estimates;
    c->rt_jc = (uint64_t)(uintptr_t)jc;
}

/* host_scratch words of this file: the block the dispatcher's last chain
 * step found for a pc, so that the entry that follows does not look the
 * same pc up again. A pc -> code pair is valid for good (the cache is
 * never flushed), so a stale pair is only unused. */
#define SCRATCH_NEXT_PC   3
#define SCRATCH_NEXT_CODE 4

int hao_jit_run_block(hao_thread *th)
{
    hao_cpu *c = &th->cpu;
    void *code = c->host_scratch[SCRATCH_NEXT_PC] == c->pc && c->host_scratch[SCRATCH_NEXT_CODE]
                     ? (void *)(uintptr_t)c->host_scratch[SCRATCH_NEXT_CODE] : NULL;
    if (!code) {
        /* The thread's jump cache first (exit_indirect): the dispatcher's own entries are mostly
         * the return from a native call, whose site no guest RET fills. A bound cache (rt_bind
         * fills every slot; slot 0 nonzero) gets the entry the table gave, one store, as the
         * probe stub does; a stale slot only costs the table probe. */
        uint64_t *slot = &th->jit_jc[(c->pc >> 2) & (HAO_JIT_JC_SLOTS - 1)];
        const tentry *e = (const tentry *)(uintptr_t)__atomic_load_n(slot, __ATOMIC_RELAXED);
        if (e && __atomic_load_n(&e->pc, __ATOMIC_ACQUIRE) == c->pc) code = __atomic_load_n(&e->code, __ATOMIC_ACQUIRE);
        else if ((e = lookup_entry(c->pc))) {
            code = __atomic_load_n(&e->code, __ATOMIC_ACQUIRE);
            if (__atomic_load_n(&th->jit_jc[0], __ATOMIC_RELAXED) && c->rt_jc == (uint64_t)(uintptr_t)th->jit_jc)
                __atomic_store_n(slot, (uint64_t)(uintptr_t)e, __ATOMIC_RELAXED);
        }
    }
    if (!code) {
        /* Once the cache is full, a miss is interpreted without the lock:
         * JIT exits leave NZCV materialised in hao_cpu, as a64_exec expects. */
        if (!__atomic_load_n(&cache_full, __ATOMIC_ACQUIRE)) code = find_or_translate(c->pc);
        if (!code) {
            /* as run_loop's interpreter: a64_step advances cpu.pc before the instruction's memory accesses,
             * so a signal handler gets the instruction's pc from HELPER (and HAO_CODE_ALIAS tells the
             * guest's own loads of its code by it) */
            th->steps++;
            c->host_scratch[HAO_SCRATCH_HELPER] = c->pc;
            c->host_scratch[HAO_SCRATCH_IN_GUEST] = 1;
            int r = a64_step(c);
            c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
            return r;
        }
    }
    c->host_scratch[0] = 0;
    /* a cpu state that was saved before this thread first ran translated
     * code, or built elsewhere, is bound again; otherwise nothing to do */
    if (c->rt_jc != (uint64_t)(uintptr_t)th->jit_jc) rt_bind(c, th->jit_jc, (void *)(uintptr_t)jit_helper);
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = 1;
    int r = jit_enter(c, code);
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = 0;
    /* chain the direct branch that just exited, if its target is translated guest code */
    uint64_t site = c->host_scratch[0];
    if (r == HAO_EXIT_BRANCH && site && !hao_opt.no_chain) {
        void *next = lookup(c->pc);
        if (!next && hao_image_for_code(c->pc) && !hao_is_trap(c->pc) && !__atomic_load_n(&cache_full, __ATOMIC_ACQUIRE))
            next = find_or_translate(c->pc);
        /* site = address of the rel32 field of the block's exit jmp
         * (exit_direct). Other threads may be executing that jmp: one
         * aligned 4-byte store, no lock, is seen by them whole, and both
         * the old target (the exit stub) and the new one are complete
         * translations of the same guest pc. That needs code that a rel32
         * can point to never to be freed or reused; the cache is never
         * flushed, and a future flush must first get every thread out of
         * the old code. An unaligned or foreign site is left unchained
         * (slower, not wrong; st_link_skips counts it). */
        bool ok = next && !(site & 3) && site >= (uint64_t)(uintptr_t)cache && site + 4 <= (uint64_t)(uintptr_t)cache_end;
        if (ok) {
            int32_t rel = (int32_t)((intptr_t)next - (intptr_t)(site + 4));
            __atomic_store_n((int32_t *)(uintptr_t)site, rel, __ATOMIC_RELEASE);
            __atomic_fetch_add(&st_links, 1, __ATOMIC_RELAXED);
        } else if (next)
            __atomic_fetch_add(&st_link_skips, 1, __ATOMIC_RELAXED);
        if (next) {
            c->host_scratch[SCRATCH_NEXT_CODE] = 0;             /* a signal handler between the two stores finds no pair */
            c->host_scratch[SCRATCH_NEXT_PC] = c->pc;
            c->host_scratch[SCRATCH_NEXT_CODE] = (uint64_t)(uintptr_t)next;
        }
    }
    return r;
}

void hao_jit_stats(void)
{
    uint64_t maps_mapped;
    uint32_t maps_grows;
    hao_gmap_totals(&maps_mapped, &maps_grows);   /* hao's growable maps (hao/gmap.h) */
    fprintf(stderr, "hao-jit: aot_blocks=%llu aot_rejected=%llu jit_blocks=%llu guest_insns=%llu native=%llu helper=%llu (%.1f%% native) links=%llu code=%lluKB"
            " sites=%llu sites_unaligned=%llu link_skips=%llu table=%u/%u table_kb=%llu maps_kb=%llu map_grows=%u fused=%llu"
            " flag_exits=%llu flag_exits_unsaved=%llu helper_calls=%llu\n",
            (unsigned long long)st_aot_blocks, (unsigned long long)st_aot_rejected, (unsigned long long)st_blocks, (unsigned long long)st_insns, (unsigned long long)st_native,
            (unsigned long long)st_helper, st_native + st_helper ? 100.0 * st_native / (st_native + st_helper) : 0,
            (unsigned long long)st_links, (unsigned long long)(st_bytes >> 10), (unsigned long long)st_sites,
            (unsigned long long)st_sites_unaligned, (unsigned long long)st_link_skips,
            __atomic_load_n(&table_used, __ATOMIC_RELAXED), table ? table->mask + 1 : 0, (unsigned long long)(st_table_bytes >> 10),
            (unsigned long long)(maps_mapped >> 10), maps_grows, (unsigned long long)st_fused,
            (unsigned long long)st_fl_exits, (unsigned long long)st_fl_unsaved,
            (unsigned long long)__atomic_load_n(&st_helper_calls, __ATOMIC_RELAXED));
    uint64_t counts[HG_COUNT][2];
    for (unsigned g = 0; g < HG_COUNT; g++)
        for (unsigned k = 0; k < 2; k++)
            counts[g][k] = __atomic_load_n(&helper_groups[g][k].calls, __ATOMIC_RELAXED);
    for (unsigned rank = 0; rank < 20; rank++) {
        unsigned best = 0, kind = 0;
        for (unsigned g = 0; g < HG_COUNT; g++)
            for (unsigned k = 0; k < 2; k++)
                if (counts[g][k] > counts[best][kind]) { best = g; kind = k; }
        if (!counts[best][kind]) break;
        fprintf(stderr, "hao-jit: helper group %s kind=%s calls=%llu example=%08x\n",
                helper_group_names[best], kind ? "declined" : "untranslated",
                (unsigned long long)counts[best][kind],
                __atomic_load_n(&helper_groups[best][kind].example, __ATOMIC_RELAXED));
        counts[best][kind] = 0;
    }
}

/* Oracle support: run code[0..n) placed at guest address `base` from its
 * first instruction until control leaves it (normally at base + 4n), through
 * the real block translator: each block (up to a block end or the end of
 * code) is emitted by emit_block, with its flag-liveness pass and multi-exit
 * branches, run, and the next block is translated at the exit pc. Blocks are
 * not cached. Returns the exit reason; HAO_EXIT_NONE when control reached
 * base + 4n, -3 when it went anywhere else. */
/* Per-thread state of hao_jit_exec_code (tests run it concurrently). A
 * pthread key, not __thread: libhao has no thread-local variables, because
 * dyld allocates a thread's TLV block on first access, and hao code runs in
 * signal handlers on threads that may hold the malloc locks (the Makefile
 * refuses a libhao with a __thread_vars section). */
typedef struct {
    uint8_t *scratch;
    uint64_t last_pc, last_base;               /* the block left in scratch, and the code it was part of */
    int last_m, last_n;
    uint32_t last_code[2 * MAX_BLOCK_INSNS];
    uint64_t jc[3 * HAO_JIT_JC_SLOTS];
} exec_state;
static pthread_key_t exec_key;
static pthread_once_t exec_once = PTHREAD_ONCE_INIT;
static void exec_key_init(void) { pthread_key_create(&exec_key, free); }

int hao_jit_exec_code(hao_cpu *c, const uint32_t *code, int n, uint64_t base)
{
    static pthread_mutex_t scratch_lock = PTHREAD_MUTEX_INITIALIZER;
    static bool scratch_taken;
    if (!hao_jit_init()) abort();
    pthread_once(&exec_once, exec_key_init);
    exec_state *es = pthread_getspecific(exec_key);
    if (!es) {
        if (!(es = calloc(1, sizeof *es))) abort();
        pthread_setspecific(exec_key, es);
    }
    if (!es->scratch) {
        pthread_mutex_lock(&scratch_lock);
        if (!scratch_taken) { es->scratch = scratch; scratch_taken = true; }
        else {
            uint8_t *p = (uint8_t *)(((uintptr_t)cache_ptr + 15) & ~(uintptr_t)15);
            if ((size_t)(cache_end - p) < 2 * SCRATCH_SIZE) abort();
            es->scratch = p; cache_ptr = p + SCRATCH_SIZE;
        }
        pthread_mutex_unlock(&scratch_lock);
    }
    uint8_t *tscratch = es->scratch;
    c->pc = base;
    rt_bind(c, es->jc, (void *)(uintptr_t)a64_exec_helper);
    for (int steps = 0; steps < 4 * n + 4; steps++) {
        uint64_t pc = c->pc;
        if (pc == base + 4u * (unsigned)n) return HAO_EXIT_NONE;
        if (pc < base || pc > base + 4u * (unsigned)n || (pc & 3)) return -3;
        int k = (int)((pc - base) / 4), m = 0;
        while (k + m < n && m < MAX_BLOCK_INSNS) if (is_block_end(code[k + m++])) break;
        /* the block left in scratch by the previous call is reused if it and
         * the code around it (its exits look at their targets) are identical */
        if (pc != es->last_pc || m != es->last_m || base != es->last_base || n != es->last_n ||
            n > 2 * MAX_BLOCK_INSNS || memcmp(es->last_code, code, 4u * (unsigned)n)) {
            tctx t;
            memset(&t, 0, sizeof t);
            t.win = code;
            t.win_base = base;
            t.win_n = n;
            xb_init(&t.b, tscratch, SCRATCH_SIZE);
            emit_block(&t, pc, code + k, m);
            if (t.b.overflow || t.b.bad) { es->last_m = 0; return -4; }
            es->last_pc = pc; es->last_m = m; es->last_base = base; es->last_n = n;
            if (n <= 2 * MAX_BLOCK_INSNS) memcpy(es->last_code, code, 4u * (unsigned)n);
        }
        int r = jit_enter(c, tscratch);
        if (r != HAO_EXIT_BRANCH && r != HAO_EXIT_NONE) return r;
    }
    return -3;
}

/* ------------------------------------------------- native-callable thunks
 * A guest function handed to native code gets an x86 entry point:
 *   movabs r11, guest ; xor r10d, r10d ; jmp [rip+0] ; .quad hao_callback_entry
 * so native callers reach the translator without a fault. A signature
 * thunk (a 64-byte slot of its own area) loads r10 with its hsig instead.
 * The guest address is always encoded as a 10-byte movabs, so it can be
 * read back at entry+2 (hao_thunk_target, hao_sig_thunk_target): x_mov_ri
 * would pick a shorter form for an address below 4 GB.
 *
 * Capacity: THUNK_SLOTS plain thunks and 65536 signature thunks. When
 * either runs out, the request is logged once: a plain thunk request gets
 * the guest address itself (the NX-fault path, which behaves like a plain
 * thunk, only slower), a signature thunk request gets 0 (abi.c notes the
 * signature for hao_callback). */
/* guest -> thunk; grows with use (hao/gmap.h), up to the area's THUNK_SLOTS */
static hao_gmap thunks = HAO_GMAP_INIT(2, 1, 8, 19);
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

static void movabs(xbuf *b, int reg, uint64_t imm)
{
    xb_u8(b, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));    /* REX.W (+B) */
    xb_u8(b, (uint8_t)(0xB8 + (reg & 7)));
    xb_u64(b, imm);
}

/* fork child: the holders of these locks were not copied (see jit_lock) */
void hao_jit_fork_child(void)
{
    jit_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    thunk_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    pw_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

static void full_once(int *flag, const char *what)
{
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) return;
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG, "%s\n", what);
}

uint64_t hao_thunk_for(uint64_t guest)
{
    static int tfull;
    const uint64_t *e = hao_gmap_get1(&thunks, guest);
    if (e) return e[1];
    if (!hao_jit_init()) return guest;   /* no executable memory: fault path */
    hao_crit_enter();
    pthread_mutex_lock(&thunk_lock);
    uint64_t r = 0;
    bool fresh;
    uint64_t *s = guest ? hao_gmap_claim1(&thunks, guest, &fresh) : NULL;
    if (s && !fresh) r = s[1];
    else if (s && thunk_ptr + 32 <= thunk_end) {
        xbuf b;
        xb_init(&b, thunk_ptr, 32);
        movabs(&b, R11, guest);
        x_alu_rr(&b, XXOR, 0, R10, R10);                    /* no signature */
        x_jmp_m(&b, xm(XRIP, 0));                           /* jmp [rip+0] */
        xb_u64(&b, (uint64_t)(uintptr_t)hao_callback_entry);
        if (b.overflow || b.bad) abort();
        r = (uint64_t)(uintptr_t)thunk_ptr;
        __atomic_store_n(&thunk_ptr, thunk_ptr + 32, __ATOMIC_RELEASE);
        s[1] = r;
        hao_gmap_publish1(&thunks, s, guest);
    }
    pthread_mutex_unlock(&thunk_lock);
    hao_crit_leave();
    if (!r) full_once(&tfull, "thunk table full: further guest functions keep the NX-fault path");
    return r ? r : guest;
}

/* thunk address -> guest function (0 if not a thunk) */
/* Is `a` inside the code cache (translated code, thunks, stubs)? */
bool hao_in_code_cache(uint64_t a)
{
    return cache && a >= (uint64_t)(uintptr_t)cache && a < (uint64_t)(uintptr_t)cache_end;
}

uint64_t hao_thunk_target(uint64_t a)
{
    uint8_t *end = __atomic_load_n(&thunk_ptr, __ATOMIC_ACQUIRE);
    if (!thunk_area || a < (uint64_t)(uintptr_t)thunk_area || a >= (uint64_t)(uintptr_t)end) return 0;
    if ((a - (uint64_t)(uintptr_t)thunk_area) & 31) return 0;
    uint64_t g;
    memcpy(&g, (void *)(uintptr_t)(a + 2), 8);   /* imm64 of movabs r11 */
    return g;
}

/* Signature thunk: mov r11, guest; mov r10, sig; jmp [rip+0]; .quad
 * hao_callback_entry, in a 64-byte slot of the signature-thunk area. 0 when
 * none can be made (no executable memory, or the area is full). One
 * per (guest, sig): the hsig is canonical per encoding (abi.c hao_sig),
 * and callers repeat (class_replaceMethod, re-registered methods). The
 * dedup map, (guest, sig) -> thunk, grows with use (hao/gmap.h); its
 * lookup is lock-free, like thunks'. */
static hao_gmap sigthunks = HAO_GMAP_INIT(3, 2, 7, 17);

uint64_t hao_sig_thunk(uint64_t guest, void *sig)
{
    uint64_t key[2] = {guest, (uint64_t)(uintptr_t)sig};
    const uint64_t *e = hao_gmap_get(&sigthunks, key);
    if (e) return e[2];
    if (!guest || !hao_jit_init()) return 0;
    hao_crit_enter();
    pthread_mutex_lock(&thunk_lock);
    uint64_t r = 0;
    bool fresh;
    uint64_t *s = hao_gmap_claim(&sigthunks, key, &fresh);
    if (s && !fresh) r = s[2];
    else if (!s || sig_ptr + SIG_SLOT > sig_end) __atomic_fetch_or(&warn_pending, WARN_SIGS, __ATOMIC_RELAXED);
    else {
        xbuf b;
        xb_init(&b, sig_ptr, SIG_SLOT);
        movabs(&b, R11, guest);
        movabs(&b, R10, key[1]);
        x_jmp_m(&b, xm(XRIP, 0));                               /* jmp [rip+0] */
        xb_u64(&b, (uint64_t)(uintptr_t)hao_callback_entry);
        if (b.overflow || b.bad) abort();
        while (xb_len(&b) < SIG_SLOT) x_int3(&b);
        r = (uint64_t)(uintptr_t)sig_ptr;
        __atomic_store_n(&sig_ptr, sig_ptr + SIG_SLOT, __ATOMIC_RELEASE);
        s[2] = r;
        hao_gmap_publish(&sigthunks, s, key);
    }
    pthread_mutex_unlock(&thunk_lock);
    hao_crit_leave();
    if (__atomic_load_n(&warn_pending, __ATOMIC_RELAXED)) jit_warn();
    return r;   /* 0: area full (warned once); abi.c notes the signature for hao_callback */
}

/* signature thunk address -> guest function (0 if not one) */
uint64_t hao_sig_thunk_target(uint64_t a)
{
    uint8_t *end = __atomic_load_n(&sig_ptr, __ATOMIC_ACQUIRE);
    if (!sig_area || a < (uint64_t)(uintptr_t)sig_area || a >= (uint64_t)(uintptr_t)end) return 0;
    if ((a - (uint64_t)(uintptr_t)sig_area) & (SIG_SLOT - 1)) return 0;
    uint64_t g;
    memcpy(&g, (void *)(uintptr_t)(a + 2), 8);
    return g;
}

/* Guest code branched to a native entry point of guest function `guest`
 * (a slot rewritten at load, a patched block invoke, ...). Point the entry
 * address at the guest function's translation, so the next indirect branch
 * to it is found by the inline table probe instead of a dispatcher exit. */
void hao_jit_alias(uint64_t entry, uint64_t guest)
{
    if (!hao_use_jit || !table || lookup(entry) || !hao_image_for_code(guest)) return;
    hao_crit_enter();
    pthread_mutex_lock(&jit_lock);
    if (!lookup(entry)) {
        void *code = lookup(guest);
        if (!code) code = translate(guest);
        if (code) insert(entry, code);
    }
    pthread_mutex_unlock(&jit_lock);
    hao_crit_leave();
}
