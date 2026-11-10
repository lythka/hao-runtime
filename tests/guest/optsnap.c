/* With an argument, sets HAO_NO_CHAIN before a hot loop: hao reads its
 * switches once at startup, so block chaining must stay on (tests/aot/run.sh). */
#include <stdio.h>
#include <stdlib.h>
__attribute__((noinline)) static long f(long x) { __asm__ volatile(""); return x * 3 + 1; }
int main(int c, char **v)
{
    (void)v;
    if (c > 1) setenv("HAO_NO_CHAIN", "1", 1);
    long s = 0;
    for (long i = 0; i < 10000000; i++) s += f(i) & 7;
    printf("%ld\n", s);
    return 0;
}
