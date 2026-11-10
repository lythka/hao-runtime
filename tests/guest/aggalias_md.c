/* tests/guest/aggalias.sh's system case: the mds crash itself.
 * MDPlistObject (MetadataUtilities SPI, 24 bytes) by value through the names mds imports:
 * Metadata.framework's (CoreServices) renamed re-exports _MDPlistX -> MetadataUtilities
 * __MDPlistX. The program imports only those alias names, as mds does. Layout from both slices
 * (tools/aggspi.txt, MetadataUtilities block): pointer at +0, 64-bit at +8, 32-bit offset at
 * +0x10, type byte at +0x14. One case per process (argv[1]).
 *   md-data   MDPlistDataGetBytePtr(obj, &len): the faulting call (mds 0x1000a4064)
 *   md-type   MDPlistGetPlistObjectType(obj): aggspi types the target __MDPlistGetPlistObjectType
 *   md-count  MDPlistArrayGetCount(obj): 16-bit count at bytes + off + 4 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef struct { const void *bytes; uint64_t aux; uint32_t off; uint8_t type; } MDPlistObject;
extern const void *MDPlistDataGetBytePtr(MDPlistObject obj, size_t *len);
extern uint8_t MDPlistGetPlistObjectType(MDPlistObject obj);
extern uint32_t MDPlistArrayGetCount(MDPlistObject obj);

static uint8_t buf[64] __attribute__((aligned(8)));

__attribute__((noinline)) static long junk(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j)
{ return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j; }

int main(int argc, char **argv)
{
    const char *k = argc > 1 ? argv[1] : "";
    uint32_t n = 0x20;
    memcpy(buf + 8, &n, 4);
    for (int i = 0; i < 0x20 && 12 + i < (int)sizeof buf; i++) buf[12 + i] = (uint8_t)(i + 0x30);
    MDPlistObject o = {buf, 0x1111222233334444ull, 8, 0x55};
    volatile long sink = junk(1, 2, 3, 4, 5, 0x6666, 0x7777, 0x8888, 0x9999, 0xaaaa);
    (void)sink;
    size_t len = 0xdead;
    if (!strcmp(k, "md-data")) {
        const uint8_t *p = MDPlistDataGetBytePtr(o, &len);
        printf("md-data: ptr=buf%+td len=%#zx\n", p - buf, len);
    } else if (!strcmp(k, "md-type")) {
        printf("md-type: %#x\n", MDPlistGetPlistObjectType(o));
    } else if (!strcmp(k, "md-count")) {
        printf("md-count: %#x\n", MDPlistArrayGetCount(o));
    } else { fprintf(stderr, "usage: aggalias_md md-data|md-type|md-count\n"); return 2; }
    return 0;
}
