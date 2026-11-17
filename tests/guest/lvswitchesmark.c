/* lvswitchesmark: a thunk library for tests/guest/lvswitches.sh. Its native
 * half's initializer creates the file LVSWITCHES_MARK names, which says the
 * library was loaded into the process. */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((constructor)) static void mark(void)
{
#if defined(__x86_64__)
    const char *p = getenv("LVSWITCHES_MARK");
    if (p) close(open(p, O_WRONLY | O_CREAT, 0644));
#endif
}
