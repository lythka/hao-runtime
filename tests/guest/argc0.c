/* Guest for tests/guest/cli.sh: tolerates an empty argv. */
#include <stdio.h>
int main(int argc, char **argv) { printf("argc=%d argv0=%s\n", argc, argc ? argv[0] : "(none)"); return 3; }
