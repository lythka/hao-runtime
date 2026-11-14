/* A program in an app bundle (Test.app/Contents/MacOS, LC_RPATH @executable_path/../Frameworks) that dlopens its
 * own libraries by @rpath, @executable_path and (from a library) @loader_path, as Xcode does with
 * @rpath/Frameworks/libIDEApplicationLoader.dylib (tests/guest/dlrpath.sh). libplug and libsub are ARM-only (guest
 * images under hao); libnat is universal (it opens natively). */
#include <dlfcn.h>
#include <stdio.h>
int main(void)
{
    void *h = dlopen("@rpath/libplug.dylib", RTLD_NOW);
    if (!h) printf("rpath plug: %s\n", dlerror());
    int (*plug)(void) = h ? (int (*)(void))dlsym(h, "plug_value") : NULL;
    printf("plug_value=%d\n", plug ? plug() : -1);
    void *h2 = dlopen("@executable_path/../Frameworks/libplug.dylib", RTLD_NOW);
    printf("executable_path plug: %s, same handle %d\n", h2 ? "opened" : "failed", h2 == h);
    void *n = dlopen("@rpath/libnat.dylib", RTLD_NOW);
    int (*nat)(void) = n ? (int (*)(void))dlsym(n, "nat_value") : NULL;
    printf("nat_value=%d\n", nat ? nat() : -1);
    printf("missing: %s\n", dlopen("@rpath/libmissing.dylib", RTLD_NOW) ? "opened" : "NULL");
    return 0;
}
