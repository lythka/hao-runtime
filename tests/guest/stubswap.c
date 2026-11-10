/* stubswap: an x86_64 library for DYLD_INSERT_LIBRARIES (tests/guest/stubself.sh).
 * Its initializer runs in an embedded program's process after the kernel's exec
 * and before the stub's main: there it repoints the symbolic link SWAP_LINK to
 * SWAP_TO, as the owner of a link in the path of an exec can at any moment. What
 * the executable's path names is then no longer the file the kernel started.
 * It acts only in a process that was started through SWAP_LINK (an absolute
 * path), not in a launcher that merely passes the environment on. */
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void swap(void)
{
    const char *link = getenv("SWAP_LINK"), *to = getenv("SWAP_TO");
    char exe[2048];
    uint32_t n = sizeof exe;
    if (!link || !to || _NSGetExecutablePath(exe, &n) != 0 || strcmp(exe, link) != 0) return;
    unlink(link);
    if (symlink(to, link) != 0) _exit(99);
}
