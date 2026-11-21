/*
 * hao — AOT use records: which guest programs and images run, and how much of
 * their code the JIT had to translate because no AOT cache covered it.
 *
 * With HAO_AOT_USE=1 (or boot argument hao_aot_use=1, so that one boot of an
 * image records every process, daemons and PID 1 included) each process logs
 * one-line records to the unified log, subsystem com.hao, category use, and
 * also to its diagnostics stream when verbose:
 *
 *   hao-use 1 RUN main|image UUID STATE PATH   when an image is initialised
 *   hao-use 1 RUN jit BLOCKS UUID PATH          per image with translated blocks
 *
 * RUN is a random 64-bit id of the process run (a fork child takes a new one),
 * STATE what hao_aot_load found (off, aot, none, rejected, unfit, mapped: hao/aot.h
 * HAO_AOT_STATE_*), BLOCKS the image's blocks the JIT translated so far
 * (cumulative: the last record of a run counts). jit records are written at
 * exit, and by one thread of hao's 60 seconds after the start and then every
 * ten minutes while the count grows, since daemons seldom exit. Paths are
 * logged public: the records are for a test image, never on by default.
 * tools/hao-aot-use turns them into an AOT list (/usr/lib/hao/aot-list).
 *
 * Nothing here runs in a signal handler; the only state the hot paths touch
 * is hao_image.jit_blocks, an atomic add under the JIT lock (jit.c). No
 * thread-local variables.
 */
#include "hao/aot.h"
#include "hao/macho.h"
#include "hao/opts.h"
#include "hao/runtime.h"
#include <os/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static os_log_t use_log;
static uint64_t use_run;
static uint64_t use_reported;   /* sum of jit_blocks at the last report */
static int use_busy;            /* one report at a time (the thread and exit) */

static const char *const state_names[] = {"off", "aot", "none", "rejected", "unfit", "mapped"};

static void use_emit(const char *line)
{
    os_log_t l = __atomic_load_n(&use_log, __ATOMIC_ACQUIRE);
    if (!l) {
        l = os_log_create("com.hao", "use");
        os_log_t none = NULL;
        if (!__atomic_compare_exchange_n(&use_log, &none, l, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) l = none;
    }
    os_log_with_type(l, OS_LOG_TYPE_DEFAULT, "%{public}s", line);
    if (hao_verbose) { fprintf(HAO_DIAG, "%s\n", line); fflush(HAO_DIAG); }
}

static void uuid_hex(const hao_image *im, char out[33])
{
    for (int k = 0; k < 16; k++) snprintf(out + 2 * k, 3, "%02x", im->has_uuid ? im->uuid[k] : 0);
}

void hao_aot_use_image(hao_image *im)
{
    if (!hao_opt.aot_use) return;
    hao_aot_use_init();   /* normally done already, before the first image (main.c) */
    char u[33], line[1300];
    uuid_hex(im, u);
    unsigned s = im->aot_state < sizeof state_names / sizeof state_names[0] ? im->aot_state : 0;
    snprintf(line, sizeof line, "hao-use 1 %016llx %s %s %s %s", (unsigned long long)use_run, im == hao_main_image ? "main" : "image",
             u, state_names[s], im->path[0] ? im->path : "?");
    use_emit(line);
}

/* jit records for every image with translated blocks, when the total changed (or always: at exit) */
static void use_report(bool always)
{
    if (__atomic_exchange_n(&use_busy, 1, __ATOMIC_ACQUIRE)) return;
    uint64_t total = 0;
    for (hao_image *im = hao_images_first(); im; im = hao_images_next(im)) total += __atomic_load_n(&im->jit_blocks, __ATOMIC_RELAXED);
    if (total != use_reported || (always && total)) {
        use_reported = total;
        for (hao_image *im = hao_images_first(); im; im = hao_images_next(im)) {
            uint64_t n = __atomic_load_n(&im->jit_blocks, __ATOMIC_RELAXED);
            if (!n || __atomic_load_n(&im->released, __ATOMIC_ACQUIRE)) continue;
            char u[33], line[1300];
            uuid_hex(im, u);
            snprintf(line, sizeof line, "hao-use 1 %016llx jit %llu %s %s", (unsigned long long)use_run, (unsigned long long)n, u,
                     im->path[0] ? im->path : "?");
            use_emit(line);
        }
    }
    __atomic_store_n(&use_busy, 0, __ATOMIC_RELEASE);
}

static void use_exit(void) { use_report(true); }

static void *use_periodic(void *arg)
{
    (void)arg;
    hao_signals_block_async();
    sleep(60);
    for (;;) {
        use_report(false);
        sleep(600);
    }
    return NULL;
}

static void use_start_thread(void)
{
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 128 << 10);
    if (pthread_create(&th, &a, use_periodic, NULL) != 0) { /* the exit record still comes */ }
    pthread_attr_destroy(&a);
}

/* A fork child is another run: its own id and its own counts, reported at its
 * exit (the reporting thread does not survive the fork, and a child that only
 * execs gets no new one); the parent's log object faults in the child
 * (runtime.c uni_fork_child), so it makes its own. */
static void use_fork_child(void)
{
    use_run = ((uint64_t)arc4random() << 32) | arc4random();
    use_log = NULL;
    use_busy = 0;
    use_reported = 0;
    for (hao_image *im = hao_images_first(); im; im = hao_images_next(im)) im->jit_blocks = 0;
}

/* Once, before the first image is initialised. */
void hao_aot_use_init(void)
{
    static int done;
    if (!hao_opt.aot_use || done++) return;
    use_run = ((uint64_t)arc4random() << 32) | arc4random();
    atexit(use_exit);
    pthread_atfork(NULL, NULL, use_fork_child);
    use_start_thread();
}
