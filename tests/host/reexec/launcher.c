/* Stand-in for launchd (tests/host/reexec.sh): spawn the proxy start-
 * suspended, as launchd spawns xpcproxy for a job, write its pid to
 * descriptor 3 (the watcher's pipe), and wait for it.
 *   launcher PROXY ARGS...   (arm64e: under hao a translated parent, as launchd is) */
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv)
{
    if (argc < 2) return 64;
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_START_SUSPENDED);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, 3);
    pid_t pid;
    int e = posix_spawn(&pid, argv[1], &fa, &a, argv + 1, environ);
    if (e) { fprintf(stderr, "launcher: spawn %s: %d\n", argv[1], e); return 2; }
    dprintf(3, "%d\n", pid);
    close(3);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
