/* Guest blocks called by native code: struct marshalling through block
 * signatures, copy/dispose helpers, patched invoke called again by the
 * guest, global blocks, blocks through objc_msgSend, extended signatures.
 * Build with tests/guest/blocks2.sh; compare arm64e under hao with native
 * arm64. Optional argv[1]: run one section (1..6). */
#import <Foundation/Foundation.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blockhelper.h"

const char *_Block_signature(void *);   /* libsystem_blocks */
/* native _Block_copy under ARC; ARC releases the copy (native _Block_release) */
#define BCOPY(T, b) ((__bridge_transfer T)_Block_copy((__bridge const void *)(b)))

static atomic_int deallocs;
@interface Tracker : NSObject
@property int tag;
@end
@implementation Tracker
- (void)dealloc { atomic_fetch_add(&deallocs, 1); }
@end

static void pr(const char *what, CGRect r) { printf("%s: {%.3f,%.3f,%.3f,%.3f}\n", what, r.origin.x, r.origin.y, r.size.width, r.size.height); }
static const char *sig(id b) { const char *s = _Block_signature((__bridge void *)b); return s ? s : "(none)"; }

/* ---- 1: native code invokes guest blocks with struct args/returns */
static void t1(void)
{
    double k = 0.25;   /* captured: stack blocks */
    hb_rect_t grow = ^CGRect(CGRect r, double s) {
        return CGRectMake(r.origin.x - s, r.origin.y - s * k, r.size.width + 2 * s, r.size.height + 2 * s * k);
    };
    CGRect in = CGRectMake(1, 2, 30, 40), out;
    hb_rect(grow, &in, 2.0, &out);
    pr("t1 rect", out);

    int bias = 7;
    hb_mixed_t mix = ^HMixed(HMixed a, HMixed b) { return (HMixed){a.d * b.d + bias, a.i * b.i + bias}; };
    HMixed m; hb_mixed(mix, 1.5, 3, 2.5, 5, &m);
    printf("t1 mixed: %.3f %d\n", m.d, m.i);

    long mul = 3;
    hb_big_t big = ^HBig(HBig b, int kk) { return (HBig){b.a * mul, b.b + kk, b.c - kk, b.d * kk, b.e + mul}; };
    HBig g; hb_big(big, 100, 9, &g);
    printf("t1 big: %ld %ld %ld %ld %ld\n", g.a, g.b, g.c, g.d, g.e);

    float off = 0.5f;
    hb_f3_t f3 = ^HF3(HF3 v, float s) { return (HF3){v.x * s + off, v.y * s + off, v.z * s + off}; };
    HF3 f; hb_f3(f3, 1, 2, 3, 4, &f);
    printf("t1 f3: %.3f %.3f %.3f\n", f.x, f.y, f.z);

    long base = 1000;
    hb_many_t many = ^long(long a, long b, long c, long d, long e, long ff, long gg) {
        return base + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * ff + 7 * gg;
    };
    printf("t1 many: %ld\n", hb_many(many, 10));

    hb_fmix_t fm = ^double(int a, double b, float c, long d, double e) { return a + b * 10 + c * 100 + d * 1000 + e * k; };
    printf("t1 fmix: %.4f\n", hb_fmix(fm, 3, 1.25));
    printf("t1 arg9: %ld\n", hb_arg9(1, 2, 3, 4, 5, 6, 7, 8, ^long(long v) { return v * base; }));
}

/* ---- 2: copy/dispose helpers: captured objects and __block variables */
static void t2(void)
{
    deallocs = 0;
    @autoreleasepool {
        dispatch_queue_t conc = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
        dispatch_queue_t ser = dispatch_queue_create("t2.serial", NULL);
        dispatch_group_t grp = dispatch_group_create();
        __block int counter = 0;                          /* byref, no helpers */
        __block NSMutableArray *acc = [NSMutableArray new]; /* byref with keep/destroy helpers */
        for (int i = 0; i < 64; i++) {
            Tracker *tr = [Tracker new];
            tr.tag = i;
            NSString *name = [NSString stringWithFormat:@"item%d", i];
            dispatch_group_async(grp, conc, ^{
                int v = tr.tag * 2 + (int)name.length;
                dispatch_sync(ser, ^{ counter += v; [acc addObject:name]; });
            });
        }
        long w = dispatch_group_wait(grp, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
        printf("t2 group: wait=%ld counter=%d acc=%lu\n", w, counter, (unsigned long)acc.count);

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block NSString *joined = nil;
        Tracker *t0 = [Tracker new]; t0.tag = 42;
        dispatch_async(conc, ^{
            NSArray *sorted = [acc sortedArrayUsingSelector:@selector(compare:)];
            joined = [NSString stringWithFormat:@"%@..%@ tag=%d", sorted.firstObject, sorted.lastObject, t0.tag];
            dispatch_semaphore_signal(sem);
        });
        long sw = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
        printf("t2 sem: wait=%ld %s\n", sw, joined.UTF8String);

        /* notify + async on a serial queue, nested copies */
        dispatch_group_t g2 = dispatch_group_create();
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block int seq = 0;
        for (int i = 0; i < 8; i++) {
            Tracker *tr = [Tracker new]; tr.tag = i;
            dispatch_group_async(g2, ser, ^{ seq = seq * 3 + tr.tag; });
        }
        dispatch_group_notify(g2, conc, ^{ printf("t2 notify: seq=%d\n", seq); dispatch_semaphore_signal(done); });
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));

        /* native code keeps a copy and calls it later */
        Tracker *kept = [Tracker new]; kept.tag = 5;
        __block int fired = 0;
        hb_store(^(int v) { fired += v * kept.tag; });
        kept = nil;
        hb_fire(3); hb_fire(4);
        hb_release();
        printf("t2 stored: fired=%d\n", fired);
    }
    usleep(100000);
    printf("t2 deallocs: %d (expect 74)\n", atomic_load(&deallocs));
}

/* ---- 3: the guest calls a block again after native code saw it, and heap copies */
static void t3(void)
{
    double k = 2;
    hb_rect_t scale = ^CGRect(CGRect r, double s) { return CGRectMake(r.origin.x * s, r.origin.y * s, r.size.width * k, r.size.height * k); };
    CGRect in = CGRectMake(1, 2, 3, 4), out;
    hb_rect(scale, &in, 3, &out);
    pr("t3 native", out);
    pr("t3 guest-after", scale(in, 5));           /* invoke now points at a native entry */
    hb_rect_t h = BCOPY(hb_rect_t, scale);              /* heap copy made by native _Block_copy */
    pr("t3 heap-guest", h(in, 7));
    hb_rect(h, &in, 9, &out);
    pr("t3 heap-native", out);
    h = nil;
    hb_rect_t h2 = hb_copy_rect(scale, &in, &out); /* copied and called in native code */
    pr("t3 native-copy", out);
    pr("t3 native-copy-guest", h2(in, 11));
    h2 = nil;

    int n = 4;
    int (^plain)(int) = ^int(int v) { return v * n; };
    NSArray *a = @[@1, @2, @3];
    __block int s = 0;
    [a enumerateObjectsUsingBlock:^(id o, NSUInteger i, BOOL *stop) { s += plain([o intValue]); }];
    id heap = [plain copy];                        /* objc_retainBlock / copy via msgSend */
    printf("t3 int: %d %d %d\n", s, plain(10), ((int (^)(int))heap)(20));

    HMixed (^mix)(HMixed, HMixed) = ^HMixed(HMixed a1, HMixed b1) { return (HMixed){a1.d - b1.d, a1.i - b1.i + n}; };
    HMixed m; hb_mixed(mix, 10, 20, 3, 4, &m);
    HMixed m2 = mix((HMixed){1.5, 1}, (HMixed){0.25, 2});
    HMixed (^mixh)(HMixed, HMixed) = BCOPY(HMixed (^)(HMixed, HMixed), mix);
    HMixed m3 = mixh((HMixed){8, 8}, (HMixed){1, 1});
    mixh = nil;
    printf("t3 mixed: %.3f %d / %.3f %d / %.3f %d\n", m.d, m.i, m2.d, m2.i, m3.d, m3.i);

    HBig (^big)(HBig, int) = ^HBig(HBig b, int kk) { return (HBig){b.e, b.d, b.c, b.b + kk, b.a + n}; };
    HBig g; hb_big(big, 1, 2, &g);
    HBig g2 = big((HBig){10, 20, 30, 40, 50}, 5);
    printf("t3 big: %ld %ld %ld %ld %ld / %ld %ld %ld %ld %ld\n", g.a, g.b, g.c, g.d, g.e, g2.a, g2.b, g2.c, g2.d, g2.e);
}

/* ---- 4: global blocks */
static hb_rect_t g_rect = ^CGRect(CGRect r, double s) { return CGRectMake(r.origin.y, r.origin.x, r.size.height * s, r.size.width * s); };
static int (^g_cmp)(const void *, const void *) = ^int(const void *a, const void *b) { return *(const int *)b - *(const int *)a; };
static HBig (^g_big)(HBig, int) = ^HBig(HBig b, int kk) { return (HBig){b.a + kk, b.b + kk, b.c + kk, b.d + kk, b.e + kk}; };
static void t4(void)
{
    CGRect in = CGRectMake(1, 2, 3, 4), out;
    hb_rect(g_rect, &in, 2, &out);
    pr("t4 global-native", out);
    pr("t4 global-guest", g_rect(in, 3));
    hb_rect(^CGRect(CGRect r, double s) { return CGRectMake(r.origin.x + s, r.origin.y - s, r.size.width - 2 * s, r.size.height + 2 * s); }, &in, 0.5, &out);   /* literal, no captures */
    pr("t4 literal", out);
    int v[32]; for (int i = 0; i < 32; i++) v[i] = (i * 13) % 32;
    qsort_b(v, 32, sizeof v[0], g_cmp);
    printf("t4 qsort_b: %d %d %d\n", v[0], v[1], v[31]);
    HBig g; hb_big(g_big, 7, 3, &g);
    printf("t4 big: %ld %ld %ld %ld %ld\n", g.a, g.b, g.c, g.d, g.e);
    dispatch_queue_t q = dispatch_queue_create("t4", NULL);
    static int hits;
    for (int i = 0; i < 5; i++) dispatch_sync(q, ^{ hits++; });
    printf("t4 dispatch: hits=%d\n", hits);
    hb_store(^(int x) { hits += x; });
    hb_fire(10);
    hb_release();
    printf("t4 stored-global: hits=%d\n", hits);
}

/* ---- 5: blocks through objc_msgSend to native methods */
static void t5(void)
{
    NSMutableArray *a = [NSMutableArray array];
    for (int i = 0; i < 20; i++) [a addObject:@((i * 7) % 20)];
    __block long sum = 0; __block NSUInteger lastIdx = 0;
    int mul = 3;
    [a enumerateObjectsUsingBlock:^(NSNumber *o, NSUInteger i, BOOL *stop) { sum += o.longValue * mul; lastIdx = i; if (i == 15) *stop = YES; }];
    printf("t5 enumerate: sum=%ld last=%lu\n", sum, (unsigned long)lastIdx);
    NSArray *sorted = [a sortedArrayUsingComparator:^NSComparisonResult(NSNumber *x, NSNumber *y) { return [y compare:x]; }];
    printf("t5 sorted: %s\n", [[sorted componentsJoinedByString:@","] UTF8String]);
    __block atomic_long csum = 0;
    [a enumerateObjectsWithOptions:NSEnumerationConcurrent usingBlock:^(NSNumber *o, NSUInteger i, BOOL *stop) { atomic_fetch_add(&csum, o.longValue * (long)i); }];
    printf("t5 concurrent: %ld\n", (long)atomic_load(&csum));
    NSIndexSet *ix = [a indexesOfObjectsPassingTest:^BOOL(NSNumber *o, NSUInteger i, BOOL *stop) { return o.intValue % mul == 0; }];
    printf("t5 indexes: count=%lu first=%lu\n", (unsigned long)ix.count, (unsigned long)ix.firstIndex);

    NSDictionary *d = @{@"a": @1, @"bb": @2, @"ccc": @3, @"dddd": @4};
    __block long dsum = 0;
    [d enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSNumber *val, BOOL *stop) { dsum += (long)key.length * val.longValue; }];
    NSArray *keys = [d keysSortedByValueUsingComparator:^NSComparisonResult(id x, id y) { return [y compare:x]; }];
    printf("t5 dict: %ld %s\n", dsum, [[keys componentsJoinedByString:@","] UTF8String]);

    NSString *text = @"alpha beta gamma\ndelta epsilon\nzeta";
    __block NSMutableString *lines = [NSMutableString string];
    [text enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) { [lines appendFormat:@"[%lu]", (unsigned long)line.length]; }];
    __block NSMutableString *words = [NSMutableString string];
    [text enumerateSubstringsInRange:NSMakeRange(0, text.length) options:NSStringEnumerationByWords
                          usingBlock:^(NSString *w, NSRange r, NSRange er, BOOL *stop) { [words appendFormat:@"%@@%lu+%lu/%lu ", w, (unsigned long)r.location, (unsigned long)r.length, (unsigned long)er.length]; }];
    printf("t5 lines: %s\nt5 words: %s\n", lines.UTF8String, words.UTF8String);

    NSMutableArray *m = [a mutableCopy];
    [m sortUsingComparator:^NSComparisonResult(NSNumber *x, NSNumber *y) { return [x compare:y]; }];
    NSSet *set = [NSSet setWithArray:a];
    __block long ssum = 0;
    [set enumerateObjectsUsingBlock:^(NSNumber *o, BOOL *stop) { ssum += o.longValue; }];
    NSUInteger at = [m indexOfObjectPassingTest:^BOOL(NSNumber *o, NSUInteger i, BOOL *stop) { return o.intValue > 12; }];
    printf("t5 mutable: %s set=%ld at=%lu\n", [[m componentsJoinedByString:@","] UTF8String], ssum, (unsigned long)at);
}

/* ---- 6: extended signatures, blocks taking blocks */
static void t6(void)
{
    double s = 0.5;
    hb_outer_t outer = ^CGRect(hb_int_t inner, CGRect r) {
        return CGRectMake(inner((int)r.origin.x), inner((int)r.origin.y), r.size.width * s, r.size.height * s);
    };
    CGRect in = CGRectMake(1, 2, 8, 10), out;
    hb_outer(outer, &in, 3, &out);
    pr("t6 outer", out);
    pr("t6 outer-guest", outer(^int(int v) { return v + 100; }, in));
    int base = 1;
    hb_outer2_t o2 = ^int(int (^inner)(int (^)(int), int), int k) {
        return inner(^int(int v) { return v * v + base; }, k);
    };
    printf("t6 outer2: %d\n", hb_outer2(o2, 4));
    void (^named)(NSString *, NSArray<NSNumber *> *, void (^)(NSString *)) = ^(NSString *a, NSArray *b, void (^cb)(NSString *)) {
        cb([NSString stringWithFormat:@"%@:%lu", a, (unsigned long)b.count]);
    };
    named(@"x", @[@1, @2], ^(NSString *r) { printf("t6 named: %s\n", r.UTF8String); });
    printf("t6 sig outer: %s\nt6 sig outer2: %s\nt6 sig named: %s\n", sig(outer), sig(o2), sig(named));
    /* an extended-signature block through a native method */
    NSArray *arr = @[@"b", @"a", @"c"];
    __block NSMutableString *acc = [NSMutableString string];
    void (^emit)(void (^)(NSString *)) = ^(void (^put)(NSString *)) { for (NSString *x in [arr sortedArrayUsingSelector:@selector(compare:)]) put(x); };
    [@[emit] enumerateObjectsUsingBlock:^(void (^e)(void (^)(NSString *)), NSUInteger i, BOOL *stop) { e(^(NSString *x) { [acc appendString:x]; }); }];
    printf("t6 via msgSend: %s\n", acc.UTF8String);
}

int main(int argc, char **argv)
{
    int only = argc > 1 ? atoi(argv[1]) : 0;
    void (*tests[])(void) = {t1, t2, t3, t4, t5, t6};
    for (int i = 0; i < 6; i++)
        if (!only || only == i + 1) { @autoreleasepool { tests[i](); } fflush(stdout); }
    return 0;
}
