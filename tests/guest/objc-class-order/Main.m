#import "Late.h"
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
int main(void) {
    puts("entered-main"); fflush(stdout);
    @autoreleasepool {
        Class early = objc_getClass("OrderEarlySwift");
        Class late = objc_getClass("OrderLateObjC");
        if (!early || !late) return 2;
        id e = [early new], l = [late new];
        NSInteger a = ((NSInteger (*)(id,SEL))objc_msgSend)(e,sel_registerName("check"));
        NSInteger b = ((NSInteger (*)(id,SEL))objc_msgSend)(l,sel_registerName("number"));
        printf("classes early=%ld late=%ld\n",(long)a,(long)b);
        return a == 42 && b == 42 ? 0 : 3;
    }
}
