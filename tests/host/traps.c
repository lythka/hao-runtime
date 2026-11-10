/* Host test: the trap table (runtime.c hao_trap_address).
 *
 *   test_traps unresolved   3 images x 6000 unresolved imports, the same
 *                           names in each: one trap per name, distinct
 *                           traps for distinct names, and a call through
 *                           one reports its own name
 *   test_traps many         70000 distinct traps: the table (65536) fills
 *                           without aborting, later ones share the
 *                           overflow trap, existing ones are found again
 *                           quickly
 * Each prints one summary line ending in PASS or FAIL; exit 0 on PASS. */
#include "hao/runtime.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void trap_nop(hao_thread *t, uint64_t arg) { (void)t; (void)arg; }

static double now_ms(void)
{
    struct timespec a;
    clock_gettime(CLOCK_MONOTONIC, &a);
    return (double)a.tv_sec * 1e3 + (double)a.tv_nsec / 1e6;
}

/* call the trap at `a` in a child; true if its stderr names `name` */
static bool call_reports(uint64_t a, const char *name, const char *dir)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/traps-call.%d.err", dir, getpid());
    pid_t p = fork();
    if (!p) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 2);
        hao_thread *t = hao_current();
        t->cpu.pc = a;
        t->cpu.x[30] = HAO_RETURN_MAGIC;
        hao_run(t, NULL);   /* top level: no enclosing guest state */
        _exit(0);   /* the trap returned: wrong */
    }
    int st;
    waitpid(p, &st, 0);
    char buf[4096] = {0};
    FILE *f = fopen(path, "r");
    if (f) { if (!fread(buf, 1, sizeof buf - 1, f)) buf[0] = 0; fclose(f); }
    unlink(path);
    char want[600];
    snprintf(want, sizeof want, "call to unresolved import %s ", name);
    return WIFSIGNALED(st) && strstr(buf, want);
}

static int t_unresolved(const char *dir)
{
    enum { NIMG = 3, NSYM = 6000 };
    static uint64_t addr[NIMG][NSYM];
    char name[64];
    int notrap = 0, found = 0;
    for (int j = 0; j < NIMG; j++) {
        hao_image *im = calloc(1, sizeof(hao_image));
        snprintf(im->path, sizeof im->path, "test-image");
        for (int k = 0; k < NSYM; k++) {
            snprintf(name, sizeof name, "_hao_test_no_such_symbol_%d", k);
            bool f;
            addr[j][k] = hao_resolve_import(im, -2, name, false, &f);
            found += f;
            notrap += !hao_is_trap(addr[j][k]);
        }
    }
    int shared_wrong = 0, distinct_wrong = 0;
    for (int k = 0; k < NSYM; k++) {
        for (int j = 1; j < NIMG; j++) shared_wrong += addr[j][k] != addr[0][k];
        if (k) distinct_wrong += addr[0][k] == addr[0][k - 1];
    }
    bool named = call_reports(addr[2][5000], "_hao_test_no_such_symbol_5000", dir);
    bool ok = !found && !notrap && !shared_wrong && !distinct_wrong && named;
    printf("unresolved: imports=%d resolved=%d not_a_trap=%d same_name_new_trap=%d same_trap_two_names=%d call_names_symbol=%s -> %s\n",
           NIMG * NSYM, found, notrap, shared_wrong, distinct_wrong, named ? "yes" : "no", ok ? "PASS" : "FAIL");
    return !ok;
}

static int t_many(void)
{
    enum { N = 70000, CAP = 65536 };
    static uint64_t a[N];
    double t0 = now_ms();
    for (int k = 0; k < N; k++) a[k] = hao_trap_address("test", trap_nop, 0x100000000ull + (uint64_t)k);
    double t1 = now_ms();
    int again_wrong = 0;
    for (int k = 0; k < N; k++) again_wrong += hao_trap_address("test", trap_nop, 0x100000000ull + (uint64_t)k) != a[k];
    double t2 = now_ms();
    /* the slots this test can still get: CAP - 1 (overflow slot) - those already used (none here) */
    int distinct = 0, overflow = 0, notrap = 0;
    for (int k = 0; k < N; k++) {
        notrap += !hao_is_trap(a[k]);
        if (k && a[k] == a[N - 1]) overflow++;
        else distinct++;
    }
    bool ok = !notrap && !again_wrong && distinct == CAP - 1 && overflow == N - (CAP - 1) && t2 - t1 < 1000;
    printf("many: traps=%d distinct=%d overflow_trap=%d not_a_trap=%d lookup_changed=%d make_ms=%.0f lookup_ms=%.0f -> %s\n", N, distinct,
           overflow, notrap, again_wrong, t1 - t0, t2 - t1, ok ? "PASS" : "FAIL");
    return !ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "unresolved")) return t_unresolved(argc > 2 ? argv[2] : ".");
    if (!strcmp(m, "many")) return t_many();
    fprintf(stderr, "usage: test_traps unresolved [DIR] | many\n");
    return 2;
}
