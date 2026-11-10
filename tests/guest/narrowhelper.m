/* Native helper for tests/guest/narrowret.m (see narrowhelper.h). */
#import "narrowhelper.h"

/* v truncated to the narrow type; on x86_64 the bits above al/ax are 0x5a */
signed char nh_c(int v);
short nh_s(int v);
#if defined(__x86_64__)
__asm__(".text\n.globl _nh_c\n_nh_c:\n  movabsq $0x5a5a5a5a5a5a5a5a, %rax\n  movb %dil, %al\n  ret\n"
        ".globl _nh_s\n_nh_s:\n  movabsq $0x5a5a5a5a5a5a5a5a, %rax\n  movw %di, %ax\n  ret\n");
#else
signed char nh_c(int v) { return (signed char)v; }
short nh_s(int v) { return (short)v; }
#endif

@implementation NHNarrow
+ (BOOL)classNo { return (BOOL)nh_c(0); }
- (BOOL)boolNo { return (BOOL)nh_c(0); }
- (BOOL)boolYes { return (BOOL)nh_c(1); }
- (char)charMinus { return (char)nh_c(-100); }
- (short)shortMinus { return nh_s(-1234); }
- (unsigned short)ushortBig { return (unsigned short)nh_s(0xfedc); }
- (unsigned char)ucharBig { return (unsigned char)nh_c(0xe7); }
- (signed char)charForRect:(NSRect)r { return nh_c(-(int)r.size.width); }
- (nh_pred_t)predicate { return [^BOOL(int x) { return (BOOL)nh_c(x > 5); } copy]; }
@end

void nh_call_with_pred(void (^cb)(nh_pred_t pred, CGRect r))
{
    cb(^BOOL(int x) { return (BOOL)nh_c(x < 0); }, CGRectMake(1, 2, 3, 4));
}
