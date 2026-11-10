#import <Foundation/Foundation.h>
typedef struct { double d; int i; } Mixed;
typedef struct { long a, b, c; } Big;
typedef struct { float a; struct { float b; int c; } s; } Nest;   /* x86_64: SSE (a, b), INT (c) */
@interface Shape : NSObject
- (NSRect)grow:(NSRect)r by:(double)d;
- (NSRange)span:(NSRange)r;
- (Mixed)mix:(Mixed)m;
- (Big)big:(Big)b extra:(float)f;
- (double)area:(NSRect)r;
- (Nest)nest:(Nest)n;
@end
@implementation Shape
- (NSRect)grow:(NSRect)r by:(double)d { return NSMakeRect(r.origin.x - d, r.origin.y - d, r.size.width + 2*d, r.size.height + 2*d); }
- (NSRange)span:(NSRange)r { return NSMakeRange(r.location + 1, r.length * 2); }
- (Mixed)mix:(Mixed)m { Mixed r = {m.d * 2, m.i + 1}; return r; }
- (Big)big:(Big)b extra:(float)f { Big r = {b.a + (long)f, b.b * 2, b.c - 1}; return r; }
- (double)area:(NSRect)r { return r.size.width * r.size.height; }
- (Nest)nest:(Nest)n { Nest r = {n.a + 1, {n.s.b * 2, n.s.c + 3}}; return r; }
@end
int main(void) { @autoreleasepool {
    Shape *s = [Shape new];
    NSRect g = [s grow:NSMakeRect(1, 2, 3, 4) by:0.5];
    printf("guest->guest grow: %s\n", NSStringFromRect(g).UTF8String);
    NSRange rg = [s span:NSMakeRange(5, 7)];
    Mixed mx = [s mix:(Mixed){1.25, 41}];
    Big bg = [s big:(Big){1, 2, 3} extra:9.0f];
    printf("span %lu,%lu mix %.2f,%d big %ld,%ld,%ld area %.1f\n", rg.location, rg.length, mx.d, mx.i, bg.a, bg.b, bg.c, [s area:g]);
    /* native -> guest through NSInvocation (native objc_msgSend into the guest IMP) */
    NSMethodSignature *sig = [s methodSignatureForSelector:@selector(grow:by:)];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
    inv.target = s; inv.selector = @selector(grow:by:);
    NSRect in = NSMakeRect(10, 20, 30, 40); double d = 2;
    [inv setArgument:&in atIndex:2]; [inv setArgument:&d atIndex:3]; [inv invoke];
    NSRect out; [inv getReturnValue:&out];
    printf("native->guest grow: %s\n", NSStringFromRect(out).UTF8String);
    NSInvocation *i2 = [NSInvocation invocationWithMethodSignature:[s methodSignatureForSelector:@selector(big:extra:)]];
    i2.target = s; i2.selector = @selector(big:extra:);
    Big b0 = {100, 200, 300}; float f = 7.0f;
    [i2 setArgument:&b0 atIndex:2]; [i2 setArgument:&f atIndex:3]; [i2 invoke];
    Big b1; [i2 getReturnValue:&b1];
    printf("native->guest big: %ld,%ld,%ld\n", b1.a, b1.b, b1.c);
    Nest n0 = {1.5f, {2.5f, 40}}, n1 = [s nest:n0], n2;
    NSInvocation *i3 = [NSInvocation invocationWithMethodSignature:[s methodSignatureForSelector:@selector(nest:)]];
    i3.target = s; i3.selector = @selector(nest:);
    [i3 setArgument:&n0 atIndex:2]; [i3 invoke]; [i3 getReturnValue:&n2];
    printf("nest guest %g,{%g,%d} native->guest %g,{%g,%d}\n", n1.a, n1.s.b, n1.s.c, n2.a, n2.s.b, n2.s.c);
    /* guest -> native struct returns */
    NSValue *v = [NSValue valueWithRect:NSMakeRect(1.5, 2.5, 3.5, 4.5)];
    NSRect vr = v.rectValue;
    NSRange r2 = [@"hello world" rangeOfString:@"world"];
    printf("native rectValue %s range %lu,%lu\n", NSStringFromRect(vr).UTF8String, r2.location, r2.length);
    NSArray *a = [NSArray arrayWithObjects:@"x", @"y", @"z", nil];
    NSString *fs = [NSString stringWithFormat:@"%@ %d %.3f %@", @"fmt", 42, 2.5, a];
    printf("%s\n", [fs stringByReplacingOccurrencesOfString:@"\n" withString:@""].UTF8String);
} return 0; }
