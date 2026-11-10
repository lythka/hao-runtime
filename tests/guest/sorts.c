#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int ncmp;
static int cmpstr(const void *a, const void *b) { ncmp++; return strcmp(*(char *const *)a, *(char *const *)b); }
int main(void) {
    const char *w[] = {"pear", "apple", "fig", "kiwi", "date", "plum", "lime", "apricot", "banana", "cherry", "grape"};
    const char *a[11];
    memcpy(a, w, sizeof w); qsort(a, 11, 8, cmpstr); printf("qsort %s..%s n=%d\n", a[0], a[10], ncmp);
    memcpy(a, w, sizeof w); ncmp = 0; int r = mergesort(a, 11, 8, cmpstr); printf("mergesort r=%d %s..%s n=%d\n", r, a[0], a[10], ncmp);
    memcpy(a, w, sizeof w); ncmp = 0; heapsort(a, 11, 8, cmpstr); printf("heapsort %s..%s n=%d\n", a[0], a[10], ncmp);
    return 0;
}
