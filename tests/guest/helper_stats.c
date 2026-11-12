/* Fixed helper routes; the same executable at N=0 removes startup counts. */
#include <stdlib.h>
int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], 0, 0) : 1000;
    for (unsigned k = 0; k < n; k++)
        __asm__ volatile("movi v1.4s, #0\n"
                         "frecpe v0.4s, v1.4s\n" /* deliberately untranslated */
                         "fabd v0.4s, v1.4s, v1.4s\n" /* not FADDP */
                         "addp d0, v1.2d\n"       /* integer, not FP pair */
                         "movi v2.4s, #1\n"      /* subnormal: input decline */
                         "fadd v0.4s, v2.4s, v2.4s\n"
                         "fmov v3.4s, #1.0\n"
                         "fadd v0.4s, v3.4s, v3.4s\n" /* admitted */
                         ::: "v0", "v1", "v2", "v3");
    return 0;
}
