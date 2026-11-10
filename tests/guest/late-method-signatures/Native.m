#import "Native.h"
#import <objc/message.h>
@implementation HaoNativeFrame
- (NSRect)frame {
    /* Native -> guest override -> native superclass must retain self,
     * independently of the hidden SysV structure-result pointer. */
    if (![self isKindOfClass:HaoNativeFrame.class]) abort();
    return NSMakeRect(1.25, -2.5, 300.75, 40.125);
}
@end
int hao_check_frame(id object) {
    NSRect result = [(HaoNativeFrame *)object frame];
    const NSRect expected = {{11.25, -2.5}, {300.75, 40.125}};
    if (memcmp(&result, &expected, sizeof result)) {
        puts("frame mismatch");
        return 1;
    }
    puts("frame preserved");
    return 0;
}
int hao_check_rect(id object) {
    const NSRect inputs[] = { {{1046,800},{304,496}}, {{-11.25,9.5},{180.75,44.125}} };
    for (unsigned i = 0; i < sizeof inputs / sizeof *inputs; i++) {
        NSRect result;
#if defined(__x86_64__)
        ((void (*)(NSRect *, id, SEL, NSRect, id))objc_msgSend_stret)(&result, object,
            sel_registerName("constrainFrameRect:toScreen:"), inputs[i], nil);
#else
        result = ((NSRect (*)(id, SEL, NSRect, id))objc_msgSend)(object,
            sel_registerName("constrainFrameRect:toScreen:"), inputs[i], nil);
#endif
        if (memcmp(&result, &inputs[i], sizeof result)) {
            printf("rectangle %u mismatch %.3f %.3f %.3f %.3f\n", i,
                result.origin.x, result.origin.y, result.size.width, result.size.height);
            return 1;
        }
    }
    puts("rectangles preserved");
    return 0;
}
