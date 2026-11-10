/* A native call whose x0 looks like an aligned pointer but is not mapped: a Swift small string ("plist" is
 * 0x7473696c70 in x0, kernelmanager_helper). HAO_VERBOSE=3 printed the word at x0 and faulted (tests/guest/cli.sh). */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    long (*volatile f)(long) = labs;
    long r = f(0x7473696c70L) + f(-0x7ff000000000L);
    printf("labs: %#lx\n", r);
    return 0;
}
