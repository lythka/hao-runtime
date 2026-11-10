// Guest heap blocks among native call arguments (src/blocks.c heap_guest_block)
// and the values that are not blocks: every argument slot of an untyped native
// call that holds a heap-looking value is read to see whether it is a block.
//   noise    malloc region bases (run with MallocNanoZone=0 MallocDebugReport=stderr) (libmalloc logs "NO ZONE for ptr" when asked
//            their size), an unmapped page, a PROT_NONE page, a file page past
//            EOF (SIGBUS) and a packed integer pair, in x0-x7 and stack slots
//   blocks   a block literal moved into the interior of a malloc allocation,
//            into anonymous mmap memory and against a PROT_NONE page, each
//            invoked by libdispatch (must reach native code as a thunk); and on a
//            PROT_NONE page passed first, then mapped over (mmap) or made readable (mprotect)
//   masked   the same with SIGSEGV and SIGBUS blocked in the thread
//   handler  the noise call inside a SIGUSR1 handler whose sa_mask is full
//   bench    ns per native call with 8 non-block heap values (stderr only)
// Usage: heapprobe [SECTION...] (default: noise blocks masked handler)
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef long (*fn12)(long, long, long, long, long, long, long, long, long, long, long, long);
static fn12 volatile native12;   // labs: reads x0 only; the bridge scans every slot of an untyped call
static long v[12];

static void make_values(void)
{
    long pg = getpagesize();
    char *t = malloc(64);            // tiny with MallocNanoZone=0 (the runner sets it)
    char *s = malloc(4000);          // small
    v[0] = (long)((uintptr_t)t & ~(uintptr_t)0xFFFFF);     // tiny region base
    v[1] = (long)((uintptr_t)s & ~(uintptr_t)0x7FFFFF);    // small region base
    v[2] = (long)(t + 64);                                 // interior of an allocation
    char *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    munmap(m + pg, pg);
    v[3] = (long)(m + pg);                                 // unmapped
    mprotect(m + 2 * pg, pg, PROT_NONE);
    v[4] = (long)(m + 2 * pg);                             // PROT_NONE
    char path[] = "/tmp/heapprobe.XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    char z[64] = {0};
    write(fd, z, sizeof z);
    char *f = mmap(NULL, 2 * pg, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    v[5] = (long)(f + pg);                                 // past EOF: SIGBUS
    v[6] = 0x0000123400000000L;                            // a packed {0, 0x1234}
    v[7] = (long)m;                                        // mapped anonymous, not a block
    for (int k = 8; k < 12; k++) v[k] = v[k - 8];          // stack slots
    v[8] = 0x00007e0000000000L;                            // far from every mapping
}

/* unreadable values no earlier section passed (hao remembers pages whose read
 * faulted and reads them through Mach from then on) */
static void fresh_values(void)
{
    static long n;
    long pg = getpagesize();
    n++;
    char *m = mmap(NULL, pg, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    v[3] = 0x00007e0000000000L + n * 0x10000000L;
    v[4] = (long)m;
    v[6] = 0x0000123400000000L + n * 0x100000000L;
    v[8] = 0x00007d0000000000L + n * 0x10000000L;
}

static long noise_call(void)
{
    return native12(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
}

static void noise(void)
{
    long r = 0;
    for (int i = 0; i < 2000; i++) r += noise_call() != labs(v[0]);
    printf("noise: %ld mismatches\n", r);
}

struct BlockHead { void *isa; int flags, reserved; void *invoke; uintptr_t *descriptor; };

static long hits;
static void run_moved(const char *what, unsigned char *dst, int at_end)
{
    long captured = 17;
    void (^block)(void) = ^{ hits += captured; };
    struct BlockHead *b = (void *)block;
    size_t size = b->descriptor[1];
    if (at_end) dst -= (size + 7) & ~(size_t)7;     // the block's last word just below dst
    volatile unsigned char *d = dst;
    const unsigned char *src = (const void *)block;
    for (size_t i = 0; i < size; i++) d[i] = src[i];
    hits = 0;
    dispatch_sync(dispatch_get_global_queue(0, 0), (dispatch_block_t)(void *)dst);
    printf("%s block: %ld\n", what, hits);
}

static void blocks(void)
{
    long pg = getpagesize();
    unsigned char *a = malloc(512);
    run_moved("malloc interior", a + 32, 0);
    unsigned char *m = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    run_moved("mmap", m + 128, 0);
    mprotect(m + pg, pg, PROT_NONE);
    run_moved("guard-adjacent", m + pg, 1);
    // a page whose read faulted, mapped again at the same address (a PROT_NONE
    // reservation of our own: an unmapped page could be taken by anyone meanwhile)
    unsigned char *r = mmap(NULL, pg, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    long n = 0;
    for (int i = 0; i < 100; i++) n += native12((long)r, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) != (long)r;
    unsigned char *r2 = mmap(r, pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    run_moved(r2 == r && !n ? "remapped" : "remapped (not at the address)", r + 64, 0);
    // the same through mprotect: PROT_NONE while it was passed, then readable
    unsigned char *q = mmap(NULL, pg, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    for (int i = 0; i < 100; i++) n += native12((long)q, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) != (long)q;
    mprotect(q, pg, PROT_READ | PROT_WRITE);
    run_moved("reprotected", q + 64, 0);
    free(a);
}

static void masked(void)
{
    sigset_t s, o;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV);
    sigaddset(&s, SIGBUS);
    pthread_sigmask(SIG_BLOCK, &s, &o);
    fresh_values();
    printf("masked: ");
    noise();
    blocks();
    pthread_sigmask(SIG_SETMASK, &o, NULL);
    sigprocmask(SIG_BLOCK, &s, &o);
    fresh_values();
    printf("procmask: ");
    noise();
    sigprocmask(SIG_SETMASK, &o, NULL);
}

static volatile long handler_r = -1;
static void on_usr1(int sig)
{
    (void)sig;
    long r = 0;
    for (int i = 0; i < 200; i++) r += noise_call() != labs(v[0]);
    handler_r = r;
}

static void handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigfillset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    fresh_values();
    raise(SIGUSR1);
    printf("handler: %ld mismatches\n", handler_r);
}

static void bench(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    long mapped[12];
    for (int k = 0; k < 12; k++) mapped[k] = k < 3 ? v[k] : v[7];   // region bases, interior, anonymous: readable
    int n = 200000;
    long r = 0;
    uint64_t t0 = mach_absolute_time();
    for (int i = 0; i < n; i++)
        r += native12(mapped[0], mapped[1], mapped[2], mapped[3], mapped[4], mapped[5], mapped[6], mapped[7], 0, 0, 0, 0);
    uint64_t t1 = mach_absolute_time();
    for (int i = 0; i < n; i++) r += native12(1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0);
    uint64_t t2 = mach_absolute_time();
    for (int i = 0; i < n / 20; i++) r += native12(v[8], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint64_t t3 = mach_absolute_time();
    double ns = (double)tb.numer / tb.denom;
    fprintf(stderr, "bench: %.1f ns/call 8 readable heap values, %.1f ns/call scalars, %.1f ns/call 1 unmapped (%ld)\n",
            (t1 - t0) * ns / n, (t2 - t1) * ns / n, (t3 - t2) * ns / (n / 20), r);
    printf("bench: done\n");
}

int main(int argc, char **argv)
{
    native12 = (fn12)(void *)labs;
    make_values();
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *all[] = {"noise", "blocks", "masked", "handler"};
    int n = argc > 1 ? argc - 1 : 4;
    for (int i = 0; i < n; i++) {
        const char *s = argc > 1 ? argv[i + 1] : all[i];
        if (!strcmp(s, "noise")) noise();
        else if (!strcmp(s, "blocks")) blocks();
        else if (!strcmp(s, "masked")) masked();
        else if (!strcmp(s, "handler")) handler();
        else if (!strcmp(s, "bench")) bench();
        else { fprintf(stderr, "unknown section %s\n", s); return 2; }
    }
    return 0;
}
