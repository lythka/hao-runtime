/* The sigreturn policy under hao (tests/host/sigpolicy.sh, HAO=...): this
 * program applies a sandbox profile to itself (sandbox_init_with_parameters,
 * as 27.0 WindowManager does with com.apple.WindowManager), then calls a
 * guest function N times through native code that reads the pointer from
 * memory (sigpolicyhelper.c). Natively (arm64) no signal is taken. Under
 * hao each call is an NX fault that on_fault redirects; its handler must
 * leave without a system call the profile denies. Exit status only after
 * sandboxing: 0 all calls right, 2 a wrong result, 99 the profile did not
 * apply, 98 usage. fault: then a native write to address 16, which no
 * handler takes (under hao: on_fault gives it up), so the process dies of
 * SIGSEGV as natively, whatever the profile denies. throw: then an
 * NSException that nothing catches out of a dispatch_sync_f callout (made,
 * with its queue, before sandboxing): natively on arm64 the callout's
 * noexcept personality ends it in "Terminating app due to uncaught
 * exception" and SIGABRT; under hao on x86_64 libunwind's call of that
 * personality faults (unwind.c hao_eh_fault) and on_fault's edited context
 * must be resumed without __sigreturn.
 *   sigpolicyguest none|PROFILE [N] [kill] [fault|throw] */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define RAW(f) ((long (*)(long))ptrauth_strip((void *)(f), ptrauth_key_function_pointer))
#else
#define RAW(f) (f)
#endif

long sph_call_slot(long (*const *slot)(long), long v);
void sph_null_write(void);
void objc_exception_throw(id);
static id exc;
static void thrower(void *ctx) { (void)ctx; objc_exception_throw(exc); }
static long twice(long v) { return 2 * v + 1; }

static char *profile_text(const char *path, int kill)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(98); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 0 || n > 1024 * 1024) exit(98);
    fseek(f, 0, SEEK_SET);
    char *s = calloc(1, (size_t)n + 256);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) exit(98);
    fclose(f);
    /* kill: a denied signal-return or thread-mask syscall kills the process
     * instead of failing with EPERM, so any use of one shows */
    if (kill) strcat(s, "\n(deny syscall-unix (with send-signal SIGKILL) (syscall-number SYS_sigreturn SYS___pthread_sigmask))\n");
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 5) { fprintf(stderr, "usage: sigpolicyguest none|PROFILE [N] [kill] [fault]\n"); return 98; }
    long n = argc > 2 ? atol(argv[2]) : 200;
    int kill = 0, fault = 0, thr = 0;
    for (int k = 3; k < argc; k++) {
        if (!strcmp(argv[k], "kill")) kill = 1;
        else if (!strcmp(argv[k], "fault")) fault = 1;
        else if (!strcmp(argv[k], "throw")) thr = 1;
        else return 98;
    }
    dispatch_queue_t q = NULL;
    if (thr) {
        id c = (id)objc_getClass("NSException");
        if (!c) return 98;
        id e = ((id (*)(id, SEL))objc_msgSend)(c, sel_registerName("alloc"));
        exc = ((id (*)(id, SEL, CFStringRef, CFStringRef, id))objc_msgSend)(
            e, sel_registerName("initWithName:reason:userInfo:"), CFSTR("SigpolicyThrow"), CFSTR("through a dispatch callout"), NULL);
        q = dispatch_queue_create("sigpolicy.callout", NULL);
        if (!exc || !q) return 98;
    }
    long (**slot)(long) = malloc(sizeof *slot);
    *slot = RAW(twice);
    if (strcmp(argv[1], "none")) {
        char *profile = profile_text(argv[1], kill);
        void *lib = dlopen("/usr/lib/libsandbox.1.dylib", RTLD_NOW);
        int (*init)(const char *, uint64_t, const char *const *, char **) =
            lib ? (int (*)(const char *, uint64_t, const char *const *, char **))dlsym(lib, "sandbox_init_with_parameters") : NULL;
        if (!init) return 98;
        const char *home = getenv("HOME");
        const char *params[] = {"HOME", home ? home : "/private/tmp", "DARWIN_CACHE_DIR", "/private/tmp/hao-sigpolicy",
                                "TMPDIR", "/private/tmp/hao-sigpolicy", "APPLICATION_BUNDLE", "/private/tmp/hao-sigpolicy.bundle", NULL};
        char *err = NULL;
        if (init(profile, 0, params, &err)) { fprintf(stderr, "sandbox init: %s\n", err ? err : "?"); return 99; }
    }
    /* no I/O after sandboxing */
    for (long k = 0; k < n; k++)
        if (sph_call_slot(slot, k) != 2 * k + 1) _exit(2);
    if (fault) sph_null_write();
    if (thr) dispatch_sync_f(q, NULL, thrower);
    _exit(0);
}
