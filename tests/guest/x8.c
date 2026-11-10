/* A guest function returning a struct in memory (arm64: through x8),
 * called from native C through a plain thunk (tests/guest/indirect-x8.sh).
 * The native caller's rax is the thunk itself; hao must not hand it to the
 * guest as x8, or the guest's result store overwrites the thunk and the
 * next call through it crashes. The struct value the C caller reads still
 * needs the callee's signature, so only the survival and an ordinary
 * callback through the same path are checked. */
#include <stdio.h>
#include "x8helper.h"

static struct x8_l3 make(long k) { struct x8_l3 r = {k, k + 1, k + 2}; return r; }
static long twice(long k) { return 2 * k; }

int main(void)
{
    for (long k = 0; k < 4; k++) (void)x8_call_l3(make, k + 10);
    printf("after struct callbacks: %ld\n", x8_call_int(twice, 20));
    for (long k = 0; k < 4; k++) (void)x8_call_l3(make, k + 20);
    printf("again: %ld\n", x8_call_int(twice, 21));
    struct x8_l3 r = make(5);
    printf("direct: %ld %ld %ld\n", r.a, r.b, r.c);
    return 0;
}
