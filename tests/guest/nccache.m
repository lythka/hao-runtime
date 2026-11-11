/* Guest of tests/guest/nccache.sh: repeated native calls whose route changes
 * after every thread has called the target many times (runtime.c's
 * native-target cache must not keep the old one).
 *   - the IMP of a native BOOL method, called bare: with a nil receiver no
 *     method shows its type, so it stays untyped; then the guest asks for the
 *     method's IMP (method_getImplementation), which types it, and every
 *     thread's later calls must get the result extended;
 *   - snprintf through dlsym, repeatedly: an interposer's trap each time. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

IMP nc_odd_imp(void);
id nc_new(void);
typedef BOOL (*odd_fn)(id, SEL, int);

#define NT 4
#define WARM 64
static odd_fn imp;
static SEL sel;
static id obj;
static volatile long sink;
static atomic_int warmed, phase;
static int wrong[NT + 1];

static int typed_calls(void)
{
    int bad = 0;
    for (int k = 0; k < 1000; k++) {
        volatile int r = imp(obj, sel, k);   /* the whole register: no range the compiler may assume */
        if (r != (k & 1)) bad++;
    }
    return bad;
}

static void *worker(void *a)
{
    int id = (int)(long)a;
    for (int k = 0; k < WARM; k++) sink += imp(nil, sel, k);   /* untyped: the result is not used */
    atomic_fetch_add(&warmed, 1);
    while (!atomic_load(&phase)) usleep(100);
    wrong[id] = typed_calls();
    return NULL;
}

int main(void)
{
    imp = (odd_fn)nc_odd_imp();
    sel = @selector(odd:);
    obj = nc_new();
    for (int k = 0; k < WARM; k++) sink += imp(nil, sel, k);
    pthread_t th[NT];
    for (long k = 0; k < NT; k++) pthread_create(&th[k], NULL, worker, (void *)k);
    while (atomic_load(&warmed) < NT) usleep(100);
    Method m = class_getInstanceMethod(object_getClass(obj), sel);
    printf("method_getImplementation is the IMP: %d\n", method_getImplementation(m) == (IMP)imp);
    atomic_store(&phase, 1);
    wrong[NT] = typed_calls();
    for (int k = 0; k < NT; k++) pthread_join(th[k], NULL);
    for (int k = 0; k <= NT; k++) printf("thread %d: %d of 1000 results wrong after typing\n", k, wrong[k]);

    int (*dsn)(char *, unsigned long, const char *, ...) =
        (int (*)(char *, unsigned long, const char *, ...))dlsym(RTLD_DEFAULT, "snprintf");
    char b[64];
    for (int k = 0; k < 3; k++) {
        dsn(b, sizeof b, "%d-%s-%.1f", k, "z", 1.5 * k);
        printf("dlsym snprintf %d: %s\n", k, b);
    }
    for (int k = 0; k < 3; k++) printf("strlen %d: %zu\n", k, strlen(b + k));

    return 0;
}
