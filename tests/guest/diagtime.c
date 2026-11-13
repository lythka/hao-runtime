/* HAO_DIAG_TIME and the lines kept while the log's volume is absent (tests/guest/diagtime.sh):
 * argv[1] is the log directory's missing parent, made here as the Data volume is mounted under
 * PID 1; then abort(), whose report is the first line after it. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc > 1 && mkdir(argv[1], 0700) != 0) { perror("mkdir"); return 2; }
    puts("made");
    fflush(stdout);
    if (argc > 2) return 0;
    abort();
}
