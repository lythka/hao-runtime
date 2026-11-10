/* A program without Swift must not get the Swift runtime: tests/objc/run.sh
 * compares this under hao with native arm64. */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    int n = 0;
    for (uint32_t k = 0; k < _dyld_image_count(); k++) n += strstr(_dyld_get_image_name(k), "libswift") != NULL;
    printf("swift_retain=%s libswift images=%d\n", dlsym(RTLD_DEFAULT, "swift_retain") ? "found" : "none", n);
    return 0;
}
