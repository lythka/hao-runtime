/*
 * x86_64 checker: replays oracle records through hao's interpreter and its
 * block translator and compares the result with the native arm64 run.
 *   check RECORDS.bin [--show N] [--bad FILE]
 *
 * Compared bit-exactly: x0-x30 (except the platform register x18), SP, NZCV,
 * FPCR, FPSR.QC, v0-v31 (NaN payloads included) and the scratch memory
 * (hash of the whole filled region). FPSR's IEEE cumulative flags are
 * counted separately and fail the gate; in fma records, a flag
 * difference is a mismatch. For fma records the checker also reports how many
 * have only normal or zero operands and results at or above 2^-125 (what the
 * round-to-odd path of fp_muladd admits), and for how many of those
 * (float)((double)a * b + c) differs from native. Outcomes:
 *   native ran:     hao must run and match; a hao UNDEF passes only for
 *                   encodings listed as unimplemented in known.h, a hao
 *                   fault never passes
 *   native SIGILL:  hao must report UNDEF, unless known.h lists the
 *                   encoding as one hao deliberately accepts
 *   native fault:   informational (hao faulting too, or not: alignment and
 *                   SP-alignment checks are not modelled)
 * Exit status 1 when any record fails.
 */
#include "record.h"
#include "known.h"
#include "hao/cpu.h"
#include "hao/opts.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int hao_jit_exec_code(hao_cpu *c, const uint32_t *code, int n, uint64_t base);   /* jit.c */

static sigjmp_buf jb;
static volatile int sig;
static void on_sig(int s) { sig = s; siglongjmp(jb, 1); }

static uint32_t host_mxcsr;
static void restore_host_fp(void) { __asm__ volatile("ldmxcsr %0" : : "m"(host_mxcsr)); }

/* Run the record's code through the interpreter. */
static int interp_code(hao_cpu *c, const oracle_rec *r)
{
    uint64_t end = r->pc + 4ull * r->n;
    c->pc = r->pc;
    for (int steps = 0; steps < 4 * (int)r->n + 4; steps++) {
        if (c->pc == end) return HAO_EXIT_NONE;
        if (c->pc < r->pc || c->pc > end || (c->pc & 3)) return -3;
        int st = a64_exec(c, r->code[(c->pc - r->pc) / 4], c->pc);
        if (st != HAO_EXIT_NONE && st != HAO_EXIT_BRANCH) return st;
    }
    return -3;
}

#define NUNIMPL (sizeof known_unimpl / sizeof *known_unimpl)
#define NLENIENT (sizeof known_lenient / sizeof *known_lenient)
_Static_assert(NLENIENT == 0, "reserved encodings must not have oracle exceptions");
static long unimpl_hits[2][NUNIMPL], lenient_hits[2][NLENIENT];

/* Count actual tolerated outcomes, not merely encodings. Counting all
 * matches also exposes overlapping exceptions for review. */
static bool known_match(const known_enc *t, size_t n, const oracle_rec *r, long *hits)
{
    bool found = false;
    for (size_t k = 0; k < n; k++)
        for (uint32_t i = 0; i < r->n; i++)
            if ((r->code[i] & t[k].mask) == t[k].value) {
                hits[k]++; found = true; break;
            }
    return found;
}

/* fma records: the operands of the lanes the instruction computes, with the
 * FMLS/FMSUB/FNMADD/FNMSUB negations applied. Returns the lane count. */
static int fma_operands(const oracle_rec *r, uint32_t a[4], uint32_t b[4], uint32_t c[4])
{
    uint32_t i = r->code[0], vn[4], vm[4], vd[4], va[4];
    memcpy(vn, &r->in.v[i >> 5 & 31], 16);
    memcpy(vm, &r->in.v[i >> 16 & 31], 16);
    memcpy(vd, &r->in.v[i & 31], 16);
    memcpy(va, &r->in.v[i >> 10 & 31], 16);
    int lanes;
    if ((i & 0xFFC00000) == 0x1F000000) {
        unsigned o1 = i >> 21 & 1, o0 = i >> 15 & 1;
        a[0] = vn[0] ^ (o0 != o1 ? 0x80000000u : 0); b[0] = vm[0]; c[0] = va[0] ^ (o1 ? 0x80000000u : 0);
        return 1;
    }
    if ((i & 0xBF60FC00) == 0x0E20CC00) {
        lanes = i >> 30 & 1 ? 4 : 2;
        for (int l = 0; l < lanes; l++) { a[l] = vn[l] ^ (i & 0x00800000 ? 0x80000000u : 0); b[l] = vm[l]; c[l] = vd[l]; }
        return lanes;
    }
    if ((i & 0x8FC0B400) != 0x0F801000) return 0;
    lanes = i >> 28 & 1 ? 1 : i >> 30 & 1 ? 4 : 2;
    for (int l = 0; l < lanes; l++) {
        a[l] = vn[l] ^ (i & 0x4000 ? 0x80000000u : 0); b[l] = vm[(i >> 11 & 1) << 1 | (i >> 21 & 1)]; c[l] = vd[l];
    }
    return lanes;
}
static bool f32_plain(uint32_t v) { uint32_t e = v >> 23 & 0xff; return e ? e != 0xff : !(v & 0x7fffff); }   /* normal or zero */
/* a * b + c through binary64 with two ordinary roundings: the sum to nearest
 * and the conversion in the guest's rounding mode. */
static uint32_t fma_widened(uint32_t a, uint32_t b, uint32_t c, unsigned rmode)
{
    volatile float fa, fb, fc, fr;
    memcpy((void *)&fa, &a, 4); memcpy((void *)&fb, &b, 4); memcpy((void *)&fc, &c, 4);
    volatile double sum = (double)fa * (double)fb;
    sum = sum + (double)fc;
    uint32_t m = 0x1f80 | (rmode == 1 || rmode == 2 ? 3 - rmode : rmode) << 13;
    __asm__ volatile("ldmxcsr %0" : : "m"(m));
    fr = (float)sum;
    restore_host_fp();
    uint32_t r;
    memcpy(&r, (void *)&fr, 4);
    return r;
}

typedef struct {
    long ran, match, mismatch, undef_known, undef_bad, fault, ill, ill_undef, lenient_known, lenient_bad;
    long nfault, nfault_hao_fault, nfault_missed, flagdiff;
} counts;

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    /* the runtime's hao_opts_init is not linked here; only this switch matters */
    hao_opt.jit_helpers_only = getenv("HAO_JIT_HELPERS_ONLY") != NULL;
    int show = 20;
    const char *badpath = "build/oracle_bad.txt";
    for (int k = 2; k < argc; k++) {
        if (!strcmp(argv[k], "--show") && k + 1 < argc) show = atoi(argv[++k]);
        else if (!strcmp(argv[k], "--bad") && k + 1 < argc) badpath = argv[++k];
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    if (mmap((void *)ORACLE_SCRATCH, ORACLE_SCRATCH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("scratch"); return 1;
    }
    oracle_fill_ro();
    if (mprotect((void *)(ORACLE_SCRATCH + ORACLE_RO), ORACLE_RO_SIZE, PROT_READ)) { perror("mprotect"); return 1; }
    signal(SIGSEGV, on_sig);
    signal(SIGBUS, on_sig);
    signal(SIGFPE, on_sig);
    signal(SIGILL, on_sig);
    __asm__ volatile("stmxcsr %0" : "=m"(host_mxcsr));

    FILE *badf = fopen(badpath, "w");
    counts cnt[2];
    memset(cnt, 0, sizeof cnt);
    long total = 0, shown = 0, fma_admitted = 0, fma_widened_wrong = 0;
    int mode = -1;
    oracle_rec r;
    while (fread(&r, sizeof r, 1, f) == 1) {
        total++;
        mode = (int)r.mode;
        if (mode == OM_FMA && r.status == 0) {
            uint32_t a[4], b[4], c[4], want[4];
            int lanes = fma_operands(&r, a, b, c), wrong = -1;
            bool admitted = lanes > 0;
            memcpy(want, &r.out.v[r.code[0] & 31], 16);
            for (int l = 0; l < lanes; l++)
                if (!f32_plain(a[l]) || !f32_plain(b[l]) || !f32_plain(c[l]) || (want[l] >> 23 & 0xff) < 2) admitted = false;
            for (int l = 0; l < lanes && admitted && wrong < 0; l++)
                if (fma_widened(a[l], b[l], c[l], r.in.fpcr >> 22 & 3) != want[l]) wrong = l;
            fma_admitted += admitted;
            if (wrong >= 0 && fma_widened_wrong++ < 4)
                printf("two roundings differ: %08x * %08x + %08x RMode %u: native %08x, widened %08x\n", a[wrong], b[wrong], c[wrong],
                       r.in.fpcr >> 22 & 3, want[wrong], fma_widened(a[wrong], b[wrong], c[wrong], r.in.fpcr >> 22 & 3));
        }
        for (int pass = 0; pass < 2; pass++) {
            counts *C = &cnt[pass];
            static hao_cpu cpu;
            memset(&cpu, 0, sizeof cpu);
            for (int k = 0; k < 31; k++) cpu.x[k] = r.in.x[k];
            cpu.x[31] = r.in.sp;
            memcpy(cpu.v, r.in.v, sizeof r.in.v);
            hao_set_nzcv(&cpu, r.in.nzcv >> 28);
            oracle_fill(r.seed);
            int st;
            sig = 0;
            if (sigsetjmp(jb, 1) == 0) {
                a64_fpcr_write(&cpu, r.in.fpcr);
                a64_fpsr_write(&cpu, r.in.fpsr);
                st = pass ? hao_jit_exec_code(&cpu, r.code, (int)r.n, r.pc) : interp_code(&cpu, &r);
            } else {
                st = -2;
            }
            uint32_t fpcr = a64_fpcr_read(&cpu), fpsr = a64_fpsr_read(&cpu);
            restore_host_fp();
            const char *pn = pass ? "jit" : "interp";
            char why[320] = "";
            if (r.status == 1) {          /* native SIGILL */
                C->ill++;
                if (st == HAO_EXIT_UNDEF || st == HAO_EXIT_UDF) { C->ill_undef++; continue; }
                if (known_match(known_lenient, NLENIENT, &r, lenient_hits[pass])) { C->lenient_known++; continue; }
                C->lenient_bad++;
                snprintf(why, sizeof why, "native SIGILL, hao %s", st == -2 ? "faulted" : "executed it");
            } else if (r.status == 2) {   /* native fault */
                C->nfault++;
                if (st == -2) C->nfault_hao_fault++; else C->nfault_missed++;
                continue;
            } else {
                C->ran++;
                if (st == -2) { C->fault++; snprintf(why, sizeof why, "hao signal %d, native ran", sig); }
                else if (st == HAO_EXIT_UNDEF) {
                    if (known_match(known_unimpl, NUNIMPL, &r, unimpl_hits[pass])) { C->undef_known++; continue; }
                    C->undef_bad++;
                    snprintf(why, sizeof why, "hao UNDEF at pc+%llu, native ran", (unsigned long long)(cpu.pc - r.pc));
                } else if (st != HAO_EXIT_NONE) {
                    C->undef_bad++;
                    snprintf(why, sizeof why, "hao exit %d, native ran", st);
                } else {
                    for (int k = 0; k < 31 && !why[0]; k++)
                        if (k != 18 && cpu.x[k] != r.out.x[k])
                            snprintf(why, sizeof why, "x%d got %016llx want %016llx", k, (unsigned long long)cpu.x[k], (unsigned long long)r.out.x[k]);
                    if (!why[0] && cpu.x[31] != r.out.sp)
                        snprintf(why, sizeof why, "sp got %016llx want %016llx", (unsigned long long)cpu.x[31], (unsigned long long)r.out.sp);
                    if (!why[0] && (hao_nzcv(&cpu) << 28) != (r.out.nzcv & 0xf0000000u))
                        snprintf(why, sizeof why, "nzcv got %x want %x", hao_nzcv(&cpu), r.out.nzcv >> 28);
                    for (int k = 0; k < 32 && !why[0]; k++) {
                        uint64_t want[2], got[2];
                        memcpy(want, &r.out.v[k], 16);
                        memcpy(got, &cpu.v[k], 16);
                        if (want[0] != got[0] || want[1] != got[1])
                            snprintf(why, sizeof why, "v%d got %016llx:%016llx want %016llx:%016llx", k,
                                     (unsigned long long)got[1], (unsigned long long)got[0], (unsigned long long)want[1], (unsigned long long)want[0]);
                    }
                    if (!why[0] && oracle_memhash() != r.memhash) snprintf(why, sizeof why, "memory differs");
                    if (!why[0] && fpcr != r.out.fpcr) snprintf(why, sizeof why, "fpcr got %x want %x", fpcr, r.out.fpcr);
                    if (!why[0] && ((fpsr ^ r.out.fpsr) & (1u << 27)))
                        snprintf(why, sizeof why, "fpsr.QC got %x want %x", fpsr, r.out.fpsr);
                    if (!why[0] && mode == OM_FMA && ((fpsr ^ r.out.fpsr) & 0x9f))
                        snprintf(why, sizeof why, "fpsr got %x want %x (in %x)", fpsr, r.out.fpsr, r.in.fpsr);
                    if (why[0]) C->mismatch++;
                    else {
                        C->match++;
                        if ((fpsr ^ r.out.fpsr) & 0x9f) {
                            C->flagdiff++;
                            if (badf) fprintf(badf, "%s %s %08x FLAGS fpsr got %x want %x (fpcr %x)\n", oracle_mode_name[mode], pn,
                                              r.code[0], fpsr, r.out.fpsr, r.in.fpcr);
                        }
                        continue;
                    }
                }
            }
            char insns[ORACLE_MAXI * 9 + 1] = "";
            for (uint32_t k = 0, o = 0; k < r.n; k++) o += (uint32_t)snprintf(insns + o, sizeof insns - o, "%s%08x", k ? " " : "", r.code[k]);
            if (badf) fprintf(badf, "%s %s %s: %s (fpcr %x)\n", oracle_mode_name[mode], pn, insns, why, r.in.fpcr);
            if (shown++ < show) printf("MISMATCH %s %s: %s (fpcr %x)\n", pn, insns, why, r.in.fpcr);
        }
    }
    if (badf) fclose(badf);
    if (mode == OM_FMA)
        printf("fma: %ld of %ld records have normal or zero operands and results >= 2^-125 in every lane;"
               " binary64 with two roundings is wrong for %ld of them\n", fma_admitted, total, fma_widened_wrong);
    long fails = 0;
    for (int kind = 0; kind < 2; kind++) {
        const known_enc *t = kind ? known_lenient : known_unimpl;
        size_t n = kind ? NLENIENT : NUNIMPL;
        for (size_t k = 0; k < n; k++) {
            long a = kind ? lenient_hits[0][k] : unimpl_hits[0][k];
            long b = kind ? lenient_hits[1][k] : unimpl_hits[1][k];
            printf("known_%s %08x/%08x interp=%ld jit=%ld %s%s\n",
                   kind ? "lenient" : "unimpl", t[k].mask, t[k].value, a, b, t[k].what,
                   mode == OM_KNOWN && (!a || !b) ? " STALE" : "");
            if (mode == OM_KNOWN && (!a || !b)) fails++;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        counts *C = &cnt[pass];
        printf("%s %-6s records %ld | native ran %ld: match %ld, mismatch %ld, hao-fault %ld, hao-undef %ld (+%ld known unimplemented)"
               " | native SIGILL %ld: hao-undef %ld, hao-accepts %ld (+%ld known) | native fault %ld: hao fault %ld, not %ld"
               " | fpsr IEEE-flag differences %ld\n",
               mode >= 0 ? oracle_mode_name[mode] : "?", pass ? "jit" : "interp", total, C->ran, C->match, C->mismatch, C->fault,
               C->undef_bad, C->undef_known, C->ill, C->ill_undef, C->lenient_bad, C->lenient_known, C->nfault,
               C->nfault_hao_fault, C->nfault_missed, C->flagdiff);
        fails += C->mismatch + C->fault + C->undef_bad + C->lenient_bad + C->flagdiff;
    }
    return fails ? 1 : 0;
}
