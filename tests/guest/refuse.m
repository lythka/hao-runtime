/* Calls hao cannot place are refused, not guessed (tests/guest/refuse.sh). One case per run:
 *   mid    a native C caller of a guest function returning 8 KB in memory, without a signature: the
 *          guest's result goes to hao's own area, not over the guest's live stack (the canary in this
 *          frame survives); the value is not the native one (no signature, known limit)
 *   call   an ObjC method taking 70000 bytes by value: refused (native: runs)
 *   block  a native call of a guest block whose parameter is 70000 bytes: refused (native: runs) */
#import <string.h>
#import "refusehelper.h"

static struct rf_mid make_mid(long k)
{
    struct rf_mid r;
    for (int i = 0; i < 1024; i++) r.w[i] = k + i;
    return r;
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "mid")) {
        volatile long canary[256];
        for (int i = 0; i < 256; i++) canary[i] = 0x5a5a0000 + i;
        long r = 0;
        for (int k = 0; k < 4; k++) r += rf_call_mid(make_mid, k);
        int bad = 0;
        for (int i = 0; i < 256; i++) bad += canary[i] != 0x5a5a0000 + i;
        printf("mid: calls %ld canary %s\n", r, bad ? "overwritten" : "intact");
        return bad != 0;
    }
    if (!strcmp(c, "call")) {
        static struct rf_huge v;
        v.b[0] = 3;
        v.b[sizeof v.b - 1] = 4;
        printf("call: %ld\n", [[RFNative new] take:v]);
        return 0;
    }
    if (!strcmp(c, "block")) {
        long r = rf_call_block(^long(struct rf_huge v) { return v.b[0] + v.b[sizeof v.b - 1]; }, 5);
        printf("block: %ld\n", r);
        return 0;
    }
    return 2;
}
