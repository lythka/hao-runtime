/* swapclose: an x86_64 library for DYLD_INSERT_LIBRARIES into `hao --aot
 * --check` (tests/aot/run.sh, case V). When hao closes the first descriptor
 * it opened on SWAP_FILE (the one whose signature it checked), it renames
 * SWAP_WITH over SWAP_FILE, so the path then names another file: what the
 * owner of a directory in the path can do at any moment. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

static char target[MAXPATHLEN];
static int done;

__attribute__((constructor)) static void init(void)
{
    const char *f = getenv("SWAP_FILE");
    if (!f || !realpath(f, target)) target[0] = 0;
}

static int swap_close(int fd)
{
    char p[MAXPATHLEN];
    int r = target[0] && !done && fcntl(fd, F_GETPATH, p) == 0 && !strcmp(p, target);
    int rc = close(fd);
    if (r) {
        done = 1;
        const char *with = getenv("SWAP_WITH");
        if (!with || rename(with, target) != 0) _exit(99);
    }
    return rc;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *new, *old; } interposers[] = {
    {(const void *)swap_close, (const void *)close},
};
