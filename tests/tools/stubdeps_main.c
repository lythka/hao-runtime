/* A program whose dependencies cover the stub link rules (tests/tools/stubdeps.sh):
 * a weak framework, a fat @rpath library, an arm64-only guest library. */
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
int stubdeps_inits(void);
int guestlib_answer(void);
int main(void)
{
    CFStringRef s = CFStringCreateWithCString(NULL, "cf", kCFStringEncodingUTF8);
    printf("inits=%d answer=%d cf=%ld\n", stubdeps_inits(), guestlib_answer(), (long)CFStringGetLength(s));
    CFRelease(s);
    return 0;
}
