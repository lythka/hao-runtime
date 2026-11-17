#include "hao/runtime.h"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static uintptr_t entry_fn[5];
static hao_image *installed;

static void call_entry(hao_thread *t, uint64_t k)
{
    hao_cpu *c = &t->cpu;
    uintptr_t fn = __atomic_load_n(&entry_fn[k], __ATOMIC_ACQUIRE);
    void *a0 = (void *)(uintptr_t)c->x[0];
    if (k == 0) {
        int (*f)(const void *, uint32_t, void **, void **, uint32_t *) = (void *)fn;
        c->x[0] = (uint32_t)f((void *)(uintptr_t)hao_tbi(c->x[0]), (uint32_t)c->x[1],
                              (void *)(uintptr_t)hao_tbi(c->x[2]), (void *)(uintptr_t)hao_tbi(c->x[3]),
                              (void *)(uintptr_t)hao_tbi(c->x[4]));
    } else if (k == 1) {
        int (*f)(void *, const void *, uint32_t) = (void *)fn;
        c->x[0] = (uint32_t)f(a0, (void *)(uintptr_t)hao_tbi(c->x[1]), (uint32_t)c->x[2]);
    } else if (k == 2) {
        int (*f)(void *, const void *, uint32_t, void **, uint32_t *) = (void *)fn;
        c->x[0] = (uint32_t)f(a0, (void *)(uintptr_t)hao_tbi(c->x[1]), (uint32_t)c->x[2],
                              (void *)(uintptr_t)hao_tbi(c->x[3]), (void *)(uintptr_t)hao_tbi(c->x[4]));
    } else {
        void (*f)(void *) = (void *)fn;
        if (a0) f(a0);
        c->x[0] = 0;
    }
}

static bool digest_is(const void *p, const char *expected)
{
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    char hex[65];
    CC_SHA256(p, 32, d);
    for (unsigned i = 0; i < sizeof d; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return !strcmp(hex, expected);
}

static bool host_image_ok(const struct mach_header_64 *h, const uint8_t uuid[16], uintptr_t off)
{
    if (h->magic != MH_MAGIC_64 || h->cputype != CPU_TYPE_X86_64) return false;
    bool match = false;
    uint64_t text = UINT64_MAX;
    const struct load_command *lc = (const void *)(h + 1);
    for (uint32_t n = 0; n < h->ncmds; n++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_UUID) match = !memcmp(((const struct uuid_command *)lc)->uuid, uuid, 16);
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT"))
            text = ((const struct segment_command_64 *)lc)->vmaddr;
    }
    if (!match || text == UINT64_MAX) return false;
    lc = (const void *)(h + 1);
    for (uint32_t n = 0; n < h->ncmds; n++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *s = (const void *)lc;
        if ((s->initprot & 4) && s->vmsize >= 32 && text + off >= s->vmaddr && text + off - s->vmaddr <= s->vmsize - 32)
            return true;
    }
    return false;
}

bool hao_ids_prepare(hao_image *img)
{
    static const uint8_t guest_uuid[16] = {0x31,0x3d,0xda,0x7e,0xde,0xd4,0x35,0x06,0xa7,0x63,0x3e,0xdd,0x8d,0x82,0xcf,0x9c};
    static const uint8_t host_uuid[16] = {0x60,0x1c,0x9e,0x2f,0x3a,0x05,0x31,0x27,0x8e,0x32,0xa4,0x85,0x2d,0xde,0xd8,0xa9};
    if (installed == img) return true;
    if (!img->has_uuid || memcmp(img->uuid, guest_uuid, 16)) return false;
    if (installed || img->aot) return false;
    static const struct { uint64_t guest, host; const char *gd, *hd; } e[] = {
        {0x80d3a0, 0x11170, "979ffc09300de9b1995bc5aac7d3848d6aa98223b9202b5c063306ac0ba3bc29", "ba8e7e95e43a88604339744bd424eb376bc590b0c55c0f61e07c6ff47942466e"},
        {0x82cdd0, 0x95600, "c3b95249dcf7f0625ecdead7a12b9fbce55572e120c2f6ae204e5578fef6836e", "3abf4141398cf4ccc4e22af7935d4b467a6aa5515767d54cf56e2a1e030b55f4"},
        {0x823f38, 0x9b6e0, "9bc360af9609829ba81f19a0290972f1123b464e012c5eeabf712c8c89218dd0", "f941cb472c714754efb57bca7b805896571f0dd6125c9c55ef1995d879b8b66c"},
        {0x823c88, 0x9ad00, "6539bb53bbdb34b50c1a2d7a50d0396c329b3c964b7f3e83b414cda3a4bc029f", "f699f6add71929cca45791c2589a5951114b9bff4285df0b562edc1a3ed1dac6"},
        {0x803d70, 0x58080, "5e7ec6e3f14bb14b8bd810e149bbeea0efe6514fb5ff1d72b69283a7f3725298", "cae24eae78e0b6bad86e1337e1c54c864225085ca7cef57325aedb5d6bd99a7b"},
    };
    void *lib = dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/Versions/A/AuthKit", RTLD_NOW | RTLD_LOCAL);
    Class cls = lib ? objc_getClass("AKAbsintheSigner") : Nil;
    Dl_info info;
    if (!cls || !dladdr((void *)cls, &info)) return false;
    uintptr_t base = (uintptr_t)info.dli_fbase;
    uint64_t trap[5];
    for (unsigned k = 0; k < 5; k++) {
        uint64_t pc = img->base + e[k].guest;
        if (hao_image_for_code(pc) != img || hao_image_for_code(pc + 31) != img ||
            !digest_is(hao_code_ptr(pc), e[k].gd) || !host_image_ok(info.dli_fbase, host_uuid, e[k].host) ||
            !digest_is((void *)(base + e[k].host), e[k].hd))
            return false;
        trap[k] = hao_trap_address("ids", call_entry, k);
        if (!trap[k]) return false;
    }
    for (unsigned k = 0; k < 5; k++) __atomic_store_n(&entry_fn[k], base + e[k].host, __ATOMIC_RELEASE);
    /* true even when a write fails: code that may differ from its signed bytes gets no AOT cache */
    installed = img;
    for (unsigned k = 0; k < 5; k++) {
        uint32_t w[4] = {0x58000050, 0xd61f0200, (uint32_t)trap[k], (uint32_t)(trap[k] >> 32)};
        if (!hao_code_write(img->base + e[k].guest, img->code_delta, w, sizeof w)) {
            hao_diag_prefix(stderr);
            fprintf(stderr, "cannot patch guest code at %#llx\n", (unsigned long long)(img->base + e[k].guest));
            break;
        }
    }
    return true;
}
