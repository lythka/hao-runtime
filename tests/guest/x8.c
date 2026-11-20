/* A guest function returning a struct in memory (arm64: through x8),
 * called from native C through a plain thunk (tests/guest/indirect-x8.sh).
 * The native caller's rax is the thunk itself; hao must not hand it to the
 * guest as x8, or the guest's result store overwrites the thunk and the
 * next call through it crashes. The struct value the C caller reads still
 * needs the callee's signature, so only the survival and an ordinary
 * callback through the same path are checked.
 *
 * x8_store reads x8 at its entry (an indirect-result function); x8_calls
 * passes x8 unwritten to x8_store (a forwarder, the shape of the SwiftUI
 * closures Weather hands over). From an image with Swift metadata, rax is x8
 * for both, since their own code takes x8 as an input; from a C image never:
 * x8_store2 (a copy of x8_store, used only by the C-image case) keeps the C
 * caller's stale rax off its buffer.
 * x8_hidden stores through x8 only behind an indirect call, which its own
 * code does not show: not forwarded, so the stale rax is not written (a rule
 * on rax alone would write it). */
#include <stdio.h>
#include <stdlib.h>
#include "x8helper.h"

long x8_store(void);
long x8_calls(void);
long x8_store2(void);
long x8_hidden(void);
__asm__(".text\n.p2align 2\n"
        "_x8_store: mov x9, #0x77\n str x9, [x8]\n mov x0, #1\n ret\n"
        "_x8_calls: stp x29, x30, [sp, #-16]!\n mov x29, sp\n bl _x8_store\n ldp x29, x30, [sp], #16\n ret\n"
        "_x8_store2: mov x9, #0x77\n str x9, [x8]\n mov x0, #1\n ret\n"
        "_x8_hidden: stp x29, x30, [sp, #-16]!\n mov x29, sp\n adrp x9, _x8_store@PAGE\n add x9, x9, _x8_store@PAGEOFF\n blr x9\n ldp x29, x30, [sp], #16\n ret\n");

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
    long *b = calloc(4, sizeof *b);
    long rc = x8s_call_with(x8_store, b);
    printf("swift image, x8 read: %ld %#lx\n", rc, b[0]);
    /* Forwarded: x8_calls hands x8 to a callee that stores through it, so it
     * returns indirectly; the caller passes its buffer, which gets the store. */
    b[1] = 0x5a5a;
    rc = x8s_call_with(x8_calls, &b[1]);
    printf("swift image, x8 forwarded to a callee: %ld %#lx\n", rc, b[1]);
    b[2] = 0x5a5a;
    rc = x8c_call_stale(x8_store2, &b[2]);
    printf("C image, x8 read: %ld %#lx\n", rc, b[2]);
    b[3] = 0x5a5a;
    rc = x8s_call_stale(x8_hidden, &b[3]);
    printf("swift image, x8 used only behind an indirect call: %ld %#lx\n", rc, b[3]);
    free(b);
    return 0;
}
