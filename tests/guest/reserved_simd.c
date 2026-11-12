/* Reserved selectors must signal in every engine; ignored bits must run. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static sigjmp_buf recovery;
static void ill(int sig) { siglongjmp(recovery, sig); }
#define OP(name, word) \
static void name(void) { __asm__ volatile(".inst " #word : : : "x0", "v0", "v1", "v2"); }
OP(misc_vector, 0x0e605820)
OP(misc_scalar, 0x5e202820)
OP(across, 0x2e31b820)
OP(copy_vector, 0x0e011c20)
OP(copy_scalar, 0x5e010c20)
OP(shift_vector, 0x0f086420)
OP(shift_scalar, 0x5f080420)
OP(permute, 0x0ec21820)
OP(indexed, 0x5f422020)
/* INS low imm4 and DUP general high imm5 bits are ignored. */
OP(ins_alias, 0x6e087c20)
OP(dup_alias, 0x4e180c20)

typedef uint32_t vec __attribute__((vector_size(16)));
#define EST(name, instruction) \
static vec name(vec a, uint64_t *flags) { \
    vec r; \
    __asm__ volatile("msr fpsr, xzr\n\t" instruction "\n\tmrs %1, fpsr" \
                     : "=w"(r), "=r"(*flags) : "w"(a)); \
    return r; \
}
EST(recip2, "urecpe %0.2s, %2.2s")
EST(recip4, "urecpe %0.4s, %2.4s")
EST(rsqrt2, "ursqrte %0.2s, %2.2s")
EST(rsqrt4, "ursqrte %0.4s, %2.4s")

int main(int argc, char **argv)
{
    struct sigaction sa = {.sa_handler = ill};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, 0)) return 2;
    void (*ops[])(void) = {misc_vector, misc_scalar, across, copy_vector,
        copy_scalar, shift_vector, shift_scalar, permute, indexed, ins_alias, dup_alias};
    unsigned first = argc > 1 ? (unsigned)atoi(argv[1]) : 9;
    unsigned last = argc > 1 ? first + 1 : sizeof ops / sizeof *ops;
    if (first >= sizeof ops / sizeof *ops) return 2;
    for (unsigned k = first; k < last; k++) {
        int sig = sigsetjmp(recovery, 1);
        if (!sig) ops[k]();
        printf("selector %u: %s\n", k, sig == SIGILL ? "SIGILL" : "ran");
        if ((sig == SIGILL) != (k < 9)) return 1;
    }
    if (argc > 1) return 0;
    vec (*est[])(vec, uint64_t *) = {recip2, recip4, rsqrt2, rsqrt4};
    uint64_t hash = 0xcbf29ce484222325ull;
    /* Every estimate table bucket, both ends, and both saturation regions. */
    for (unsigned k = 0; k < 4; k++) for (uint32_t a = 0; a < 512; a++) {
        vec v = {a << 23, (a << 23) | 0x7fffff, 0x40000000, 0xffffffff};
        uint64_t flags;
        vec r = est[k](v, &flags);
        for (unsigned e = 0; e < 4; e++) hash = (hash ^ r[e]) * 0x100000001b3ull;
        if (flags) return 1;
    }
    printf("unsigned estimates: %016llx\n", (unsigned long long)hash);
    return 0;
}
