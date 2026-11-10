/* src/commpage.c with both pages relocated to free addresses (the real ones
 * hold the host's own arm64 commpage under Rosetta): the pages are mapped
 * and filled at startup; a page that is gone later (or never was) is mapped
 * again on first use by hao_commpage_fault, as on_fault does for translated
 * code (logd's ldr [0xfffffc104] faults on an unmapped page otherwise);
 * a page some other mapping holds is left alone and reported.
 *   commpage                      prints PASS/FAIL lines, exit 0 if all pass */
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define TEST_BASE 0x300000004000ULL
#define TEST_RO   0x300000010000ULL
int hao_verbose;
FILE *hao_diag_file;
/* src/runtime.c's diagnostics channel: stderr alone here */
void hao_diag_write(const char *b, size_t n) { fwrite(b, 1, n, stderr); }
void hao_commpage_init(void);
bool hao_commpage_fault(uint64_t addr);

static sigjmp_buf jb;
static volatile int handled, refused;
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)ctx;
    if (hao_commpage_fault((uint64_t)(uintptr_t)si->si_addr)) { handled++; return; }
    refused++;
    siglongjmp(jb, 1);
}

static int fail;
static void check(const char *name, bool ok, const char *detail)
{
    printf("%-22s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) fail = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    /* the RO page's address is taken before startup */
    void *other = mmap((void *)(uintptr_t)TEST_RO, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    check("setup", other == (void *)(uintptr_t)TEST_RO, "a foreign PROT_NONE mapping at the RO page");
    hao_commpage_init();
    volatile const char *sig = (const char *)(uintptr_t)TEST_BASE;
    check("startup-mapped", !memcmp((const char *)sig, "commpage 64-bit", 16) && handled == 0, "signature at the base page");
    munmap((void *)(uintptr_t)TEST_BASE, 0x4000);
    volatile uint32_t v = *(volatile const uint32_t *)(uintptr_t)(TEST_BASE + 0x104);   /* logd's read */
    check("unmapped-remapped", handled == 1 && !memcmp((const char *)sig, "commpage 64-bit", 16) && v == 0,
          "read after munmap: mapped again on first use, contents intact");
    v = *(volatile const uint32_t *)(uintptr_t)(TEST_BASE + 0x104);
    check("second-read", handled == 1, "no further fault");
    if (!sigsetjmp(jb, 1)) { v = *(volatile const uint32_t *)(uintptr_t)(TEST_RO + 0x84); check("occupied-refused", false, "read of a foreign mapping succeeded"); }
    else check("occupied-refused", refused == 1 && handled == 1, "a page another mapping holds is not replaced");
    (void)v;
    return fail;
}
