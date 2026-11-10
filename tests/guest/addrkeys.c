/* tests/guest/addrkeys.sh's guest: native functions called under names other than the ones
 * their types and traps are recorded under (src/bridge.c: plans keyed by code identity, names
 * are labels). One case per process (argv[1]):
 *   same     akx_same, an alias at akx_flag's address (the unit types akx_flag)
 *   rev      akx_rev, whose alias akx_revb is the one the unit types
 *   sh       akx_sh2, an alias of akx_sh (int16_t result)
 *   reexp    akp_flag, a renamed re-export of akx_flag from another library
 *   dl-same, dl-reexp   the same through dlsym (bare addresses)
 *   dladdr   akp_dladdr, a renamed re-export of dladdr (a trap), on main's address: the image
 *   dl-dladdr  the same through dlsym
 *   chkstk   akp_chkstk, a renamed re-export of ___chkstk_darwin (a trap), size in x9
 *   thread-chkstk   libsystem_pthread's thread_chkstk_darwin, imported: the alias at the
 *            address of ____chkstk_darwin, which ___chkstk_darwin re-exports (no hao table
 *            names either)
 * Built with -DONE=N, the program imports only case N's function (the plan table is keyed by
 * address, so a process that also imports another name of a function shares that name's plan:
 * each name must be typed on its own). */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool akx_same(int v);
bool akx_rev(int v);
int16_t akx_sh2(int v);
bool akp_flag(int v);
int akp_dladdr(const void *addr, Dl_info *info);
void akp_chkstk(void);
void thread_chkstk_darwin(void);

#ifndef ONE
#define ONE 0
#endif
/* case n's function, or (in a one-case build of another case) NULL: no import */
#define USE(n, f) (ONE == 0 || ONE == (n) ? (f) : NULL)

static void flags(const char *k, bool (*f)(int))
{
    volatile int a = f(5), b = f(-5);
    printf("%s: %d %d\n", k, a, b);
}

int main(int argc, char **argv);

static void dl(const char *k, int (*f)(const void *, Dl_info *))
{
    Dl_info d;
    memset(&d, 0, sizeof d);
    int r = f((const void *)&main, &d);
    const char *b = d.dli_fname ? strrchr(d.dli_fname, '/') : NULL;
    const char *fn = b ? b + 1 : d.dli_fname ? d.dli_fname : "-";
    /* the image only: hao's dladdr trap names no symbol of a guest image (dli_sname NULL) */
    printf("%s: r=%d file=%s\n", k, r, !strncmp(fn, "addrkeys", 8) ? "the program" : fn);
}

static void probe(const char *k, void *f)
{
#if defined(__aarch64__)
    /* x8 large: a guest call that reaches the x86_64 probe (size in rax) walks off the stack */
    register uint64_t x9 __asm__("x9") = 32;
    register uint64_t x8 __asm__("x8") = 0x7ffffffff000ull;
    register void *x12 __asm__("x12") = f;
    __asm__ volatile("blr x12" : "+r"(x9), "+r"(x8) : "r"(x12) : "x16", "x17", "x30", "memory");
    printf("%s: returned, x9=%llu\n", k, (unsigned long long)x9);
#else
    /* the x86_64 reference: the size in rax */
    uint64_t rax = 32;
    __asm__ volatile("call *%1" : "+a"(rax) : "r"(f) : "r10", "r11", "memory");
    printf("%s: returned, x9=%llu\n", k, (unsigned long long)rax);
#endif
}

int main(int argc, char **argv)
{
    const char *k = argc > 1 ? argv[1] : "";
    if (!strcmp(k, "same")) flags(k, USE(1, akx_same));
    else if (!strcmp(k, "rev")) flags(k, USE(2, akx_rev));
    else if (!strcmp(k, "sh")) {
        int16_t (*f)(int) = USE(3, akx_sh2);
        volatile int a = f(100), b = f(-7);
        printf("sh: %d %d\n", a, b);
    }
    else if (!strcmp(k, "reexp")) flags(k, USE(4, akp_flag));
    else if (!strcmp(k, "dl-same") || !strcmp(k, "dl-reexp")) {
        bool (*f)(int) = (bool (*)(int))dlsym(RTLD_DEFAULT, k[3] == 's' ? "akx_same" : "akp_flag");
        if (!f) { printf("%s: not found\n", k); return 1; }
        flags(k, f);
    } else if (!strcmp(k, "dladdr")) dl(k, USE(7, akp_dladdr));
    else if (!strcmp(k, "dl-dladdr")) {
        int (*f)(const void *, Dl_info *) = (int (*)(const void *, Dl_info *))dlsym(RTLD_DEFAULT, "akp_dladdr");
        if (!f) { printf("%s: not found\n", k); return 1; }
        dl(k, f);
    } else if (!strcmp(k, "chkstk")) probe(k, (void *)USE(9, akp_chkstk));
    else if (!strcmp(k, "thread-chkstk")) probe(k, (void *)USE(10, thread_chkstk_darwin));
    else { fprintf(stderr, "usage: addrkeys same|rev|sh|reexp|dl-same|dl-reexp|dladdr|dl-dladdr|chkstk|thread-chkstk\n"); return 2; }
    return 0;
}
