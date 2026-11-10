#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static __thread int tls_counter = 41;
static jmp_buf jb;
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void *worker(void *arg) { tls_counter += (int)(long)arg; return (void *)(long)tls_counter; }
static int depth(int n) { if (n == 0) longjmp(jb, 42); return depth(n - 1) + 1; }
struct S { double x, y; };
static struct S mid(struct S a, struct S b) { struct S r = {(a.x + b.x) / 2, (a.y + b.y) / 2}; return r; }
__attribute__((constructor)) static void ctor(void) { printf("constructor ran\n"); }
static void bye(void) { printf("atexit handler ran\n"); }

int main(void) {
    int v[] = {5, 3, 9, 1, 7, 2, 8};
    qsort(v, 7, sizeof v[0], cmp);
    printf("qsort:");
    for (int i = 0; i < 7; i++) printf(" %d", v[i]);
    printf("\n");
    char buf[64];
    snprintf(buf, sizeof buf, "%s-%d-%.2f-%lx", "fmt", -12, 2.5, 0xdeadbeefUL);
    printf("snprintf: %s len=%zu\n", buf, strlen(buf));
    int r = setjmp(jb);
    if (!r) depth(10); else printf("longjmp returned %d\n", r);
    printf("math: sqrt=%.6f sin=%.6f pow=%.3f\n", sqrt(2.0), sin(1.0), pow(2.0, 10.5));
    tls_counter++;
    pthread_t th; void *ret;
    pthread_create(&th, NULL, worker, (void *)100L);
    pthread_join(th, &ret);
    printf("tls main=%d thread=%ld\n", tls_counter, (long)ret);
    struct S m = mid((struct S){1, 2}, (struct S){3, 6});
    printf("struct: %.1f %.1f\n", m.x, m.y);
    int fd = open("/nonexistent/x", O_RDONLY);
    printf("open: fd=%d errno=%d (%s)\n", fd, errno, strerror(errno));
    char *heap = malloc(1 << 20); memset(heap, 0xab, 1 << 20);
    long sum = 0; for (int i = 0; i < (1 << 20); i += 4096) sum += (unsigned char)heap[i];
    printf("heap sum=%ld\n", sum); free(heap);
    atexit(bye);
    long long acc = 0; for (long i = 1; i <= 3000000; i++) acc += i * (i & 7) ^ (acc >> 3);
    printf("loop acc=%lld\n", acc);
    return 0;
}
