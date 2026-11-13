/* tests/guest/faultstack.sh: a fatal fault in native code (strlen of a bad pointer) at a chosen depth of
 * nested native->guest callbacks (qsort calling the guest comparator, which calls qsort again), on a
 * thread whose stack the guest sizes: faultstack KB DEPTH. Prints "faulting at depth N" just before. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile const char *bad = (const char *)8;
static int depth, want;

static int cmp(const void *a, const void *b)
{
    if (depth < want) {
        int v[2] = {2, 1};
        depth++;
        qsort(v, 2, sizeof v[0], cmp);
    } else if (depth == want) {
        depth++;
        fprintf(stderr, "faulting at depth %d\n", want);
        return (int)strlen((const char *)bad);
    }
    return *(const int *)a - *(const int *)b;
}

static void *run(void *a)
{
    (void)a;
    int v[2] = {2, 1};
    qsort(v, 2, sizeof v[0], cmp);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    want = atoi(argv[2]);
    pthread_attr_t at;
    pthread_attr_init(&at);
    if (pthread_attr_setstacksize(&at, strtoul(argv[1], NULL, 0) << 10)) return 2;
    pthread_t th;
    if (pthread_create(&th, &at, run, NULL)) return 2;
    pthread_join(th, NULL);
    return 0;
}
