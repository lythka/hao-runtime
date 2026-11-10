/* Native helper for tests/guest/narrowret.m. Built universal (x86_64 + arm64 +
 * arm64e). Its x86_64 slice returns bool/char/short results the way x86_64
 * SysV allows: al/ax hold the value and the rest of rax holds other bits
 * (0x5a5a...). Its arm64 slices follow Apple's ABI (extended to 32 bits), so
 * native arm64 is the reference. */
#ifndef NARROWHELPER_H
#define NARROWHELPER_H
#import <Foundation/Foundation.h>
#include <stdbool.h>

typedef BOOL (^nh_pred_t)(int);

@interface NHNarrow : NSObject
+ (BOOL)classNo;
- (BOOL)boolNo;
- (BOOL)boolYes;
- (char)charMinus;                        /* -100 */
- (short)shortMinus;                      /* -1234 */
- (unsigned short)ushortBig;              /* 0xfedc */
- (unsigned char)ucharBig;                /* 0xe7 */
- (signed char)charForRect:(NSRect)r;     /* -(int)width: the signature path (NSRect is an HFA / MEMORY) */
- (nh_pred_t)predicate;                   /* native block: x > 5 */
@end

/* calls cb(native predicate block, rect): the guest block has a struct
 * argument, so it is entered through a signature thunk */
void nh_call_with_pred(void (^cb)(nh_pred_t pred, CGRect r));
#endif
