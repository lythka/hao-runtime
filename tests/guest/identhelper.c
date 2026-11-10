/*
 * Native side of tests/guest/identity.m: the main executable as native
 * libraries see it, read the ways they read it (libmacho's implicit lookups
 * as SkyLight and FuseBoardServices use them, CoreFoundation's
 * _CFBundleGetSectData walk over dyld's image list, ExtensionFoundation's
 * read of the Swift entry point). Built for x86_64 (hao loads it natively)
 * and arm64/arm64e (the native reference run).
 */
#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <ptrauth.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
extern intptr_t _dyld_get_image_slide(const struct mach_header *mh);   /* libdyld (dyld_priv.h) */
extern const struct mach_header *_dyld_get_prog_image_header(void);    /* libdyld (dyld_priv.h) */

static const char *same(const void *p, const void *q) { return p && p == q ? "yes" : "no"; }

/* The slid getters on the main executable's header from this library's
 * initializer, before main: in the CLI route before libhao has mapped the
 * guest (the copies carried from its file answer), in the embedded route
 * from the stub's own copies. Printed by identhelper_print. (The segment's
 * size is not printed: the stub's copy of __HAOID is one x86_64 page.) */
static char early[512];
__attribute__((constructor)) static void early_lookups(void)
{
    const struct mach_header_64 *mh = (const void *)_dyld_get_prog_image_header();
    unsigned long n = 0, m = 0;
    uint8_t *p = mh ? getsectiondata(mh, "__HAOID", "__haoid", &n) : NULL;
    uint8_t *q = mh ? getsegmentdata(mh, "__HAOID", &m) : NULL;
    const struct section_64 *s = mh ? getsectbynamefromheader_64(mh, "__HAOID", "__haoid") : NULL;
    snprintf(early, sizeof early,
             "native.initializer.getsectiondata(prog,__HAOID,__haoid)==getsectbyname+slide=%s\n"
             "native.initializer.getsectiondata(prog,__HAOID,__haoid)=%.*s (%lu bytes)\n"
             "native.initializer.getsegmentdata(prog,__HAOID)==its section=%s, holds it=%s\n",
             same(p, s ? (const void *)(uintptr_t)(s->addr + (uint64_t)_dyld_get_image_slide((const void *)mh)) : NULL),
             p && n >= 11 ? 11 : 6, p && n >= 11 ? (const char *)p : "(none)", n, same(q, p), q && m >= n ? "yes" : "no");
}

/* Guest-only sections (identity.m: no hao front end has them or carries
 * them) through libmacho's slid getters on the main executable's header,
 * each used as its native reader uses it. The address must be the one the
 * unslid getter gives plus the main executable's slide, as natively. */
typedef int (*entry_fn)(int);
static uint8_t *slid(const char *via, const struct mach_header_64 *mh, intptr_t slide, const char *seg, const char *sect,
                     unsigned long *n)
{
    *n = 0;
    uint8_t *p = getsectiondata(mh, seg, sect, n);
    const struct section_64 *s = getsectbynamefromheader_64(mh, seg, sect);
    printf("native.%s.getsectiondata(%s,%s)==getsectbyname+slide=%s (%lu bytes)\n", via, seg, sect,
           same(p, s ? (const void *)(uintptr_t)(s->addr + (uint64_t)slide) : NULL), *n);
    fflush(stdout);
    return p;
}
static void guest_only(const char *via, const struct mach_header_64 *mh, intptr_t slide)
{
    unsigned long n;
    uint8_t *p = slid(via, mh, slide, "__TEXT", "__haotxt", &n);
    printf("native.%s.__TEXT,__haotxt=%.*s\n", via, p ? (int)n : 6, p ? (const char *)p : "(none)");
    p = slid(via, mh, slide, "__DATA_CONST", "__haodc", &n);
    printf("native.%s.__DATA_CONST,__haodc[0]=%s\n", via, p && n >= sizeof(char *) ? *(const char *const *)(const void *)p : "(none)");
    p = slid(via, mh, slide, "__DATA", "__haodata", &n);
    printf("native.%s.__DATA,__haodata=%.*s\n", via, p ? (int)n : 6, p ? (const char *)p : "(none)");
    /* ExtensionFoundation's +[_EXUtil _getEntryPointFunction]: the entry is the section plus its first int32 */
    p = slid(via, mh, slide, "__TEXT", "__swift5_entry", &n);
    if (p && n >= 4) {
        int32_t rel;
        memcpy(&rel, p, 4);
        entry_fn fn = (entry_fn)ptrauth_sign_unauthenticated((void *)(p + rel), ptrauth_key_function_pointer, 0);
        printf("native.%s.__swift5_entry(20)=%d\n", via, fn(20));
    } else printf("native.%s.__swift5_entry=(none)\n", via);
    /* the segment holding __haodata, and the section read through it */
    unsigned long m = 0;
    uint8_t *d = getsegmentdata(mh, "__DATA", &m);
    const struct segment_command_64 *sg = getsegbyname("__DATA");
    const struct section_64 *s = getsectbynamefromheader_64(mh, "__DATA", "__haodata");
    printf("native.%s.getsegmentdata(__DATA)==getsegbyname+slide=%s, size==vmsize=%s\n", via,
           same(d, sg ? (const void *)(uintptr_t)(sg->vmaddr + (uint64_t)slide) : NULL), sg && m == sg->vmsize ? "yes" : "no");
    fflush(stdout);
    if (d && sg && s && s->addr >= sg->vmaddr && s->addr + s->size <= sg->vmaddr + m)
        printf("native.%s.getsegmentdata(__DATA)+(__haodata)=%.*s\n", via, (int)s->size, (const char *)d + (s->addr - sg->vmaddr));
    else printf("native.%s.getsegmentdata(__DATA)+(__haodata)=(none)\n", via);
}

static void hash(const char *k, const void *p, unsigned long n)
{
    if (!p) { printf("native.%s=(none)\n", k); return; }
    uint32_t h = 2166136261u;
    for (unsigned long i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    printf("native.%s=%lu bytes fnv %08x\n", k, n, h);
}

void identhelper_print(void)
{
    const struct mach_header_64 *mh = (const void *)_NSGetMachExecuteHeader();
    int idx = -1;
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if ((const void *)_dyld_get_image_header(i) == mh) { idx = (int)i; break; }
    intptr_t slide = idx >= 0 ? _dyld_get_image_vmaddr_slide((uint32_t)idx) : 0;
    char buf[2048]; uint32_t n = sizeof buf;
    printf("native._NSGetExecutablePath=%s\n", _NSGetExecutablePath(buf, &n) == 0 ? buf : "(error)");
    printf("native.getprogname=%s\n", getprogname());
    printf("native._dyld_get_image_header(0)==_NSGetMachExecuteHeader()=%s\n", (const void *)_dyld_get_image_header(0) == mh ? "yes" : "no");
    printf("native._dyld_get_image_name(main)=%s\n", idx >= 0 ? _dyld_get_image_name((uint32_t)idx) : "(not in dyld's list)");
    Dl_info di;
    printf("native.dladdr(_NSGetMachExecuteHeader()).dli_fname=%s\n", dladdr(mh, &di) ? di.dli_fname : "(none)");
    /* CoreFoundation: _CFBundleCreateInfoDictFromMainExecutable */
    const struct section_64 *s = getsegbyname("__TEXT") ? getsectbynamefromheader_64(mh, "__TEXT", "__info_plist") : NULL;
    hash("CFBundle(__TEXT,__info_plist)", s ? (const void *)(uintptr_t)(s->addr + slide) : NULL, s ? s->size : 0);
    unsigned long sz = 0;
    char *d = getsectdata("__TEXT", "__info_plist", &sz);
    hash("getsectdata(__TEXT,__info_plist)+slide", d ? d + slide : NULL, sz);
    static const char *const want[][2] = {{"__TEXT", "__info_plist"}, {"__HAOID", "__haoid"}, {"__HAOMARK", "__haomark"}};
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        char k[96];
        s = getsectbyname(want[i][0], want[i][1]);
        snprintf(k, sizeof k, "getsectbyname(%s,%s)+slide", want[i][0], want[i][1]);
        hash(k, s ? (const void *)(uintptr_t)(s->addr + slide) : NULL, s ? s->size : 0);
        unsigned long m = 0;
        uint8_t *p = getsectiondata(mh, want[i][0], want[i][1], &m);
        snprintf(k, sizeof k, "getsectiondata(main,%s,%s)", want[i][0], want[i][1]);
        hash(k, p ? p : (s ? (const void *)"" : NULL), m);
    }
    /* a linker set, read as os_subcommand_main reads __subcommands (LINKER_SET_FOREACH,
     * os/linker_set.h: getsectdatafromheader_64 + _dyld_get_image_slide), then called through */
    struct haocmd { const char *name; int (*fn)(int); };
#if defined(__x86_64__)
    const char *lseg = "__DATA";
#else
    const char *lseg = "__DATA_CONST";
#endif
    uint64_t csz = 0;
    char *cd = getsectdatafromheader_64(mh, lseg, "__haocmds", &csz);
    const struct haocmd *const *hc = cd ? (const void *)(cd + _dyld_get_image_slide((const void *)mh)) : NULL;
    if (hc && csz >= sizeof *hc) printf("native.linker_set(__haocmds)[0]=%s(21) = %d\n", hc[0]->name, hc[0]->fn(21));
    else printf("native.linker_set(__haocmds)[0]=(none)\n");
    const struct segment_command_64 *sg = getsegbyname("__HAOID");
    if (sg) {
        const struct section_64 *ss = (const void *)(sg + 1);
        printf("native.getsegbyname(__HAOID)=%u section(s), first %.16s\n", sg->nsects, sg->nsects ? ss->sectname : "-");
        unsigned long m = 0;
        uint8_t *p = getsegmentdata(mh, "__HAOID", &m);
        hash("getsegmentdata(main,__HAOID)[0..12)", p && m >= 12 ? p : NULL, 12);
        hash("getsegbyname(__HAOID).section0+slide", sg->nsects ? (const void *)(uintptr_t)(ss->addr + slide) : NULL,
             sg->nsects ? ss->size : 0);
    } else printf("native.getsegbyname(__HAOID)=(none)\n");
    fputs(early, stdout);
    guest_only("prog", (const void *)_dyld_get_prog_image_header(), slide);
    guest_only("image0", (const void *)_dyld_get_image_header(0), _dyld_get_image_vmaddr_slide(0));
}
