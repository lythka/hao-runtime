/* Exceptions and longjmp across the guest/native boundary.
 *   eh [SECTION]   (no argument: every non-fatal section)
 * No std::string crosses the boundary: arm64 libc++ uses the alternate
 * string layout, so a guest-built string handed to native libc++ code is
 * misread (a separate bridge gap).
 * Run by tests/guest/eh.sh: the arm64 build natively is the reference for
 * the arm64e build under hao. Under hao every library frame (libc++abi,
 * Foundation, CoreFoundation, libdispatch, libc, libehhelper) is native
 * x86_64 code between translated guest frames.
 *   1  guest C++ throw, guest catch, guest cleanups
 *   2  native C++ throw (libc++, helper), guest catch
 *   3  native NSException (Foundation), guest @catch/@finally
 *   4  guest @throw, guest @catch by class, id, and NSException
 *   5  guest throw in a callback (qsort, CFArrayApplyFunction, a block,
 *      a native cleanup frame), guest catch above the native frames
 *   6  guest throw in a callback, native catch above (helper try/catch)
 *   7  rethrow: guest throw;, native throw;, exception_ptr, ObjC rethrow
 *   8  @try around NSKeyedUnarchiver, NSXPCInterface, NSJSONSerialization
 *   9  longjmp across native frames: qsort, helper, signal handlers
 *   10 exceptions on a second thread
 *   11 nested: an exception thrown and caught inside a cleanup
 *   20 uncaught C++ exception (fatal)
 *   21 uncaught NSException (fatal)
 *   22 C++ exception out of a dispatch_sync block (fatal)
 *   23-37 an exception that nothing catches reaches a libdispatch client
 *      callout or a CFRunLoop observer callout (fatal). Native arm64:
 *      std::terminate (libdispatch's callouts have the noexcept personality;
 *      the observer's exception reaches the end of the stack), the
 *      uncaught-exception or terminate handler runs, the reason is logged,
 *      SIGABRT. The thrower is the guest or native code (CoreFoundation,
 *      libc++ in the helper); the handler is none, NSSetUncaughtExceptionHandler
 *      (ObjC) or std::set_terminate:
 *        23 async   ObjC native (nil dictionary key)   NS handler
 *        24 async   ObjC native (nil dictionary key)   none
 *        25 async   ObjC guest @throw                  NS handler
 *        26 async   ObjC native, worker has only native frames
 *                   (dispatch_async_f(objc_exception_throw))  NS handler
 *        27 async   ObjC native doesNotRecognizeSelector:  NS handler
 *        28 async   C++ guest throw                    none
 *        29 async   C++ guest throw                    set_terminate
 *        30 async   C++ native throw (helper)          NS handler (does not run for C++)
 *        31 sync    ObjC guest @throw                  NS handler
 *        32 observer ObjC guest @throw                 NS handler
 *        33 observer ObjC native (nil dictionary key)  none
 *        34 observer C++ guest throw                   none
 *        35 observer C++ native throw (helper)         set_terminate
 *        36 observer (block) ObjC native               NS handler
 *        37 observer of a run loop run inside a dispatch_async block:
 *                   ObjC guest @throw through CFRunLoop into the callout  NS handler
 */
#import <Foundation/Foundation.h>
#import <objc/objc-exception.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <exception>
#include <functional>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include "ehhelper.h"

struct MyErr : std::runtime_error {
    int code;
    MyErr(const char *m, int c) : std::runtime_error(m), code(c) {}
};
struct Other {};

@interface MyException : NSException
@end
@implementation MyException
@end

static int trace_depth;
struct Raii {
    const char *name;
    explicit Raii(const char *n) : name(n) { trace_depth++; }
    ~Raii() { trace_depth--; printf("  guest cleanup: %s\n", name); }
};

__attribute__((noinline)) static void g_throw_myerr(int code) { Raii r("g_throw_myerr"); throw MyErr("myerr", code); }
__attribute__((noinline)) static int g_mid(int code) { Raii r("g_mid"); g_throw_myerr(code); return 1; }
__attribute__((noinline)) static int g_top(int code) { Raii r("g_top"); return g_mid(code) + 1; }

/* ---------------------------------------------------------------- 1 */
static void s1(void)
{
    printf("[1] guest C++ throw\n");
    try { g_top(7); } catch (MyErr &e) { printf("  caught MyErr what=%s code=%d depth=%d\n", e.what(), e.code, trace_depth); }
    try { g_top(8); } catch (std::exception &e) { printf("  caught std::exception what=%s\n", e.what()); }
    try { throw 42; } catch (const char *) { printf("  wrong\n"); } catch (int v) { printf("  caught int %d\n", v); }
    try { throw Other(); } catch (std::exception &) { printf("  wrong\n"); } catch (...) { printf("  caught ... (Other)\n"); }
    try {
        try { throw "inner"; } catch (const char *s) { printf("  inner caught %s\n", s); throw MyErr("from catch", 3); }
    } catch (MyErr &e) { printf("  outer caught %s %d\n", e.what(), e.code); }
    printf("  uncaught_exceptions=%d\n", std::uncaught_exceptions());
}

/* ---------------------------------------------------------------- 2 */
static void s2(void)
{
    printf("[2] native C++ throw\n");
    try { Raii r("s2 frame"); eh_throw_runtime("from helper"); } catch (std::runtime_error &e) { printf("  caught runtime_error: %s\n", e.what()); }
    try { std::vector<int> v(3); printf("  %d\n", v.at(10)); } catch (std::out_of_range &) { printf("  caught out_of_range from vector::at\n"); }
    try { std::function<void()> f; f(); } catch (std::bad_function_call &) { printf("  caught bad_function_call\n"); }
    try { eh_throw_int(99); } catch (long) { printf("  wrong\n"); } catch (int v) { printf("  caught int %d from helper\n", v); }
    try { eh_throw_runtime("as base"); } catch (std::exception &e) { printf("  caught as std::exception: %s\n", e.what()); }
    try { eh_throw_runtime("catch-all"); } catch (...) { printf("  caught ...\n"); }
    try { volatile int n = -1; int *p = new int[n]; printf("  %p\n", (void *)p); } catch (std::bad_alloc &e) { printf("  caught %s from new[]\n", typeid(e).name()); }
}

/* ---------------------------------------------------------------- 3 */
static void s3(void)
{
    printf("[3] native NSException\n");
    @try {
        NSArray *a = @[];
        printf("  %p\n", (__bridge void *)[a objectAtIndex:5]);
    } @catch (NSException *e) {
        printf("  caught %s\n", e.name.UTF8String);
    } @finally {
        printf("  finally 1\n");
    }
    @try {
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        id nilv = nil;
        [d setObject:nilv forKey:@"k"];
    } @catch (NSString *s) {
        printf("  wrong\n");
    } @catch (id e) {
        printf("  caught id %s\n", [[e name] UTF8String]);
    }
    @try {
        @try {
            [NSException raise:NSGenericException format:@"raised %d", 5];
        } @finally {
            printf("  inner finally\n");
        }
    } @catch (NSException *e) {
        printf("  caught %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
    }
}

/* ---------------------------------------------------------------- 4 */
static void s4(void)
{
    printf("[4] guest @throw\n");
    @try {
        @throw [MyException exceptionWithName:@"Mine" reason:@"guest" userInfo:nil];
    } @catch (NSString *s) {
        printf("  wrong\n");
    } @catch (MyException *e) {
        printf("  caught MyException %s %s\n", e.name.UTF8String, e.reason.UTF8String);
    }
    @try {
        @throw [MyException exceptionWithName:@"Mine2" reason:@"as base" userInfo:nil];
    } @catch (NSException *e) {
        printf("  caught as NSException %s class=%s\n", e.name.UTF8String, object_getClassName(e));
    }
    @try {
        @throw @"a string";
    } @catch (NSException *e) {
        printf("  wrong\n");
    } @catch (id e) {
        printf("  caught id %s\n", [[e description] UTF8String]);
    }
    @try {
        try { throw MyErr("c++ in objc", 1); } catch (std::exception &e) { printf("  c++ inside @try: %s\n", e.what()); }
        @throw [NSException exceptionWithName:@"After" reason:@"r" userInfo:nil];
    } @catch (NSException *e) {
        printf("  then caught %s\n", e.name.UTF8String);
    } @finally {
        printf("  finally\n");
    }
}

/* ---------------------------------------------------------------- 5 */
static int cmp_throw(const void *a, const void *b)
{
    Raii r("cmp_throw");
    if (*(const int *)a == 3 || *(const int *)b == 3) throw MyErr("from comparator", 3);
    return *(const int *)a - *(const int *)b;
}
static void cf_apply_throw(const void *value, void *ctx)
{
    (*(int *)ctx)++;
    if ([(__bridge id)value intValue] == 2) @throw [NSException exceptionWithName:@"CFApply" reason:@"value 2" userInfo:nil];
}
static void g_throw_cb(void *arg) { Raii r("g_throw_cb"); throw MyErr((const char *)arg, 5); }
static void g_native_throw_cb(void *arg) { Raii r("g_native_throw_cb"); eh_throw_runtime((const char *)arg); }

static void s5(void)
{
    printf("[5] guest throw in a callback, guest catch\n");
    int v[] = {5, 4, 3, 2, 1};
    try { qsort(v, 5, sizeof v[0], cmp_throw); printf("  no throw\n"); } catch (MyErr &e) { printf("  caught from qsort: %s\n", e.what()); }
    NSArray *arr = @[@1, @2, @3];
    int visited = 0;
    @try {
        CFArrayApplyFunction((__bridge CFArrayRef)arr, CFRangeMake(0, 3), cf_apply_throw, &visited);
    } @catch (NSException *e) {
        printf("  caught from CFArrayApplyFunction: %s visited=%d\n", e.reason.UTF8String, visited);
    }
    try {
        [arr enumerateObjectsUsingBlock:^(id obj, NSUInteger idx, BOOL *stop) {
            Raii r("block");
            if (idx == 1) throw MyErr("from block", (int)idx);
        }];
    } catch (MyErr &e) { printf("  caught from enumerateObjectsUsingBlock: %s %d\n", e.what(), e.code); }
    try { eh_call(g_throw_cb, (void *)"via native cleanup frame"); } catch (MyErr &e) { printf("  caught %s\n", e.what()); }
    try { eh_call(g_native_throw_cb, (void *)"native->guest->native"); } catch (std::exception &e) { printf("  caught %s\n", e.what()); }
    @try {
        [arr enumerateObjectsUsingBlock:^(id obj, NSUInteger idx, BOOL *stop) {
            if (idx == 2) [NSException raise:@"BlockRaise" format:@"idx %lu", (unsigned long)idx];
        }];
    } @catch (NSException *e) { printf("  caught %s: %s\n", e.name.UTF8String, e.reason.UTF8String); }
}

/* ---------------------------------------------------------------- 6 */
static void g_nested(void *arg)
{
    Raii r("g_nested");
    eh_call(g_throw_cb, arg);
}
static void g_int_cb(void *) { throw 17; }
static void g_ok_cb(void *) { printf("  callback returned normally\n"); }

static void s6(void)
{
    printf("[6] guest throw in a callback, native catch\n");
    char buf[128];
    int r = eh_catch(g_throw_cb, (void *)"guest MyErr", buf, sizeof buf);
    printf("  eh_catch=%d %s\n", r, buf);
    r = eh_catch(g_native_throw_cb, (void *)"helper via guest", buf, sizeof buf);
    printf("  eh_catch=%d %s\n", r, buf);
    r = eh_catch(g_int_cb, NULL, buf, sizeof buf);
    printf("  eh_catch=%d %s\n", r, buf);
    r = eh_catch(g_nested, (void *)"nested", buf, sizeof buf);
    printf("  eh_catch=%d %s\n", r, buf);
    r = eh_catch(g_ok_cb, NULL, buf, sizeof buf);
    printf("  eh_catch=%d %s\n", r, buf);
}

/* ---------------------------------------------------------------- 7 */
static void g_rethrow_inner(void)
{
    try { g_top(1); } catch (MyErr &e) { printf("  inner caught, rethrowing\n"); throw; }
}
static void s7(void)
{
    printf("[7] rethrow\n");
    try { g_rethrow_inner(); } catch (MyErr &e) { printf("  outer caught %s %d\n", e.what(), e.code); }
    try { eh_catch_rethrow(g_throw_cb, (void *)"native rethrow"); } catch (MyErr &e) { printf("  caught after native rethrow: %s\n", e.what()); }
    std::exception_ptr p;
    try { throw MyErr("saved", 9); } catch (...) { p = std::current_exception(); }
    try { std::rethrow_exception(p); } catch (MyErr &e) { printf("  rethrow_exception: %s %d\n", e.what(), e.code); }
    @try {
        @try { @throw [NSException exceptionWithName:@"Re" reason:@"objc" userInfo:nil]; }
        @catch (NSException *e) { printf("  objc inner caught, rethrowing\n"); @throw; }
    } @catch (NSException *e) { printf("  objc outer caught %s\n", e.name.UTF8String); }
}

/* ---------------------------------------------------------------- 8 */
@protocol EHPlain
@end

static void s8(void)
{
    printf("[8] Foundation API inside @try\n");
    @try {
        NSData *junk = [@"this is not an archive" dataUsingEncoding:NSUTF8StringEncoding];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        id o = [NSKeyedUnarchiver unarchiveObjectWithData:junk];
#pragma clang diagnostic pop
        printf("  unarchive returned %s\n", o ? "object" : "nil");
    } @catch (NSException *e) {
        printf("  caught from NSKeyedUnarchiver: %s\n", e.name.UTF8String);
    }
    @try {
        NSXPCInterface *i = [NSXPCInterface interfaceWithProtocol:@protocol(EHPlain)];
        printf("  interface ok\n");
        [i setClasses:[NSSet setWithObject:[NSString class]] forSelector:@selector(description) argumentIndex:0 ofReply:NO];
        printf("  no throw\n");
    } @catch (NSException *e) {
        printf("  caught from NSXPCInterface: %s\n", e.name.UTF8String);
    }
    @try {
        NSData *d = [NSJSONSerialization dataWithJSONObject:@[[NSObject new]] options:0 error:nil];
        printf("  json %lu\n", (unsigned long)d.length);
    } @catch (NSException *e) {
        printf("  caught from NSJSONSerialization: %s\n", e.name.UTF8String);
    }
    @try {
        printf("  %s\n", [@"abc" substringFromIndex:10].UTF8String);
    } @catch (NSException *e) {
        printf("  caught from substringFromIndex: %s\n", e.name.UTF8String);
    }
}

/* ---------------------------------------------------------------- 9 */
static jmp_buf jb9;
static sigjmp_buf sjb9;
static int lj_calls;
static int cmp_longjmp(const void *a, const void *b)
{
    if (++lj_calls == 3) longjmp(jb9, 33);
    return *(const int *)a - *(const int *)b;
}
static void g_longjmp_cb(void *) { longjmp(jb9, 44); }
static void on_usr1(int) { siglongjmp(sjb9, 55); }
static volatile sig_atomic_t spin;
static void on_alarm(int) { siglongjmp(sjb9, 66); }

static void s9(void)
{
    printf("[9] longjmp across native frames\n");
    int v[] = {5, 4, 3, 2, 1};
    int r = setjmp(jb9);
    if (!r) { qsort(v, 5, sizeof v[0], cmp_longjmp); printf("  no longjmp\n"); }
    else printf("  longjmp out of qsort comparator: %d (calls %d)\n", r, lj_calls);
    r = setjmp(jb9);
    if (!r) { eh_call_plain(g_longjmp_cb, NULL); printf("  no longjmp\n"); }
    else printf("  longjmp out of helper callback: %d\n", r);
    signal(SIGUSR1, on_usr1);
    r = sigsetjmp(sjb9, 1);
    if (!r) { raise(SIGUSR1); printf("  no longjmp\n"); }
    else printf("  siglongjmp out of SIGUSR1 handler: %d\n", r);
    sigset_t m;
    sigprocmask(SIG_BLOCK, NULL, &m);
    printf("  SIGUSR1 blocked after siglongjmp: %d\n", sigismember(&m, SIGUSR1));
    signal(SIGALRM, on_alarm);
    r = sigsetjmp(sjb9, 1);
    if (!r) {
        struct itimerval it = {{0, 0}, {0, 20000}};
        setitimer(ITIMER_REAL, &it, NULL);
        for (unsigned long k = 0;; k++) spin = (sig_atomic_t)k;
    } else printf("  siglongjmp out of SIGALRM during guest loop: %d\n", r);
    /* the bridge still works afterwards */
    int w[] = {3, 1, 2};
    qsort(w, 3, sizeof w[0], [](const void *a, const void *b) { return *(const int *)a - *(const int *)b; });
    char buf[64];
    int c = eh_catch(g_throw_cb, (void *)"after longjmp", buf, sizeof buf);
    printf("  afterwards: qsort %d %d %d, eh_catch=%d %s\n", w[0], w[1], w[2], c, buf);
}

/* ---------------------------------------------------------------- 10 */
static void *thread10(void *)
{
    /* copy inside the handlers: the exception (and what() with it) is freed when the catch ends */
    char out[256], buf[64], a[64] = "?", b[64] = "?";
    try { g_top(10); } catch (MyErr &e) { snprintf(a, sizeof a, "%s", e.what()); }
    @try { [@[] objectAtIndex:1]; } @catch (NSException *e) { snprintf(b, sizeof b, "%s", e.name.UTF8String); }
    eh_catch(g_throw_cb, (void *)"thread", buf, sizeof buf);
    snprintf(out, sizeof out, "%s %s %s", a, b, buf);
    return strdup(out);
}
static void s10(void)
{
    printf("[10] second thread\n");
    pthread_t th;
    void *res;
    pthread_create(&th, NULL, thread10, NULL);
    pthread_join(th, &res);
    printf("  thread: %s\n", (char *)res);
    free(res);
}

/* ---------------------------------------------------------------- 11 */
struct CatchingDtor {
    ~CatchingDtor()
    {
        try { eh_throw_runtime("inside cleanup"); } catch (std::exception &e) { printf("  cleanup caught %s\n", e.what()); }
    }
};
__attribute__((noinline)) static void g_dtor_throw(void) { CatchingDtor d; g_top(11); }
static void s11(void)
{
    printf("[11] exception inside a cleanup\n");
    try { g_dtor_throw(); } catch (MyErr &e) { printf("  outer caught %s %d\n", e.what(), e.code); }
}

/* ---------------------------------------------------------------- fatal */
static void s20(void) { printf("[20] uncaught C++\n"); fflush(stdout); g_top(20); }
static void s21(void) { printf("[21] uncaught NSException\n"); fflush(stdout); [@[] objectAtIndex:3]; }
static void s22(void)
{
    printf("[22] C++ out of dispatch_sync\n");
    fflush(stdout);
    dispatch_queue_t q = dispatch_queue_create("eh", NULL);
    dispatch_sync(q, ^{ throw MyErr("from dispatch", 22); });
    printf("  not reached\n");
}

/* ---------------------------------------------------------------- fatal: uncaught at a callout */
static void uncaught_ns(NSException *e)
{
    NSString *r = e.reason;
    NSRange at = [r rangeOfString:@" 0x"];   /* instance addresses differ between runs */
    if (at.location != NSNotFound) r = [r substringToIndex:at.location];
    printf("  uncaught handler: %s: %s\n", e.name.UTF8String, r.UTF8String);
    fflush(stdout);
}
static void on_terminate(void)
{
    std::exception_ptr p = std::current_exception();
    try {
        if (p) std::rethrow_exception(p);
        printf("  terminate handler: no exception\n");
    } catch (MyErr &e) {
        printf("  terminate handler: MyErr %s %d\n", e.what(), e.code);
    } catch (std::exception &e) {
        printf("  terminate handler: std::exception %s\n", e.what());
    } catch (...) {
        printf("  terminate handler: other\n");
    }
    fflush(stdout);
    abort();
}
enum { H_NONE, H_NS, H_TERM };
static void set_handler(int h)
{
    if (h == H_NS) NSSetUncaughtExceptionHandler(uncaught_ns);
    if (h == H_TERM) std::set_terminate(on_terminate);
}
__attribute__((noinline)) static void t_objc_native(void)
{
    NSMutableDictionary *d = [NSMutableDictionary new];
    id key = nil;
    if (getpid() == 0) key = @"never";
    d[key] = @1;
}
__attribute__((noinline)) static void t_objc_guest(void)
{
    @throw [NSException exceptionWithName:@"EhCallout" reason:@"guest throw" userInfo:nil];
}
__attribute__((noinline)) static void t_objc_dnr(void)
{
    id o = [NSObject new];
    [o performSelector:@selector(compiledRankingFactsConfiguration)];
}
__attribute__((noinline)) static void t_cxx_guest(void) { Raii r("t_cxx_guest"); throw MyErr("from callout", 7); }
__attribute__((noinline)) static void t_cxx_native(void) { Raii r("t_cxx_native"); eh_throw_runtime("native from callout"); }

/* the main thread waits; natively the process is gone long before */
static void wait_for_worker(int s)
{
    sleep(5);
    printf("  [%d] survived\n", s);
}
static void async_throw(int s, void (*fn)(void))
{
    dispatch_queue_t q = dispatch_queue_create("eh.callout", NULL);
    dispatch_async(q, ^{ fn(); printf("  not reached\n"); });
    wait_for_worker(s);
}
static void observer_cb(CFRunLoopObserverRef, CFRunLoopActivity, void *info)
{
    printf("  observer\n");
    ((void (*)(void))info)();
    printf("  not reached\n");
}
static void timer_cb(CFRunLoopTimerRef, void *) {}
/* a run loop mode needs a source or a timer, or CFRunLoopRunInMode returns
 * before the entry observers run */
static void run_main_loop(int s)
{
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 100, 0, 0, 0, timer_cb, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2, false);
    printf("  [%d] survived\n", s);
}
static void observer_throw(int s, void (*fn)(void))
{
    CFRunLoopObserverContext ctx = {0, (void *)fn, NULL, NULL, NULL};
    CFRunLoopObserverRef o = CFRunLoopObserverCreate(NULL, kCFRunLoopEntry, false, 0, observer_cb, &ctx);
    CFRunLoopAddObserver(CFRunLoopGetCurrent(), o, kCFRunLoopDefaultMode);
    run_main_loop(s);
}
static void observer_block_throw(int s, void (*fn)(void))
{
    CFRunLoopObserverRef o = CFRunLoopObserverCreateWithHandler(NULL, kCFRunLoopEntry, false, 0,
        ^(CFRunLoopObserverRef, CFRunLoopActivity) { printf("  observer block\n"); fn(); printf("  not reached\n"); });
    CFRunLoopAddObserver(CFRunLoopGetCurrent(), o, kCFRunLoopDefaultMode);
    run_main_loop(s);
}
static bool callout_section(int s)
{
    static const char *const what[] = {
        "async ObjC native, NS handler", "async ObjC native, no handler",
        "async ObjC guest, NS handler", "async_f ObjC native-only worker, NS handler",
        "async doesNotRecognizeSelector, NS handler", "async C++ guest, no handler",
        "async C++ guest, terminate handler", "async C++ native, NS handler",
        "sync ObjC guest, NS handler", "observer ObjC guest, NS handler",
        "observer ObjC native, no handler", "observer C++ guest, no handler",
        "observer C++ native, terminate handler", "block observer ObjC native, NS handler",
        "observer in a dispatch_async block ObjC guest, NS handler",
    };
    if (s < 23 || s > 37) return false;
    printf("[%d] uncaught at a callout: %s\n", s, what[s - 23]);
    fflush(stdout);
    switch (s) {
    case 23: set_handler(H_NS); async_throw(s, t_objc_native); break;
    case 24: set_handler(H_NONE); async_throw(s, t_objc_native); break;
    case 25: set_handler(H_NS); async_throw(s, t_objc_guest); break;
    case 26: {
        set_handler(H_NS);
        NSException *e = [NSException exceptionWithName:@"EhCallout" reason:@"native-only frames" userInfo:nil];
        dispatch_async_f(dispatch_queue_create("eh.callout", NULL), (__bridge_retained void *)e,
                         (dispatch_function_t)objc_exception_throw);
        wait_for_worker(s);
        break;
    }
    case 27: set_handler(H_NS); async_throw(s, t_objc_dnr); break;
    case 28: set_handler(H_NONE); async_throw(s, t_cxx_guest); break;
    case 29: set_handler(H_TERM); async_throw(s, t_cxx_guest); break;
    case 30: set_handler(H_NS); async_throw(s, t_cxx_native); break;
    case 31:
        set_handler(H_NS);
        dispatch_sync(dispatch_queue_create("eh.callout", NULL), ^{ t_objc_guest(); });
        printf("  [%d] survived\n", s);
        break;
    case 32: set_handler(H_NS); observer_throw(s, t_objc_guest); break;
    case 33: set_handler(H_NONE); observer_throw(s, t_objc_native); break;
    case 34: set_handler(H_NONE); observer_throw(s, t_cxx_guest); break;
    case 35: set_handler(H_TERM); observer_throw(s, t_cxx_native); break;
    case 36: set_handler(H_NS); observer_block_throw(s, t_objc_native); break;
    case 37:
        set_handler(H_NS);
        dispatch_async(dispatch_queue_create("eh.callout", NULL), ^{ observer_throw(s, t_objc_guest); });
        wait_for_worker(s);
        break;
    }
    return true;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    static void (*const sec[])(void) = {
        NULL, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11,
    };
    int n = (int)(sizeof sec / sizeof *sec);
    if (argc > 1) {
        int k = atoi(argv[1]);
        if (k == 20) s20();
        else if (k == 21) s21();
        else if (k == 22) s22();
        else if (callout_section(k)) {}
        else if (k > 0 && k < n) sec[k]();
        else { fprintf(stderr, "no section %d\n", k); return 2; }
    } else {
        for (int k = 1; k < n; k++) sec[k]();
    }
    printf("done\n");
    return 0;
}
