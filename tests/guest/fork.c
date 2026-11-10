/* Guest fork safety (tests/guest/fork.sh runs it under hao and natively).
 *   1. pthread_atfork order: prepare LIFO, parent and child FIFO.
 *   2. fork while 4 threads translate new code: each child translates new
 *      code and makes a new callback thunk (qsort), then exits 0.
 *   3. vfork + execl("/usr/bin/true") while the threads translate: the
 *      child translates new code before it execs.
 *   4. fork by raw `svc #0x80` (x16 = 2) while the threads translate: the
 *      kernel convention (parent x0 = child pid, x1 = 0; child x0 = its own
 *      pid, x1 = 1) and new code in the child.
 * A child that has not exited after 3 s is hung. Prints one line per
 * section; exit 0 only if no child hung or failed. Sections 3 and 4 also
 * count the children that ran the atfork child handlers: none natively
 * (vfork and a raw fork run no handlers), all under hao, which makes both
 * a fork so that its own child handler runs.
 *   fork [N]   (N forks in each of sections 2-4, default 100) */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "forkgen.h"

#define CHILD_FNS 400                     /* fns[NFN-400..]: only children call these */

static volatile int *shm;                 /* [0] child log length, [1..16] child log, [32] child handler runs */
static int plog[16], nplog, logging = 1;
static void logp(int v) { if (logging && nplog < 16) plog[nplog++] = v; }
static void logc(int v) { if (logging && shm[0] < 16) shm[1 + shm[0]++] = v; }
static void prep1(void) { logp(1); }
static void prep2(void) { logp(2); }
static void par1(void) { logp(11); }
static void par2(void) { logp(12); }
static void chi1(void) { logc(21); }
static void chi2(void) { logc(22); }
static void chi3(void) { __atomic_fetch_add(&shm[32], 1, __ATOMIC_RELAXED); }

/* workers: one new function about every 1.5 ms each, a new comparator
 * thunk every 8th; paced so that fresh code lasts through sections 2-4 */
static unsigned next_fn, next_cmp;
static volatile int stop;
static volatile int sink;
static void *worker(void *a)
{
    (void)a;
    while (!stop) {
        unsigned i = __atomic_fetch_add(&next_fn, 1, __ATOMIC_RELAXED);
        if (i >= NFN - CHILD_FNS) { usleep(1000); continue; }
        sink += fns[i]((int)i);
        if (!(i & 7)) {
            unsigned c = __atomic_fetch_add(&next_cmp, 1, __ATOMIC_RELAXED);
            int v[4] = {3, 1, 4, 2};
            if (c < NCMP / 2) qsort(v, 4, sizeof *v, cmps[c]);
        }
        usleep(1500);
    }
    return NULL;
}

/* new code in a child: the parent never runs these, so the child translates them */
static int child_work(int f)
{
    int s = 0;
    for (int k = 0; k < 20; k++) s += fns[NFN - CHILD_FNS + (f * 20 + k) % CHILD_FNS](k);
    int v[8] = {5, 3, 7, 1, 8, 2, 6, 4};
    qsort(v, 8, sizeof *v, cmps[NCMP / 2 + f % (NCMP / 2)]);
    return v[0] == 1 && v[7] == 8 ? s : -1;
}

/* wait up to 3 s: 0 ok, 1 hung (killed), 2 bad exit */
static int reap(pid_t p)
{
    int st = 0;
    for (int ms = 0; ms < 3000; ms += 5) {
        if (waitpid(p, &st, WNOHANG) == p) return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 2;
        usleep(5000);
    }
    kill(p, SIGKILL);
    waitpid(p, &st, 0);
    return 1;
}

static void report(const char *what, int n, const int r[3])
{
    printf("%s: n=%d ok=%d hung=%d bad=%d\n", what, n, r[0], r[1], r[2]);
    fflush(stdout);
}

/* raw BSD syscalls: libSystem is not involved (its getpid caches the pid) */
static void raw_fork(uint64_t *x0, uint64_t *x1, int *carry)
{
    register uint64_t r0 __asm__("x0") = 0, r1 __asm__("x1") = 0, r16 __asm__("x16") = 2;
    uint64_t f;
    __asm__ volatile("svc #0x80\n\tmrs %3, nzcv" : "+r"(r0), "+r"(r1), "+r"(r16), "=r"(f) : : "memory", "cc");
    *x0 = r0; *x1 = r1; *carry = (int)((f >> 29) & 1);
}
static uint64_t raw_getpid(void)
{
    register uint64_t r0 __asm__("x0") = 0, r16 __asm__("x16") = 20;
    __asm__ volatile("svc #0x80" : "+r"(r0), "+r"(r16) : : "memory", "cc", "x1");
    return r0;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 100;
    shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 2; }
    pthread_atfork(prep1, par1, chi1);
    pthread_atfork(prep2, par2, chi2);
    pthread_atfork(NULL, NULL, chi3);

    /* 1 */
    pid_t p = fork();
    if (p == 0) _exit(0);
    int st;
    waitpid(p, &st, 0);
    logging = 0;
    printf("atfork parent log:");
    for (int k = 0; k < nplog; k++) printf(" %d", plog[k]);
    printf("  child log:");
    for (int k = 0; k < shm[0]; k++) printf(" %d", shm[1 + k]);
    printf("\n");
    fflush(stdout);

    pthread_t th[4];
    for (int k = 0; k < 4; k++) pthread_create(&th[k], NULL, worker, NULL);
    usleep(20000);
    int fail = 0;

    /* 2 */
    int r[3] = {0};
    for (int f = 0; f < n; f++) {
        p = fork();
        if (p == 0) { alarm(10); _exit(child_work(f) < 0 ? 3 : 0); }
        if (p < 0) { perror("fork"); return 2; }
        r[reap(p)]++;
    }
    report("fork under translation", n, r);
    fail |= r[1] || r[2];

    /* 3 */
    r[0] = r[1] = r[2] = 0;
    shm[32] = 0;
    for (int f = 0; f < n; f++) {
        p = vfork();
        if (p == 0) {
            if (child_work(n + f) < 0) _exit(3);
            execl("/usr/bin/true", "true", (char *)NULL);
            _exit(127);
        }
        if (p < 0) { perror("vfork"); return 2; }
        r[reap(p)]++;
    }
    report("vfork+exec under translation", n, r);
    printf("vfork children that ran atfork child handlers: %d/%d\n", shm[32], n);
    fflush(stdout);
    fail |= r[1] || r[2];

    /* 4 */
    r[0] = r[1] = r[2] = 0;
    shm[32] = 0;
    for (int f = 0; f < n; f++) {
        uint64_t x0, x1;
        int c;
        raw_fork(&x0, &x1, &c);
        if (c) { fprintf(stderr, "raw fork: errno %llu\n", (unsigned long long)x0); return 2; }
        if (x1 == 1) {
            /* only raw syscalls and pure code here: natively, libSystem's
             * fork handlers did not run in this child */
            if (x0 != raw_getpid()) _exit(4);
            int s = 0;
            for (int k = 0; k < 20; k++) s += fns[NFN - CHILD_FNS + ((2 * n + f) * 20 + k) % CHILD_FNS](k);
            sink = s;
            _exit(0);
        }
        if (x1 != 0 || (int64_t)x0 <= 0) { fprintf(stderr, "raw fork: parent x0=%llu x1=%llu\n", (unsigned long long)x0, (unsigned long long)x1); return 2; }
        r[reap((pid_t)x0)]++;
    }
    report("raw svc fork under translation", n, r);
    printf("raw svc fork children that ran atfork child handlers: %d/%d\n", shm[32], n);
    fflush(stdout);
    fail |= r[1] || r[2];

    stop = 1;
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);
    printf("workers translated %u functions\n", next_fn > NFN - CHILD_FNS ? NFN - CHILD_FNS : next_fn);
    return fail;
}
