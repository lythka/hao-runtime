#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int hao_cli_main(int, char **, char **);

/* The signed fixture launcher runs its bundled guest and accepts no guest path. */
int main(int argc, char **argv, char **envp) {
    char executable[PATH_MAX], resolved[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) || !realpath(executable, resolved)) return 2;
    char *leaf = strrchr(resolved, '/');
    if (!leaf) return 2;
    *leaf = 0;
    char guest[PATH_MAX], observer[PATH_MAX];
    if (snprintf(guest, sizeof(guest), "%s/Probe", resolved) >= (int)sizeof(guest) ||
        snprintf(observer, sizeof(observer), "%s/../Frameworks/ProbeObserver.dylib", resolved) >=
            (int)sizeof(observer)) return 2;
    if (argc == 2 && !strcmp(argv[0], guest) && !strcmp(argv[1], observer) &&
        getenv("HAO_EXEC") && !strcmp(getenv("HAO_EXEC"), guest))
        return hao_cli_main(argc, argv, envp);
    if (argc != 1) {
        fprintf(stderr, "unexpected declared fixture handoff (argc=%d)\n", argc);
        return 2;
    }
    char *arguments[] = {argv[0], guest, observer, NULL};
    return hao_cli_main(3, arguments, envp);
}
