/* A program in an app bundle (Test.app/Contents/MacOS, LC_RPATH @executable_path/../Frameworks) that dlopens its
 * own libraries by @rpath, @executable_path and (from a library) @loader_path, as Xcode does with
 * @rpath/Frameworks/libIDEApplicationLoader.dylib (tests/guest/dlrpath.sh). libplug and libsub are ARM-only (guest
 * images under hao); libnat is universal (its ARM slice runs under hao). */
#include <dlfcn.h>
#include <stdio.h>
static int read_value(const char *path, const char *symbol)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    int (*get)(void) = h ? (int (*)(void))dlsym(h, symbol) : NULL;
    return get ? get() : -1;
}

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
    /* The two parents name distinct files as @rpath/leaf.dylib. First load
     * wins for dependency lookup; an explicit file open still distinguishes them. */
    int a = read_value("@executable_path/../Frameworks/A/parent.dylib", "parent_value");
    int b = read_value("@executable_path/../Frameworks/B/parent.dylib", "parent_value");
    int explicit_b = read_value("@executable_path/../Frameworks/B/leaf.dylib", "leaf_value");
    printf("rpath dependencies %d %d, explicit leaf %d\n", a, b, explicit_b);
    return 0;
}
