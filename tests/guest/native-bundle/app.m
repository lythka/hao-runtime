#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <stdio.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
        NSError *error = nil;
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "%s\n", error.description.UTF8String);
            return 1;
        }
        long (*value)(id, SEL) = (void *)objc_msgSend;
        return value(bundle.principalClass, @selector(value)) == 41 ? 0 : 1;
    }
}
