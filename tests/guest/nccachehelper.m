/* Native helper of tests/guest/nccache.sh: a class whose -odd: method returns
 * BOOL. On x86_64 its IMP leaves 0x5a in every byte above al, which SysV
 * allows and an Apple arm64 caller does not expect: only a typed plan (the
 * method's encoding, kind 'c') gives the guest the right value. nc_odd_imp
 * hands the IMP out from native code, so nothing types it on the way. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdbool.h>

@interface NCObj : NSObject   /* -(BOOL)odd:(int)v, added on first use */
@end

#if defined(__x86_64__)
__asm__(".text\n"
        "_nc_odd_x86:\n"
        "  movabsq $0x5a5a5a5a5a5a5a5a, %rax\n"
        "  testl $1, %edx\n"
        "  setne %al\n"
        "  ret\n");
extern BOOL nc_odd_x86(id self, SEL _cmd, int v);
#define ODD_IMP ((IMP)nc_odd_x86)
#else
static BOOL odd_c(id self, SEL _cmd, int v) { return (v & 1) != 0; }
#define ODD_IMP ((IMP)odd_c)
#endif

@implementation NCObj
+ (BOOL)resolveInstanceMethod:(SEL)sel
{
    if (sel == @selector(odd:)) {
        char types[16];
        snprintf(types, sizeof types, "%s@:i", @encode(BOOL));
        class_addMethod(self, sel, ODD_IMP, types);
        return YES;
    }
    return [super resolveInstanceMethod:sel];
}
@end

/* the IMP of -[NCObj odd:], found by native code (no guest import types it) */
IMP nc_odd_imp(void) { return class_getMethodImplementation([NCObj class], @selector(odd:)); }
id nc_new(void) { return [NCObj new]; }
