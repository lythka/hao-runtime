#include <stdio.h>
#include <unistd.h>

int main(void)
{
#if defined(__arm64__)
    printf("guest-arm=11 pid=%d\n", getpid());
    return 11;
#else
    printf("native-intel=22 pid=%d\n", getpid());
    return 22;
#endif
}
