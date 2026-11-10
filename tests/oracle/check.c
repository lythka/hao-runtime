/*
 * x86_64 checker: replays oracle records through hao's interpreter and its
 * block translator and compares the result with the native arm64 run.
 *   check RECORDS.bin [--show N] [--bad FILE]
 *
 * Compared bit-exactly: x0-x30 (except the platform register x18), SP, NZCV,
 * FPCR, FPSR.QC, v0-v31 (NaN payloads included) and the scratch memory
 * (hash of the whole filled region). FPSR's IEEE cumulative flags are
 * counted separately (informational). Outcomes:
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

static const char *known_match(const known_enc *t, size_t n, const oracle_rec *r)
{
    for (size_t k = 0; k < n; k++)
        for (uint32_t i = 0; i < r->n; i++)
            if ((r->code[i] & t[k].mask) == t[k].value) return t[k].what;
    return NULL;
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
    long total = 0, shown = 0;
    int mode = -1;
    oracle_rec r;
    while (fread(&r, sizeof r, 1, f) == 1) {
        total++;
        mode = (int)r.mode;
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
                if (known_match(known_lenient, sizeof known_lenient / sizeof *known_lenient, &r)) { C->lenient_known++; continue; }
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
                    if (known_match(known_unimpl, sizeof known_unimpl / sizeof *known_unimpl, &r)) { C->undef_known++; continue; }
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
    long fails = 0;
    for (int pass = 0; pass < 2; pass++) {
        counts *C = &cnt[pass];
        printf("%s %-6s records %ld | native ran %ld: match %ld, mismatch %ld, hao-fault %ld, hao-undef %ld (+%ld known unimplemented)"
               " | native SIGILL %ld: hao-undef %ld, hao-accepts %ld (+%ld known) | native fault %ld: hao fault %ld, not %ld"
               " | fpsr IEEE-flag differences %ld\n",
               mode >= 0 ? oracle_mode_name[mode] : "?", pass ? "jit" : "interp", total, C->ran, C->match, C->mismatch, C->fault,
               C->undef_bad, C->undef_known, C->ill, C->ill_undef, C->lenient_bad, C->lenient_known, C->nfault,
               C->nfault_hao_fault, C->nfault_missed, C->flagdiff);
        fails += C->mismatch + C->fault + C->undef_bad + C->lenient_bad;
    }
    return fails ? 1 : 0;
}
