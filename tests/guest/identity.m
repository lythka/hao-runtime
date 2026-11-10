/*
 * Every way native code identifies the main executable, printed as key=value
 * lines (tests/guest/identity.sh diffs them against the native arm64 run of
 * the same file). Values that legitimately differ per run (pids, addresses,
 * cdhashes) are not printed; data is printed as its size and FNV-1a hash.
 *
 * The file is linked with an embedded Info.plist (__TEXT,__info_plist), a
 * marker segment like WindowServer's __SLSERVER (__HAOID,__haoid), an empty
 * marker like __CGPreLoginApp/__RESTRICT (__HAOMARK,__haomark), a pointer
 * section (__DATA_CONST,__haocmds, below), sections no hao front end has
 * or carries (below: __TEXT,__haotxt, __TEXT,__swift5_entry,
 * __DATA_CONST,__haodc, __DATA,__haodata), and is
 * signed with an identifier and a public entitlement (identity.sh). Lines
 * without a prefix are what translated code sees; "native." lines come from
 * identhelper.c, a native library, and are what native frameworks see.
 */
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <bsm/libbsm.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <xpc/xpc.h>

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
extern const struct mach_header *_dyld_get_prog_image_header(void);
extern const char *dyld_image_path_containing_address(const void *addr);
extern xpc_object_t xpc_copy_entitlement_for_self(const char *);
void identhelper_print(void);   /* identhelper.c, a native library under hao */

/* A linker set (os/linker_set.h) that native code reads from the main
 * executable, as libsystem_darwin's os_subcommand_main reads __subcommands
 * (27.0 devmodectl, trustcachectl, cryptexctl.research): pointers to
 * entries, in __DATA_CONST on arm64 and in __DATA on x86_64. */
struct haocmd { const char *name; int (*fn)(int); };
static int twice(int x) { return 2 * x; }
static const struct haocmd twice_cmd = {"twice", twice};
__attribute__((used, section("__DATA_CONST,__haocmds"))) static const struct haocmd *const haocmds[] = {&twice_cmd};

/* Sections only the guest has: neither hao nor hao-stub has them and
 * hao-embed does not carry them (it carries a few __TEXT sections and
 * non-toolchain segments). Native code reads them from the main header with
 * getsectiondata/getsegmentdata (identhelper.c), as ExtensionFoundation's
 * +[_EXUtil _getEntryPointFunction] reads __TEXT,__swift5_entry: what swiftc
 * emits for @main, a 32-bit relative pointer to the entry function, which
 * it then calls. */
__attribute__((used, section("__TEXT,__haotxt"))) static const char haotxt[] = "guest-only __TEXT section";
static const char haodc_str[] = "reached through __DATA_CONST,__haodc";
__attribute__((used, section("__DATA_CONST,__haodc"))) static const char *const haodc[] = {haodc_str};
__attribute__((used, section("__DATA,__haodata"))) static char haodata[] = "guest-only __DATA section";
__attribute__((used)) int identity_entry(int x) { return 3 * x + 1; }
__asm__(".section __TEXT,__swift5_entry\n.p2align 2\nl_identity_entry: .long _identity_entry - l_identity_entry\n.text\n");

static void kv(const char *k, const char *v) { printf("%s=%s\n", k, v ? v : "(null)"); }
static void kvs(const char *k, NSString *v) { kv(k, v ? v.UTF8String : NULL); }
static const char *yesno(int b) { return b ? "yes" : "no"; }

static void kdata(const char *k, const void *p, unsigned long n)
{
    if (!p) { kv(k, "(none)"); return; }
    uint32_t h = 2166136261u;
    for (unsigned long i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    printf("%s=%lu bytes fnv %08x\n", k, n, h);
}

/* A section found through a header (by hand, as CF and libsystem do): its
 * data, located with the slide of that header's own __TEXT. */
static const struct section_64 *sect(const struct mach_header_64 *mh, const char *seg, const char *name)
{
    return getsectbynamefromheader_64(mh, seg, name);
}
static intptr_t slide_of(const struct mach_header_64 *mh)
{
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT"))
            return (intptr_t)mh - (intptr_t)((const struct segment_command_64 *)lc)->vmaddr;
    return 0;
}

static void sections(const char *via, const struct mach_header_64 *mh)
{
    static const char *const want[][2] = {{"__TEXT", "__info_plist"}, {"__HAOID", "__haoid"}, {"__HAOMARK", "__haomark"},
                                          {"__TEXT", "__haotxt"}, {"__DATA", "__haodata"}};
    char k[128];
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        unsigned long n = 0;
        uint8_t *d = mh ? getsectiondata(mh, want[i][0], want[i][1], &n) : NULL;
        const struct section_64 *s = mh ? sect(mh, want[i][0], want[i][1]) : NULL;
        snprintf(k, sizeof k, "%s.getsectiondata(%s,%s)", via, want[i][0], want[i][1]);
        if (d || n || s) kdata(k, d ? d : (void *)"", n); else kv(k, "(none)");
        snprintf(k, sizeof k, "%s.getsectbynamefromheader_64(%s,%s)+slide", via, want[i][0], want[i][1]);
        if (s) kdata(k, (const void *)(uintptr_t)(s->addr + slide_of(mh)), s->size); else kv(k, "(none)");
    }
}

int main(int argc, char **argv) { @autoreleasepool {
    pid_t pid = getpid();
    /* process name and arguments */
    kv("argv[0]", argc > 0 ? argv[0] : NULL);
    kv("_NSGetArgv[0]", *_NSGetArgc() > 0 ? (*_NSGetArgv())[0] : NULL);
    kv("getprogname", getprogname());
    kv("_NSGetProgname", *_NSGetProgname());
    char buf[PATH_MAX * 2]; uint32_t n = sizeof buf;
    kv("_NSGetExecutablePath", _NSGetExecutablePath(buf, &n) == 0 ? buf : "(error)");
    char pp[PROC_PIDPATHINFO_MAXSIZE];
    kv("proc_pidpath", proc_pidpath(pid, pp, sizeof pp) > 0 ? pp : "(error)");
    char nm[2 * MAXCOMLEN + 1];
    kv("proc_name", proc_name(pid, nm, sizeof nm) > 0 ? nm : "(error)");
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == sizeof bi) { kv("pbi_comm", bi.pbi_comm); kv("pbi_name", bi.pbi_name); }
    struct kinfo_proc kp; size_t kl = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    kv("kinfo_proc.p_comm", sysctl(mib, 4, &kp, &kl, NULL, 0) == 0 ? kp.kp_proc.p_comm : "(error)");
    static char args[1 << 16]; size_t al = sizeof args; int mib2[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    if (sysctl(mib2, 3, args, &al, NULL, 0) == 0 && al > sizeof(int)) {
        const char *exe = args + sizeof(int), *p = exe + strlen(exe);
        while (p < args + al && !*p) p++;
        kv("KERN_PROCARGS2.exec_path", exe);
        kv("KERN_PROCARGS2.argv[0]", p < args + al ? p : NULL);
    }

    /* Foundation and CoreFoundation */
    NSProcessInfo *pi = NSProcessInfo.processInfo;
    kvs("NSProcessInfo.processName", pi.processName);
    kvs("NSProcessInfo.arguments[0]", pi.arguments.count ? pi.arguments[0] : nil);
    NSBundle *mb = NSBundle.mainBundle;
    kvs("NSBundle.mainBundle.bundlePath", mb.bundlePath);
    kvs("NSBundle.mainBundle.executablePath", mb.executablePath);
    kvs("NSBundle.mainBundle.bundleIdentifier", mb.bundleIdentifier);
    kvs("NSBundle.mainBundle[HaoIdentityKey]", [mb objectForInfoDictionaryKey:@"HaoIdentityKey"]);
    kvs("NSBundle.mainBundle.infoDictionary.keys",
        [[mb.infoDictionary.allKeys sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@","]);
    CFBundleRef cb = CFBundleGetMainBundle();
    kvs("CFBundleGetIdentifier(main)", (__bridge NSString *)CFBundleGetIdentifier(cb));
    CFURLRef eu = CFBundleCopyExecutableURL(cb);
    kvs("CFBundleCopyExecutableURL(main)", eu ? ((__bridge NSURL *)eu).path : nil);
    if (eu) CFRelease(eu);
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(cb, CFSTR("HaoIdentityKey"));
    kvs("CFBundleGetValueForInfoDictionaryKey(main,HaoIdentityKey)", (__bridge NSString *)v);

    /* dyld's view of the main executable */
    const struct mach_header_64 *exe = (const void *)_NSGetMachExecuteHeader();
    const struct mach_header_64 *img0 = (const void *)_dyld_get_image_header(0);
    kv("_dyld_get_image_name(0)", _dyld_get_image_name(0));
    kv("_dyld_get_image_header(0)==_NSGetMachExecuteHeader()", yesno(img0 == exe));
    kv("_dyld_get_prog_image_header()==_NSGetMachExecuteHeader()", yesno((const void *)_dyld_get_prog_image_header() == exe));
    kv("_NSGetMachExecuteHeader()->filetype==MH_EXECUTE", yesno(exe && exe->filetype == MH_EXECUTE));
    kv("_dyld_get_image_vmaddr_slide(0)==slide(header 0)", yesno(_dyld_get_image_vmaddr_slide(0) == slide_of(img0)));
    Dl_info di;
    kv("dladdr(_NSGetMachExecuteHeader()).dli_fname", dladdr(exe, &di) ? di.dli_fname : "(none)");
    kv("dladdr(main).dli_fname", dladdr((const void *)main, &di) ? di.dli_fname : "(none)");
    kv("dyld_image_path_containing_address(main)", dyld_image_path_containing_address((const void *)main));
    sections("_NSGetMachExecuteHeader", exe);
    sections("_dyld_get_image_header(0)", img0);
    /* libmacho's implicit main-executable lookups */
    unsigned long sz = 0;
    kv("getsectdata(__TEXT,__info_plist)", getsectdata("__TEXT", "__info_plist", &sz) ? "found" : "(none)");
    printf("getsectdata(__TEXT,__info_plist).size=%lu\n", sz);
    const struct section_64 *s = getsectbyname("__HAOID", "__haoid");
    if (s) kdata("getsectbyname(__HAOID,__haoid)+slide", (const void *)(uintptr_t)(s->addr + slide_of(exe)), s->size);
    else kv("getsectbyname(__HAOID,__haoid)+slide", "(none)");
    kv("getsectbyname(__HAOMARK,__haomark)", getsectbyname("__HAOMARK", "__haomark") ? "found" : "(none)");
    kv("getsegbyname(__HAOID)", getsegbyname("__HAOID") ? "found" : "(none)");
    kv("getsegbyname(__TEXT)", getsegbyname("__TEXT") ? "found" : "(none)");

    /* code-signing identity and entitlements */
    uint32_t st = 0;
    if (csops(pid, 0 /* CS_OPS_STATUS */, &st, sizeof st) == 0)
        printf("csops.status: valid=%s adhoc=%s get-task-allow=%s platform=%s\n", yesno(st & 0x1), yesno(st & 0x2), yesno(st & 0x4),
               yesno(st & 0x4000000));
    struct { uint32_t magic, len; char id[256]; } idb = {0};
    kv("csops.identity", csops(pid, 11 /* CS_OPS_IDENTITY */, &idb, sizeof idb) == 0 ? idb.id : "(error)");
    SecTaskRef task = SecTaskCreateFromSelf(NULL);
    CFStringRef sid = task ? SecTaskCopySigningIdentifier(task, NULL) : NULL;
    kvs("SecTaskCopySigningIdentifier", (__bridge NSString *)sid);
    CFTypeRef ent = task ? SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.get-task-allow"), NULL) : NULL;
    kv("SecTaskCopyValueForEntitlement(get-task-allow)", ent ? (CFEqual(ent, kCFBooleanTrue) ? "true" : "other") : "(none)");
    xpc_object_t xe = xpc_copy_entitlement_for_self("com.apple.security.get-task-allow");
    kv("xpc_copy_entitlement_for_self(get-task-allow)", xe ? (xpc_bool_get_value(xe) ? "true" : "false") : "(none)");
    SecCodeRef code = NULL;
    CFDictionaryRef info = NULL;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &code) == errSecSuccess) {
        SecStaticCodeRef sc = NULL;
        if (SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &sc) == errSecSuccess)
            SecCodeCopySigningInformation(sc, kSecCSSigningInformation, &info);
    }
    kvs("SecCodeCopySelf.identifier", info ? (__bridge NSString *)CFDictionaryGetValue(info, kSecCodeInfoIdentifier) : nil);
    NSURL *mx = info ? (__bridge NSURL *)CFDictionaryGetValue(info, kSecCodeInfoMainExecutable) : nil;
    kvs("SecCodeCopySelf.mainExecutable", mx.path);

    /* the audit token the kernel gives this task (what peers check) */
    audit_token_t tok; mach_msg_type_number_t cnt = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&tok, &cnt) == KERN_SUCCESS) {
        kv("audit_token.pid==getpid", yesno(audit_token_to_pid(tok) == pid));
        kv("proc_pidpath(audit_token)", proc_pidpath(audit_token_to_pid(tok), pp, sizeof pp) > 0 ? pp : "(error)");
        SecTaskRef tt = SecTaskCreateWithAuditToken(NULL, tok);
        CFStringRef tid = tt ? SecTaskCopySigningIdentifier(tt, NULL) : NULL;
        kvs("SecTaskCreateWithAuditToken.identifier", (__bridge NSString *)tid);
    }
    fflush(stdout);
    identhelper_print();
} return 0; }
