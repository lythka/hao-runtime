/* Native code calling a code pointer into an inaccessible range (tests/guest/fetchgiveup.sh): libc's
 * qsort calls a comparator that points into a PROT_NONE page, as native code called a bad pointer into
 * the unused part of the shared region (26B131). The instruction fetch faults with nothing to handle it:
 * natively the process dies of that fault; under hao on_fault gives up (hao_signal_giveup) and the
 * fetch must fault again, so the process dies the same way, with hao's "given up" line naming the
 * address. The target is mid-page, so the bytes before it are on the same inaccessible page. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

int main(void)
{
    char *p = mmap(NULL, 0x4000, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 2; }
    int (*cmp)(const void *, const void *) = (int (*)(const void *, const void *))(void *)(p + 0xe94);
    fprintf(stderr, "target=%p\n", (void *)(p + 0xe94));
    int a[2] = {2, 1};
    printf("calling qsort\n");
    fflush(stdout);
    qsort(a, 2, sizeof a[0], cmp);
    printf("qsort returned\n");
    return 0;
}
