/* Fill the rel32 window except for a hole missed by the old eight hints.
 * The large mapping is PROT_NONE and consumes address space, not 6 GB of RAM. */
#include "../../src/near_map.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void *hao_near_try(uint64_t at, uint64_t lo, uint64_t hi, uint64_t size)
{
    return hao_near_try_prot(at, lo, hi, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_JIT);
}
static void *old_map(uint64_t lo, uint64_t hi, uint64_t size)
{
    uint64_t pg = getpagesize();
    for (uint64_t k = 0; k < 4; k++) {
        void *p = hao_near_try(((hi + pg - 1) & ~(pg - 1)) + k * (64ull << 20), lo, hi, size);
        if (p) return p;
        p = hao_near_try(((lo - size) & ~(pg - 1)) - k * (64ull << 20), lo, hi, size);
        if (p) return p;
    }
    return NULL;
}
int main(void)
{
    uint64_t pg = getpagesize(), span = 6ull << 30;
    void *mem = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mem != MAP_FAILED);
    uint64_t base = (uint64_t)(uintptr_t)mem, lo = base + (3ull << 30), hi = lo + (8ull << 20);
    uint64_t hole = lo - (333ull << 20), size = 4 * pg;
    CHECK(munmap((void *)(uintptr_t)hole, size) == 0);
    void *old = old_map(lo, hi, size);
    CHECK(old == NULL);
    void *p = hao_map_near(lo, hi, size);
    CHECK(p == (void *)(uintptr_t)hole);
    CHECK(hao_near_range(hole, lo, hi, size));
    CHECK(munmap(p, size) == 0);
    p = hao_map_near_prot(lo, hi, size, PROT_READ | PROT_WRITE, 0);
    CHECK(p == (void *)(uintptr_t)hole);
    CHECK(!hao_near_range(lo - 0x7fff0000ull + pg, lo, hi, size));
    CHECK(!hao_near_range(lo + 0x7fff0000ull - pg, lo, hi, size));
    CHECK(hao_map_near(lo, lo + (5ull << 30), size) == NULL);
    CHECK(hao_map_near(lo, hi, 0) == NULL);
    CHECK(hao_map_near(lo, hi, 1) == NULL);
    CHECK(hao_map_near(lo, lo - 1, size) == NULL);
    munmap(mem, span);
    puts("near-map: PASS (old hints fail; free rel32 hole found; bounds checked)");
}
