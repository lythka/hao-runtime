/*
 * hao — the arm64-only register-variant entry points of the runtimes
 * (src/regvar_gen.inc, tools/gen_regvariants.py): objc_retain_xN /
 * objc_release_xN (objc4 runtime/objc-abi.h, "#if __arm64__";
 * runtime/retain-release-helpers-arm64.s), swift_retain_xN, swift_release_xN,
 * swift_bridgeObjectRetain_xN, swift_bridgeObjectRelease_xN and
 * swift_{retain,release}_preservemost (libswiftCore). clang and Swift's IRGen
 * call them with the object in xN instead of x0. Neither 26A428's RosettaOS
 * runtimes nor the 27.2 host's x86_64 ones define any of them (a native
 * dlsym returns NULL), so without this every import was an unresolved-import
 * trap and the first call aborted.
 *
 * What they do, from the arm64e code: retain variants start `mov x0, xN`
 * (objc: `ands x0, xN, xN`), run the base's fast path and return the object
 * in x0; release variants start `mov x8, xN` (or read x8 directly) and
 * clobber x0; both clobber x1, x2 and x8-x12 and leave through the base's slow
 * path (incrementSlow, doDecrementSlow, objc_msgSend, a runtime hook), so
 * callers may assume no more than AAPCS64's caller-saved clobbers. Over the
 * 74,598 bl sites of the 49 27.0 executables that import the Swift ones, no
 * caller reads a caller-saved register (x0 after a release included) before
 * writing it. The _preservemost names are aliases of swift_retain and
 * swift_release (preserve_most on arm64).
 *
 * The trap: x0 = xN, the argument scan for slot 0, then the native base
 * (RuntimeFunctions.def / objc-internal.h: one pointer argument, retain
 * returning it) through the register bridge, as a plain call of the base
 * would go. The bridge's other result registers (x1-x3, v0-v3) are put back,
 * so every register but x0 keeps its value, which satisfies AAPCS64 and
 * preserve_most alike; a release leaves the object in x0.
 */
#include "hao/bridge.h"
#include "hao/runtime.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *name, *base; uint8_t reg, result; } regvar_ent;
static const regvar_ent regvars[] = {
#include "regvar_gen.inc"
};
#define NREGVAR (sizeof regvars / sizeof *regvars)
static _Atomic uint64_t regvar_native[NREGVAR];

static void trap_regvar(hao_thread *t, uint64_t k)
{
    hao_cpu *c = &t->cpu;
    const regvar_ent *e = &regvars[k];
    uint64_t obj = c->x[e->reg], x[4], fn = atomic_load_explicit(&regvar_native[k], memory_order_acquire);
    __typeof__(c->v[0]) v[4];
    memcpy(x, c->x, sizeof x);
    memcpy(v, c->v, sizeof v);
    c->x[0] = obj;
    hao_thunk_native_args_mask(t, 0x1);       /* the object: a guest block gets its thunks */
    hao_native_call(fn, c, 0);
    uint64_t r = c->x[0];
    memcpy(&c->x[1], &x[1], 3 * sizeof x[0]);
    memcpy(c->v, v, sizeof v);
    c->x[0] = e->result ? r : obj;
}

/* every name this trap answers, for the bridge's address-keyed trap table (B0) */
void hao_regvar_interpose_names(hao_name_fn cb, void *ctx)
{
    for (size_t k = 0; k < NREGVAR; k++) cb(regvars[k].name, ctx);
}

/* the import binds here only when the native base exists: otherwise it stays
 * an unresolved-import trap, which names the symbol on its first call */
bool hao_regvar_interpose(const char *name, uint64_t *addr)
{
    if (strncmp(name, "_objc_re", 8) && strncmp(name, "_swift_", 7)) return false;
    for (size_t k = 0; k < NREGVAR; k++) {
        if (strcmp(name, regvars[k].name)) continue;
        uint64_t fn = atomic_load_explicit(&regvar_native[k], memory_order_acquire);
        if (!fn) {
            fn = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, regvars[k].base);
            if (!fn) {
                if (hao_verbose) fprintf(stderr, "hao: %s: no native %s\n", name, regvars[k].base);
                return false;
            }
            atomic_store_explicit(&regvar_native[k], fn, memory_order_release);
        }
        *addr = hao_trap_address(regvars[k].name, trap_regvar, k);
        return true;
    }
    return false;
}
