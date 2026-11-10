/* The main executable's load-time identity as native code sees it (tests/tools/identity.sh).
 * Built as a fat dylib (x86_64, arm64, arm64e): under hao the guest calls its x86_64 slice, so every value
 * comes from the process's real main executable (the stub), exactly as a native framework reads it; natively it
 * is the arm64 program's own. Roles: -DROLE_HELPER (idh_report), -DROLE_PLUG / -DROLE_ENV (dlopen targets),
 * -DROLE_INSERT (a DYLD_INSERT_LIBRARIES library that says so). Only libSystem: no display. */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(ROLE_HELPER)
extern uint32_t dyld_get_program_sdk_version(void);
extern uint32_t dyld_get_program_min_os_version(void);
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

static const char *std_seg[] = {"__PAGEZERO", "__TEXT", "__DATA", "__DATA_CONST", "__LINKEDIT", "__AUTH",
                                "__AUTH_CONST", "__DATA_DIRTY", NULL};

void idh_report(void) {
    const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(0);
    const struct load_command *lc = (const void *)(mh + 1);
    printf("mh pie=%d no_heap_exec=%d allow_stack_exec=%d\n", !!(mh->flags & MH_PIE),
           !!(mh->flags & MH_NO_HEAP_EXECUTION), !!(mh->flags & MH_ALLOW_STACK_EXECUTION));
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SOURCE_VERSION) {
            uint64_t v = ((const struct source_version_command *)lc)->version;
            printf("source_version %llu.%llu.%llu.%llu.%llu\n", v >> 40, (v >> 30) & 0x3ff, (v >> 20) & 0x3ff,
                   (v >> 10) & 0x3ff, v & 0x3ff);
        } else if (lc->cmd == LC_RPATH) {
            const char *p = (const char *)lc + ((const struct rpath_command *)lc)->path.offset;
            if (p[0] == '@')   /* the program's; the stub's own (/usr/lib/hao, a test build directory) come first */
                printf("rpath %s\n", p);
        } else if (lc->cmd == LC_DYLD_ENVIRONMENT) {
            printf("dyld_environment %s\n", (const char *)lc + ((const struct dylinker_command *)lc)->name.offset);
        } else if (lc->cmd == LC_MAIN) {
            printf("LC_MAIN stacksize %#llx\n", ((const struct entry_point_command *)lc)->stacksize);
        } else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            int std = 0;
            for (int k = 0; std_seg[k]; k++)
                std |= !strncmp(s->segname, std_seg[k], 16);
            if (!strncmp(s->segname, "__PAGEZERO", 16))
                printf("__PAGEZERO %#llx\n", s->vmsize);
            else if (!std)
                printf("segment %.16s prot %d/%d\n", s->segname, s->maxprot, s->initprot);
        }
    }
    uint32_t sdk = dyld_get_program_sdk_version(), minos = dyld_get_program_min_os_version();
    printf("sdk %u.%u minos %u.%u\n", sdk >> 16, (sdk >> 8) & 255, minos >> 16, (minos >> 8) & 255);
    int32_t lsv = NSVersionOfLinkTimeLibrary("System");
    printf("libSystem link version %d.%d.%d\n", lsv >> 16, (lsv >> 8) & 255, lsv & 255);
    uint32_t cs = 0;
    csops(0, 0 /* CS_OPS_STATUS */, &cs, sizeof cs);
    printf("cs kill=%d hard=%d\n", !!(cs & 0x200), !!(cs & 0x100));
    printf("main thread stack %#zx\n", pthread_get_stacksize_np(pthread_self()));
    void *h = dlopen("@rpath/libidplug.dylib", RTLD_NOW);
    printf("dlopen @rpath/libidplug.dylib: %s\n", h ? "ok" : "failed");
    h = dlopen("libidenv.dylib", RTLD_NOW);
    printf("dlopen libidenv.dylib (DYLD_LIBRARY_PATH): %s\n", h ? "ok" : "failed");
    fflush(stdout);
}
#elif defined(ROLE_PLUG)
int idplug_marker(void) { return 1; }
#elif defined(ROLE_ENV)
int idenv_marker(void) { return 2; }
#elif defined(ROLE_INSERT)
__attribute__((constructor)) static void inserted(void) {
    printf("inserted library loaded\n");
    fflush(stdout);
}
#else
#error "build with one of -DROLE_HELPER, -DROLE_PLUG, -DROLE_ENV, -DROLE_INSERT"
#endif
