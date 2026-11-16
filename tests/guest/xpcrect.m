/* Messages no method implements, sent to native forwarding objects: an NSXPC remote object proxy and
 * an NSProtocolChecker. Native forwarding reads the x86 frame by the receiver's
 * -methodSignatureForSelector:, so the call must be placed by that signature: a struct of four floats
 * (s0-s3 on arm64, two floats per xmm register on x86_64), a CGRect (d0-d3; the x86 stack) with a
 * CGPoint and a float after it, and struct results (an HFA in v0-v3; the x86 _stret forwarding).
 * Then a guest forwarder: -forwardingTargetForSelector: for one message, -forwardInvocation: for others.
 * tests/guest/xpcrect.sh */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>

typedef struct FloatRect { float x, y, w, h; } FloatRect;

@protocol MenuHelper
- (void)popupWithMenu:(NSData *)menu direction:(long)direction rect:(FloatRect)rect dark:(BOOL)dark
          orientation:(long)orientation fromKeyboard:(BOOL)kb touchInitiated:(BOOL)touch
accessibilityContainer:(NSData *)ac withReply:(void (^)(NSData *))reply;
- (void)placeRect:(CGRect)r point:(CGPoint)p scale:(float)s withReply:(void (^)(NSString *))reply;
@end

@protocol Geometry
- (CGRect)inset:(CGRect)r by:(double)d;
- (FloatRect)flip:(FloatRect)r height:(float)h;
- (CGPoint)mid:(CGRect)r;
@end

@interface Helper : NSObject <MenuHelper, Geometry, NSXPCListenerDelegate>
@end
@implementation Helper
- (void)popupWithMenu:(NSData *)menu direction:(long)direction rect:(FloatRect)rect dark:(BOOL)dark
          orientation:(long)orientation fromKeyboard:(BOOL)kb touchInitiated:(BOOL)touch
accessibilityContainer:(NSData *)ac withReply:(void (^)(NSData *))reply
{
    printf("popup menu=%lu dir=%ld rect={%g,%g,%g,%g} dark=%d orient=%ld kb=%d touch=%d ac=%lu\n",
           (unsigned long)menu.length, direction, rect.x, rect.y, rect.w, rect.h, dark, orientation, kb, touch,
           (unsigned long)ac.length);
    reply([NSData dataWithBytes:"ok" length:2]);
}
- (void)placeRect:(CGRect)r point:(CGPoint)p scale:(float)s withReply:(void (^)(NSString *))reply
{
    reply([NSString stringWithFormat:@"place rect={%g,%g,%g,%g} point={%g,%g} scale=%g", r.origin.x, r.origin.y,
                                     r.size.width, r.size.height, p.x, p.y, s]);
}
- (CGRect)inset:(CGRect)r by:(double)d { return CGRectInset(r, d, d); }
- (FloatRect)flip:(FloatRect)r height:(float)h { return (FloatRect){r.x, h - r.y - r.h, r.w, r.h}; }
- (CGPoint)mid:(CGRect)r { return CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r)); }
- (BOOL)listener:(NSXPCListener *)l shouldAcceptNewConnection:(NSXPCConnection *)c
{
    c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MenuHelper)];
    c.exportedObject = self;
    [c resume];
    return YES;
}
@end

@interface Forwarder : NSObject
@property(strong) Helper *target;
@end
@implementation Forwarder
- (id)forwardingTargetForSelector:(SEL)sel
{
    return sel == @selector(inset:by:) ? self.target : nil;
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
    return [self.target methodSignatureForSelector:sel] ?: [super methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)inv
{
    printf("forwardInvocation %s\n", sel_getName(inv.selector));
    [inv invokeWithTarget:self.target];
}
@end

static void wait_for(dispatch_semaphore_t s, const char *what)
{
    if (dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC))) printf("%s: no reply\n", what);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    @autoreleasepool {
        Helper *h = [Helper new];
        NSXPCListener *l = [NSXPCListener anonymousListener];
        l.delegate = h;
        [l resume];
        NSXPCConnection *c = [[NSXPCConnection alloc] initWithListenerEndpoint:l.endpoint];
        c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MenuHelper)];
        [c resume];
        id<MenuHelper> p = [c remoteObjectProxy];
        SEL popup = @selector(popupWithMenu:direction:rect:dark:orientation:fromKeyboard:touchInitiated:accessibilityContainer:withReply:);
        printf("proxy implements popup: %s\n", class_getInstanceMethod(object_getClass(p), popup) ? "yes" : "no");

        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        FloatRect fr = {640.5f, 742.f, 52.f, 60.f};
        [p popupWithMenu:[NSData dataWithBytes:"abc" length:3] direction:3 rect:fr dark:YES orientation:2
            fromKeyboard:NO touchInitiated:YES accessibilityContainer:[NSData dataWithBytes:"xy" length:2]
               withReply:^(NSData *d) { printf("popup reply %lu\n", (unsigned long)d.length); dispatch_semaphore_signal(s); }];
        wait_for(s, "popup");
        [p placeRect:CGRectMake(12.25, -3.5, 1280, 800) point:CGPointMake(687, 767.5) scale:2.0f
           withReply:^(NSString *r) { printf("%s\n", r.UTF8String); dispatch_semaphore_signal(s); }];
        wait_for(s, "placeRect");
        [c invalidate];

        /* a native NSProxy forwarding to a guest object: struct arguments and results */
        id<Geometry> g = (id<Geometry>)[NSProtocolChecker protocolCheckerWithTarget:h protocol:@protocol(Geometry)];
        CGRect r = [g inset:CGRectMake(10, 20, 300, 200) by:4.5];
        printf("inset {%g,%g,%g,%g}\n", r.origin.x, r.origin.y, r.size.width, r.size.height);
        FloatRect f = [g flip:fr height:800.f];
        printf("flip {%g,%g,%g,%g}\n", f.x, f.y, f.w, f.h);
        CGPoint m = [g mid:CGRectMake(600, 740, 60, 54)];
        printf("mid {%g,%g}\n", m.x, m.y);

        Forwarder *fw = [Forwarder new];
        fw.target = h;
        id<Geometry> gf = (id<Geometry>)fw;
        r = [gf inset:CGRectMake(1, 2, 30, 40) by:0.5];
        printf("forwarded inset {%g,%g,%g,%g}\n", r.origin.x, r.origin.y, r.size.width, r.size.height);
        f = [gf flip:fr height:600.f];
        printf("forwarded flip {%g,%g,%g,%g}\n", f.x, f.y, f.w, f.h);
        m = [gf mid:CGRectMake(0, 0, 1280, 800)];
        printf("forwarded mid {%g,%g}\n", m.x, m.y);
    }
    return 0;
}
