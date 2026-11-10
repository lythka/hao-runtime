/*
 * How long hao_aot_load takes for one image (against libhao):
 *   clang -arch x86_64 -O1 -Iinclude -o loadtime tests/aot/loadtime.c -Lbuild -lhao -Wl,-rpath,$PWD/build
 *   codesign -s - -f loadtime
 *   HAO_CACHE=dir timeout -s KILL 300 ./loadtime GUEST [COPIES [HAO_AOT_ARGS...]]
 * HAO_CACHE holds GUEST's cache (`build/hao --aot --cache dir GUEST`), or
 * nothing to time a miss. GUEST is mapped COPIES times (default 9; no
 * fixups, no guest code runs), each copy at its own address, and the cache is
 * loaded into each: the file is opened through the trust checks, read,
 * validated (format, payload hash, the image's code identity) and its code
 * and block table are installed. Prints each load's microseconds, then one
 * line with the minimum and median, the number that loaded and the guest's
 * executable bytes. Exit 0 always: this measures, tests/aot/run.sh checks.
 */
#include "hao/runtime.h"
#include "hao/aot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: loadtime GUEST [COPIES]\n"); return 2; }
    int n = argc > 2 ? atoi(argv[2]) : 9;
    if (n < 1 || n > 64) n = 9;
    if (!hao_jit_init()) { fprintf(stderr, "loadtime: no code cache\n"); return 2; }
    hao_image *im[64];
    char err[512];
    for (int k = 0; k < n; k++)
        if (!(im[k] = hao_map_image_only(argv[1], err, sizeof err))) { fprintf(stderr, "loadtime: %s\n", err); return 2; }
    uint64_t code = 0;
    for (int k = 0; k < im[0]->ncode; k++) code += im[0]->code_hi[k] - im[0]->code_lo[k];
    double us[64];
    int loaded = 0;
    for (int k = 0; k < n; k++) {
        double t0 = now_us();
        bool ok = hao_aot_load(im[k]);
        us[k] = now_us() - t0;
        loaded += ok;
        printf("load %d: %.0f us%s\n", k, us[k], ok ? "" : " (not loaded)");
    }
    qsort(us, (size_t)n, sizeof us[0], cmp);
    printf("%s: %d/%d loaded, min %.0f us, median %.0f us, %llu KB of guest code\n", argv[1], loaded, n, us[0], us[n / 2],
           (unsigned long long)(code >> 10));
    return 0;
}
