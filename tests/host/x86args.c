/* Host test: x86_64 instruction lengths and the stack-argument slots of a native function
 * (src/x86args.c, include/hao/x86args.h), which swiftcc.c checks a packed layout from an arm64 call
 * site against.
 *
 *   lengths    instructions of the forms Swift and C code use (prefixes, REX, ModRM/SIB/disp,
 *              immediates by operand size, 0F/0F38/0F3A maps, VEX) decode to their lengths; EVEX,
 *              3DNow! and opcodes invalid in 64-bit mode, and a cut-off instruction, decode to 0
 *   slots      a function's rbp+16+8k accesses: count and widest width per slot (pinsrd 4, movzbl 1,
 *              movq 8, movups covering two slots)
 *   jumps      a one-jump function and a `push; mov; pop; jmp` thunk give their target
 *   tables     decoding stops at the jump table after the last block (bytes that would decode as an
 *              rbp access are not counted); a jump through a register after `pop %rbp` is a tail call
 *   refusals   no frame-pointer prologue, the address of an argument taken, an indexed access through
 *              rbp, a register jump with no table, an undecodable instruction; zero padding is not one
 *
 *   test_x86args     prints one line per check; exit 0 only if all pass.
 */
#include "hao/x86args.h"
#include <stdio.h>
#include <string.h>

static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const char *name; unsigned len; uint8_t b[16]; } lc;

static void lengths(void)
{
    static const lc cases[] = {
        {"push rbp", 1, {0x55}},
        {"mov rsp,rbp", 3, {0x48, 0x89, 0xe5}},
        {"nopw cs:0(rax,rax)", 10, {0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {"movabs imm64", 10, {0x48, 0xb8, 1, 2, 3, 4, 5, 6, 7, 8}},
        {"mov imm32 eax", 5, {0xb8, 1, 2, 3, 4}},
        {"mov imm16 ax", 4, {0x66, 0xb8, 1, 2}},
        {"testb imm, 0x10(rbp)", 4, {0xf6, 0x45, 0x10, 0x01}},
        {"testl imm, 0x10(rbp)", 7, {0xf7, 0x45, 0x10, 1, 2, 3, 4}},
        {"notl 0x10(rbp)", 3, {0xf7, 0x55, 0x10}},
        {"movw imm, 0x10(rbp)", 6, {0x66, 0xc7, 0x45, 0x10, 0x34, 0x12}},
        {"movq imm32, 0x100(rsp)", 12, {0x48, 0xc7, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00, 1, 2, 3, 4}},
        {"lea rip", 7, {0x48, 0x8d, 0x05, 1, 2, 3, 4}},
        {"movslq (rcx,rax,4)", 4, {0x48, 0x63, 0x04, 0x81}},
        {"mov (,rax,8)+disp32", 7, {0x8b, 0x04, 0xc5, 1, 2, 3, 4}},
        {"pinsrd", 7, {0x66, 0x0f, 0x3a, 0x22, 0x45, 0x10, 0x01}},
        {"pmovzxbw", 5, {0x66, 0x0f, 0x38, 0x30, 0xc1}},
        {"movzbl 0x28(rbp)", 4, {0x0f, 0xb6, 0x4d, 0x28}},
        {"movups disp32", 7, {0x0f, 0x10, 0x85, 1, 2, 3, 4}},
        {"pshufd", 5, {0x66, 0x0f, 0x70, 0xc0, 0x1b}},
        {"vzeroupper", 3, {0xc5, 0xf8, 0x77}},
        {"vmovups (rdi),ymm0", 4, {0xc5, 0xfc, 0x10, 0x07}},
        {"vbroadcastss rip", 9, {0xc4, 0xe2, 0x79, 0x18, 0x05, 1, 2, 3, 4}},
        {"vpalignr", 6, {0xc4, 0xe3, 0x7d, 0x0f, 0xc1, 0x08}},
        {"jcc rel32", 6, {0x0f, 0x84, 1, 2, 3, 4}},
        {"jmp rel8", 2, {0xeb, 0x10}},
        {"call rel32", 5, {0xe8, 1, 2, 3, 4}},
        {"call *0x30(r15)", 4, {0x41, 0xff, 0x57, 0x30}},
        {"jmp *rax", 2, {0xff, 0xe0}},
        {"ret imm16", 3, {0xc2, 8, 0}},
        {"enter", 4, {0xc8, 0x10, 0x00, 0x00}},
        {"lock cmpxchg", 5, {0xf0, 0x48, 0x0f, 0xb1, 0x0f}},
        {"fs mov", 9, {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}},
        {"movabs moffs", 9, {0xa1, 1, 2, 3, 4, 5, 6, 7, 8}},
        {"endbr64", 4, {0xf3, 0x0f, 0x1e, 0xfa}},
        {"ud2", 2, {0x0f, 0x0b}},
        {"stc", 1, {0xf9}},
        {"cqo; syscall", 2, {0x48, 0x99}},
        {"evex", 0, {0x62, 0xf1, 0x7c, 0x48, 0x28, 0x05, 1, 2, 3, 4}},
        {"3dnow", 0, {0x0f, 0x0f, 0xc1, 0x9e}},
        {"push es (invalid)", 0, {0x06}},
        {"jmp far (invalid)", 0, {0xea, 1, 2, 3, 4, 5, 6}},
    };
    int bad = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        hao_x86_insn o;
        int n = hao_x86_decode(cases[k].b, sizeof cases[k].b, &o);
        if ((unsigned)n != cases[k].len) { bad = 1; CHECK(0, "%s: length %d, want %u", cases[k].name, n, cases[k].len); }
    }
    hao_x86_insn o;
    static const uint8_t cut[] = {0x48, 0xb8, 1, 2, 3};
    CHECK(hao_x86_decode(cut, sizeof cut, &o) == 0, "a cut-off movabs decodes");
    static const uint8_t m[] = {0x48, 0x8b, 0x4d, 0x30};   /* movq 0x30(%rbp), %rcx */
    CHECK(hao_x86_decode(m, sizeof m, &o) == 4 && o.mem && o.base == 5 && o.index == -1 && o.disp == 0x30 && o.width == 8 &&
          o.access, "movq 0x30(%%rbp): base %d disp %d width %d", o.base, o.disp, o.width);
    static const uint8_t r13[] = {0x49, 0x8b, 0x45, 0x10};   /* movq 0x10(%r13), %rax */
    CHECK(hao_x86_decode(r13, sizeof r13, &o) == 4 && o.base == 13, "r13 base %d", o.base);
    static const uint8_t lea[] = {0x48, 0x8d, 0x45, 0x10};
    CHECK(hao_x86_decode(lea, sizeof lea, &o) == 4 && o.mem && !o.access, "lea is no access");
    if (!bad) printf("ok   lengths: %zu encodings\n", sizeof cases / sizeof *cases);
}

/* prologue, body, `pop %rbp; ret` */
#define PRO 0x55, 0x48, 0x89, 0xe5
#define EPI 0x5d, 0xc3

static bool run(const uint8_t *c, size_t n, hao_x86_stackuse *u) { return hao_x86_stackargs(c, n, NULL, 0, u); }

static void slots(void)
{
    /* FindMyLocate Friend.init's reads: pinsrd 0x10/0x18/0x20, movzbl 0x28, movq 0x30, 0x38 */
    static const uint8_t f[] = {PRO,
        0x66, 0x0f, 0x3a, 0x22, 0x45, 0x10, 0x01, 0x66, 0x0f, 0x3a, 0x22, 0x45, 0x18, 0x02,
        0x66, 0x0f, 0x3a, 0x22, 0x45, 0x20, 0x03, 0x0f, 0xb6, 0x4d, 0x28, 0x48, 0x8b, 0x4d, 0x30,
        0x48, 0x8b, 0x7d, 0x38, 0x48, 0x8b, 0x45, 0xf8, EPI};
    hao_x86_stackuse u;
    bool ok = run(f, sizeof f, &u);
    static const uint8_t want[] = {4, 4, 4, 1, 8, 8};
    CHECK(ok && u.nslots == 6 && !memcmp(u.width, want, 6) && !u.jump, "friend: ok %d nslots %d widths %d %d %d %d %d %d", ok,
          u.nslots, u.width[0], u.width[1], u.width[2], u.width[3], u.width[4], u.width[5]);
    /* movups 0x18(%rbp) covers slots 1 and 2; movb to slot 0 and movzbl from it: widest 1 */
    static const uint8_t v[] = {PRO, 0x0f, 0x10, 0x45, 0x18, 0xc6, 0x45, 0x10, 0x01, 0x0f, 0xb6, 0x45, 0x10, EPI};
    ok = run(v, sizeof v, &u);
    CHECK(ok && u.nslots == 3 && u.width[0] == 1 && u.width[1] == 8 && u.width[2] == 8, "movups: nslots %d", u.nslots);
    /* an x87 load: width unknown */
    static const uint8_t x87[] = {PRO, 0xdd, 0x45, 0x10, EPI};
    ok = run(x87, sizeof x87, &u);
    CHECK(ok && u.nslots == 1 && u.width[0] == HAO_X86_WIDTH_UNKNOWN, "x87: width %d", u.width[0]);
    /* no stack argument: locals only, odd zero padding at the end */
    static const uint8_t none[] = {0xf3, 0x0f, 0x1e, 0xfa, PRO, 0x48, 0x89, 0x7d, 0xf0, EPI, 0x00, 0x00, 0x00};
    ok = run(none, sizeof none, &u);
    CHECK(ok && u.nslots == 0, "locals: ok %d nslots %d", ok, u.nslots);
    if (!failed) printf("ok   slots\n");
}

static void jumps(void)
{
    hao_x86_stackuse u;
    static const uint8_t j[] = {0xe9, 0x10, 0x00, 0x00, 0x00};
    CHECK(run(j, sizeof j, &u) && u.jump == (uint64_t)(uintptr_t)(j + 5 + 0x10), "jmp: target");
    static const uint8_t t[] = {PRO, 0x5d, 0xeb, 0xfe};
    CHECK(run(t, sizeof t, &u) && u.jump == (uint64_t)(uintptr_t)(t + 5), "thunk: target");
    if (!failed) printf("ok   jumps\n");
}

static void tables(void)
{
    hao_x86_stackuse u;
    /* lea table(%rip), %rcx; movslq (%rcx,%rax,4), %rax; add %rcx, %rax; jmp *%rax; movzbl 0x10(%rbp);
     * pop; ret; xchg ax,ax; then a table whose bytes decode as pushq 0x40(%rbp) */
    static const uint8_t f[] = {PRO, 0x48, 0x8d, 0x0d, 0x11, 0x00, 0x00, 0x00, 0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8,
        0xff, 0xe0, 0x0f, 0xb6, 0x45, 0x10, EPI, 0x66, 0x90, 0xff, 0x75, 0x40, 0x00, 0xf0, 0xff, 0xff, 0xff};
    bool ok = run(f, sizeof f, &u);
    CHECK(ok && u.nslots == 1 && u.width[0] == 1, "table: ok %d nslots %d (%s)", ok, u.nslots, u.why ? u.why : "-");
    /* a tail call through a register after the epilogue */
    static const uint8_t tc[] = {PRO, 0x0f, 0xb6, 0x45, 0x10, 0x5d, 0xff, 0xe0};
    ok = run(tc, sizeof tc, &u);
    CHECK(ok && u.nslots == 1, "tail call: ok %d (%s)", ok, u.why ? u.why : "-");
    /* the same jump inside the frame, with no table: not shown */
    static const uint8_t nt[] = {PRO, 0x0f, 0xb6, 0x45, 0x10, 0xff, 0xe0, EPI};
    CHECK(!run(nt, sizeof nt, &u), "register jump without a table shown");
    /* data in code skipped */
    static const uint8_t dic[] = {PRO, 0xeb, 0x04, 0xff, 0x75, 0x40, 0x00, 0x0f, 0xb6, 0x45, 0x18, EPI};
    uint32_t skip[2] = {6, 10};
    ok = hao_x86_stackargs(dic, sizeof dic, skip, 1, &u);
    CHECK(ok && u.nslots == 2 && u.width[0] == 0 && u.width[1] == 1, "data in code: ok %d nslots %d", ok, u.nslots);
    if (!failed) printf("ok   tables\n");
}

static void refusals(void)
{
    hao_x86_stackuse u;
    static const uint8_t noframe[] = {0x48, 0x8b, 0x44, 0x24, 0x08, 0xc3};   /* movq 8(%rsp), %rax; ret */
    CHECK(!run(noframe, sizeof noframe, &u), "no prologue shown");
    static const uint8_t addr[] = {PRO, 0x48, 0x8d, 0x45, 0x10, EPI};   /* lea 0x10(%rbp), %rax */
    CHECK(!run(addr, sizeof addr, &u), "address of an argument shown");
    static const uint8_t idx[] = {PRO, 0x48, 0x8b, 0x44, 0xc5, 0x10, EPI};   /* movq 0x10(%rbp,%rax,8), %rax */
    CHECK(!run(idx, sizeof idx, &u), "indexed access shown");
    static const uint8_t off[] = {PRO, 0x0f, 0xb6, 0x45, 0x11, EPI};   /* movzbl 0x11(%rbp) */
    CHECK(!run(off, sizeof off, &u), "access off a slot's start shown");
    static const uint8_t evex[] = {PRO, 0x62, 0xf1, 0x7c, 0x48, 0x28, 0x45, 0x01, EPI};
    CHECK(!run(evex, sizeof evex, &u), "EVEX shown");
    static const uint8_t junk[] = {PRO, 0x06, EPI};
    CHECK(!run(junk, sizeof junk, &u), "an invalid opcode shown");
    if (!failed) printf("ok   refusals\n");
}

int main(void)
{
    lengths();
    slots();
    jumps();
    tables();
    refusals();
    printf(failed ? "test_x86args: FAIL\n" : "test_x86args: PASS\n");
    return failed;
}
