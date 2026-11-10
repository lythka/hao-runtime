/* Native helper for tests/guest/signals.c, built universal (x86_64, arm64,
 * arm64e). Under hao the guest binds to the x86_64 slice, so a thread
 * started here runs only native code: it has no guest state until a guest
 * signal handler runs on it. */
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

/* Allocate and free until *stop is set: a signal is likely to land while
 * the thread holds the malloc zone lock. */
void *sighelper_malloc_loop(void *stop)
{
    unsigned long n = 0;
    while (!__atomic_load_n((volatile int *)stop, __ATOMIC_ACQUIRE)) {
        void *p = malloc(16 + (n++ & 255));
        __asm__ volatile("" ::"r"(p) : "memory");
        free(p);
    }
    return (void *)n;
}

/* Signal this thread while it holds the locks of every malloc zone
 * (force_lock is what fork uses; the default nano zone's is a no-op, the
 * helper zone's blocks every allocation it serves): the handler, and
 * whatever runs it, must not allocate. */
void *sighelper_locked_signal(void *sig)
{
    vm_address_t *zones;
    unsigned n = 0;
    if (malloc_get_all_zones(mach_task_self(), NULL, &zones, &n) != KERN_SUCCESS) return NULL;
    for (unsigned k = 0; k < n; k++) {
        malloc_zone_t *z = (malloc_zone_t *)zones[k];
        if (z->introspect && z->introspect->force_lock) z->introspect->force_lock(z);
    }
    pthread_kill(pthread_self(), (int)(intptr_t)sig);   /* delivered on return from the call */
    for (unsigned k = n; k-- > 0;) {
        malloc_zone_t *z = (malloc_zone_t *)zones[k];
        if (z->introspect && z->introspect->force_unlock) z->introspect->force_unlock(z);
    }
    return (void *)1;
}

/* Count the allocations made on this thread while a signal is delivered to
 * it. libmalloc calls malloc_logger for every allocation and free of every
 * zone (nano included, whose force_lock above is a no-op, so the mlocked
 * mode cannot catch a small allocation): set it, signal this thread, and
 * read the count. Natively the kernel calls the guest handler directly and
 * nothing allocates; under hao, delivery (thread state, the handler's
 * translation and its native calls) must not either. */
typedef void malloc_logger_fn(uint32_t type, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t result, uint32_t skip);
extern malloc_logger_fn *malloc_logger;
static malloc_logger_fn *prev_logger;
static pthread_t counted;
static volatile long allocs;

static void count_logger(uint32_t type, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t result, uint32_t skip)
{
    if (counted && pthread_equal(counted, pthread_self())) __atomic_fetch_add(&allocs, 1, __ATOMIC_RELAXED);
    if (prev_logger) prev_logger(type, a1, a2, a3, result, skip + 1);
}

long sighelper_count_signal(int sig)
{
    static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&one);
    if (malloc_logger != count_logger) { prev_logger = malloc_logger; malloc_logger = count_logger; }
    allocs = 0;
    __atomic_store_n(&counted, pthread_self(), __ATOMIC_SEQ_CST);
    pthread_kill(pthread_self(), sig);   /* delivered on return from the call */
    __atomic_store_n(&counted, (pthread_t)0, __ATOMIC_SEQ_CST);
    long n = allocs;
    pthread_mutex_unlock(&one);
    return n;
}

/* The same on a thread that runs only native code until the signal. */
void *sighelper_count_thread(void *sig) { return (void *)sighelper_count_signal((int)(intptr_t)sig); }
