/* P25 step A's three new sources of native signatures, one representative call each
 * group (tests/guest/p25src.sh). `p25src CASE` runs one case; `p25src list` names them.
 *   oss-*   open-source Darwin prototypes no SDK header has (tools/gen_bridge.py --oss:
 *           libplatform's os/log_simple_private_impl.h, os/lock_private.h, os/once_private.h;
 *           dyld's mach-o/dyld_priv.h). Declared here as those headers declare them.
 *   sys-*   libsystem_kernel syscall and Mach trap stubs typed from xnu's syscalls.master and
 *           syscall_sw.c (tools/gen_syscalls.py). Declared here as syscalls.master gives them.
 * The C++ representatives (typed operator new/delete, __cxa_guard_*) are p25srccxx.cpp.
 * Every case prints only what is the same natively and under hao (results, errno). */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* libplatform-375.120.2 */
uint64_t os_log_simple_now(void);
uint8_t os_log_simple_type_from_asl(int level);
typedef struct os_unfair_recursive_lock_s { os_unfair_lock ourl_lock; uint32_t ourl_count; } os_unfair_recursive_lock,
    *os_unfair_recursive_lock_t;
typedef uint32_t os_unfair_lock_options_t;
void os_unfair_recursive_lock_lock_with_options(os_unfair_recursive_lock_t lock, os_unfair_lock_options_t options);
bool os_unfair_recursive_lock_trylock(os_unfair_recursive_lock_t lock);
void os_unfair_recursive_lock_unlock(os_unfair_recursive_lock_t lock);
typedef long os_once_t;
typedef void (*os_function_t)(void *);
void _os_once(os_once_t *predicate, void *context, os_function_t function);
/* dyld-1378 */
bool _dyld_get_shared_cache_uuid(unsigned char uuid[16]);
/* xnu-12377.121.6 bsd/kern/syscalls.master, osfmk/kern/syscall_sw.c */
int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout);
int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
typedef uint64_t guardid_t;
int guarded_close_np(int fd, const guardid_t *guard);
int __mac_syscall(const char *policy, int call, void *arg);
int csops_audittoken(pid_t pid, uint32_t ops, void *useraddr, size_t usersize, void *uaudittoken);
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);
kern_return_t pid_for_task(mach_port_name_t t, int *x);

static volatile int seed = 3;

static void c_oss_now(void)
{
    uint64_t a = os_log_simple_now(), b = os_log_simple_now();
    printf("os_log_simple_now nonzero=%d monotonic=%d\n", a != 0, b >= a);
}
static void c_oss_asl(void)
{
    printf("os_log_simple_type_from_asl");
    for (int l = 0; l < 8; l++) {
        long t = os_log_simple_type_from_asl(l + seed - 3);   /* widened from w0 */
        printf(" %ld", t);
    }
    printf("\n");
}
static void c_oss_rlock(void)
{
    os_unfair_recursive_lock l = {OS_UNFAIR_LOCK_INIT, 0};
    os_unfair_recursive_lock_lock_with_options(&l, 0);
    os_unfair_recursive_lock_lock_with_options(&l, 0);
    long t = os_unfair_recursive_lock_trylock(&l);
    printf("os_unfair_recursive_lock trylock=%ld count=%u\n", t, l.ourl_count);
    os_unfair_recursive_lock_unlock(&l);
    os_unfair_recursive_lock_unlock(&l);
    os_unfair_recursive_lock_unlock(&l);
    printf("os_unfair_recursive_lock after count=%u lock=%u\n", l.ourl_count, l.ourl_lock._os_unfair_lock_opaque);
}
static int once_runs;
static void once_fn(void *ctx) { once_runs += *(int *)ctx; }
static void c_oss_once(void)
{
    static os_once_t pred;
    int ctx = seed * 10;
    _os_once(&pred, &ctx, once_fn);
    _os_once(&pred, &ctx, once_fn);
    printf("_os_once runs=%d predicate=%s\n", once_runs, pred ? "set" : "zero");
}
static void c_oss_dscuuid(void)
{
    unsigned char u[16];
    memset(u, 0, sizeof u);
    long ok = _dyld_get_shared_cache_uuid(u);
    int nz = 0;
    for (int i = 0; i < 16; i++) nz |= u[i];
    printf("_dyld_get_shared_cache_uuid ok=%ld nonzero=%d\n", ok, nz != 0);
}
static void c_sys_ulock(void)
{
    uint32_t word = 5;
    errno = 0;
    int w = __ulock_wake(1, &word, 0);   /* UL_COMPARE_AND_WAIT, no waiter */
    int e1 = errno;
    errno = 0;
    int r = __ulock_wait(1, &word, 6, 1000);   /* the value differs: returns at once */
    printf("__ulock_wake %d errno=%d __ulock_wait %d errno=%d\n", w, e1, r, errno);
}
static void c_sys_guarded(void)
{
    int fd = open("/dev/null", O_RDONLY);
    guardid_t g = 0x1234;
    errno = 0;
    int r = guarded_close_np(fd, &g);   /* fd has no guard */
    int e = errno;
    errno = 0;
    int r2 = guarded_close_np(-1, &g);
    printf("guarded_close_np %d errno=%d bad fd %d errno=%d\n", r, e, r2, errno);
    close(fd);
}
static void c_sys_mac(void)
{
    errno = 0;
    int r = __mac_syscall("hao-no-such-policy", 0, 0);
    printf("__mac_syscall %d errno=%d\n", r, errno);
}
static void c_sys_csops(void)
{
    uint32_t flags = 0;
    errno = 0;
    int r = csops_audittoken(getpid(), 0xffff, &flags, sizeof flags, 0);   /* no token, bad op */
    printf("csops_audittoken %d errno=%d\n", r, errno);
}
static void c_sys_memstatus(void)
{
    errno = 0;
    int r = memorystatus_control(0xffff, getpid(), 0, 0, 0);
    printf("memorystatus_control %d errno=%d\n", r, errno);
}
static void c_sys_pidfortask(void)
{
    int pid = -1;
    kern_return_t kr = pid_for_task(mach_task_self(), &pid);
    printf("pid_for_task kr=%d self=%d\n", kr, pid == getpid());
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"oss-now", c_oss_now}, {"oss-asl", c_oss_asl}, {"oss-rlock", c_oss_rlock}, {"oss-once", c_oss_once},
    {"oss-dscuuid", c_oss_dscuuid}, {"sys-ulock", c_sys_ulock}, {"sys-guarded", c_sys_guarded}, {"sys-mac", c_sys_mac},
    {"sys-csops", c_sys_csops}, {"sys-memstatus", c_sys_memstatus}, {"sys-pidfortask", c_sys_pidfortask},
};

int main(int argc, char **argv)
{
    int n = (int)(sizeof cases / sizeof cases[0]);
    if (argc < 2) { fprintf(stderr, "usage: p25src CASE|list\n"); return 2; }
    for (int i = 0; i < n; i++) {
        if (!strcmp(argv[1], "list")) { printf("%s\n", cases[i].name); continue; }
        if (!strcmp(argv[1], cases[i].name)) { cases[i].fn(); fflush(stdout); return 0; }
    }
    if (!strcmp(argv[1], "list")) return 0;
    fprintf(stderr, "p25src: no case %s\n", argv[1]);
    return 2;
}
