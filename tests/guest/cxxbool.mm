/* C++ functions with ObjC BOOL parameters in a native library
 * (cxxboolhelper.mm): the guest imports their arm64 names (bool, b), the
 * x86_64 slice exports them with signed char (a). Every call must bind and
 * print what native arm64 prints. */
#import "cxxboolhelper.h"
#include <stdio.h>

int main(void)
{
    @autoreleasepool {
        CBCell *cell = [CBCell new];
        printf("set_text %d %d %d %d\n", cb_set_text(cell, @"abc", YES, NO), cb_set_text(cell, @"abcd", NO, YES),
               cb_set_text(cell, @"", YES, YES), cell.value);
        printf("mixed %d %d %d %d\n", cb_mixed(true, NO, 5), cb_mixed(false, YES, 6), cb_mixed(true, YES, 7),
               cb_mixed(false, NO, 8));
        BOOL b = YES;
        cb_store(&b, YES);
        int first = b;
        cb_store(&b, NO);
        printf("store %d %d\n", first, (int)b);
        printf("pick %d %d\n", cbns::Table::pick(1, 2, YES), cbns::Table::pick(1, 2, NO));
        printf("choose %d %g\n", cbns::choose<int>(3, 4, NO), cbns::choose<double>(1.5, 2.5, YES));
        printf("plain %d %d\n", cb_plain(true), cb_plain(false));
    }
    return 0;
}
