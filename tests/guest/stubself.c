/* stubself: what an embedded program's process looks like after the stub's start
 * (tests/guest/stubself.sh). Built with -DWHO='"A"' or '"B"' to tell two programs
 * apart. Prints
 *   stubself WHO
 *   re-exec: yes|no      the kernel's copy of this process's exec-time environment
 *                        (KERN_PROCARGS2, which unsetenv does not change) holds
 *                        HAO_SELF: the stub re-exec'd itself
 *   tags: plain|obfuscated|none   libobjc's tagged-pointer obfuscator
 *   ids: same|changed    real and effective user and group IDs are equal or not
 *   pidinfo: yes|no      proc_pidinfo answers this process about itself (a sandbox
 *                        can deny it)
 * and returns 7. */
#include <dlfcn.h>
#include <libproc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef WHO
#define WHO "A"
#endif

static int exec_env_has(const char *name)
{
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, getpid()};
    size_t n = 0, nl = strlen(name);
    if (sysctl(mib, 3, NULL, &n, NULL, 0) != 0 || n < sizeof(int)) return -1;
    char *b = malloc(n + 1);
    if (!b || sysctl(mib, 3, b, &n, NULL, 0) != 0) return -1;
    b[n] = 0;
    int argc;
    memcpy(&argc, b, sizeof argc);
    char *p = b + sizeof argc, *e = b + n;
    p += strlen(p);                         /* the exec path */
    while (p < e && !*p) p++;               /* its padding */
    for (int k = 0; k < argc && p < e; k++) p += strlen(p) + 1;
    for (; p < e && *p; p += strlen(p) + 1)
        if (!strncmp(p, name, nl) && p[nl] == '=') return 1;
    return 0;
}

int main(void)
{
    printf("stubself %s\n", WHO);
    int r = exec_env_has("HAO_SELF");
    printf("re-exec: %s\n", r < 0 ? "unknown" : r ? "yes" : "no");
    const uintptr_t *obf = dlsym(RTLD_DEFAULT, "objc_debug_taggedpointer_obfuscator");
    printf("tags: %s\n", !obf ? "none" : *obf ? "obfuscated" : "plain");
    printf("ids: %s\n", getuid() == geteuid() && getgid() == getegid() ? "same" : "changed");
    struct proc_bsdinfo bi;
    printf("pidinfo: %s\n", proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi ? "yes" : "no");
    return 7;
}
