/* CPUID-off helper execution, optional-profile cache rejection, and
 * position-independent deterministic AES instruction emission. */
#include "hao/cpu.h"
#include <cpuid.h>
#include <stdio.h>
#include <string.h>
static int aes_masked;
static unsigned aes_helpers;
static int aes_cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
    int ok = __get_cpuid(leaf, a, b, c, d);
    if (leaf == 1 && aes_masked) *c &= ~bit_AES;
    return ok;
}
int aes_count_helper(hao_cpu *, uint64_t, uint64_t);
#define __get_cpuid aes_cpuid
#define a64_exec_helper aes_count_helper
#include "../../src/jit.c"
#undef a64_exec_helper
#undef __get_cpuid
int aes_count_helper(hao_cpu *c, uint64_t descriptor, uint64_t pc)
{
    aes_helpers++;
    return a64_exec_helper(c, descriptor, pc);
}
static int profile_cache(const uint8_t on[16], const uint8_t off[16])
{
    union { uint64_t align; uint8_t b[sizeof(hao_aot_hdr) + sizeof(hao_aot_ent) + 32]; } file = {0};
    hao_aot_hdr h = {0};
    memcpy(h.magic, HAO_AOT_MAGIC, 8); h.version = HAO_AOT_VERSION; h.hdr_size = sizeof h;
    memcpy(h.gen, on, 16); h.vmsize = 4; h.nblocks = 1; h.code_size = 32;
    h.file_size = sizeof file.b; h.id_kind = HAO_AOT_ID_CODE;
    hao_aot_ent ent = {0, 16}; memcpy(file.b + sizeof h, &ent, sizeof ent);
    h.payload_hash = hao_aot_hash64(file.b + sizeof h, sizeof file.b - sizeof h, 0);
    memcpy(file.b, &h, sizeof h);
    uint64_t lo = 0, hi = 4;
    hao_aot_expect x = {.uuid=h.uuid, .gen=on, .vmsize=4, .range_lo=&lo, .range_hi=&hi,
                        .nranges=1, .id_kind=HAO_AOT_ID_CODE, .code_id=h.code_id};
    char why[128];
    if (hao_aot_check(file.b, sizeof file.b, &x, NULL, why, sizeof why)) {
        printf("FAIL matching AES cache: %s\n", why); return 1;
    }
    x.gen = off;
    if (hao_aot_check(file.b, sizeof file.b, &x, NULL, why, sizeof why) != -1 || !strstr(why, "CPU profile")) {
        printf("FAIL AES cache accepted without AES: %s\n", why); return 1;
    }
    return 0;
}
int main(void)
{
    aes_masked = 1;
    if (!hao_jit_init() || jit_aes) return 1;
    gen_init(); uint8_t off[16], on[16]; memcpy(off, gen_uuid, 16);
    if (!gen_ok) return 1;
    for (unsigned op = 4; op <= 7; op++) for (unsigned alias = 0; alias < 2; alias++) {
        uint32_t word = 0x4e280800u | op << 12 | (alias ? 0 : 1) << 5;
        uint32_t code[] = {0xeb01001f, word, 0x9a818002};
        hao_cpu a = {0}, b;
        a.x[0] = 7; a.x[1] = 3; a64_fpcr_write(&a, 0); a64_fpsr_write(&a, 0x0800009f);
        for (unsigned r=0; r<32; r++) for (unsigned e=0; e<16; e++) a.v[r].b[e] = (uint8_t)(r*7+e*13);
        b = a;
        uint64_t pc = 0x7300000000ull + op * 32 + alias * 16;
        for (int k=0; k<3; k++) if (a64_exec(&a, code[k], pc + (unsigned)k*4)) return 1;
        aes_helpers = 0;
        if (hao_jit_exec_code(&b, code, 3, pc) || aes_helpers != 1 || a.pc != b.pc ||
            memcmp(a.x,b.x,sizeof a.x) || memcmp(a.v,b.v,sizeof a.v) ||
            hao_nzcv(&a) != hao_nzcv(&b) || a64_fpsr_read(&a) != a64_fpsr_read(&b)) {
            printf("FAIL CPUID-off helper/state op %u alias %u calls %u\n", op, alias, aes_helpers); return 1;
        }
    }
    /* Never execute AES after overriding its feature bit: this test also
     * runs on hosts that physically lack AES-NI. */
    jit_aes = true;
    gen_init(); memcpy(on, gen_uuid, 16);
    uint8_t expected[16]; memcpy(expected, off, 16); expected[15] ^= 1;
    if (memcmp(expected, on, 16) || profile_cache(on, off)) return 1;
    for (unsigned op=4; op<=7; op++) {
        uint8_t bytes[3][256]; size_t size[3];
        for (int mode=0; mode<3; mode++) {
            tctx t = {0}; t.pic = mode == 1; t.dry = mode == 2;
            xb_init(&t.b, bytes[mode], sizeof bytes[mode]);
            if (!t_aes(&t, 0x4e280820u | op << 12) || t.b.bad || t.b.overflow || t.not_pic) return 1;
            size[mode] = xb_len(&t.b);
        }
        if (size[0] != size[1] || size[0] != size[2] ||
            memcmp(bytes[0], bytes[1], size[0]) || memcmp(bytes[0], bytes[2], size[0])) return 1;
    }
    puts("AES profile: 8 CPUID-off helper/state cases, cache rejection and four JIT/AOT/recovery byte comparisons PASS");
    return 0;
}
