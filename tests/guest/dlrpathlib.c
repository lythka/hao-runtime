/* Libraries of tests/guest/dlrpath.c: built three times (-DPLUG, -DSUB, -DNAT). */
#include <dlfcn.h>
#ifdef PLUG
int plug_value(void)
{   /* a library opening its sibling by @loader_path */
    void *h = dlopen("@loader_path/sub/libsub.dylib", RTLD_NOW);
    int (*sub)(void) = h ? (int (*)(void))dlsym(h, "sub_value") : 0;
    return 40 + (sub ? sub() : 100);
}
#endif
#ifdef SUB
int sub_value(void) { return 2; }
#endif
#ifdef NAT
int nat_value(void) { return 7; }
#endif
