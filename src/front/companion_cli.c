/* Owner-signed companion launcher. HAO_PROGRAM is the selected program's
 * canonical path, fixed at signing time. hao_cli_main retains its normal
 * executable signature and entitlement checks on the descriptor it loads. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int hao_cli_main(int, char **, char **);

int main(int argc, char **argv, char **envp) {
    const char *handoff = getenv("HAO_EXEC");
    if (argc && handoff && !strcmp(handoff, HAO_PROGRAM) && !strcmp(argv[0], HAO_PROGRAM))
        return hao_cli_main(argc, argv, envp);

    if (argc < 1 || argc == INT_MAX) return 2;
    char **args = calloc((size_t)argc + 2, sizeof(*args));
    if (!args) return 1;
    unsetenv("HAO_EXEC");
    unsetenv("HAO_EXEC_FLAGS");
    args[0] = argv[0];
    args[1] = HAO_PROGRAM;
    memcpy(args + 2, argv + 1, (size_t)(argc - 1) * sizeof(*args));
    int status = hao_cli_main(argc + 1, args, envp);
    free(args);
    return status;
}
