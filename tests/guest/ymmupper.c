/*
 * Upper ymm halves at the guest boundary (tests/guest/ymmupper.sh): a native
 * function that returns with dirty uppers, then a native callee that reports
 * them, called from guest code; and a native caller that dirties them before
 * calling back into guest code, which reports them at once. hao clears them
 * after each native call and on entry to translated code, so both report
 * none, as on arm64, which has no ymm registers.
 */
#include <stdio.h>
unsigned ymm_upper(void);
void ymm_dirty(void);
unsigned ymm_call(unsigned (*fn)(void));

static unsigned report(void) { return ymm_upper(); }

int main(void)
{
    ymm_dirty();
    printf("after a native return: %02x\n", ymm_upper());
    printf("on a callback: %02x\n", ymm_call(report));
    return 0;
}
