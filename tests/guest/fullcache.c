/*
 * A guest SIGSEGV handler reads the pc of the faulting load, before and
 * after hao's code cache is full (tests/guest/fullcache.sh runs it with
 * HAO_JIT_CACHE_MB=1, so the JIT interprets every block it meets after the
 * fill: hao_jit_run_block's a64_step). Natively, and under hao, the pc is the
 * load's own address (offset 0).
 * Build: the script generates fullcache_fill.c (FILL_N small functions).
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

extern long fill(long);
extern char fault_ldr[], fault_str[];
static sigjmp_buf jb;
static volatile unsigned long seen_pc;
static volatile int seen_sig;

static void handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    seen_sig = sig;
    seen_pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    siglongjmp(jb, 1);
}

__attribute__((noinline)) static long load(volatile long *p)
{
    long v;
    __asm__ volatile(".globl _fault_ldr\n_fault_ldr: ldr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

__attribute__((noinline)) static void store(volatile long *p)
{
    __asm__ volatile(".globl _fault_str\n_fault_str: str xzr, [%0]" : : "r"(p) : "memory");
}

static void check(const char *what, void *p)
{
    seen_pc = 0;
    if (!sigsetjmp(jb, 1)) {
        if (!strcmp(what, "load")) load(p); else store(p);
        printf("%s: no fault\n", what);
        return;
    }
    char *at = !strcmp(what, "load") ? fault_ldr : fault_str;
    printf("%s: signal %s, pc - faulting instruction = %ld\n", what, seen_sig == SIGSEGV ? "SEGV" : seen_sig == SIGBUS ? "BUS" : "?",
           (long)(seen_pc - (unsigned long)at));
}

/* fullcache {load|store} {fill|nofill}: one fault per process (a second fault, after a handler that
 * left by siglongjmp, is a separate case: see in_fault_handler in src/signals.c) */
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    void *p = mmap(0, 16384, strcmp(argv[1], "load") ? PROT_READ : PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (!strcmp(argv[2], "fill")) printf("fill: %ld\n", fill(1) & 0xff);   /* translates every function: fills a 1 MB cache */
    check(argv[1], p);
    return 0;
}
