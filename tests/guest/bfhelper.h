/* Native helper for tests/guest/bitfields.m. Built universal (x86_64 +
 * arm64 + arm64e); under hao the guest binds to the x86_64 slice, so
 * BFNative's methods are native methods the guest calls, and bfh_drive /
 * bfh_block call guest methods and blocks from native code.
 * Types: aggregates holding bitfields, whose ObjC encodings give only the
 * bitfield widths, and two whose SysV classes depend on nesting. */
#ifndef BFHELPER_H
#define BFHELPER_H
#import <Foundation/Foundation.h>

typedef struct { unsigned a:8, b:8; float f; } BF3;                /* 8 bytes: one INTEGER eightbyte */
typedef struct { unsigned a:30; unsigned b:4; float f; } BFF;      /* 12 bytes: b at bit 32; INTEGER, SSE */
typedef struct { unsigned char a:4, b:4; char c; } BFC;           /* 2 bytes */
typedef struct { BFC s; char tail[20]; } BNEST;                    /* 22 bytes: memory / x8 */
typedef struct { float a; struct { float b; int c; } s; } NST;     /* x86_64: SSE (a, b), INTEGER (c) */
typedef union { float f; float g[2]; } UF2;                        /* x86_64: SSE; arm64: HFA of 2 */

/* canaries around a result slot: the slot is filled with 0x5a first */
#define BF_CANARY 0xc0dec0dec0dec0deull
typedef struct { uint64_t pre; NSDecimal d; uint32_t pad; uint64_t post[2]; } BFDecBox;
typedef struct { uint64_t pre; BNEST n; uint16_t pad; uint64_t post[2]; } BFNestBox;

@interface BFNative : NSObject
- (NSDecimal)dec:(NSDecimal)d plus:(int)k;
- (NSString *)describe:(NSDecimal)a tag:(int)t other:(NSDecimal)b;
- (BF3)bf3:(BF3)x k:(int)k;
- (int)sumBF3:(BF3)x :(BF3)y k:(int)k;
- (BFF)bff:(BFF)x;
- (BFC)bfc:(BFC)x k:(int)k;
- (BNEST)nest:(BNEST)n k:(int)k;
- (int)nestSum:(BNEST)a :(BNEST)b k:(int)k;
- (NST)nst:(NST)x;
- (UF2)uf:(UF2)u k:(int)k;
@end

/* Call the guest object's methods (same selectors as BFNative) from native
 * code, with canaries around the native result buffers; prints results. */
void bfh_drive(id guest);
/* Call a guest block taking and returning NSDecimal from native code. */
typedef NSDecimal (^bfh_dec_block_t)(NSDecimal d, int k);
typedef BF3 (^bfh_bf3_block_t)(BF3 x, int k);
void bfh_block(bfh_dec_block_t b, bfh_bf3_block_t b3);
/* print a decimal and check a box's canaries (shared by both sides) */
void bfh_print_dec(const char *what, const NSDecimal *d);
void bfh_check_box(const char *what, const void *box, size_t size, const void *slot, size_t slot_size);
#endif
