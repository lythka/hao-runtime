#import <Foundation/Foundation.h>

NSError *fp_error(const char *func, int line, NSString *domain, NSInteger code, NSError *underlying,
                  NSDictionary *info, NSString *fmt, ...);
NSString *fp_six(long a, long b, long c, long d, long e, NSString *fmt, ...);
const char *fp_eight(long a, long b, long c, long d, long e, long f, long g, const char *fmt, ...);
const char *fp_tagged(const char *fmt, long tag, ...);
const char *fp_unopt(long tag, const char *fmt, ...);

static void show(NSError *e)
{
    printf("error %s %ld: %s (%s:%s)%s\n", e.domain.UTF8String, (long)e.code, e.localizedDescription.UTF8String,
           [e.userInfo[@"func"] UTF8String], [e.userInfo[@"line"] description].UTF8String,
           e.userInfo[NSUnderlyingErrorKey] ? " +underlying" : "");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        NSString *client = @"client<7>", *bundle = @"com.example.app";
        show(fp_error(__func__, 323, @"TestErrorDomain", 18, nil, nil,
                      @"Promise was not attached and is no longer referenced by client %@", client));
        show(fp_error(__func__, 411, @"TestErrorDomain", 6, nil, nil,
                      @"Failed to locate an install with bundle ID %@ for client %@", bundle, client));
        NSError *under = [NSError errorWithDomain:@"Under" code:2 userInfo:nil];
        show(fp_error("f", 9, @"TestErrorDomain", 34, under, @{@"k" : @"v"}, @"%d %@ %s %ld %@ %.3f %@ %f %@", 5,
                      @"obj", "cstr", 1L << 40, @[ @1, @2 ].description, 2.0 / 3, @3, 1.5, bundle));
        show(fp_error("g", 10, @"TestErrorDomain", 1, nil, nil, @"%f %f %f %f %f %f %f %f %f %f %@ %d", 1.0, 2.0,
                      3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, client, 11));
        printf("six   %s\n", fp_six(1, 2, 3, 4, 5, @"%@ %d %.2f %@", client, 6, 7.25, bundle).UTF8String);
        printf("eight %s\n", fp_eight(1, 2, 3, 4, 5, 6, 7, "%s %d %.2f %ld", "str", 8, 9.5, 10L));
        printf("tag   %s\n", fp_tagged("%s %.2f %d %g", 42, "t", 0.5, 3, 4.25));
        printf("unopt %s\n", fp_unopt(43, "%s %.2f %d %ld", "u", 1.25, 4, 5L));
    }
    printf("done\n");
    return 0;
}
