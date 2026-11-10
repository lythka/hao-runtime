/* Prints every open descriptor above 2 that names this program's own file
 * (hao must not keep the guest image open; tests/aot/run.sh). */
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    char self[PATH_MAX], real[PATH_MAX], p[PATH_MAX];
    (void)argc;
    if (!realpath(argv[0], self)) return 2;
    int n = 0;
    for (int fd = 3; fd < 256; fd++)
        if (fcntl(fd, F_GETPATH, p) == 0 && realpath(p, real) && !strcmp(real, self)) { printf("open fd %d: %s\n", fd, p); n++; }
    printf("%d descriptors on the program file\n", n);
    return 0;
}
