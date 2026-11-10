/* ___chkstk_darwin reached under each of its names (tests/guest/chkstk.sh).
 *
 * Apple arm64's stack probe takes the frame size in x9 (bytes) and keeps every other
 * register (libsystem_pthread's arm64e code saves x10/x11 below sp); x86_64's takes
 * it in rax. hao answers the function with a
 * trap, keyed by the name programs import (libcompiler_rt's re-export
 * ___chkstk_darwin). libsystem_pthread defines it as ____chkstk_darwin, with
 * thread_chkstk_darwin at the same address, and a route that names the function
 * by its definition (a shared-cache image's stub slot, a dlsym'd address) missed
 * the trap: the guest call ran the x86_64 probe with rax = x8 and walked off the
 * stack (sharingd). The trap now answers every name of the function's
 * address (src/bridge.c trap_at). x8 is set large so that such a call faults. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

static void probe(const char *name)
{
    void *f = dlsym(RTLD_DEFAULT, name);
    if (!f) { printf("%s: absent\n", name); return; }
    register uint64_t x9 __asm__("x9") = 32;
    register uint64_t x8 __asm__("x8") = 0x7ffffffff000ull;
    register void *x12 __asm__("x12") = f;
    __asm__ volatile("blr x12" : "+r"(x9), "+r"(x8) : "r"(x12) : "x16", "x17", "x30", "memory");
    printf("%s: returned, x9=%llu\n", name, (unsigned long long)x9);
}

int main(void)
{
    probe("__chkstk_darwin");          /* the import name (Mach-O ___chkstk_darwin) */
    probe("___chkstk_darwin");         /* the definition (Mach-O ____chkstk_darwin) */
    probe("thread_chkstk_darwin");     /* the alias at the same address */
    return 0;
}
