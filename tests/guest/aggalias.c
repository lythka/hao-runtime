/* tests/guest/aggalias.sh's guest: native functions with ABI differences between the slices,
 * called through renamed re-exports (libaggaliaspub.dylib's aap_X -> libaggaliasimpl.dylib's
 * aai_X). The interface unit types the defining library's names only, so a bridge that types by
 * the import name finds nothing for aap_X. Built plain, the program imports only the alias names,
 * as mds imports only Metadata.framework's _MDPlistX names; built with -DMIXED it also imports
 * the defining names (the X-direct cases), and the alias calls must not depend on that.
 * One case per process (argv[1]); each prints what it got, with pointers relative to buf. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "aggaliashelper.h"

static uint8_t buf[96] __attribute__((aligned(8)));

/* leave nonzero junk in x2-x7 and the outgoing stack area, as a real caller does */
__attribute__((noinline)) static long junk(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j)
{ return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j; }

static void pobj(const char *k, aa_obj r)
{
    printf("%s: bytes=buf%+td aux=%#llx off=%#x type=%#x\n", k, (const uint8_t *)r.bytes - buf,
           (unsigned long long)r.aux, r.off, r.type);
}

int main(int argc, char **argv)
{
    const char *k = argc > 1 ? argv[1] : "";
    uint32_t n = 0x20;
    memcpy(buf + 8, &n, 4);
    for (int i = 0; i < 0x20; i++) buf[12 + i] = (uint8_t)i;
    aa_obj o = {buf, 0x1111222233334407ull, 8, 0x55};
    aa_rect rc = {{1.5, 2.0}, {11.5, 6.0}};
    volatile long sink = junk(1, 2, 3, 4, 5, 0x6666, 0x7777, 0x8888, 0x9999, 0xaaaa);
    (void)sink;
    size_t len = 0xdead;
    if (!strcmp(k, "data")) {
        const uint8_t *p = aap_data(o, &len);
        printf("data: ptr=buf%+td len=%#zx\n", p - buf, len);
    } else if (!strcmp(k, "type")) {
        printf("type: %#x\n", aap_type(o));
    } else if (!strcmp(k, "index")) {
        pobj("index", aap_index(o, 3));
    } else if (!strcmp(k, "area")) {
        printf("area: %.3f\n", aap_area(rc, 0.5));
    } else if (!strcmp(k, "flag")) {
        volatile int a = aap_flag(5), b = aap_flag(-5);
        printf("flag: %d %d\n", a, b);
    } else if (!strcmp(k, "dl")) {
        /* a bare address: the alias name reaches the defining library's function */
        const void *(*f)(aa_obj, size_t *) = (const void *(*)(aa_obj, size_t *))dlsym(RTLD_DEFAULT, "aap_dl");
        if (!f) { puts("dl: aap_dl not found"); return 1; }
        const uint8_t *p = f(o, &len);
        printf("dl: ptr=buf%+td len=%#zx\n", p - buf, len);
#ifdef MIXED
    } else if (!strcmp(k, "data-direct")) {
        const uint8_t *p = aai_data(o, &len);
        printf("data-direct: ptr=buf%+td len=%#zx\n", p - buf, len);
    } else if (!strcmp(k, "type-direct")) {
        printf("type-direct: %#x\n", aai_type(o));
    } else if (!strcmp(k, "index-direct")) {
        pobj("index-direct", aai_index(o, 3));
    } else if (!strcmp(k, "area-direct")) {
        printf("area-direct: %.3f\n", aai_area(rc, 0.5));
    } else if (!strcmp(k, "flag-direct")) {
        volatile int a = aai_flag(5), b = aai_flag(-5);
        printf("flag-direct: %d %d\n", a, b);
#endif
    } else { fprintf(stderr, "usage: aggalias data|type|index|area|flag|dl%s\n",
#ifdef MIXED
                     "|X-direct"
#else
                     ""
#endif
                     ); return 2; }
    return 0;
}
