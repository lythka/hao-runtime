/* tests/guest/objcimp.sh: a native ObjC class with QuartzCore's method shapes
 * (CALayer setBounds:/setTransform:/setPosition:, which hao must not load on
 * the development Mac): a CGRect (HFA of 4 doubles), a CATransform3D (16
 * doubles, MEMORY on both ABIs), a CGPoint, a BOOL result, a rect result and
 * a 24-byte result returned through memory. Built for x86_64, arm64 and arm64e. */
#import <Foundation/Foundation.h>
typedef struct { double x, y; } HPoint;
typedef struct { HPoint origin; struct { double w, h; } size; } HRect;
typedef struct { double m[16]; } HTransform;
typedef struct { long a, b, c; } HTriple;
typedef void (^HRectBlock)(HRect);
@interface HaoLayer : NSObject
- (void)setBounds:(HRect)r;
- (HRect)bounds;
- (void)setTransform:(HTransform)t;
- (HTransform)transform;
- (void)setPosition:(HPoint)p;
- (HPoint)position;
- (BOOL)isWide;
- (HTriple)triple;
- (float)scale:(float)f rect:(HRect)r;
+ (HRectBlock)rectBlock;
@end
HRectBlock hao_rect_block(void);
/* Native blocks whose descriptor signature (v40@?0{?={?=dd}{?=dd}}8, or an 84-byte one) lies where
 * hao_native_block_sig must read past one guarded piece: cases 0-4 cross a page boundary 1, 2, 8,
 * 16 and 22 bytes after their start, 5 has only its NUL in the next page, 6 ends at the page's
 * last byte, 7 is the long one, crossing after 40 bytes. Each has its own invoke; NULL past them. */
#define HAO_SPLIT_CASES 8
HRectBlock hao_split_block(int i);
double hao_last_sum(void);
