#include <stdio.h>
#include <stdlib.h>
#include <dispatch/dispatch.h>
#include <Block.h>
static int cmpi(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int main(void) {
    __block int sum = 0;
    dispatch_queue_t q = dispatch_queue_create("t", NULL);
    for (int i = 0; i < 100; i++) dispatch_sync(q, ^{ sum += i; });
    dispatch_apply(50, q, ^(size_t k) { sum += (int)k; });
    int v[64]; for (int i = 0; i < 64; i++) v[i] = (i * 37) % 64;
    qsort_b(v, 64, sizeof v[0], ^int(const void *a, const void *b) { return cmpi(a, b); });
    qsort(v, 64, sizeof v[0], cmpi);
    printf("sum=%d first=%d last=%d\n", sum, v[0], v[63]);
    return 0;
}
