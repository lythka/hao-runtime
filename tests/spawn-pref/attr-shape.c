#include "spawn_pref.h"
#include <mach/machine.h>
#include <signal.h>
#include <stdio.h>

extern const void *hao_identity_original(const void *);

int main(void)
{
    const void *original = hao_identity_original(posix_spawn);
    /* An unrecognized native ABI must never enable the retry. */
    if (!hao_spawn_attr_abi(original)) {
        puts("unrecognized native spawn ABI, retry disabled");
        return 0;
    }
    posix_spawnattr_t guest, host;
    size_t count;
    cpu_type_t cpu = CPU_TYPE_ARM64;
    if (posix_spawnattr_init(&guest) ||
        posix_spawnattr_setbinpref_np(&guest, 1, &cpu, &count) || count != 1) return 1;
    if (!hao_spawn_host_attr(original, &guest, &host)) return 1;
    if (posix_spawnattr_getbinpref_np(&host, 1, &cpu, &count) ||
        count != 1 || cpu != CPU_TYPE_X86_64) return 2;
    posix_spawnattr_destroy(&host);
    if (posix_spawnattr_getbinpref_np(&guest, 1, &cpu, &count) ||
        count != 1 || cpu != CPU_TYPE_ARM64) return 2;
    if (posix_spawnattr_setflags(&guest, POSIX_SPAWN_SETEXEC) ||
        !hao_spawn_host_attr(original, &guest, &host)) return 3;
    short flags;
    if (posix_spawnattr_getflags(&host, &flags) || flags != POSIX_SPAWN_SETEXEC) return 3;
    posix_spawnattr_destroy(&host);
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGUSR1);
    if (posix_spawnattr_setsigmask(&guest, &sigs) || hao_spawn_host_attr(original, &guest, &host)) return 4;
    posix_spawnattr_destroy(&guest);
    puts("ordinary, SETEXEC and hidden signal-mask controls passed");
    return 0;
}
