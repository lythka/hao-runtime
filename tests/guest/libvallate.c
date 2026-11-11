/* libvallate (tests/guest/libval.sh): a program that rewrites the library it
 * is linked against, in place, once it is running, and only then calls into
 * it: libvallate LIBRARY REPLACEMENT. The library was checked and loaded
 * with the bytes it had at launch; what it returns must come from those. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

extern int gplug_value(void);

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int in = open(argv[2], O_RDONLY), out = open(argv[1], O_WRONLY);
    if (in < 0 || out < 0) { perror("libvallate"); return 2; }
    char buf[65536];
    off_t at = 0;
    for (ssize_t n; (n = read(in, buf, sizeof buf)) > 0; at += n)
        if (pwrite(out, buf, (size_t)n, at) != n) { perror("libvallate: write"); return 2; }
    if (ftruncate(out, at) != 0 || fsync(out) != 0) { perror("libvallate"); return 2; }
    close(in);
    close(out);
    printf("late libgplug: %d\n", gplug_value());
    return 0;
}
