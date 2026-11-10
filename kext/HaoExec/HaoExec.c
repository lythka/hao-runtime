/*
 * HaoExec — route arm64/arm64e-only executables to the hao translator on
 * an x86_64 XNU kernel ("reverse Rosetta" exec support).
 *
 * XNU activates images through a short table of activators (Mach-O, fat,
 * interpreter script, then a NULL terminator). HaoExec replaces only the
 * Mach-O slot. For an image with no x86_64 slice it hands the exec to XNU's
 * own `#!` path with a synthetic interpreter line, so argument/environment
 * copying and MAC policy checks are XNU's unmodified code:
 *
 *     /path/to/arm64-tool a b   ->   /usr/libexec/hao -- /path/to/arm64-tool a b
 *
 * ("--": the routed path is the program even if it looks like an option.)
 * hao re-presents the guest's argv to native code and runs it with the
 * native x86_64 libraries.
 *
 * What the `#!` route does NOT preserve (docs/security-model.md):
 *  - code identity: the process is /usr/libexec/hao, validated and
 *    attributed (TCC, sandbox, launchd) as hao. The guest's signature is
 *    checked by hao itself (src/guest_sig.c): invalid or unsigned code and
 *    non-Apple code claiming entitlements are refused. Anyone can also run
 *    /usr/libexec/hao directly, so hao must never be granted privileges.
 *  - setuid/setgid: XNU's shell activator clears them (kern.sugid_scripts=0);
 *    hao refuses such a program rather than run it unprivileged.
 *  - argv[0]: the `#!` path passes the exec path instead of the caller's
 *    argv[0].
 *  - interpreters: a script whose `#!` interpreter is itself an arm64-only
 *    program reaches this hook with IMGPF_INTERPRET set, and XNU cannot
 *    interpret twice, so that exec fails with EBADARCH. Programs that serve
 *    as interpreters (osascript, ...) must be embedded (tools/hao-embed).
 *
 * Every kernel-specific fact (UUID, table address, activator addresses in
 * the BootKC layout, the unslid addresses of the kernel functions this kext
 * calls) is stamped into __DATA,__hao_identity by kext/stamp_identity.py
 * from the BootKC the image boots, and is re-verified at load; an unstamped
 * or mismatched kext stays inactive. The kernel header is read only after
 * every stamped function gives the same slide, so a kext stamped for
 * another kernel never dereferences a guessed address.
 *
 * Controls:  sysctl kern.hao.enabled=0|1   (root; registered at the first exec)
 * Counters:  kern.hao.routed
 * The translator path is the compile-time constant HAO_INTERPRETER. It is
 * not a sysctl: a writable path would let one write redirect every arm64
 * exec on the machine, and would race the activator that reads it.
 *
 * Once the hook is installed the kext refuses to unload. Other threads call
 * the activator through the table slot with no reference on the kext, so no
 * counter kept inside the activator can tell when its text is unused: a
 * thread may have read the slot and not yet entered, or be in the epilogue
 * after its last instruction that touches the counter. Set
 * kern.hao.enabled=0 to stop routing; restart to remove the kext.
 *
 * OS executables normally never reach this hook: tools/hao-embed gives them
 * an x86_64 slice signed with their own identity. The kext covers arm64
 * programs that were not prepared that way.
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <mach-o/loader.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <stddef.h>

/* Filled in by kext/stamp_identity.py (layout "<II16sQQQQQQQQQ", version 2). */
struct hao_identity {
    uint32_t magic, version;
    unsigned char uuid[16];
    uint64_t header, printf_addr, table, act_mach, act_fat, act_shell;
    uint64_t iosleep_addr, intr_addr, sysctl_reg_addr;   /* unslid: cross-check the slide */
};
__attribute__((used, section("__DATA,__hao_identity")))
static volatile struct hao_identity hao_id = {0x494F4148, 2, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0};

extern boolean_t ml_set_interrupts_enabled(boolean_t enable);
extern void IOSleep(unsigned milliseconds);   /* linked for the slide cross-check only */

/* bsd/sys/imgact.h struct image_params: only this prefix is touched. */
struct hao_imgp {
    uint64_t ip_user_fname, ip_user_argv, ip_user_envv;
    int ip_seg;
    int pad0;
    void *ip_vp, *ip_vattr, *ip_origvattr;
    int ip_origcputype, ip_origcpusubtype;
    char *ip_vdata;            /* first page of the file */
    int ip_flags;
};
_Static_assert(offsetof(struct hao_imgp, ip_origcputype) == 0x38, "image_params ABI");
_Static_assert(offsetof(struct hao_imgp, ip_vdata) == 0x40, "image_params ABI");
_Static_assert(offsetof(struct hao_imgp, ip_flags) == 0x48, "image_params ABI");
#define IMGPF_INTERPRET 0x00000001
#define IMG_SHSIZE 512

typedef int (*activator_t)(struct hao_imgp *);
static activator_t *table;
static activator_t orig_mach, orig_shell;
static int hooked;

#define HAO_INTERPRETER "/usr/libexec/hao"
static const char hao_shebang[] = "#! " HAO_INTERPRETER " --\n";
_Static_assert(sizeof hao_shebang <= IMG_SHSIZE, "interpreter line fits XNU's script header");

static int hao_enabled = 1;
static uint64_t hao_routed;

/* ------------------------------------------------------- classification */

#define CPU_X86_64 0x01000007
#define CPU_ARM64  0x0100000c

static uint32_t rd32(const unsigned char *b, int big)
{
    return big ? ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3])
               : ((uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0]);
}

/* true: executable with arm64 code and no x86_64 slice */
static int needs_translation(const unsigned char *h, size_t n)
{
    if (n < 32) return 0;
    uint32_t magic = rd32(h, 0);
    if (magic == MH_MAGIC_64)
        return rd32(h + 4, 0) == CPU_ARM64 && rd32(h + 12, 0) == MH_EXECUTE;
    int big;
    size_t entry;
    if (magic == 0xbebafeca) { big = 1; entry = 20; }        /* FAT_MAGIC, read little-endian */
    else if (magic == 0xbfbafeca) { big = 1; entry = 32; }   /* FAT_MAGIC_64 */
    else return 0;
    uint32_t count = rd32(h + 4, big);
    if (count == 0 || count > (n - 8) / entry) return 0;
    int arm = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t cpu = rd32(h + 8 + k * entry, big);
        if (cpu == CPU_X86_64) return 0;
        if (cpu == CPU_ARM64) arm = 1;
    }
    return arm;
}

/* ------------------------------------------------------------ activator */

static void register_sysctls(void);
static volatile SInt32 sysctls_claimed;   /* 0 -> 1 once, by the exec that registers them */

static int hao_mach_imgact(struct hao_imgp *imgp)
{
    /* KECs start before the BSD layer is fully up; register the sysctl
     * nodes from the first exec (process context) instead. Exactly one
     * exec wins the claim; the nodes are never unregistered, because the
     * kext does not unload once this activator is installed. */
    if (sysctls_claimed == 0 && OSCompareAndSwap(0, 1, &sysctls_claimed))
        register_sysctls();
    int r;
    if (!hao_enabled || !imgp || !imgp->ip_vdata || imgp->ip_origcputype ||
        (imgp->ip_flags & IMGPF_INTERPRET) || !needs_translation((const unsigned char *)imgp->ip_vdata, 4096)) {
        r = orig_mach(imgp);
    } else {
        char line[IMG_SHSIZE];
        bzero(line, sizeof line);
        memcpy(line, hao_shebang, sizeof hao_shebang);
        char *saved = imgp->ip_vdata;
        imgp->ip_vdata = line;
        r = orig_shell(imgp);
        imgp->ip_vdata = saved;
        OSIncrementAtomic64((SInt64 *)&hao_routed);
    }
    return r;
}

/* ------------------------------------------------------- kernel writing */

static inline uintptr_t get_cr0(void) { uintptr_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void set_cr0(uintptr_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uintptr_t get_cr4(void) { uintptr_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
#define CR4_CET ((uintptr_t)1 << 23)

/* One aligned pointer store into the kernel's read-only activator table.
 * With interrupts off the thread cannot be preempted or migrate, so the
 * CR0.WP change stays on this CPU and lasts for the one store; other CPUs
 * keep write protection and see the old or the new pointer, never a torn
 * one (the caller checks the slot's alignment). An NMI taken inside the
 * window runs without write protection; nothing here can mask it. Called
 * once, from HaoExec_start, and only when CR4.CET is clear (clearing
 * CR0.WP with CET enabled is a #GP). */
static void write_const_ptr(activator_t *slot, activator_t v)
{
    boolean_t intr = ml_set_interrupts_enabled(FALSE);
    uintptr_t cr0 = get_cr0();
    set_cr0(cr0 & ~(uintptr_t)(1u << 16));   /* CR0.WP */
    __atomic_store_n(slot, v, __ATOMIC_SEQ_CST);
    set_cr0(cr0);
    ml_set_interrupts_enabled(intr);
}

static int kernel_matches(uintptr_t slide)
{
    /* Nothing is read through a stamped address until the stamp agrees with
     * the running kernel: every kernel function this kext links gives the
     * same slide (a different kernel build moves them relative to printf),
     * and the header is page aligned and below printf. */
    if ((uintptr_t)&IOSleep - (uintptr_t)hao_id.iosleep_addr != slide ||
        (uintptr_t)&ml_set_interrupts_enabled - (uintptr_t)hao_id.intr_addr != slide ||
        (uintptr_t)&sysctl_register_oid - (uintptr_t)hao_id.sysctl_reg_addr != slide)
        return 0;
    if ((hao_id.header & 0xfff) || hao_id.header >= hao_id.printf_addr || hao_id.printf_addr - hao_id.header > (256u << 20))
        return 0;
    const struct mach_header_64 *h = (const struct mach_header_64 *)(hao_id.header + slide);
    if (h->magic != MH_MAGIC_64 || h->cputype != CPU_X86_64 || h->sizeofcmds > 0x10000) return 0;
    const unsigned char *p = (const unsigned char *)(h + 1), *end = p + h->sizeofcmds;
    for (uint32_t k = 0; k < h->ncmds && p + sizeof(struct load_command) <= end; k++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmdsize < sizeof *lc || p + lc->cmdsize > end) return 0;
        if (lc->cmd == LC_UUID) {
            unsigned char want[16];
            for (int j = 0; j < 16; j++) want[j] = hao_id.uuid[j];
            return memcmp(((const struct uuid_command *)p)->uuid, want, 16) == 0;
        }
        p += lc->cmdsize;
    }
    return 0;
}

/* --------------------------------------------------------------- sysctl */

SYSCTL_NODE(_kern, OID_AUTO, hao, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "hao arm64 -> x86_64 translation");
SYSCTL_INT(_kern_hao, OID_AUTO, enabled, CTLFLAG_RW | CTLFLAG_LOCKED, &hao_enabled, 0,
           "route arm64-only executables to the translator");
SYSCTL_QUAD(_kern_hao, OID_AUTO, routed, CTLFLAG_RD | CTLFLAG_LOCKED, &hao_routed, "executables routed");

static void register_sysctls(void)
{
    sysctl_register_oid(&sysctl__kern_hao);
    sysctl_register_oid(&sysctl__kern_hao_enabled);
    sysctl_register_oid(&sysctl__kern_hao_routed);
}

/* ------------------------------------------------------------ lifecycle */

kern_return_t HaoExec_start(kmod_info_t *ki, void *d);
kern_return_t HaoExec_stop(kmod_info_t *ki, void *d);

kern_return_t HaoExec_start(kmod_info_t *ki, void *d)
{
    (void)ki; (void)d;
    if (hao_id.version != 2 || !hao_id.header || !hao_id.printf_addr || !hao_id.table || !hao_id.iosleep_addr ||
        !hao_id.intr_addr || !hao_id.sysctl_reg_addr) {
        printf("HaoExec: no kernel identity stamped; hook inactive\n");
        return KERN_FAILURE;
    }
    uintptr_t slide = (uintptr_t)&printf - (uintptr_t)hao_id.printf_addr;
    if (!kernel_matches(slide)) {
        printf("HaoExec: kernel UUID does not match the build identity; hook inactive\n");
        return KERN_FAILURE;
    }
    table = (activator_t *)(hao_id.table + slide);
    if ((uintptr_t)table & (sizeof(activator_t) - 1)) {
        printf("HaoExec: image activator table is not pointer aligned; hook inactive\n");
        return KERN_FAILURE;
    }
    /* exactly [mach, fat, shell, NULL]: orig_shell below is table[2] */
    if ((uintptr_t)table[0] != hao_id.act_mach + slide || (uintptr_t)table[1] != hao_id.act_fat + slide ||
        (uintptr_t)table[2] != hao_id.act_shell + slide || table[3] != NULL) {
        printf("HaoExec: image activator table differs from the build identity; hook inactive\n");
        return KERN_FAILURE;
    }
    if (get_cr4() & CR4_CET) {
        printf("HaoExec: CR4.CET is set, the activator table cannot be written; hook inactive\n");
        return KERN_FAILURE;
    }
    orig_mach = table[0];
    orig_shell = table[2];
    hooked = 1;   /* before the store: from here on the text must stay mapped */
    write_const_ptr(&table[0], hao_mach_imgact);
    printf("HaoExec: arm64-only executables now run through " HAO_INTERPRETER "\n");
    return KERN_SUCCESS;
}

kern_return_t HaoExec_stop(kmod_info_t *ki, void *d)
{
    (void)ki; (void)d;
    /* See the header comment: there is no point at which the activator is
     * known to be unused, so a hooked kext stays. The slot is left alone
     * too; kern.hao.enabled=0 turns routing off. (A start that failed
     * installed nothing and registered no sysctl node.) */
    if (hooked) {
        printf("HaoExec: the exec hook is installed; not unloading (kern.hao.enabled=0 stops routing)\n");
        return KERN_FAILURE;
    }
    return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(com.apple.kec.HaoExec, "0.2.0", HaoExec_start, HaoExec_stop)
__private_extern__ kmod_start_func_t *_realmain = HaoExec_start;
__private_extern__ kmod_stop_func_t *_antimain = HaoExec_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
