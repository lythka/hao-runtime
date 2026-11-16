/*
 * hao — run an arm64/arm64e Mach-O on x86_64 with native x86_64 libraries.
 *   hao [-v] [--interp] PROGRAM [ARGS...]
 */
#include "hao/runtime.h"
#include "hao/swiftvalue.h"
#include "hao/identity.h"
#include "hao/native_bundle.h"
#include "hao/bridge.h"
#include <cpuid.h>
#include <crt_externs.h>
#include <sys/sysctl.h>
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
                    "       hao --aot [--check] [--cache DIR] [--] [FILE...] | hao --aot --auto\n"
                    "(internal: use `hao run` / `hao aot`)\n");
    exit(64);
}

#include <signal.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
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
extern bool hao_ids_prepare(hao_image *img);
void hao_guest_image_init(hao_image *img)
{
    bool prepared = hao_ids_prepare(img);
    hao_tlv_setup(img);
    /* Swift records first: ObjC registration runs the metadata
     * initializers of Swift classes laid out at run time (objc.c), which
     * may look up this image's types and conformances. */
    extern void hao_swift_register_image(hao_image *);
    hao_swiftmeta_image(img);   /* relative pointers to guest functions -> island thunks, before the runtime reads them */
    hao_swift_register_image(img);
    hao_swift_witness_setup();   /* the Optional witnesses guest calls are recognized by (swiftvalue.c) */
    hao_objc_register_image(img);
    hao_blocks_register_image(img);
    hao_fnslots_image(img);
    /* HAO_DIAG_TIME: "registered NAME" covers the steps above and the previous image's initializers */
    char what[160];
    const char *base = strrchr(img->path, '/') ? strrchr(img->path, '/') + 1 : img->path;
    if (hao_opt.diag_time) { snprintf(what, sizeof what, "registered %s", base); hao_phase(what); }
    if (hao_use_jit) {
        if (!prepared) hao_aot_load(img);
        if (hao_opt.diag_time) { snprintf(what, sizeof what, "aot %s", base); hao_phase(what); }
    }
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

/* The default set of `hao --aot` with no FILE: the image's list of programs
 * (/usr/lib/hao/aot-list, absolute paths, one per line), else the paths of
 * its embedded programs (the fourth field of /usr/lib/hao/stub-identities).
 * Either file must be root's and writable by no one else. */
static FILE *aot_open_list(const char *path, uid_t owner)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != owner || (st.st_mode & 022)) {
        fprintf(stderr, "hao-aot: %s is not a file of uid %u writable only by it: not used\n", path, (unsigned)owner);
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) close(fd);
    return f;
}

static int aot_default(const char *cache, bool check)
{
    hao_opts_init(NULL);
    const char *dir = hao_opt.aot_list_dir[0] ? hao_opt.aot_list_dir : "/usr/lib/hao";
    uid_t owner = hao_opt.aot_list_dir[0] ? geteuid() : 0;
    char lpath[1100], spath[1100];
    snprintf(lpath, sizeof lpath, "%s/aot-list", dir);
    snprintf(spath, sizeof spath, "%s/stub-identities", dir);
    bool stubs = false;
    FILE *f = aot_open_list(lpath, owner);
    if (!f && (f = aot_open_list(spath, owner))) stubs = true;
    if (!f) { fprintf(stderr, "hao-aot: no FILE given and neither %s nor %s is usable\n", lpath, spath); return 1; }
    char self[1100];
    if (self_path(self, sizeof self) != 0) { fprintf(stderr, "hao-aot: cannot find this program's path\n"); fclose(f); return 1; }
    char **paths = NULL, *line = NULL;
    size_t cap = 0, np = 0, pcap = 0;
    ssize_t len;
    int missing = 0;
    while ((len = getline(&line, &cap, f)) > 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *p = line;
        if (!p[0] || p[0] == '#') continue;
        if (stubs) {
            for (int k = 0; k < 3; k++) {
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
            }
            if (!*p) continue;
        }
        char path[PATH_MAX];
        struct stat st;
        if (snprintf(path, sizeof path, "%s%s", p[0] == '/' ? "" : "/", p) >= (int)sizeof path || lstat(path, &st) != 0 ||
            !S_ISREG(st.st_mode)) { missing++; continue; }
        if (np == pcap) {
            char **q = realloc(paths, (pcap = pcap ? 2 * pcap : 1024) * sizeof *paths);
            if (!q) { fprintf(stderr, "hao-aot: out of memory\n"); return 1; }
            paths = q;
        }
        if (!(paths[np] = strdup(path))) { fprintf(stderr, "hao-aot: out of memory\n"); return 1; }
        np++;
    }
    free(line);
    fclose(f);
    int jobs = 1;
    size_t jl = sizeof jobs;
    if (sysctlbyname("hw.activecpu", &jobs, &jl, NULL, 0) != 0 || jobs < 1) jobs = 1;
    if (jobs > 4) jobs = 4;
    if (check) fprintf(stderr, "hao-aot: no FILE given: checking the %zu programs %s names\n", np, stubs ? spath : lpath);
    else fprintf(stderr, "hao-aot: no FILE given: the %zu programs %s names, %d at a time\n", np, stubs ? spath : lpath, jobs);
    int running = 0, ok = 0, failed = 0, valid = 0, absent = 0, stale = 0;
    for (size_t i = 0; i <= np; i++) {
        if (i < np) {
            int r = hao_aot_check_file(paths[i], cache, false);
            if (check) {
                const char *what = r == 0 ? "valid" : r == 3 ? "missing" : r == 4 ? "stale" : "not checkable";
                fprintf(stderr, "hao-aot: [%zu/%zu] %s: %s\n", i + 1, np, paths[i], what);
                if (r == 0) valid++; else if (r == 3) absent++; else if (r == 4) stale++; else failed++;
                continue;
            }
            if (r == 0) { valid++; fprintf(stderr, "hao-aot: [%zu/%zu] %s: valid, skipped\n", i + 1, np, paths[i]); continue; }
            fprintf(stderr, "hao-aot: [%zu/%zu] %s\n", i + 1, np, paths[i]);
            char *av[7];
            int n = 0;
            av[n++] = self;
            av[n++] = "--aot";
            if (cache) { av[n++] = "--cache"; av[n++] = (char *)cache; }
            av[n++] = "--";
            av[n++] = paths[i];
            av[n] = NULL;
            pid_t pid;
            if (posix_spawn(&pid, self, NULL, NULL, av, *_NSGetEnviron()) != 0) failed++;
            else running++;
        }
        while (running > 0 && (running >= jobs || i == np)) {
            int status;
            pid_t w = wait(&status);
            if (w < 0) { if (errno == EINTR) continue; running = 0; break; }
            running--;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) ok++;
            else failed++;
        }
    }
    for (size_t i = 0; i < np; i++) free(paths[i]);
    free(paths);
    if (check) {
        fprintf(stderr, "hao-aot: %d valid, %d missing, %d stale, %d not checkable, %d listed but not present\n", valid, absent, stale,
                failed, missing);
        return (absent || stale || failed) ? 1 : 0;
    }
    fprintf(stderr, "hao-aot: %d programs cached, %d already valid, %d failed, %d listed but not present\n", ok, valid, failed, missing);
    return failed ? 1 : 0;
}

/* hao --aot --auto (the hao.aot LaunchDaemon, root): once a user is logged in, the default set, then a marker
 * naming this libhao's generation, so it runs again only after libhao changes (an update). Under HAO_AOT_LIST_DIR
 * (tests) the marker is kept there, any user may run it, and it does not wait. */
static int aot_auto(void)
{
    hao_opts_init(NULL);
    bool test = hao_opt.aot_list_dir[0] != 0;
    if (!test && geteuid() != 0) { fprintf(stderr, "hao-aot: --auto runs as root\n"); return 1; }
    uint8_t g[16];
    if (!hao_aot_generation(g)) { fprintf(stderr, "hao-aot: this hao build has no LC_UUID\n"); return 1; }
    char want[64], have[64] = "", marker[1100], tmp[1200];
    int l = snprintf(want, sizeof want, "libhao ");
    for (int k = 0; k < 16; k++) l += snprintf(want + l, sizeof want - (size_t)l, "%02x", g[k]);
    snprintf(marker, sizeof marker, "%s/auto-aot.done", test ? hao_opt.aot_list_dir : HAO_AOT_SYS_DIR);
    int fd = open(marker, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, have, sizeof have - 1);
        close(fd);
        have[n > 0 ? n : 0] = 0;
        char *nl = strchr(have, '\n');
        if (nl) *nl = 0;
        if (!strcmp(have, want)) { fprintf(stderr, "hao-aot: --auto: done already for %s\n", want); return 0; }
    }
    if (!test) {
        struct stat st;
        while (stat("/dev/console", &st) != 0 || st.st_uid < 501) sleep(30);
        sleep(120);   /* the first login's own start-up first */
    }
    fprintf(stderr, "hao-aot: --auto: start for %s\n", want);
    int rc = aot_default(NULL, false);
    snprintf(tmp, sizeof tmp, "%s.tmp", marker);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) { fprintf(stderr, "hao-aot: --auto: %s: %s\n", tmp, strerror(errno)); return 1; }
    char line[96];
    int m = snprintf(line, sizeof line, "%s\n", want);
    bool ok = write(fd, line, (size_t)m) == m;
    if (close(fd) != 0 || !ok || rename(tmp, marker) != 0) { fprintf(stderr, "hao-aot: --auto: cannot write %s\n", marker); unlink(tmp); return 1; }
    fprintf(stderr, "hao-aot: --auto: done for %s%s\n", want, rc ? " (some programs failed; see above)" : "");
    return rc;
}

static int aot_main(int argc, char **argv)
{
    /* hao --aot [--cache DIR] [--] BINARY...: generate AOT caches (no execution).
     * --cache names the output directory on the command line: a guarded
     * process (root, for one) ignores HAO_CACHE, and would otherwise write
     * into the system directory of the machine it runs on. */
    int bad = 0, k = 2;
    const char *cache = NULL;
    bool check = false;
    if (argc == 3 && !strcmp(argv[k], "--auto")) return aot_auto();
    if (k < argc && !strcmp(argv[k], "--check")) { check = true; k++; }
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
    if (k >= argc) return aot_default(cache, check);
    hao_opts_init(NULL);
    hao_verbose = 1;
    if (check) {
        int worst = 0;
        for (; k < argc; k++) {
            int r = hao_aot_check_file(argv[k], cache, true);
            if (r < 0) worst = 1;
            else if (r > worst && worst != 1) worst = r;
        }
        return worst;
    }
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
    hao_phase_mark();
    if (argc >= 2 && !strcmp(argv[1], "--aot")) return aot_main(argc, argv);
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
    hao_phase_mark();
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
/* The CPU profile (docs/releases.md): libhao and translated code use SSSE3, SSE4.1, SSE4.2, POPCNT
 * and CMPXCHG16B (x86-64-v2), and 128-bit atomics rely on movdqa being single-copy atomic, which
 * Intel guarantees on CPUs that enumerate AVX. Rosetta enumerates no AVX; it runs the x86-64-v2
 * subset and is where the tests run, so AVX is required on Intel hardware only. */
static const char *cpu_profile_missing(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return "CPUID leaf 1";
    if (!(d & bit_SSE2)) return "SSE2";
    if (!(c & bit_SSE3)) return "SSE3";
    if (!(c & bit_SSSE3)) return "SSSE3";
    if (!(c & bit_CMPXCHG16B)) return "CMPXCHG16B";
    if (!(c & bit_SSE41)) return "SSE4.1";
    if (!(c & bit_SSE42)) return "SSE4.2";
    if (!(c & bit_POPCNT)) return "POPCNT";
    int translated = 0;
    size_t n = sizeof translated;
    if (sysctlbyname("sysctl.proc_translated", &translated, &n, NULL, 0) != 0) translated = 0;
    if (!translated && !(c & bit_AVX)) return "AVX";
    return NULL;
}

static int run_guest(const char *path, int fd, int argc, char **argv, char **envp)
{
    const char *missing = cpu_profile_missing();
    if (missing) {
        fprintf(stderr, "hao: this CPU lacks %s; hao needs an Intel CPU with AVX (Sandy Bridge or later)\n", missing);
        return 126;
    }
    g_argc = argc; g_argv = argv; g_envp = envp;
    hao_opts_init(NULL);          /* the front end already took the snapshot */
    hao_verbose = hao_opt.verbose;
    if (hao_opt.interp) hao_use_jit = 0;
    hao_fork_init();

    hao_install_fault_handler();
    hao_phase_begin();
    extern void hao_commpage_init(void);
    hao_commpage_init();
    if (hao_opt.watchdog) { signal(SIGALRM, watchdog); alarm(hao_opt.watchdog); }
    hao_bridge_init();            /* plan table, interface units, hao_bridge_stats */
    hao_phase("bridge");
    /* The code cache (translated code and the native-callable thunks) is
     * mapped before the guest runs, also for --interp: a sandbox the guest
     * applies later may deny dynamic code. Without it hao_use_jit drops to 0. */
    hao_jit_init();
    hao_phase("jit");
    /* images with Swift metadata are mapped with room for their island entries (macho.c) */
    hao_island_reserve = !hao_opt.no_swift_thunks && hao_have_thunks();
    /* generated thunk pairs: before the main image sets library validation */
    hao_thunks_init();
    hao_phase("thunks");

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
    hao_phase("load");
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
    hao_phase("bound");
    hao_run_initializers();
    hao_phase("main");
    uint64_t args[4] = {(uint64_t)argc, (uint64_t)(uintptr_t)argv, (uint64_t)(uintptr_t)envp, (uint64_t)(uintptr_t)g_apple};
    uint64_t rc = hao_call_guest(hao_current(), img->entry, args, 4);
    exit((int)rc);
}
