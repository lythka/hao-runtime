/* Stand-in for LaunchServices and launchd's job report (tests/host/reexec.sh):
 * spawn LAUNCHER (the stand-in for launchd, which spawns the proxy start-
 * suspended and writes its pid to descriptor 3), watch that pid with
 * EVFILT_PROC NOTE_EXEC as launchd's DISPATCH_SOURCE_TYPE_PROC does, resume
 * it, and on its first exec send SIGCONT, as launchservicesd starts a
 * stopped app once launchd and RunningBoard report the job running.
 *   watcher DELAY_US x86|arm LAUNCHER ARGS...
 * Prints one line per exec (the image path, and whether the exec's
 * environment, KERN_PROCARGS2, holds OBJC_DISABLE_TAG_OBFUSCATION) and
 *   RESULT ran|stopped execs=N
 * Native code (arm64): it is the observer, not a guest. */
#include <fcntl.h>
#include <libproc.h>
#include <mach/machine.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

/* the exec's environment, as the kernel recorded it, names the switch */
static int tag_in_exec_env(pid_t pid)
{
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    static char buf[1 << 20];
    size_t n = sizeof buf;
    if (sysctl(mib, 3, buf, &n, NULL, 0) != 0 || n < sizeof(int)) return -1;
    int argc;
    memcpy(&argc, buf, sizeof argc);
    char *p = buf + sizeof argc, *end = buf + n;
    p += strnlen(p, (size_t)(end - p));                 /* exec path */
    while (p < end && !*p) p++;
    for (int k = 0; k < argc && p < end; k++) p += strnlen(p, (size_t)(end - p)) + 1;
    for (; p < end && *p; p += strlen(p) + 1)
        if (!strncmp(p, "OBJC_DISABLE_TAG_OBFUSCATION=", 29)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) return 64;
    useconds_t delay = (useconds_t)atoi(argv[1]);
    cpu_type_t pref = argv[2][0] == 'a' ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
    int p[2];
    if (pipe(p)) return 2;
    for (int k = 0; k < 2; k++) { int h = fcntl(p[k], F_DUPFD_CLOEXEC, 10); close(p[k]); p[k] = h; }   /* not 3 */
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    size_t got;
    posix_spawnattr_setbinpref_np(&a, 1, &pref, &got);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 3);
    pid_t lpid;
    if (posix_spawn(&lpid, argv[3], &fa, &a, argv + 3, environ)) { perror("watcher: spawn"); return 2; }
    close(p[1]);
    char line[32] = "";
    ssize_t r = read(p[0], line, sizeof line - 1);
    pid_t pid = r > 0 ? atoi(line) : 0;
    if (pid <= 0) { fprintf(stderr, "watcher: no pid from the launcher\n"); waitpid(lpid, NULL, 0); return 2; }
    int kq = kqueue();
    struct kevent ev;
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD, NOTE_EXEC | NOTE_EXIT, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL)) { perror("watcher: kevent"); return 2; }
    kill(pid, SIGCONT);                    /* the proxy was spawned suspended: no exec event is missed */
    int execs = 0, sent = 0, ran = 0;
    struct timespec to = {atoi(getenv("WATCH_T") ? getenv("WATCH_T") : "5"), 0};
    while (!ran && kevent(kq, NULL, 0, &ev, 1, &to) > 0) {
        if (ev.fflags & NOTE_EXEC) {
            execs++;
            char path[PROC_PIDPATHINFO_MAXSIZE] = "?";
            proc_pidpath(pid, path, sizeof path);
            printf("exec %d: %s tagenv=%d\n", execs, path, tag_in_exec_env(pid));
            if (!sent) { if (delay) usleep(delay); kill(pid, SIGCONT); sent = 1; }
        }
        if (ev.fflags & NOTE_EXIT) ran = 1;
    }
    struct proc_bsdinfo bi;
    int stopped = !ran && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == sizeof bi && bi.pbi_status == SSTOP;
    if (!ran) kill(pid, SIGKILL);
    int st = 0;
    waitpid(lpid, &st, 0);
    printf("RESULT %s execs=%d\n", ran ? "ran" : stopped ? "stopped" : "timeout", execs);
    return ran ? 0 : 1;
}
