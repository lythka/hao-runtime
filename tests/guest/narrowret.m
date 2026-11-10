/* Narrow integer results across the bridge. Apple's arm64 ABI makes the
 * callee extend a bool/char/short result to 32 bits and arm64 callers rely
 * on it (cbz w0, cmp w0, sxtw); x86_64 callees define only al/ax. Each line
 * prints what compiled code concluded and the raw w0 the callee left, so a
 * native x86_64 result reaching the guest unextended shows as a difference
 * from native arm64. Run by tests/guest/narrowret.sh. */
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreServices/CoreServices.h>
#import <objc/message.h>
#include <os/lock.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <xpc/xpc.h>
#import "narrowhelper.h"

/* call fn(a, b) from hand-written code and return w0 exactly as the callee left it */
static uint32_t raw_w0(void *fn, uint64_t a, uint64_t b)
{
    register uint64_t x0 __asm__("x0") = a;
    register uint64_t x1 __asm__("x1") = b;
    register void *x16 __asm__("x16") = fn;
    __asm__ volatile("blr x16" : "+r"(x0), "+r"(x1), "+r"(x16) :
                     : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                       "x17", "x30", "memory", "cc");
    return (uint32_t)x0;
}

/* raw w0 of [o s] through objc_msgSend */
static uint32_t raw_msg(id o, SEL s)
{
    return raw_w0((void *)objc_msgSend, (uint64_t)(uintptr_t)(__bridge void *)o, (uint64_t)(uintptr_t)(const void *)s);
}

#define TF(e) ((e) ? "true" : "false")

int main(void)
{
    @autoreleasepool {
        /* libxpc bool getters: launchd's KeepAlive/RunAtLoad/Disabled pattern */
        xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_bool(d, "KeepAlive", false);
        xpc_dictionary_set_bool(d, "RunAtLoad", true);
        printf("xpc_dictionary_get_bool(false key) %s raw=%#x\n", TF(xpc_dictionary_get_bool(d, "KeepAlive")),
               raw_w0((void *)xpc_dictionary_get_bool, (uint64_t)d, (uint64_t)"KeepAlive"));
        printf("xpc_dictionary_get_bool(true key) %s raw=%#x\n", TF(xpc_dictionary_get_bool(d, "RunAtLoad")),
               raw_w0((void *)xpc_dictionary_get_bool, (uint64_t)d, (uint64_t)"RunAtLoad"));
        printf("xpc_dictionary_get_bool(missing) %s\n", TF(xpc_dictionary_get_bool(d, "Disabled")));
        xpc_object_t f = xpc_dictionary_get_value(d, "KeepAlive"), t = xpc_dictionary_get_value(d, "RunAtLoad");
        printf("xpc_bool_get_value(false) %s raw=%#x\n", TF(xpc_bool_get_value(f)), raw_w0((void *)xpc_bool_get_value, (uint64_t)f, 0));
        printf("xpc_bool_get_value(true) %s raw=%#x\n", TF(xpc_bool_get_value(t)), raw_w0((void *)xpc_bool_get_value, (uint64_t)t, 0));
        printf("xpc_bool_get_value(XPC_BOOL_FALSE) %s\n", TF(xpc_bool_get_value(XPC_BOOL_FALSE)));
        int sum = 0;
        for (int k = 0; k < 4; k++) sum += xpc_dictionary_get_bool(d, k & 1 ? "RunAtLoad" : "KeepAlive");
        printf("sum of four get_bool results %d\n", sum);
        /* through a function pointer (the import's address taken) */
        bool (*volatile getb)(xpc_object_t, const char *) = xpc_dictionary_get_bool;
        printf("get_bool via pointer %s\n", TF(getb(d, "KeepAlive")));

        /* os_unfair_lock_trylock on a held lock must fail */
        static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
        os_unfair_lock_lock(&lock);
        printf("os_unfair_lock_trylock(held) %s raw=%#x\n", TF(os_unfair_lock_trylock(&lock)),
               raw_w0((void *)os_unfair_lock_trylock, (uint64_t)&lock, 0));
        os_unfair_lock_unlock(&lock);
        bool got = os_unfair_lock_trylock(&lock);
        printf("os_unfair_lock_trylock(free) %s\n", TF(got));
        if (got) os_unfair_lock_unlock(&lock);

        /* CoreFoundation Boolean (unsigned char) */
        CFStringRef a = CFSTR("launchd"), b = CFSTR("xpcproxy");
        printf("CFEqual(different) %s raw=%#x\n", TF(CFEqual(a, b)), raw_w0((void *)CFEqual, (uint64_t)a, (uint64_t)b));
        printf("CFEqual(same) %s\n", TF(CFEqual(a, a)));
        printf("CFStringHasPrefix(no) %s raw=%#x\n", TF(CFStringHasPrefix(a, b)),
               raw_w0((void *)CFStringHasPrefix, (uint64_t)a, (uint64_t)b));
        printf("CFBooleanGetValue(kCFBooleanFalse) %s raw=%#x\n", TF(CFBooleanGetValue(kCFBooleanFalse)),
               raw_w0((void *)CFBooleanGetValue, (uint64_t)kCFBooleanFalse, 0));
        printf("CFStringGetCharacterAtIndex %d raw=%#x\n", (int)CFStringGetCharacterAtIndex(a, 0),
               raw_w0((void *)CFStringGetCharacterAtIndex, (uint64_t)a, 0));

        /* signed short (OSErr) and unsigned short (mode_t) from C */
        SInt32 resp = 0;
        OSErr e = Gestalt('zzzz', &resp);
        printf("Gestalt(unknown selector) %d negative=%s raw=%#x\n", (int)e, TF(e < 0),
               raw_w0((void *)Gestalt, 'zzzz', (uint64_t)&resp));
        mode_t old = umask(022);
        umask(old);
        printf("umask round trip %s raw=%#x\n", TF(umask(old) == old), raw_w0((void *)umask, old, 0));

        /* Objective-C methods of a native class: BOOL/char/short results */
        NHNarrow *n = [NHNarrow new];
        printf("+classNo %s\n", TF([NHNarrow classNo]));
        printf("-boolNo %s raw=%#x\n", TF([n boolNo]), raw_msg(n, @selector(boolNo)));
        printf("-boolYes %s\n", TF([n boolYes]));
        printf("-charMinus %d raw=%#x\n", (int)[n charMinus], raw_msg(n, @selector(charMinus)));
        printf("-shortMinus %d raw=%#x\n", (int)[n shortMinus], raw_msg(n, @selector(shortMinus)));
        printf("-ushortBig %d raw=%#x\n", (int)[n ushortBig], raw_msg(n, @selector(ushortBig)));
        printf("-ucharBig %d\n", (int)[n ucharBig]);
        printf("-charForRect: %d\n", (int)[n charForRect:NSMakeRect(0, 0, 7, 1)]);
        long long wide = [n shortMinus];
        printf("shortMinus as long long %lld\n", wide);

        /* native blocks called from guest code */
        nh_pred_t p = [n predicate];
        printf("native block from a method: pred(3) %s pred(9) %s\n", TF(p(3)), TF(p(9)));
        nh_call_with_pred(^(nh_pred_t q, CGRect r) {
            printf("native block argument: q(1) %s q(-1) %s rect %.0f %.0f %.0f %.0f\n", TF(q(1)), TF(q(-1)),
                   r.origin.x, r.origin.y, r.size.width, r.size.height);
        });
    }
    return 0;
}
