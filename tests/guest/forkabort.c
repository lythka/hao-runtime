// tests/guest/forkabort.sh: a fork child that aborts after a few native calls.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
    pid_t p = fork();
    if (p == 0) {
        (void)getpid();
        (void)getppid();
        abort();
    }
    int st = 0;
    waitpid(p, &st, 0);
    printf("child signal %d\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return 0;
}
