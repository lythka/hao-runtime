/*
 * hao — run an arm64/arm64e Mach-O on x86_64 with native x86_64 libraries.
 *   hao [-v] [--interp] PROGRAM [ARGS...]
 */
#include "hao/runtime.h"
#include "hao/identity.h"
#include "hao/native_bundle.h"
#include "hao/bridge.h"
#include <crt_externs.h>
#include <errno.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#include <sys/param.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

static void usage(void)
{
    fprintf(stderr, "usage: hao [-v|-vv] [--interp] [--stats] [--allow-unsigned] [--] PROGRAM [ARGS...]\n"
                    "       hao --aot [--cache DIR] [--] FILE...\n");
    exit(64);
}

#include <signal.h>
#include <dlfcn.h>
static void watchdog(int sig)
{
    (void)sig;
    hao_thread *t = hao_current_peek();
    if (!t) { fprintf(stderr, "hao: watchdog: (no guest thread)\n"); _exit(124); }
    hao_cpu *c = &t->cpu;
    hao_image *im = hao_image_for_code(c->pc);
    fprintf(stderr, "hao: watchdog: guest pc=%#llx", (unsigned long long)c->pc);
    if (im) fprintf(stderr, " (file vmaddr %#llx)", (unsigned long long)(c->pc - im->slide));
    else { Dl_info di; if (dladdr((void *)(uintptr_t)c->pc, &di) && di.dli_sname) fprintf(stderr, " (native %s)", di.dli_sname); }
    fprintf(stderr, " lr=%#llx", (unsigned long long)c->x[30]);
    if (im) fprintf(stderr, " (file vmaddr %#llx)", (unsigned long long)(c->x[30] - im->slide));
    fprintf(stderr, "\n");
    for (int k = 0; k < 31; k += 4)
        fprintf(stderr, "  x%-2d %016llx x%-2d %016llx x%-2d %016llx x%-2d %016llx\n", k, (unsigned long long)c->x[k], k + 1,
                (unsigned long long)c->x[k + 1], k + 2, (unsigned long long)c->x[k + 2], k + 3, (unsigned long long)c->x[k + 3]);
    _exit(124);
}
static void print_stats(void)
{
    hao_thread *t = hao_current();
    fprintf(stderr, "hao: blocks=%llu steps=%llu native_calls=%llu callbacks=%llu traps=%llu\n",
            (unsigned long long)t->blocks, (unsigned long long)t->steps, (unsigned long long)t->native_calls,
            (unsigned long long)t->callbacks, (unsigned long long)t->traps);
    fprintf(stderr, "hao: threads live=%llu freed=%llu\n", (unsigned long long)hao_threads_live,
            (unsigned long long)hao_threads_freed);
    if (hao_use_jit) hao_jit_stats();
    hao_blocks_stats();
    hao_fnslots_stats();
    hao_swiftmeta_stats();
    hao_callbacks_stats();
    hao_signals_stats();
    hao_code_stats();
    if (!hao_opt.bridge_stats) hao_bridge_stats();   /* with it, the atexit report prints this */
}

/*
 * Per-image guest initialisation (dyld's job for native images), the same
 * steps for the main executable and for guest dylibs. hao_run_initializers
 * (runtime.c) calls it once per image after every image is bound,
 * dependencies first: TLV, Swift, ObjC, global blocks, function-pointer
 * slots, the AOT cache, then the initializers. Every initializer gets the program's argc, argv, envp,
 * apple and ProgramVars, as from dyld.
 */
static int g_argc; static char **g_argv, **g_envp; static const char *g_apple[2];
static struct { uint64_t mh, argc, argv, envp, progname; } g_progvars;
void hao_guest_image_init(hao_image *img)
{
    hao_tlv_setup(img);
    /* Swift records first: ObjC registration runs the metadata
     * initializers of Swift classes laid out at run time (objc.c), which
     * may look up this image's types and conformances. */
    extern void hao_swift_register_image(hao_image *);
    hao_swiftmeta_image(img);   /* relative pointers to guest functions -> island thunks, before the runtime reads them */
    hao_swift_register_image(img);
    hao_objc_register_image(img);
    hao_blocks_register_image(img);
    hao_fnslots_image(img);
    if (hao_use_jit) hao_aot_load(img);
    hao_thread *t = hao_current();
    g_progvars.mh = hao_main_image ? hao_main_image->base : img->base;
    g_progvars.argc = (uint64_t)(uintptr_t)_NSGetArgc();
    g_progvars.argv = (uint64_t)(uintptr_t)_NSGetArgv();
    g_progvars.envp = (uint64_t)(uintptr_t)_NSGetEnviron();
    g_progvars.progname = (uint64_t)(uintptr_t)_NSGetProgname();
    uint64_t args[5] = {(uint64_t)g_argc, (uint64_t)(uintptr_t)g_argv, (uint64_t)(uintptr_t)g_envp,
                        (uint64_t)(uintptr_t)g_apple, (uint64_t)(uintptr_t)&g_progvars};
    for (uint64_t k = 0; k < img->init_ptrs_count; k++)
        hao_call_guest(t, ((uint64_t *)(uintptr_t)img->init_ptrs)[k], args, 5);
    for (uint64_t k = 0; k < img->init_offs_count; k++)
        hao_call_guest(t, img->base + ((uint32_t *)(uintptr_t)img->init_offs)[k], args, 5);
}

/* Native ObjC tagged pointers use bit 0 on x86_64 but bit 63 on arm64.
 * Without obfuscation x86 tagged pointers keep bit 63 clear, so translated
 * code treats them as opaque objects and hands them back to native code.
 * libobjc reads this before any code of ours runs: the exec that starts a
 * hao process must pass it. Translated parents pass it to every hao stub
 * they exec (src/process_identity.c); otherwise the stub re-execs itself
 * (hao_self_main), and the CLI's first stage sets it for the second. The
 * guest never sees it in its environment, as natively. */
#define HAO_TAG_ENV "OBJC_DISABLE_TAG_OBFUSCATION"

/* 1: libobjc's obfuscator is zero (the exec passed the switch and libobjc
 * took it); 0: tagged pointers are obfuscated; -1: no libobjc here. */
static int plain_tags(void)
{
    const uintptr_t *obf = (const uintptr_t *)dlsym(RTLD_DEFAULT, "objc_debug_taggedpointer_obfuscator");
    return obf ? *obf == 0 : -1;
}

static int oslog_is_guest(const void *p) { return hao_image_for_addr((uint64_t)(uintptr_t)p) != NULL; }
/* [p, p+n) inside one segment of a guest image that its load commands map without write access
 * (__TEXT; a shared-cache image's read-only segments): what dyld calls immutable for its own images */
static int oslog_immutable(const void *p, size_t n)
{
    uint64_t a = (uint64_t)(uintptr_t)p;
    hao_image *im = hao_image_for_addr(a);
    if (!im || a + n < a) return 0;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)(uintptr_t)im->base;
    if (mh->magic != MH_MAGIC_64) return 0;
    uint64_t slide = im->base - im->pref_base;
    const uint8_t *lc = (const uint8_t *)(mh + 1), *end = lc + mh->sizeofcmds;
    for (uint32_t k = 0; k < mh->ncmds && lc + sizeof(struct load_command) <= end; k++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmdsize < sizeof *c || lc + c->cmdsize > end) return 0;
        if (c->cmd == LC_SEGMENT_64 && c->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            uint64_t lo = sg->vmaddr + slide, hi = lo + sg->vmsize;
            if (a >= lo && a + n <= hi) return !(sg->initprot & VM_PROT_WRITE) && (sg->initprot & VM_PROT_READ);
        }
        lc += c->cmdsize;
    }
    return 0;
}

static int run_guest(const char *path, int fd, int argc, char **argv, char **envp);

/*
 * The file this process runs, from the kernel: the device and inode of the
 * vnode that backs the main executable's Mach-O header (proc_pidinfo's
 * region information about this process), and that vnode's path as the
 * kernel resolves it now (kpath; "" when it has none). Where the query is
 * denied (a sandbox), the pair the kernel put in apple[] at exec answers
 * ("executable_file=FSID,FILEID", which dyld reads for the same purpose; the
 * device is the low word of the file system ID). Neither is a path the
 * caller of exec chose. false: not known.
 */
static bool self_vnode(char **apple, dev_t *dev, ino_t *ino, char *kpath, size_t n)
{
    struct proc_regionwithpathinfo ri;
    uint64_t hdr = (uint64_t)(uintptr_t)_NSGetMachExecuteHeader();
    memset(&ri, 0, sizeof ri);
    kpath[0] = 0;
    if (proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, hdr, &ri, sizeof ri) == (int)sizeof ri &&
        ri.prp_prinfo.pri_address <= hdr && hdr - ri.prp_prinfo.pri_address < ri.prp_prinfo.pri_size &&
        ri.prp_vip.vip_vi.vi_stat.vst_ino) {
        *dev = (dev_t)ri.prp_vip.vip_vi.vi_stat.vst_dev;
        *ino = (ino_t)ri.prp_vip.vip_vi.vi_stat.vst_ino;
        ri.prp_vip.vip_path[sizeof ri.prp_vip.vip_path - 1] = 0;
        snprintf(kpath, n, "%s", ri.prp_vip.vip_path);
        return true;
    }
    for (char **a = apple; a && *a; a++) {
        unsigned long long fsid, id;
        char tail;
        if (!strncmp(*a, "executable_file=", 16) && sscanf(*a + 16, "%llx,%llx%c", &fsid, &id, &tail) == 2 && id) {
            *dev = (dev_t)(uint32_t)fsid;
            *ino = (ino_t)id;
            return true;
        }
    }
    return false;
}

/* path names that file now */
static bool names_vnode(const char *path, dev_t dev, ino_t ino)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && st.st_dev == dev && st.st_ino == ino;
}

static int self_path(char *out, size_t n)
{
    char raw[1100];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0) return -1;
    if (!realpath(raw, out)) snprintf(out, n, "%s", raw);
    return 0;
}

/* PATH made absolute as dyld makes the main executable's path (what native
 * _NSGetExecutablePath, CFBundle and NSProcessInfo report): an absolute path
 * as given, else the working directory, "/" and the path without leading
 * "./"; symlinks and ".." are not resolved. */
static void dyld_abs_path(const char *path, char *out, size_t n)
{
    char cwd[1100];
    if (path[0] == '/' || !getcwd(cwd, sizeof cwd)) { snprintf(out, n, "%s", path); return; }
    while (path[0] == '.' && path[1] == '/') path += 2;
    snprintf(out, n, "%s/%s", cwd, path);
}

static int aot_main(int argc, char **argv)
{
    /* hao --aot [--cache DIR] [--] BINARY...: generate AOT caches (no execution).
     * --cache names the output directory on the command line: a guarded
     * process (root, for one) ignores HAO_CACHE, and would otherwise write
     * into the system directory of the machine it runs on. */
    int bad = 0, k = 2;
    const char *cache = NULL;
    char abs[PATH_MAX];
    if (k < argc && !strcmp(argv[k], "--cache")) {
        if (k + 1 >= argc || !argv[k + 1][0]) usage();
        cache = argv[k + 1];
        if (cache[0] != '/') {          /* relative to where the command was typed */
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof cwd) || snprintf(abs, sizeof abs, "%s/%s", cwd, cache) >= (int)sizeof abs) {
                fprintf(stderr, "hao-aot: --cache %s: cannot make the path absolute\n", cache);
                return 1;
            }
            cache = abs;
        }
        k += 2;
    }
    if (k < argc && !strcmp(argv[k], "--")) k++;
    if (k >= argc) usage();
    hao_opts_init(NULL);
    hao_verbose = 1;
    for (; k < argc; k++)
        if (hao_aot_generate_file(argv[k], true, cache) < 0) bad++;   /* maps a private copy of the file (jit.c) */
    return bad ? 1 : 0;
}

/*
 * CLI / kext-launcher form: hao [opts] PROGRAM ARGS...
 * The process is hao, so it re-execs itself with the guest's argv and the
 * tag-obfuscation switch; the second stage finds HAO_EXEC and runs the
 * guest. libhao_identity.dylib (linked by hao, so its dyld interposers apply
 * process-wide while the main executable stays dyld image 0) presents the
 * guest's path and its carried sections to native code.
 *
 * Anyone can start hao as its own second stage, so the hand-off carries a
 * path and switches and no trust: stage 2 opens the file, checks the
 * guest's signature on that descriptor (guest_sig.c) and loads the image
 * from it. Stage 1 checks nothing.
 */
int hao_cli_main(int argc, char **argv, char **envp)
{
    if (argc >= 3 && !strcmp(argv[1], "--aot")) return aot_main(argc, argv);
    /* HAO_EXEC is stage 1's hand-off to stage 2, which stage 1 runs with
     * argv[0] = the program: it is the absolute path stage 1 made of that
     * argv[0]. Any other HAO_EXEC was not set by stage 1 for this exec:
     * drop it and start at stage 1. (The comparison is of the two strings,
     * not of the files they name: a program that does not exist is still
     * stage 2's to report, not a reason to read its arguments as hao's.) */
    const char *path = getenv("HAO_EXEC");
    char own[1100] = "";
    if (path && argc >= 1 && argv[0]) dyld_abs_path(argv[0], own, sizeof own);
    if (path && (path[0] != '/' || strcmp(own, path) != 0)) {
        unsetenv("HAO_EXEC");
        path = NULL;
    }
    if (!path) {
        /* The options go to stage 2 as letters (v, V, i, s, u, as in
         * HAO_FLAGS) in HAO_EXEC_FLAGS, apart from an inherited HAO_FLAGS:
         * stage 2 honours each where its tier honours the environment
         * switch of the same effect, while HAO_FLAGS is all or nothing
         * (trust.c). HaoExec's "#! /usr/libexec/hao --" line makes the
         * routed file argv[i] after "--", so a file named like an option is
         * still the program. */
        static const char letters[] = "vVisu";
        bool on[sizeof letters - 1] = {false};
        int i = 1;
        for (; i < argc && argv[i][0] == '-'; i++) {
            if (!strcmp(argv[i], "-v")) on[0] = true;
            else if (!strcmp(argv[i], "-vv")) on[0] = on[1] = true;
            else if (!strcmp(argv[i], "--interp")) on[2] = true;
            else if (!strcmp(argv[i], "--stats")) on[3] = true;
            else if (!strcmp(argv[i], "--allow-unsigned")) on[4] = true;
            else if (!strcmp(argv[i], "--")) { i++; break; }
            else usage();
        }
        if (i >= argc) usage();
        char flags[sizeof letters] = "";
        for (size_t k = 0, n = 0; k < sizeof on; k++) if (on[k]) flags[n++] = letters[k];
        char self[1100];
        if (self_path(self, sizeof self) != 0) { fprintf(stderr, "hao: cannot find own path\n"); return 126; }
        /* The guest's path as dyld would present it had the kernel run the program. */
        char abs[1100];
        dyld_abs_path(argv[i], abs, sizeof abs);
        setenv("HAO_EXEC", abs, 1);
        setenv("HAO_EXEC_FLAGS", flags, 1);
        setenv(HAO_TAG_ENV, "YES", 1);
        execv(self, argv + i);
        perror("hao: re-exec");
        return 126;
    }
    char guest_path[1100];
    snprintf(guest_path, sizeof guest_path, "%s", path);
    hao_opts_init_cli(getenv("HAO_FLAGS"), getenv("HAO_EXEC_FLAGS"));
    unsetenv("HAO_EXEC");
    unsetenv("HAO_EXEC_FLAGS");
    unsetenv("HAO_FLAGS");
    unsetenv(HAO_TAG_ENV);
    /* a hand-off stage 1 did not make may lack the switch */
    if (plain_tags() == 0) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "%s: started without " HAO_TAG_ENV " (not by `hao PROGRAM`): ObjC tagged pointers are "
                "obfuscated, which translated ObjC and Swift code does not handle\n", guest_path);
    }
    /* This process has hao's code identity, not the guest's, and nothing
     * before this point is evidence about the file: check the guest's own
     * signature here (guest_sig.c), on the descriptor run_guest hands to
     * the loader. */
    int fd = open(guest_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "open %s: %s\n", guest_path, strerror(errno));
        return 126;
    }
    if (hao_guest_sig_enforce(fd, guest_path) != 0) return 126;
    /* The kernel named this process after hao's file. Its long name (p_name:
     * proc_name, proc_pidinfo's pbi_name) is settable, as setprogname does:
     * give it the program file's own name, as an exec of the program would.
     * p_comm stays "hao". */
    char real[MAXPATHLEN];
    const char *base = fcntl(fd, F_GETPATH, real) == 0 ? real : guest_path;
    base = strrchr(base, '/') ? strrchr(base, '/') + 1 : base;
    int procname[2] = {CTL_KERN, KERN_PROCNAME};
    sysctl(procname, 2, NULL, NULL, (void *)base, strlen(base));
    return run_guest(guest_path, fd, argc, argv, envp);
}

/*
 * Embedded form: this process's main executable is a fat file whose x86_64
 * slice is the hao stub and whose arm64e slice is the program itself. The
 * kernel already recorded the program's path, name and code signature
 * (the stub slice is signed with the program's identifier and
 * entitlements, and carries the sections native code reads from the main
 * executable: tools/hao-embed CARRY), so no identity interposition is
 * needed. The ObjC tagged-pointer switch must be on before libobjc
 * initialises. A translated parent's exec passes it (process_identity.c),
 * so libobjc's obfuscator is already zero at main and the program runs in
 * the process its parent exec'd: launchd, RunningBoard and LaunchServices
 * see one exec per spawn, as natively. Otherwise (a native parent) one
 * re-exec sets it; it execs the path the kernel was given (apple[]
 * "executable_path="), so the process name and dyld's executable path stay
 * what the caller's exec made them. If that path no longer names this file
 * (a link repointed since the exec), it execs the vnode's own path as the
 * kernel resolves it, and never another file.
 *
 * A process whose credentials changed at exec (set-user-ID, set-group-ID)
 * does not re-exec: a second exec by path would hand the caller, who may
 * own a link in that path, the choice of what runs with those credentials,
 * and where issetugid() is set libobjc ignores its environment, so the
 * switch could not take effect anyway. Such a program runs with obfuscated
 * tagged pointers unless its stub is marked __hao_noreexec
 * (docs/security-model.md, limitations).
 *
 * The ARM slice is read from a descriptor proven to be the file the kernel
 * is running (self_vnode), never from whatever the executable's path names
 * by the time it is opened.
 */
/*
 * PID 1 (launchd) starts with no descriptors: hao's own diagnostics go to
 * the console on a high close-on-exec descriptor. Its switches come from the
 * boot arguments (hao_opts_parse: hao=FLAGS with v/V/i/s as for the CLI,
 * hao_verbose=N, and hao_<knob>=1 as in every process).
 */
static void pid1_setup(void)
{
    if (fcntl(2, F_GETFD) == -1) {
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (fd >= 0) {
            int hi = fcntl(fd, F_DUPFD_CLOEXEC, 200);
            if (hi >= 0) { close(fd); fd = hi; }
            FILE *f = fdopen(fd, "w");
            if (f) { setvbuf(f, NULL, _IOLBF, 0); stderr = f; }
        }
    }
}

static void pid1_report(void)
{
    char on[256] = "";
    static const struct { const char *n; const bool *v; } k[] = {
        {"nx_stats", &hao_opt.nx_stats}, {"crash_trace", &hao_opt.crash_trace}, {"jit_helpers_only", &hao_opt.jit_helpers_only},
        {"no_aot", &hao_opt.no_aot}, {"no_chain", &hao_opt.no_chain}, {"no_msgsend", &hao_opt.no_msgsend},
        {"no_closures", &hao_opt.no_closures}, {"no_swiftreg", &hao_opt.no_swiftreg},
        {"no_block_thunks", &hao_opt.no_block_thunks}, {"aot_learn", &hao_opt.aot_learn},
        {"no_libval", &hao_opt.no_libval}, {"bridge_stats", &hao_opt.bridge_stats}, {"bridge_check", &hao_opt.bridge_check},
        {"no_ifunits", &hao_opt.no_ifunits}, {"libval_user", &hao_opt.libval_user}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (*k[i].v) snprintf(on + strlen(on), sizeof on - strlen(on), "%s%s", on[0] ? "," : "", k[i].n);
    fprintf(stderr, "hao: PID 1 running translated (verbose=%d jit=%d opts=%s)\n", hao_opt.verbose, !hao_opt.interp,
            on[0] ? on : "none");
}

int hao_self_main(int argc, char **argv, char **envp, char **apple)
{
    char exe[1100], vpath[MAXPATHLEN], self[64];
    int pid1 = getpid() == 1;
    if (pid1) pid1_setup();
    uint32_t esz = sizeof exe;
    if (_NSGetExecutablePath(exe, &esz) != 0) {
        fprintf(stderr, "hao: cannot find own path\n");
        return 126;
    }
    dev_t dev = 0;
    ino_t ino = 0;
    if (!self_vnode(apple, &dev, &ino, vpath, sizeof vpath)) {
        fprintf(stderr, "hao: %s: the kernel does not say which file this process runs; refusing to run\n", exe);
        return 126;
    }
    /* the re-exec's loop guard: this file, whatever path reaches it */
    snprintf(self, sizeof self, "%llu:%llu", (unsigned long long)dev, (unsigned long long)ino);
    const char *kpath = exe;
    for (char **a = apple; a && *a; a++)
        if (!strncmp(*a, "executable_path=", 16) && (*a)[16]) { kpath = *a + 16; break; }
    const char *stage = getenv("HAO_SELF");
    /* Programs that do not use ObjC (no libobjc/CoreFoundation/Foundation;
     * hao-embed marks their stub with __TEXT,__hao_noreexec) need no
     * tagged-pointer switch, and their re-exec would look to launchd like the
     * spawn of an xpcproxy-managed service. */
    unsigned long marker = 0;
    int noreexec = getsectiondata((const struct mach_header_64 *)_NSGetMachExecuteHeader(), "__TEXT",
                                  "__hao_noreexec", &marker) != NULL;
    /* 1: the exec passed the switch and libobjc took it: no re-exec. */
    int tags = plain_tags();
    bool setid = issetugid() || getuid() != geteuid() || getgid() != getegid();
    /* PID 1 does not re-exec: dyld's PID-1 bootstrap (Preboot, cryptex
     * graft) runs once, and launchd itself does not touch ObjC objects. */
    if (!pid1 && !noreexec && !setid && tags != 1) {
        if (!stage || strcmp(stage, self) != 0) {
            const char *target = names_vnode(kpath, dev, ino) ? kpath : names_vnode(vpath, dev, ino) ? vpath : NULL;
            int e = ENOENT;
            if (target) {
                setenv("HAO_SELF", self, 1);
                setenv(HAO_TAG_ENV, "YES", 1);
                extern char **environ;
                execve(target, argv, environ);
                e = errno;
            }
            /* exec refused (e.g. resource limits), or no path names this file any more: run with obfuscated tags */
            fprintf(stderr, "hao: %s: re-exec failed (%s); ObjC tagged pointers stay obfuscated, which translated "
                    "ObjC and Swift code does not handle\n", exe, strerror(e));
        } else if (tags == 0) {
            /* re-exec'd (or started with HAO_SELF naming this file), and libobjc still obfuscates */
            fprintf(stderr, "hao: %s: ObjC tagged pointers are still obfuscated after the re-exec with " HAO_TAG_ENV
                    ", which translated ObjC and Swift code does not handle\n", exe);
        }
    }
    unsetenv("HAO_SELF");
    unsetenv(HAO_TAG_ENV);
    hao_opts_init(getenv("HAO_FLAGS"));
    if (pid1) pid1_report();
    /* exe is the path the exec was given, made absolute. What it names now
     * need not be what the kernel started: load only this process's own file. */
    int fd = open(exe, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "open %s: %s\n", exe, strerror(errno));
        return 126;
    }
    if (fstat(fd, &st) != 0 || st.st_dev != dev || st.st_ino != ino) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "%s no longer names the file this process runs; refusing to load its ARM code from another file\n", exe);
        return 126;
    }
    return run_guest(exe, fd, argc, argv, envp);
}

/* fd: the open main executable, checked by the caller; the loader takes it. */
static int run_guest(const char *path, int fd, int argc, char **argv, char **envp)
{
    g_argc = argc; g_argv = argv; g_envp = envp;
    hao_opts_init(NULL);          /* the front end already took the snapshot */
    hao_verbose = hao_opt.verbose;
    if (hao_opt.interp) hao_use_jit = 0;
    hao_fork_init();

    hao_install_fault_handler();
    extern void hao_commpage_init(void);
    hao_commpage_init();
    if (hao_opt.watchdog) { signal(SIGALRM, watchdog); alarm(hao_opt.watchdog); }
    hao_interpose_init();
    hao_bridge_init();            /* plan table, interface units, hao_bridge_stats */
    /* The code cache (translated code and the native-callable thunks) is
     * mapped before the guest runs, also for --interp: a sandbox the guest
     * applies later may deny dynamic code. Without it hao_use_jit drops to 0. */
    hao_jit_init();
    /* images with Swift metadata are mapped with room for their island entries (macho.c) */
    hao_island_reserve = !hao_opt.no_swift_thunks && hao_have_thunks();
    /* generated thunk pairs: before the main image sets library validation */
    hao_thunks_init();

    /* Publish the resolver before loading: a native initializer can query a
     * just-mapped guest dependency before its application entry point runs. */
    void (*identity_lookup)(hao_identity_lookup_fn) =
        (void (*)(hao_identity_lookup_fn))dlsym(RTLD_DEFAULT, "hao_identity_set_image_lookup");
    if (identity_lookup) identity_lookup(hao_identity_guest_image);
    /* the real path of the file being loaded: the kernel's path for this
     * descriptor, not a second resolution of PATH (the loader closes fd) */
    char err[512], real[MAXPATHLEN];
    if (fcntl(fd, F_GETPATH, real) != 0 && !realpath(path, real)) snprintf(real, sizeof real, "%s", path);
    hao_image *img = hao_load_main_image(path, fd, err, sizeof err);
    if (!img) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "%s\n", err); return 126; }
    /* PATH is absolute and unresolved, as dyld presents the main executable */
    extern char hao_guest_exec_path[1024], hao_guest_image_path[1024];
    snprintf(hao_guest_exec_path, sizeof hao_guest_exec_path, "%s", path);
    snprintf(hao_guest_image_path, sizeof hao_guest_image_path, "%s", real);
    /* native lookups on the main executable's header fall back to the guest's
     * mapped image (src/process_identity.c, linked by hao and hao-stub) */
    void (*identity_image)(const void *) = (void (*)(const void *))dlsym(RTLD_DEFAULT, "hao_identity_image");
    if (identity_image) identity_image((const void *)(uintptr_t)img->base);
    /* os_log records whose dso is a guest image are composed in process (src/oslog.c) */
    void (*oslog_hook)(int (*)(const void *), int (*)(const void *, size_t)) =
        (void (*)(int (*)(const void *), int (*)(const void *, size_t)))dlsym(RTLD_DEFAULT, "hao_identity_oslog_hook");
    if (oslog_hook) oslog_hook(oslog_is_guest, oslog_immutable);
    hao_native_bundles_init();
    if (hao_verbose)
        fprintf(stderr, "hao: loaded %s at %#llx (slide %#llx): %llu rebases, %llu binds, %llu auth, %llu unresolved\n",
                path, (unsigned long long)img->base, (unsigned long long)img->slide, (unsigned long long)img->n_rebase,
                (unsigned long long)img->n_bind, (unsigned long long)img->n_auth, (unsigned long long)img->n_unresolved);
    if (hao_verbose) {
        static const char *const lv[] = {"undecided", "off", "on (kernel)", "on (guest images platform only)"};
        int m = hao_libval_mode();
        fprintf(stderr, "hao: library validation %s (%s)\n", lv[m >= 0 && m <= 3 ? m : 0], hao_libval_reason());
    }
    if (!img->entry) { fprintf(stderr, "hao: %s has no LC_MAIN entry point\n", path); return 126; }

    /* getprogname is the basename of argv[0], as libc's startup made it for
     * this argv; an empty argv (argc 0, argv[0] NULL) is legal: name the
     * process after the file. Not setprogname(), which also renames the
     * process in the kernel (KERN_PROCNAME): natively that name stays the
     * file's own. */
    const char *name = argc > 0 && argv[0] ? argv[0] : path;
    *_NSGetProgname() = (char *)(strrchr(name, '/') ? strrchr(name, '/') + 1 : name);
    if (hao_opt.stats) { atexit(print_stats); extern void hao_nx_stats_atexit(void); hao_nx_stats_atexit(); }

    static char exe_path[1100];
    snprintf(exe_path, sizeof exe_path, "executable_path=%s", path);
    g_apple[0] = exe_path;

    /* guest dylibs bottom-up, then the main executable */
    hao_run_initializers();
    uint64_t args[4] = {(uint64_t)argc, (uint64_t)(uintptr_t)argv, (uint64_t)(uintptr_t)envp, (uint64_t)(uintptr_t)g_apple};
    uint64_t rc = hao_call_guest(hao_current(), img->entry, args, 4);
    exit((int)rc);
}
