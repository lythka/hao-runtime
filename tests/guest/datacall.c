/* A guest call to an import that its native library defines as data
 * (tests/guest/datacall.sh). The program is built against a declaration
 * of a function, as 27.0's airportd is built against Apple's IO80211. */
#include <stdio.h>
#include <string.h>

const char *placeholder_typetoStr(int);
const char *real_typetoStr(int);

int main(int argc, char **argv)
{
    printf("real %s\n", real_typetoStr(2));
    fflush(stdout);
    if (argc > 1 && !strcmp(argv[1], "--no-call")) return 0;
    printf("placeholder %s\n", placeholder_typetoStr(2));   /* executes the data word */
    return 0;
}
