/* spawn0 PROGRAM: run PROGRAM with an empty argv (argc 0), print how it ended. */
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
extern char **environ;
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char *none[] = {NULL};
    pid_t pid;
    if (posix_spawn(&pid, argv[1], NULL, NULL, none, environ) != 0) { perror("posix_spawn"); return 2; }
    int st;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) printf("signal %d\n", WTERMSIG(st));
    else printf("exit %d\n", WEXITSTATUS(st));
    return 0;
}
