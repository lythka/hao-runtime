/* The two slices deliberately expose different initialization and return values. */
#include <dlfcn.h>
#include <stdio.h>

#ifdef __arm64__
#define SLICE "arm"
#define VALUE 1
#else
#define SLICE "intel"
#define VALUE 2
#endif

#if defined(IMAGE_OBSERVER)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static unsigned matches(const char *wanted, void *function) {
    uint32_t count = _dyld_image_count();
    unsigned found = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        char real[PATH_MAX];
        if (!name || !realpath(name, real) || strcmp(real, wanted)) continue;
        found++;
        if (!function) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i);
        Dl_info info;
        if (!mh || mh->cputype != CPU_TYPE_ARM64 || !dladdr(function, &info) ||
            info.dli_fbase != (const void *)mh) exit(7);
        const struct load_command *lc = (const void *)(mh + 1);
        int valid_text = 0;
        for (uint32_t j = 0; j < mh->ncmds; j++) {
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *seg = (const void *)lc;
                if (!strcmp(seg->segname, "__TEXT") &&
                    seg->vmaddr + _dyld_get_image_vmaddr_slide(i) == (uintptr_t)mh) valid_text = 1;
            }
            lc = (const void *)((const char *)lc + lc->cmdsize);
        }
        if (!valid_text) exit(8);
    }
    if (_dyld_get_image_name(count) || _dyld_get_image_header(count) ||
        _dyld_get_image_vmaddr_slide(count)) exit(9);
    return found;
}
int main(int argc, char **argv) {
    char real[PATH_MAX];
    if (argc != 2 || !realpath(argv[1], real)) return 2;
    if (matches(real, NULL)) return 3;
    void *h = dlopen(real, RTLD_NOW | RTLD_LOCAL);
    if (!h) return 4;
    void *symbol = dlsym(h, "branch");
    if (!symbol || matches(real, symbol) != 1) return 5;
    puts("observed one matching ARM image, header and slide");
    return 0;
}
#elif defined(SPAWNER)
#include <spawn.h>
#include <mach/machine.h>
#include <sys/wait.h>
extern char **environ;
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    posix_spawnattr_t attr;
    cpu_type_t cpu = CPU_TYPE_X86_64;
    size_t count = 0;
    pid_t child;
    if (posix_spawnattr_init(&attr) || posix_spawnattr_setbinpref_np(&attr, 1, &cpu, &count) ||
        count != 1 || posix_spawn(&child, argv[1], NULL, &attr, argv + 1, environ)) return 6;
    posix_spawnattr_destroy(&attr);
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) ? WEXITSTATUS(status) : 7;
}
#elif defined(LEAF)
const char signature_marker[] = "hao-domain-verification-leaf";
__attribute__((constructor)) static void report(void) { fprintf(stderr, "leaf-%s\n", SLICE); }
int leaf(void) { return VALUE; }
#elif defined(OPENER)
int branch(void) {
    void *handle = dlopen("Plugin.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "%s\n", dlerror()); return -8; }
    int (*value)(void) = dlsym(handle, "branch");
    return value ? value() : -9;
}
#elif defined(BRANCH)
extern int leaf(void);
__attribute__((constructor)) static void report(void) { fprintf(stderr, "branch-%s\n", SLICE); }
int branch(void) { return 10 * VALUE + leaf(); }
#elif defined(LINKED)
extern int branch(void);
int main(void) { printf("result %d\n", branch()); return 0; }
#else
int main(int argc, char **argv) {
    if (argc != 2) return 2;
#ifdef REPORT_ENTRY
    fprintf(stderr, "validation-control-entered\n");
#endif
    void *system = dlopen("libSystem.B.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!system || !dlsym(system, "getpid")) return 5;
    dlclose(system);
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "dlopen refused: %s\n", dlerror()); return 3; }
    int (*branch)(void) = dlsym(handle, "branch");
    if (!branch) return 4;
    printf("result %d\n", branch());
    return 0;
}
#endif
