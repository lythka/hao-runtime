#include <mach/arm/thread_status.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern unsigned long trigger(unsigned long);
extern const char marker[], continued[];
__asm__(".text\n.align 2\n.globl _trigger\n_trigger:\nmov x9,x0\n"
        ".globl _marker\n_marker:\nbrk #0x123\nmov x9,#0\n"
        ".globl _continued\n_continued:\nmov x0,x9\nret\n");
static volatile sig_atomic_t hits, raised;
static int repeat;
static void handler(int sig, siginfo_t *si, void *context) {
    ucontext_t *uc = context;
    arm_thread_state64_t *state = &uc->uc_mcontext->__ss;
    if (sig != SIGTRAP) _exit(90);
    uintptr_t pc = arm_thread_state64_get_pc(*state);
    if (pc != (uintptr_t)marker) { raised++; return; }
    if (si->si_code != 0 || si->si_addr != marker || state->__x[9] != 71) _exit(91);
    hits++;
    if (repeat && hits == 1) return;
    state->__x[9] = 72;
    arm_thread_state64_set_pc_fptr(*state, (void *)continued);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "ignored")) {
        signal(SIGTRAP, SIG_IGN);
        if (write(1, "R", 1) != 1) return 96;
        trigger(71);
        return 94;
    }
    if (argc > 1 && !strcmp(argv[1], "blocked")) {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTRAP);
        sigprocmask(SIG_BLOCK, &blocked, NULL);
        if (write(1, "R", 1) != 1) return 96;
        trigger(71);
        return 95;
    }
    if (argc > 1 && !strcmp(argv[1], "default")) { trigger(71); return 92; }
    repeat = argc > 1 && !strcmp(argv[1], "repeat");
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    int once = argc > 1 && !strcmp(argv[1], "once");
    if (once) action.sa_flags |= SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP, &action, NULL) || (!once && raise(SIGTRAP))) return 93;
    unsigned long first = trigger(71), second = trigger(71);
    printf("stops=%d ordinary-raise=%d results=%lu,%lu\n", hits, raised, first, second);
    return !(hits == 2 + repeat && raised == !once && first == 72 && second == 72);
}
