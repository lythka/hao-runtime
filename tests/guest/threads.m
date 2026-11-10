/* Guest thread exit and fork (run by tests/guest/threads.sh).
 *
 * Creates and joins N threads (argv[1], default 400). On each thread, guest
 * code also runs during thread exit, after libpthread has started the TSD
 * destructor passes:
 *   - a guest pthread key whose destructor re-arms itself three times and
 *     autoreleases a guest object each time;
 *   - a guest object autoreleased with no pool, drained by the ObjC
 *     autorelease-pool TSD destructor (guest -dealloc);
 *   - a guest SIGUSR1 handler, raised with pthread_kill(self);
 *   - on odd threads, pthread_exit from 20 nested guest frames.
 * Prints the counters and the growth of the task's virtual size. Then it
 * parks 4 threads in read(), each of which publishes a pointer to a local
 * on its own stack, forks, and the child prints the sum of those locals:
 * a fork child keeps the other threads' stacks mapped. */
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_key_t gk;
static volatile int dtor_runs, deallocs, sigs, nested_exits;
static __thread int tlv_counter;

static double vm_mb(void)
{
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt);
    return vi.virtual_size / 1048576.0;
}

@interface Guest : NSObject
@end
@implementation Guest
- (void)dealloc
{
    __atomic_fetch_add(&deallocs, 1, __ATOMIC_RELAXED);
    tlv_counter++;
    [super dealloc];
}
@end

static void gdtor(void *v)
{
    /* guest code on the dying thread: use some guest stack, touch a TLV,
     * autorelease another guest object, ask for another pass */
    volatile char buf[8192];
    buf[0] = 1;
    buf[8191] = (char)(long)v;
    tlv_counter += buf[0];
    __atomic_fetch_add(&dtor_runs, 1, __ATOMIC_RELAXED);
    [[[Guest alloc] init] autorelease];
    if ((long)v < 3) pthread_setspecific(gk, (void *)((long)v + 1));
}

static void onsig(int s)
{
    (void)s;
    __atomic_fetch_add(&sigs, 1, __ATOMIC_RELAXED);
}

static void __attribute__((noinline)) deep(int n)
{
    if (n) {
        deep(n - 1);
        __asm__ volatile("" ::: "memory");  /* not a tail call */
        return;
    }
    __atomic_fetch_add(&nested_exits, 1, __ATOMIC_RELAXED);
    pthread_exit(NULL);
}

static void *thr(void *x)
{
    pthread_setspecific(gk, (void *)1);
    [[[Guest alloc] init] autorelease];  /* no pool: drained by objc's TSD destructor */
    pthread_kill(pthread_self(), SIGUSR1);
    if ((long)x & 1) deep(20);
    return NULL;
}

static int fds[2];
static volatile int *shared[4];

static void *parked(void *a)
{
    volatile int local[1024];
    local[0] = (int)(long)a + 100;
    __atomic_store_n(&shared[(long)a], local, __ATOMIC_RELEASE);
    char c;
    read(fds[0], &c, 1);
    return NULL;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 400;
    signal(SIGUSR1, onsig);
    pthread_key_create(&gk, gdtor);
    double v0 = vm_mb();
    for (int k = 0; k < n; k++) {
        pthread_t t;
        pthread_create(&t, 0, thr, (void *)(long)k);
        pthread_join(t, 0);
    }
    printf("threads=%d dtor_runs=%d deallocs=%d sigs=%d nested_exits=%d\n", n, dtor_runs, deallocs, sigs, nested_exits);
    printf("vm_growth=%.0f\n", vm_mb() - v0);
    fflush(stdout);

    pipe(fds);
    pthread_t park[4];
    for (long k = 0; k < 4; k++) pthread_create(&park[k], 0, parked, (void *)k);
    for (int k = 0; k < 4; k++)
        while (!__atomic_load_n(&shared[k], __ATOMIC_ACQUIRE)) usleep(1000);
    pid_t pid = fork();
    if (pid == 0) {
        int s = 0;
        for (int k = 0; k < 4; k++) s += shared[k][0];
        printf("child: sum=%d\n", s);
        fflush(stdout);
        _exit(0);
    }
    int st = -1;
    waitpid(pid, &st, 0);
    printf("child status=%d\n", st);
    close(fds[1]);
    for (int k = 0; k < 4; k++) pthread_join(park[k], 0);
    return 0;
}
