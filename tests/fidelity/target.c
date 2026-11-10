/* Fixture for tests/fidelity/run.sh: sets up process state an outside observer
 * (tools/fidelity/observe) can read, prints "ready PID", then waits until it is
 * killed (at most SECONDS). Mode a sets everything below; mode b differs
 * deliberately in six places. Four are visible from outside:
 *   signal    SIGUSR1 ignored (a) / left at its default (b)
 *   resource  nice 5 (a) / 7 (b)
 *   fd        a unix stream socket (a) / none (b)
 *   pgrp      own process group by setpgid(0, 0) (a) / the parent's (b)
 * and two are not (xnu has no interface for another process's rlimits or a
 * thread's signal mask), so the observer must record them as unreadable on
 * both sides rather than invent a value:
 *   rlimit    RLIMIT_CORE soft 1 MiB (a) / 2 MiB (b)
 *   sigmask   SIGUSR2 blocked (a) / not (b)
 * Both modes: SIGTERM and SIGUSR2 caught, SIGPIPE ignored, SIGINT and SIGQUIT
 * back to their defaults (a background job of a script inherits them ignored), RLIMIT_FSIZE
 * 64 MiB soft, darwinbg, main thread QoS utility, a regular file opened
 * read-write, a pipe.
 *   target a|b SECONDS DATAFILE
 * Built for arm64 (native reference) and arm64e (the guest). */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void on_term(int s) { (void)s; _exit(0); }
static void on_usr2(int s) { (void)s; }

static void die(const char *what) {
    fprintf(stderr, "target: %s: %s\n", what, strerror(errno));
    exit(2);
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "a") && strcmp(argv[1], "b"))) {
        fprintf(stderr, "usage: target a|b SECONDS DATAFILE\n");
        return 2;
    }
    int a = argv[1][0] == 'a';
    int secs = atoi(argv[2]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL)) die("sigaction SIGTERM");
    sa.sa_handler = on_usr2;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR2, &sa, NULL)) die("sigaction SIGUSR2");
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) die("signal SIGPIPE");
    if (signal(SIGINT, SIG_DFL) == SIG_ERR || signal(SIGQUIT, SIG_DFL) == SIG_ERR) die("signal SIGINT/SIGQUIT");
    if (a && signal(SIGUSR1, SIG_IGN) == SIG_ERR) die("signal SIGUSR1");
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    if (a && sigprocmask(SIG_BLOCK, &m, NULL)) die("sigprocmask");

    struct rlimit rl;
    if (getrlimit(RLIMIT_CORE, &rl)) die("getrlimit core");
    rl.rlim_cur = (a ? 1 : 2) << 20;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_cur > rl.rlim_max) rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_CORE, &rl)) die("setrlimit core");
    if (getrlimit(RLIMIT_FSIZE, &rl)) die("getrlimit fsize");
    rl.rlim_cur = 64 << 20;
    if (setrlimit(RLIMIT_FSIZE, &rl)) die("setrlimit fsize");

    if (setpriority(PRIO_PROCESS, 0, a ? 5 : 7)) die("setpriority nice");
    if (setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG)) die("setpriority darwinbg");
    if (pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0)) die("qos");

    if (open(argv[3], O_RDWR | O_CREAT, 0644) < 0) die("open");
    int p[2];
    if (pipe(p)) die("pipe");
    if (a && socket(AF_UNIX, SOCK_STREAM, 0) < 0) die("socket");
    if (a && setpgid(0, 0)) die("setpgid");

    printf("ready %d\n", (int)getpid());
    fflush(stdout);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}
