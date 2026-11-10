/* SIGSEGV/SIGBUS handlers that native code installs with the native API
 * (tests/guest/nativesighelper.c), in a guest whose own code native code
 * calls through the NX-fault path (src/signals.c, src/process_identity.c).
 * Modes:
 *   crc       LLVM's CrashRecoveryContext (libBNNS): a context stays active
 *             on a native thread B while this thread calls guest code from
 *             native code 200 times; then B crashes inside it twice (a
 *             fault, a sent SIGBUS) after one other crash, and recovers
 *   crcnull   crc, then a native call of address 0 with no context: the
 *             handler restores the old handlers and raises again, which
 *             waits for it to return (it prints first), then the default
 *             action
 *   ret       a handler that returns: a fault in its own probe resumes at
 *             a recovery point; guest code called from native code still
 *             works with it installed; then a fault it does not know is
 *             passed to the default action
 *   readback  native sigaction, signal, bsd_signal, sigset, sigvec and
 *             sigignore of SIGBUS read back what they installed; the guest
 *             sees the native handler too
 *   raise     a native raise(SIGBUS) with the default disposition
 *   tok       a SIGSEGV handler that returns with the registers _sigtramp
 *             keeps the ucontext and token in changed, with SIGILL blocked:
 *             its __sigreturn fails and _sigtramp traps (SIGTRAP)
 *   tokspin   the same with SIGILL and SIGTRAP blocked: natively the trap
 *             is never delivered and the thread spins forever (the native
 *             run is killed by the timeout); hao takes SIGTRAP's default
 *             action
 *   sigret    a failed __sigreturn (nsh_sigret: _sigtramp's trap after the
 *             call, reached as after one) of a native SIGUSR1 handler's frame
 *             with its own token: SIGTRAP natively (arm64 brk); hao's ud2
 *             SIGILL ends in the failed-sigreturn report and SIGTRAP's
 *             default action
 *   sigretbad the same with an unmapped ucontext and a wrong token
 *   sigretfwd the same on the frame of a native SIGSEGV handler (under hao
 *             on_fault's, which forwards the fault to it)
 *   sigretpolicy readable frame and matching token, but EPERM (policy denial)
 *   giveup    no handler at all: a native write to address 16, which hao
 *             gives up on (signals.c hao_signal_giveup): the process dies
 *             of that fault, SIGSEGV
 *   giveupbus the same with a native write to a read-only page: SIGBUS */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define STRIP(f) ((void (*)(int))ptrauth_strip((void *)(f), ptrauth_key_function_pointer))
#else
#define STRIP(f) (f)
#endif

typedef int (*nsh_fn)(int);
int nsh_call_slot(nsh_fn *slot, int v);
void nsh_crc_enable(void);
void nsh_crc_handler(int);
void nsh_crc_thread_start(void);
void nsh_crc_thread_finish(int out[3]);
void nsh_ret_install(void);
int nsh_ret_hits(void);
int nsh_ret_other(void);
long nsh_probe(const long *p);
void nsh_readback(int sig);
void nsh_h1(int);
const char *nsh_name(void (*h)(int));
void nsh_bad_install(void);
void nsh_null_write(void);
void nsh_ro_write(int *p);
int nsh_raise(int sig);
void nsh_sigret(int how);

static int twice(int v) { return 2 * v; }

/* the guest's own view of a disposition */
static const char *guest_view(int sig)
{
    struct sigaction oa;
    if (sigaction(sig, NULL, &oa) != 0) return "failed";
    return nsh_name(STRIP(oa.sa_handler));
}

static int calls(nsh_fn *slot, int n)
{
    int sum = 0;
    for (int k = 0; k < n; k++) sum += nsh_call_slot(slot, k);
    return sum;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *m = argc > 1 ? argv[1] : "";
    nsh_fn slot = twice;   /* guest code reached only through memory */
    printf("before: %d\n", nsh_call_slot(&slot, 21));
    if (!strcmp(m, "crc") || !strcmp(m, "crcnull")) {
        nsh_crc_enable();
        printf("guest sees SIGSEGV %s, SIGBUS %s\n", guest_view(SIGSEGV), guest_view(SIGBUS));
        nsh_crc_thread_start();
        printf("while B is in RunSafely: %d\n", calls(&slot, 200));
        int r[3];
        nsh_crc_thread_finish(r);
        printf("B recovered from signals %d %d %d\n", r[0], r[1], r[2]);
        printf("after: %d\n", calls(&slot, 10));
        if (!strcmp(m, "crcnull")) {
            slot = NULL;
            printf("null call\n");
            printf("not reached %d\n", nsh_call_slot(&slot, 1));
        }
        return 0;
    }
    if (!strcmp(m, "ret")) {
        nsh_ret_install();
        printf("guest sees SIGSEGV %s\n", guest_view(SIGSEGV));
        printf("guest code from native: %d\n", calls(&slot, 50));
        long v = 7;
        printf("probe ok %ld, probe NULL %ld\n", nsh_probe(&v), nsh_probe(NULL));
        printf("guest code from native: %d\n", calls(&slot, 50));
        printf("hits %d, other faults %d\n", nsh_ret_hits(), nsh_ret_other());
        printf("unknown fault\n");
        nsh_null_write();
        printf("not reached\n");
        return 1;
    }
    if (!strcmp(m, "readback")) {
        nsh_readback(SIGBUS);
        printf("guest sees SIGBUS %s\n", guest_view(SIGBUS));
        printf("guest code from native: %d\n", calls(&slot, 20));
        return 0;
    }
    if (!strcmp(m, "raise")) {
        printf("raising\n");
        nsh_raise(SIGBUS);
        printf("survived raise(SIGBUS)\n");
        return 0;
    }
    if (!strcmp(m, "tok") || !strcmp(m, "tokspin")) {
        sigset_t b;
        sigemptyset(&b);
        sigaddset(&b, SIGILL);
        if (!strcmp(m, "tokspin")) sigaddset(&b, SIGTRAP);
        sigprocmask(SIG_BLOCK, &b, NULL);
        nsh_bad_install();
        printf("guest code from native: %d\n", calls(&slot, 20));
        printf("faulting\n");
        nsh_null_write();
        printf("not reached\n");
        return 1;
    }
    if (!strcmp(m, "sigret") || !strcmp(m, "sigretbad") || !strcmp(m, "sigretfwd") || !strcmp(m, "sigretpolicy")) {
        printf("guest code from native: %d\n", calls(&slot, 20));
        printf("failing a sigreturn\n");
        nsh_sigret(!strcmp(m, "sigret") ? 0 : !strcmp(m, "sigretbad") ? 1 : !strcmp(m, "sigretfwd") ? 2 : 3);
        printf("not reached\n");
        return 1;
    }
    if (!strcmp(m, "giveup") || !strcmp(m, "giveupbus")) {
        printf("guest code from native: %d\n", calls(&slot, 20));
        int *ro = mmap(NULL, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (ro == MAP_FAILED) return 3;
        printf("faulting\n");
        if (!strcmp(m, "giveup")) nsh_null_write();
        else nsh_ro_write(ro + 5);
        printf("not reached\n");
        return 1;
    }
    fprintf(stderr, "usage: nativesig crc|crcnull|ret|readback|raise|tok|tokspin|sigret|sigretbad|sigretfwd|sigretpolicy|giveup|giveupbus\n");
    return 2;
}
