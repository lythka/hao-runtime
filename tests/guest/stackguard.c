/* Values that point into the main thread's stack guard, handed to native functions in argument slots
 * (tests/guest/stackguard.sh). Natively nothing reads them. hao's argument scan reads a slot's target to
 * see whether it is a guest block, and xnu takes a protection fault in [user_stack - MAXSSIZ, user_stack)
 * for a stack overflow, delivering SIGSEGV with SIG_DFL forced unless the handler is SA_ONSTACK: the
 * process died ("Thread stack size exceeded", e.g. ControlCenter with HAO_BRIDGE_CHECK=1). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

long sg_sink8(long, long, long, long, long, long, long, long);
long sg_sink10(long, long, long, long, long, long, long, long, long, long);

int main(void)
{
    pthread_t m = pthread_self();   /* main runs on the main thread */
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(m), size = pthread_get_stacksize_np(m);
    uintptr_t bottom = top - size;
    /* the guard below the stack: just below it, in the middle, near the reservation's bottom */
    uintptr_t g[] = {bottom - 0x1000 + 0x300, bottom - (24u << 20) + 0x40, top - (64u << 20) + 0x2000};
    long r = 0;
    for (unsigned k = 0; k < sizeof g / sizeof *g; k++) {
        long v = (long)g[k];
        r += sg_sink8(v, v, v, v, v, v, v, v);
        r += sg_sink10(1, 2, 3, 4, 5, 6, 7, 8, v, v);
    }
    printf("stackguard: %ld\n", r);
    return 0;
}
