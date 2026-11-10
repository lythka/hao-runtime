/* Guest fork while another thread runs guest code under a dyld lock.
 * One thread registers an add-image callback. dyld calls it once per loaded
 * image while it holds its loaders lock, which libSystem's fork prepare also
 * takes, and each call translates new code. The main thread forks 20 times
 * meanwhile. If hao took its JIT lock in a fork prepare handler, a fork
 * could hold it while waiting for dyld's lock, with the callback holding
 * dyld's lock while waiting for the JIT lock: the parent would hang (the
 * alarm then kills it, exit 142). */
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "forkgen.h"

static volatile int started, n_cb;
static volatile long sink;

static void cb(const struct mach_header *mh, intptr_t slide)
{
    (void)mh; (void)slide;
    started = 1;
    int k = __atomic_fetch_add(&n_cb, 1, __ATOMIC_RELAXED);
    for (int j = 0; j < 40; j++) sink += fns[(k * 40 + j) % NFN](j);   /* new code on each call */
    usleep(2000);
}

static void *reg(void *a) { (void)a; _dyld_register_func_for_add_image(cb); return NULL; }

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, reg, NULL);
    while (!started) usleep(100);
    alarm(20);
    int forks = 0;
    for (; forks < 20; forks++) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        if (p < 0) { perror("fork"); return 2; }
        int st;
        waitpid(p, &st, 0);
        usleep(1000);
    }
    pthread_join(t, NULL);
    printf("dyld callback + fork: forks=%d parent ok\n", forks);
    return 0;
}
