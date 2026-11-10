/* Host test: fork while other threads hold hao's locks.
 *
 * Worker threads translate fresh guest blocks (jit_lock), make callback
 * thunks (thunk_lock), parse signatures / make signature thunks
 * (sig_lock, thunk_lock) and bind native entry points (bridge_lock). The main thread forks FORKS times. Each child
 * makes a thunk, a signature thunk and one new translation, then checks that
 * the last 64 signatures the workers asked for still parse, and exits 0.
 * A child that has not exited after 3 s is hung (a lock the parent's other
 * threads held came across locked); one that exits non-zero is bad (3: a
 * signature the parent had cached reads back as NULL).
 *
 *   test_fork [FORKS] [all|jit|thunk|sig|bridge]     SIG_SLOW=us throttles the sig
 *                                             workers so the cache never fills
 * Prints one summary line; exit 0 only if every child exited 0.
 *
 * hao_fork_init is looked up at run time, so the same source runs against a
 * libhao without it (the line then says "absent") to show the failure. */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct hsig hsig;
hsig *hao_sig(const char *enc);   /* abi.c */

#define NPC (1u << 22)                  /* distinct guest pcs, each a `ret` */
static uint32_t *code;
static uint32_t next_pc;
static uint64_t next_guest = 0x7000000000ull;
static uint32_t next_sig;
static volatile int stop;
static volatile int *stage;             /* MAP_SHARED: how far the child got */
static int sig_slow;

static void run_one(uint32_t i)
{
    hao_thread *t = hao_current();
    t->cpu.pc = (uint64_t)(uintptr_t)&code[i % NPC];
    t->cpu.x[30] = HAO_RETURN_MAGIC;
    hao_jit_run_block(t);
}

static void *w_jit(void *a)
{
    (void)a;
    while (!stop) {
        uint32_t i = __atomic_fetch_add(&next_pc, 1, __ATOMIC_RELAXED);
        if (i >= 900000) { usleep(1000); continue; }   /* leave pcs for the children */
        run_one(i);
    }
    return NULL;
}

static void *w_thunk(void *a)
{
    (void)a;
    while (!stop) hao_thunk_for(__atomic_fetch_add(&next_guest, 4, __ATOMIC_RELAXED));
    return NULL;
}

/* not "simple": a struct of four doubles needs a signature thunk */
static void sigstr(char *b, size_t n, uint32_t k) { snprintf(b, n, "v{s%u=dddd}i", k); }

static void *w_sig(void *a)
{
    (void)a;
    char b[64];
    while (!stop) {
        uint32_t k = __atomic_fetch_add(&next_sig, 1, __ATOMIC_RELAXED);
        sigstr(b, sizeof b, k);
        if (k & 1) hao_sig(b);
        else hao_guest_entry_for_sig(0x6000000000ull + 4ull * k, b);
        if (sig_slow) usleep((useconds_t)sig_slow);
    }
    return NULL;
}

/* binds (bridge_lock) of a bounded set of entry points: each call takes the lock */
static void *w_bridge(void *a)
{
    (void)a;
    for (uint32_t k = 0; !stop; k++) hao_bridge_bind("_hao_fork_test", "_hao_fork_test", 0x3000000000ull + 16ull * (k % 65536));
    return NULL;
}

static void child(int f)
{
    alarm(10);   /* never leave a stuck child behind */
    *stage = 1; hao_thunk_for(0x5000000000ull + 4ull * (unsigned)f);
    *stage = 2;
    char b[64];
    sigstr(b, sizeof b, 0x80000000u + (unsigned)f);
    hao_guest_entry_for_sig(0x4000000000ull + 4ull * (unsigned)f, b);
    *stage = 3; run_one(3000000u + (unsigned)f);
    *stage = 4;
    uint32_t top = __atomic_load_n(&next_sig, __ATOMIC_RELAXED);
    int nulls = 0;
    for (uint32_t k = top > 64 ? top - 64 : 0; k < top; k++) { sigstr(b, sizeof b, k); if (!hao_sig(b)) nulls++; }
    /* NULL is also the answer once the 4096-entry cache is full */
    if (nulls && hao_sig("v{fork_fresh=dddd}i")) _exit(3);
    *stage = 5;
    hao_bridge_note(0x2000000000ull + 16ull * (unsigned)f, 'B', NULL, HAO_BRS_BLOCKSIG);
    const hao_plan *p = hao_bridge_lookup(0x2000000000ull + 16ull * (unsigned)f);
    if (!p || p->kind != 'B') _exit(4);
    _exit(0);
}

int main(int argc, char **argv)
{
    int forks = argc > 1 ? atoi(argv[1]) : 300;
    const char *mode = argc > 2 ? argv[2] : "all";
    if (getenv("SIG_SLOW")) sig_slow = atoi(getenv("SIG_SLOW"));
    hao_jit_init();
    hao_bridge_init();
    void (*fork_init)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "hao_fork_init");
    if (fork_init) fork_init();
    printf("hao_fork_init: %s\n", fork_init ? "registered" : "absent");
    code = mmap(NULL, NPC * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    stage = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (code == MAP_FAILED || stage == MAP_FAILED) { perror("mmap"); return 2; }
    for (uint32_t k = 0; k < NPC; k++) code[k] = 0xd65f03c0;   /* ret */

    bool all = !strcmp(mode, "all");
    pthread_t th[8];
    int n = 0;
    for (int r = 0; r < 2; r++) {
        if (all || !strcmp(mode, "jit")) pthread_create(&th[n++], NULL, w_jit, NULL);
        if (all || !strcmp(mode, "thunk")) pthread_create(&th[n++], NULL, w_thunk, NULL);
        if (all || !strcmp(mode, "sig")) pthread_create(&th[n++], NULL, w_sig, NULL);
        if (all || !strcmp(mode, "bridge")) pthread_create(&th[n++], NULL, w_bridge, NULL);
    }
    if (!n) { fprintf(stderr, "mode: all | jit | thunk | sig | bridge\n"); return 2; }
    usleep(20000);

    int ok = 0, hung = 0, bad = 0, hung_at[6] = {0};
    for (int f = 0; f < forks; f++) {
        *stage = 0;
        pid_t p = fork();
        if (p == 0) child(f);
        if (p < 0) { perror("fork"); return 2; }
        int st = 0, waited = 0;
        for (; waited < 3000; waited += 5) { if (waitpid(p, &st, WNOHANG) == p) break; usleep(5000); }
        if (waited >= 3000) {
            hung++; hung_at[*stage]++;
            kill(p, SIGKILL); waitpid(p, &st, 0);
        } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++;
        else bad++;
        usleep(2000);
    }
    stop = 1;
    for (int k = 0; k < n; k++) pthread_join(th[k], NULL);
    printf("mode=%s forks=%d ok=%d hung=%d bad=%d (hung in: thunk_for=%d entry_for_sig=%d translate=%d sig_check=%d bridge=%d)\n",
           mode, forks, ok, hung, bad, hung_at[1], hung_at[2], hung_at[3], hung_at[4], hung_at[5]);
    return hung || bad ? 1 : 0;
}
