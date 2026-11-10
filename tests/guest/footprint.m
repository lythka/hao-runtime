/*
 * The process's own memory footprint (task_info TASK_VM_INFO phys_footprint,
 * the number jetsam compares with a process's limit), after a small
 * daemon-like workload. tests/guest/footprint.sh runs it natively and under
 * hao and compares the two.
 *   footprint c          printf only
 *   footprint objc       Foundation, a dispatch queue, a run loop turn
 *   footprint thunks N   N distinct guest functions handed to native code
 *                        (qsort comparators: each one gets a thunk), N <=
 *                        FP_NFNS (footprint_fns.h, written by the runner)
 * FP_HOLD=1 in the environment: sleep after the report, for footprint(1)/vmmap(1).
 * Prints "footprint_kb=K" and exits 0.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef FP_FNS
#include FP_FNS
#else
#define FP_NFNS 1
static int fp_cmp0(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int (*const fp_fns[FP_NFNS])(const void *, const void *) = {fp_cmp0};
#endif

static unsigned long long footprint_kb(void)
{
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return 0;
    return vi.phys_footprint >> 10;
}

static int work_c(void)
{
    printf("hello\n");
    return 0;
}

static int work_objc(void)
{
    @autoreleasepool {
        NSMutableArray *a = [NSMutableArray array];
        for (int k = 0; k < 100; k++) [a addObject:[NSString stringWithFormat:@"item %d", k]];
        NSDictionary *d = @{@"count" : @(a.count), @"first" : a[0]};
        dispatch_queue_t q = dispatch_queue_create("footprint", DISPATCH_QUEUE_SERIAL);
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        __block NSUInteger seen = 0;
        dispatch_async(q, ^{ seen = [d[@"count"] unsignedIntegerValue]; dispatch_semaphore_signal(s); });
        dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        printf("objc %lu %s\n", (unsigned long)seen, [d[@"first"] UTF8String]);
    }
    return 0;
}

static int work_thunks(int n)
{
    if (n < 0 || n > FP_NFNS) { fprintf(stderr, "thunks: N must be 0..%d\n", FP_NFNS); return 2; }
    long sum = 0;
    for (int k = 0; k < n; k++) {
        int v[2] = {2, 1};
        qsort(v, 2, sizeof v[0], fp_fns[k]);
        sum += v[0];
    }
    printf("thunks %d %ld\n", n, sum);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: footprint c|objc|thunks N\n"); return 2; }
    int r;
    if (!strcmp(argv[1], "c")) r = work_c();
    else if (!strcmp(argv[1], "objc")) r = work_objc();
    else if (!strcmp(argv[1], "thunks") && argc > 2) r = work_thunks(atoi(argv[2]));
    else { fprintf(stderr, "unknown mode %s\n", argv[1]); return 2; }
    if (r) return r;
    printf("footprint_kb=%llu\n", footprint_kb());
    fflush(stdout);
    const char *hold = getenv("FP_HOLD");
    if (hold && *hold == '1') pause();
    return 0;
}
