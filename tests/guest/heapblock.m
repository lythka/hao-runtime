// A compiler block literal moved into the interior of heap storage, as Swift
// async lowering does. NSInvocation calls its invoke through __invoking___.
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct BlockHead { void *isa; int flags, reserved; void *invoke; uintptr_t *descriptor; };
int main(void) {
    @autoreleasepool {
        long captured = 17;
        long (^block)(long) = ^long(long n) { return captured + n * 3; };
        struct BlockHead *b = (void *)block;
        size_t size = b->descriptor[1];
        unsigned char *allocation = malloc(size + 64);
        volatile unsigned char *dst = allocation + 32;
        volatile const unsigned char *src = (void *)block;
        for (size_t i = 0; i < size; i++) dst[i] = src[i];
        id interior = (id)(void *)dst;
        NSMethodSignature *sig = [NSMethodSignature signatureWithObjCTypes:"q@?q"];
        NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
        long input = 5, result = 0;
        [inv setArgument:&input atIndex:1];
        [inv invokeWithTarget:interior];
        [inv getReturnValue:&result];
        printf("heap block invocation %ld\n", result);
        free(allocation);
        return result != 32;
    }
}
