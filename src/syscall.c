/*
 * hao — guest `svc #0x80` bridge (rare in hybrid mode: guest code normally
 * reaches the kernel through native libSystem). BSD and Mach numbering is
 * shared between Darwin arm64 and x86_64; only the trap convention differs:
 *   arm64:  x16 = number (>0 BSD, <0 Mach, 0x80000000 platform), x0-x8 args,
 *           carry set on error, results x0/x1
 *   x86_64: rax = class<<24 | number, rdi rsi rdx r10 r8 r9 + stack,
 *           carry set on error, results rax/rdx
 */
#include "hao/runtime.h"
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* the kernel's result convention: x0 = value, or C set with x0 = errno */
static void svc_result(hao_cpu *c, long r, int err)
{
    unsigned nzcv = hao_nzcv(c) & ~2u;
    if (err) { nzcv |= 2; r = err; }
    c->x[0] = (uint64_t)r;
    hao_set_nzcv(c, nzcv);
}

/* BSD calls that would bypass hao's own state if passed through: the
 * signal dispositions (a raw sigaction would replace hao's fault handler
 * and trampoline) and the thread's signal mask go through the same code as
 * the libc calls; thread creation and sigreturn, which hand the kernel
 * entry points and register state hao does not set up, are refused.
 * Returns whether the call was handled. */
static bool raw_mediated(hao_thread *t, long bsd, const uint64_t *a)
{
    hao_cpu *c = &t->cpu;
    switch (bsd) {
    case SYS_sigaction: {
        int r = hao_signal_raw_sigaction((int)a[0], (const void *)(uintptr_t)a[1], (void *)(uintptr_t)a[2]);
        svc_result(c, 0, r < 0 ? errno : 0);
        return true;
    }
    case SYS_sigprocmask: case SYS___pthread_sigmask: {
        /* libc's pthread_sigmask: libhao_identity's hook tracks the mask */
        int e = pthread_sigmask((int)a[0], (const sigset_t *)(uintptr_t)a[1], (sigset_t *)(uintptr_t)a[2]);
        svc_result(c, 0, e);
        return true;
    }
    case SYS_sigreturn: case SYS_bsdthread_create: case SYS_bsdthread_register:
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG, "raw system call %ld (%s) refused: guest threads and signal returns go through libSystem under hao\n",
                bsd, bsd == SYS_sigreturn ? "sigreturn" : bsd == SYS_bsdthread_create ? "bsdthread_create" : "bsdthread_register");
        svc_result(c, 0, ENOTSUP);
        return true;
    default:
        return false;
    }
}

void hao_guest_svc(hao_thread *t)
{
    hao_cpu *c = &t->cpu;
    int64_t num = (int64_t)c->x[16];
    if ((uint32_t)c->x[16] == 0x80000000u) {           /* platform syscall */
        switch (c->x[3]) {
        case 2: c->tpidrro_el0 = c->x[0]; break;     /* set cthread: only affects the guest view */
        case 3: c->x[0] = c->tpidrro_el0; break;
        default: break;
        }
        return;
    }
    if (num == 2 || num == 66) {
        /* fork, vfork: native fork, so libSystem's and hao's atfork child
         * handlers run (a raw fork child keeps hao's locks and libSystem's
         * as other threads held them). Then the kernel's convention: the
         * parent gets x0 = child pid, x1 = 0; the child x0 = its own pid,
         * x1 = 1; an error sets C with x0 = errno. */
        pid_t p = fork();
        unsigned nzcv = hao_nzcv(c) & ~2u;
        if (p < 0) { c->x[0] = (uint64_t)errno; nzcv |= 2; }
        else if (p == 0) { c->x[0] = (uint64_t)getpid(); c->x[1] = 1; }
        else { c->x[0] = (uint64_t)p; c->x[1] = 0; }
        hao_set_nzcv(c, nzcv);
        return;
    }
    if (num == -3) { c->x[0] = mach_absolute_time(); return; }
    if (num == -4) { c->x[0] = mach_continuous_time(); return; }
    long xnum = num > 0 ? (0x2000000L | num) : num < 0 ? (0x1000000L | -num) : 0x2000000L;
    uint64_t args[9];
    int shift = num == 0 ? 1 : 0;                   /* indirect syscall: number in x0 */
    if (shift) xnum = 0x2000000L | (long)c->x[0];
    for (int k = 0; k < 9; k++) args[k] = c->x[k + shift];
    if ((xnum >> 24) == 2 && raw_mediated(t, xnum & 0xffffff, args)) return;
    /* execve, __mac_execve, posix_spawn: a hao stub gets the tagged-pointer
     * switch in its environment, as through libSystem (process_identity.c) */
    long bsd = xnum & 0xffffff;
    char **env = NULL;
    size_t envlen = 0;
    if ((xnum >> 24) == 2 && (bsd == SYS_execve || bsd == SYS___mac_execve || bsd == SYS_posix_spawn)) {
        static char **(*exec_env)(const char *, char *const *, size_t *);
        if (!exec_env) exec_env = (char **(*)(const char *, char *const *, size_t *))dlsym(RTLD_DEFAULT, "hao_identity_exec_env");
        int pi = bsd == SYS_posix_spawn ? 1 : 0, ei = bsd == SYS_posix_spawn ? 4 : 2;
        if (exec_env && (env = exec_env((const char *)(uintptr_t)args[pi], (char *const *)(uintptr_t)args[ei], &envlen)))
            args[ei] = (uint64_t)(uintptr_t)env;
    }
    uint64_t rdx = 0;
    int err = 0;
    long r = hao_raw_syscall(xnum, args, &rdx, &err);
    if (env) munmap(env, envlen);
    /* executable pages the guest maps itself hold arm64 code (interpose.c, as for libc's mmap) */
    if (!err && (xnum >> 24) == 2) {
        if (bsd == SYS_mmap) hao_guest_vm_note((uint64_t)r, args[1], (args[2] & PROT_EXEC) || (args[3] & MAP_JIT));
        else if (bsd == SYS_munmap) hao_guest_vm_note(args[0], args[1], false);
        else if (bsd == SYS_mprotect && (args[2] & PROT_EXEC)) hao_guest_vm_note(args[0], args[1], true);
    }
    c->x[0] = (uint64_t)r;
    c->x[1] = rdx;
    unsigned nzcv = hao_nzcv(c) & ~2u;
    if (err) nzcv |= 2;
    hao_set_nzcv(c, nzcv);
}
