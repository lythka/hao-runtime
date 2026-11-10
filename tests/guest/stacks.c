/* Guest stack sizes: deep recursion on the main thread (the program is
 * linked with -Wl,-stack_size,0x4000000: LC_MAIN stacksize 64 MB) and on a
 * thread made with pthread_attr_setstacksize(64 MB). Each recursion uses
 * about 40 MB, more than hao's old fixed 8 MB guest stack. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int deep(int n)
{
    volatile char buf[4000];
    memset((char *)buf, n & 0xff, sizeof buf);
    if (!n) return buf[7];
    return deep(n - 1) + (buf[n % sizeof buf] & 1);
}

static void *thr(void *a) { return (void *)(long)deep((int)(long)a); }

int main(void)
{
    printf("main: %d\n", deep(10000));
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 << 20);
    pthread_t t;
    void *r;
    pthread_create(&t, &at, thr, (void *)10000L);
    pthread_join(t, &r);
    printf("thread: %ld\n", (long)r);
    return 0;
}
