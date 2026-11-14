/* Guest -> native -> guest recursion on a 512 KB thread, a libdispatch worker's size: qsort's comparator
 * sorts again, DEPTH levels deep. Each level costs native stack for hao's frames between the native caller
 * and the guest code (hao_callback_entry .. run_loop .. native_call); WallpaperAgent on 26B131 ran a worker
 * out of stack at 134 levels of value-witness destroys calling back into guest code, where native arm64
 * needs ~200 bytes a level. tests/guest/deepcallback.sh; output must equal native arm64. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int target, reached, depth;
static int cmp(const void *a, const void *b);

static void level(int d)
{
    int v[2] = {2, 1};
    if (d > reached) reached = d;
    if (d < target) qsort(v, 2, sizeof v[0], cmp);
}

static int cmp(const void *a, const void *b)
{
    int d = ++depth;
    level(d);
    --depth;
    return *(const int *)a - *(const int *)b;
}

static void *run(void *arg)
{
    (void)arg;
    level(0);
    return NULL;
}

int main(int argc, char **argv)
{
    target = argc > 1 ? atoi(argv[1]) : 250;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);
    pthread_t t;
    if (pthread_create(&t, &a, run, NULL)) return 2;
    pthread_join(t, NULL);
    printf("depth %d reached %d\n", target, reached);
    return 0;
}
