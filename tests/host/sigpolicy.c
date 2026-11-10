/* Native-only signal-return policy oracle. No hao, GUI, target process or
 * installed profile is modified. Each invocation restricts only itself.
 * raw: invalid frame, so an allowed syscall returns EFAULT, a denied one
 * EPERM. handler: queue a signal before sandboxing, then return normally
 * from its handler; exit 81 identifies the post-sigreturn trap with EPERM.
 * The optional allow clause is a control in this disposable test process.
 */
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <unistd.h>

_Static_assert(SYS_sigreturn == 184, "update the raw syscall below");
#if defined(__x86_64__)
__attribute__((naked)) static long raw_sigreturn(void *ctx, int flavor, uintptr_t token)
{
    __asm__("movl $0x20000b8, %eax\n syscall\n retq");
}
#elif defined(__aarch64__)
__attribute__((naked)) static long raw_sigreturn(void *ctx, int flavor, uintptr_t token)
{
    __asm__("mov x16, #184\n svc #0x80\n ret");
}
#else
#error native macOS arm64 or x86_64 required
#endif

static volatile sig_atomic_t seen;
static void handler(int sig) { (void)sig; seen = 1; }
static void failed_return(int sig) { (void)sig; _exit(seen && errno == EPERM ? 81 : 82); }

static char *profile_text(const char *path, int allow)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(98); }
    if (fseek(f, 0, SEEK_END)) exit(98);
    long n = ftell(f);
    if (n < 0 || n > 1024 * 1024 || fseek(f, 0, SEEK_SET)) exit(98);
    char *s = calloc(1, (size_t)n + 128);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) exit(98);
    fclose(f);
    if (allow) strcat(s, "\n(allow syscall-unix (syscall-number SYS_sigreturn))\n");
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || (strcmp(argv[1], "raw") && strcmp(argv[1], "handler")) ||
        (argc == 4 && strcmp(argv[3], "allow"))) {
        fprintf(stderr, "usage: sigpolicy raw|handler none|deny|PROFILE [allow]\n");
        return 98;
    }
    int translated = 0;
    size_t n = sizeof translated;
    if (!sysctlbyname("sysctl.proc_translated", &translated, &n, NULL, 0) && translated) {
        fprintf(stderr, "SKIP: real sigreturn failure is not a reliable oracle under Rosetta\n");
        return 77;
    }
    const char *profile = NULL;
    if (!strcmp(argv[2], "deny")) {
        if (argc == 4) return 98; /* an explicit deny is not overridden by an allow */
        profile = "(version 1)(allow default)(deny syscall-unix (syscall-number SYS_sigreturn))";
    } else if (strcmp(argv[2], "none")) {
        profile = profile_text(argv[2], argc == 4);
    } else if (argc == 4) return 98;

    void *lib = dlopen("/usr/lib/libsandbox.1.dylib", RTLD_NOW);
    int (*init)(const char *, uint64_t, const char *const *, char **) =
        lib ? dlsym(lib, "sandbox_init_with_parameters") : NULL;
    if (!init) { fprintf(stderr, "sandbox_init_with_parameters unavailable\n"); return 98; }
    const char *home = getenv("HOME");
    const char *params[] = {
        "HOME", home ? home : "/private/tmp",
        "DARWIN_CACHE_DIR", "/private/tmp/hao-sigpolicy",
        "TMPDIR", "/private/tmp/hao-sigpolicy",
        "APPLICATION_BUNDLE", "/private/tmp/hao-sigpolicy.bundle", NULL
    };
    sigset_t blocked;
    int real_handler = !strcmp(argv[1], "handler");
    if (real_handler) {
        struct sigaction sa = {0};
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = handler;
        if (sigaction(SIGUSR1, &sa, NULL)) return 98;
        sa.sa_handler = failed_return;
        if (sigaction(SIGTRAP, &sa, NULL) || sigaction(SIGILL, &sa, NULL)) return 98;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGUSR1);
        /* Queue before applying policy: a deny of kill must not masquerade
         * as a failed signal return. Only sigprocmask runs afterward. */
        if (sigprocmask(SIG_BLOCK, &blocked, NULL) || raise(SIGUSR1)) return 98;
    }
    if (profile) {
        char *err = NULL;
        if (init(profile, 0, params, &err)) {
            fprintf(stderr, "sandbox init: %s\n", err ? err : "unknown error");
            return 99;
        }
    }
    /* No I/O or cleanup after sandboxing: return only an exit status. */
    if (!real_handler) _exit((int)raw_sigreturn(NULL, 30, 0)); /* UC_TRAD */
    errno = 0;
    if (sigprocmask(SIG_UNBLOCK, &blocked, NULL)) _exit(84);
    _exit(seen ? 0 : 83);
}
