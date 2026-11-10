/* libvalswap: an x86_64 library for DYLD_INSERT_LIBRARIES into hao
 * (tests/guest/libval.sh). It rewrites the file SWAP_FILE in place with the
 * bytes of SWAP_WITH at a fixed moment: after hao has read SWAP_FILE (to
 * check its code signature) and before it maps the image's first segment,
 * which is the first fixed-address mmap after a pread of that file. The
 * owner of a guest file can do the same at any moment; here it is the one a
 * loader that checks the file and then maps it cannot survive. The file
 * keeps its inode, so the descriptor hao holds names the rewritten file. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>

static char target[MAXPATHLEN];
static int armed, done;

__attribute__((constructor)) static void init(void)
{
    const char *f = getenv("SWAP_FILE");
    if (!f || !realpath(f, target)) target[0] = 0;
}

static void swap(void)
{
    done = 1;
    const char *with = getenv("SWAP_WITH");
    int in = with ? open(with, O_RDONLY) : -1, out = open(target, O_WRONLY);
    if (in < 0 || out < 0) _exit(99);
    char buf[65536];
    off_t at = 0;
    for (ssize_t n; (n = read(in, buf, sizeof buf)) > 0; at += n)
        if (pwrite(out, buf, (size_t)n, at) != n) _exit(99);
    if (ftruncate(out, at) != 0 || fsync(out) != 0) _exit(99);
    close(in);
    close(out);
}

static ssize_t swap_pread(int fd, void *buf, size_t n, off_t off)
{
    char p[MAXPATHLEN];
    if (target[0] && !done && fcntl(fd, F_GETPATH, p) == 0 && !strcmp(p, target)) armed = 1;
    return pread(fd, buf, n, off);
}

static void *swap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (armed && !done && (flags & MAP_FIXED)) swap();
    return mmap(addr, len, prot, flags, fd, off);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *new, *old; } interposers[] = {
    {(const void *)swap_pread, (const void *)pread},
    {(const void *)swap_mmap, (const void *)mmap},
};
