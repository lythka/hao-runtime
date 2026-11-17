/* Translation profiles (hao/cpufeat.h). Under a profile without AES-NI or
 * PCLMULQDQ, AES and PMULL .1Q run in the helper with complete architectural
 * state; CRC32C never does. Under one profile, emission is the same bytes in
 * JIT, AOT and fault-recovery modes; a block of an AOT cache is re-emitted
 * under the cache's profile, not the process's; the generator identity is
 * the plain libhao UUID. It never executes an instruction of a feature it
 * only pretends the host has: the test also runs on hosts without them. */
#include "hao/cpu.h"
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>
static unsigned helpers;
int count_helper(hao_cpu *, uint64_t, uint64_t);
#define a64_exec_helper count_helper
#include "../../src/jit.c"
#undef a64_exec_helper
int count_helper(hao_cpu *c, uint64_t descriptor, uint64_t pc)
{
    helpers++;
    return a64_exec_helper(c, descriptor, pc);
}

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

/* CMP x0, x1; insn; CSEL x2, x0, x1, HI through the interpreter and the JIT
 * of this process's profile: equal state, and `want` helper calls */
static int same(uint32_t insn, unsigned want, const char *what)
{
    uint32_t code[] = {0xeb01001f, insn, 0x9a818002};
    hao_cpu a = {0}, b;
    a.x[0] = 7; a.x[1] = 3;
    for (unsigned r = 2; r < 31; r++) a.x[r] = 0x9e3779b97f4a7c15ull * r;
    a64_fpcr_write(&a, 0); a64_fpsr_write(&a, 0x0800009f);
    for (unsigned r = 0; r < 32; r++) for (unsigned e = 0; e < 16; e++) a.v[r].b[e] = (uint8_t)(r * 7 + e * 13);
    b = a;
    uint64_t pc = 0x7300000000ull + (insn & 0xff0) * 4;
    for (int k = 0; k < 3; k++) if (a64_exec(&a, code[k], pc + (unsigned)k * 4)) FAIL("%s %08x: interpreter", what, insn);
    helpers = 0;
    if (hao_jit_exec_code(&b, code, 3, pc) || helpers != want || a.pc != b.pc || memcmp(a.x, b.x, sizeof a.x) ||
        memcmp(a.v, b.v, sizeof a.v) || hao_nzcv(&a) != hao_nzcv(&b) || a64_fpsr_read(&a) != a64_fpsr_read(&b))
        FAIL("%s %08x: helper calls %u (want %u) or state differs", what, insn, helpers, want);
    return 0;
}

/* One block emitted three ways under profile cpu, at the same address: as
 * the JIT does, as the AOT generator does (position independent: guest
 * addresses through the base slot) and as hao_jit_locate does (dry run).
 * The same bytes and instruction boundaries in all three, so recovery finds
 * the instruction. */
static int emit_modes(const uint32_t *code, int n, uint64_t cpu, uint32_t *off_out, size_t *len_out)
{
    static uint8_t slot[16], work[4096], bytes[3][4096];
    size_t size[3];
    uint32_t off[3][MAX_BLOCK_INSNS + 1];
    bool fl[MAX_BLOCK_INSNS + 1];
    for (int mode = 0; mode < 3; mode++) {
        tctx t;
        memset(&t, 0, sizeof t);
        t.cpu = cpu; t.pic = mode == 1; t.dry = mode == 2;
        t.win = code; t.win_base = 0x7300000000ull; t.win_n = n;
        t.base = 0x7300000000ull; t.base_slot = slot;
        t.rec_off = off[mode]; t.rec_fl = fl;
        memset(work, 0xcc, sizeof work);
        xb_init(&t.b, work, sizeof work);
        emit_block(&t, 0x7300000000ull, code, n);
        if (t.b.bad || t.b.overflow || t.not_pic) FAIL("emission under profile %#llx, mode %d", (unsigned long long)cpu, mode);
        size[mode] = xb_len(&t.b);
        memcpy(bytes[mode], work, size[mode]);
    }
    if (size[0] != size[1] || size[0] != size[2] || memcmp(bytes[0], bytes[1], size[0]) || memcmp(bytes[0], bytes[2], size[0]) ||
        memcmp(off[0], off[1], sizeof off[0][0] * (unsigned)(n + 1)) || memcmp(off[0], off[2], sizeof off[0][0] * (unsigned)(n + 1)))
        FAIL("profile %#llx: JIT, AOT and recovery emission differ", (unsigned long long)cpu);
    memcpy(off_out, off[0], sizeof off[0][0] * (unsigned)(n + 1));
    *len_out = size[0];
    return 0;
}

int main(void)
{
    /* the profile without optional features (what HAO_CPU_FEATURES=0 asks for) */
    hao_opt.cpu_clear = HAO_CPU_ALL;
    if (!hao_jit_init() || jit_cpu) FAIL("profile 0 not taken (%#llx)", (unsigned long long)jit_cpu);
    int cases = 0;
    for (unsigned op = 4; op <= 7; op++) for (unsigned alias = 0; alias < 2; alias++, cases++)
        if (same(0x4e280800u | op << 12 | (alias ? 0 : 1) << 5, 1, "AES without AES-NI")) return 1;
    for (unsigned q = 0; q < 2; q++) for (unsigned alias = 0; alias < 2; alias++, cases++)
        if (same(q << 30 | 0x0EE0E000u | (alias ? 1 : 3) << 16 | 1 << 5 | 2, 1, "PMULL .1Q without PCLMULQDQ")) return 1;
    for (unsigned sz = 0; sz < 4; sz++, cases++)
        if (same((sz == 3) << 31 | 0x1AC05000u | sz << 10 | 4 << 16 | 5 << 5 | 6, 0, "CRC32C")) return 1;

    /* the generator identity: libhao's (here: this executable's) LC_UUID as it is */
    const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(0);
    const uint8_t *p = (const uint8_t *)(mh + 1), *uuid = NULL;
    for (uint32_t k = 0; k < mh->ncmds; k++, p += ((const struct load_command *)(const void *)p)->cmdsize)
        if (((const struct load_command *)(const void *)p)->cmd == LC_UUID) uuid = ((const struct uuid_command *)(const void *)p)->uuid;
    uint8_t g[16];
    if (!uuid || !hao_aot_generation(g) || memcmp(g, uuid, 16)) FAIL("the AOT generator identity is not the plain LC_UUID");

    /* emission per profile; never executed */
    const uint64_t full = HAO_CPU_AES | HAO_CPU_PCLMUL;
    uint32_t block[] = {0x4e284820u, 0x4ee2e020u, 0x8b020020u, 0x1ac25020u};   /* AESE, PMULL .1Q, ADD, CRC32CB */
    uint32_t off0[8], offf[8];
    size_t len0, lenf;
    if (emit_modes(block, 4, 0, off0, &len0) || emit_modes(block, 4, full, offf, &lenf)) return 1;
    if (offf[2] >= off0[2] || lenf >= len0) FAIL("AES-NI and PCLMULQDQ lowerings not taken under their profile");
    for (unsigned k = 0; k < 2; k++) {
        tctx t;
        uint8_t buf[256];
        memset(&t, 0, sizeof t);
        xb_init(&t.b, buf, sizeof buf);
        t.cpu = k ? full : 0;
        bool aes = t_aes(&t, block[0]), pm = t_simd_three_diff(&t, block[1]);
        if (aes != (bool)k || pm != (bool)k) FAIL("profile %#llx: AES %d PMULL %d translated", (unsigned long long)t.cpu, aes, pm);
    }
    /* hao_jit_locate re-emits a cached block under its cache's profile */
    static uint8_t blob[256];
    hao_image im;
    memset(&im, 0, sizeof im);
    if (aot_cpu_at(&im, (uint64_t)(uintptr_t)blob) != jit_cpu || aot_cpu_at(NULL, 0) != jit_cpu) FAIL("no cache: not the JIT's profile");
    aot_publish(&im, blob, sizeof blob, full);
    if (aot_cpu_at(&im, (uint64_t)(uintptr_t)blob) != full || aot_cpu_at(&im, (uint64_t)(uintptr_t)blob + sizeof blob - 1) != full)
        FAIL("a block of the cache is not re-emitted under its profile");
    if (aot_cpu_at(&im, (uint64_t)(uintptr_t)blob + sizeof blob) != jit_cpu || aot_cpu_at(&im, (uint64_t)(uintptr_t)blob - 1) != jit_cpu)
        FAIL("code outside the cache is not re-emitted under the JIT's profile");
    printf("CPU profile: %d helper/state cases under profile 0, the plain generator identity, JIT/AOT/recovery emission under "
           "profiles 0 and %#llx, re-emission routed by cache PASS\n", cases, (unsigned long long)full);
    return 0;
}
