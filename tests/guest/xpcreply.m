/* Guest code calls NSXPC reply blocks of different types. The exported object's reply blocks are
 * native forwarding blocks (__NSMakeSpecialForwardingCaptureBlock): every one has the same invoke
 * function, CoreFoundation's __forwarding_prep_b___, which reads its arguments by the calling
 * block's own signature. The first reply's type (a struct of two doubles, which the register
 * bridge cannot place) must not type the replies after it: two objects, a double between objects,
 * seven objects (two on the x86 stack), nine objects, and the first type again. An anonymous
 * listener in the same process. tests/guest/xpcreply.sh */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

@protocol Replies
- (void)sizeWithReply:(void (^)(CGSize, NSString *))reply;
- (void)pairWithReply:(void (^)(NSString *, NSString *))reply;
- (void)mixedWithReply:(void (^)(NSString *, double, NSString *, long))reply;
- (void)sevenWithReply:(void (^)(NSString *, NSString *, NSString *, NSString *, NSString *, NSString *, NSString *))reply;
- (void)nineWithReply:(void (^)(NSString *, NSString *, NSString *, NSString *, NSString *, NSString *, NSString *,
                                NSString *, NSString *))reply;
@end

@interface Server : NSObject <Replies, NSXPCListenerDelegate>
@end
@implementation Server
- (void)sizeWithReply:(void (^)(CGSize, NSString *))reply { reply(CGSizeMake(1280.5, 800.25), @"size"); }
- (void)pairWithReply:(void (^)(NSString *, NSString *))reply { reply(@"first", @"second"); }
- (void)mixedWithReply:(void (^)(NSString *, double, NSString *, long))reply { reply(@"m1", 2.75, @"m2", -42); }
- (void)sevenWithReply:(void (^)(NSString *, NSString *, NSString *, NSString *, NSString *, NSString *, NSString *))reply
{
    reply(@"s1", @"s2", @"s3", @"s4", @"s5", @"s6", @"s7");
}
- (void)nineWithReply:(void (^)(NSString *, NSString *, NSString *, NSString *, NSString *, NSString *, NSString *,
                                NSString *, NSString *))reply
{
    reply(@"n1", @"n2", @"n3", @"n4", @"n5", @"n6", @"n7", @"n8", @"n9");
}
- (BOOL)listener:(NSXPCListener *)l shouldAcceptNewConnection:(NSXPCConnection *)c
{
    c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(Replies)];
    c.exportedObject = self;
    [c resume];
    return YES;
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
        Server *srv = [Server new];
        NSXPCListener *l = [NSXPCListener anonymousListener];
        l.delegate = srv;
        [l resume];
        NSXPCConnection *c = [[NSXPCConnection alloc] initWithListenerEndpoint:l.endpoint];
        c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(Replies)];
        [c resume];
        id<Replies> p = [c remoteObjectProxyWithErrorHandler:^(NSError *e) { printf("error %ld\n", (long)e.code); }];
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        for (int round = 0; round < 2; round++) {
            [p sizeWithReply:^(CGSize z, NSString *t) {
                printf("size {%g,%g} %s\n", z.width, z.height, t.UTF8String);
                dispatch_semaphore_signal(s);
            }];
            wait_for(s, "size");
            [p pairWithReply:^(NSString *a, NSString *b) {
                printf("pair %s %s\n", a.UTF8String, b.UTF8String);
                dispatch_semaphore_signal(s);
            }];
            wait_for(s, "pair");
            [p mixedWithReply:^(NSString *a, double d, NSString *b, long n) {
                printf("mixed %s %g %s %ld\n", a.UTF8String, d, b.UTF8String, n);
                dispatch_semaphore_signal(s);
            }];
            wait_for(s, "mixed");
            [p sevenWithReply:^(NSString *a, NSString *b, NSString *c3, NSString *d, NSString *e, NSString *f, NSString *g) {
                printf("seven %s %s %s %s %s %s %s\n", a.UTF8String, b.UTF8String, c3.UTF8String, d.UTF8String,
                       e.UTF8String, f.UTF8String, g.UTF8String);
                dispatch_semaphore_signal(s);
            }];
            wait_for(s, "seven");
            [p nineWithReply:^(NSString *a, NSString *b, NSString *c3, NSString *d, NSString *e, NSString *f, NSString *g,
                               NSString *h, NSString *i) {
                printf("nine %s %s %s %s %s %s %s %s %s\n", a.UTF8String, b.UTF8String, c3.UTF8String, d.UTF8String,
                       e.UTF8String, f.UTF8String, g.UTF8String, h.UTF8String, i.UTF8String);
                dispatch_semaphore_signal(s);
            }];
            wait_for(s, "nine");
        }
        [c invalidate];
        [l invalidate];
    }
    printf("done\n");
    return 0;
}
