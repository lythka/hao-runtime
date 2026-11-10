/* Stand-in for xpcproxy (tests/host/reexec.sh): links libobjc as xpcproxy
 * does (so its hao stub has no __hao_noreexec), then runs the target in
 * place by one of the exec routes:
 *   proxy setexec|spawnp|execve|execvp|svc TARGET ARGS...
 * setexec/spawnp: posix_spawn/posix_spawnp with POSIX_SPAWN_SETEXEC |
 * POSIX_SPAWN_START_SUSPENDED (xpcproxy for a WaitForDebugger job: every
 * LaunchServices launch); execve/execvp: libSystem; svc: a raw execve trap. */
#include <objc/runtime.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
extern char **environ;
int main(int argc, char **argv)
{
    if (argc < 3) return 64;
    if (!objc_getClass("NSObject")) return 3;
    const char *m = argv[1], *t = argv[2];
    char **av = argv + 2;
    if (!strcmp(m, "setexec") || !strcmp(m, "spawnp")) {
        posix_spawnattr_t a;
        posix_spawnattr_init(&a);
        posix_spawnattr_setflags(&a, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_START_SUSPENDED);
        pid_t p;
        int e = m[0] == 's' && m[1] == 'e' ? posix_spawn(&p, t, NULL, &a, av, environ) : posix_spawnp(&p, t, NULL, &a, av, environ);
        fprintf(stderr, "proxy: %s %s: %d\n", m, t, e);
        return 2;
    }
    if (!strcmp(m, "execve")) execve(t, av, environ);
    else if (!strcmp(m, "execvp")) execvp(t, av);
    else if (!strcmp(m, "svc")) {
        register long x0 __asm__("x0") = (long)t;
        register long x1 __asm__("x1") = (long)av;
        register long x2 __asm__("x2") = (long)environ;
        register long x16 __asm__("x16") = SYS_execve;
        __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    }
    perror("proxy: exec");
    return 2;
}
