/* Exercise the production scanner on a private synthetic cache, without
 * loading or running a system framework. The ARM interpreter executes the
 * patched loads and compares their values with native sel_registerName.
 * Including dsc.c keeps the test seam out of the runtime's public ABI. */
#include "../../src/dsc.c"

enum { WORDS = 128, BYTES = 0x18000 };
static const uint64_t VA = 0x100000000ull;
static uint8_t *mem;
static uint32_t *code;
static hao_image fixture;
static selmap selectors;
static uint64_t starts[2];
static uint64_t strings[2], slots[2], native[2];
static unsigned checks;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint32_t add(unsigned rd, unsigned rn, uint64_t target)
{
    return 0x91000000u | ((target & 0xfff) << 10) | rn << 5 | rd;
}
static void setup(bool split)
{
    memset(&D, 0, sizeof D);
    npool = 0;
    memset(pool, 0, sizeof pool);
    mem = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mem != MAP_FAILED);
    D.start = VA; D.size = BYTES; D.slide = (uint64_t)(uintptr_t)mem - VA;
    D.state = 1;
    D.maps[0] = (dmap){.addr = VA, .size = 0x8000};
    D.maps[1] = (dmap){.addr = VA + 0x10000, .size = 0x8000};
    D.nmaps = 2; // The middle is a reserved hole available to the slot pool.
    code = (uint32_t *)(mem + 0x1000);
    for (unsigned i = 0; i < WORDS; i++) code[i] = 0xd503201f; // nop
    code[WORDS - 1] = 0xd65f03c0; // ret
    strings[0] = (uint64_t)(uintptr_t)(mem + 0x4100);
    strings[1] = (uint64_t)(uintptr_t)(mem + 0x4120);
    strcpy((char *)(uintptr_t)strings[0], "init");
    strcpy((char *)(uintptr_t)strings[1], "description");
    slots[0] = (uint64_t)(uintptr_t)(mem + 0x10000);
    slots[1] = (uint64_t)(uintptr_t)(mem + (split ? 0x11000 : 0x10008));
    selectors = (selmap){calloc(16, sizeof(selent)), 16};
    CHECK(selectors.v != NULL);
    for (unsigned i = 0; i < 2; i++) {
        native[i] = (uint64_t)(uintptr_t)sel_registerName((const char *)(uintptr_t)strings[i]);
        *(uint64_t *)(uintptr_t)slots[i] = native[i];
        size_t h = (strings[i] >> 2) & 15;
        while (selectors.v[h].str) h = (h + 1) & 15;
        selectors.v[h] = (selent){strings[i], slots[i]};
    }
    memset(&fixture, 0, sizeof fixture);
    snprintf(fixture.path, sizeof fixture.path, "synthetic-hoisted-selectors");
    fixture.base = (uint64_t)(uintptr_t)mem;
    fixture.slide = D.slide;
    fixture.nsects = 1;
    fixture.sects[0].addr = (uint64_t)(uintptr_t)code;
    fixture.sects[0].size = WORDS * 4;
    fixture.sects[0].flags = S_ATTR_PURE_INSTRUCTIONS;
    starts[0] = (uint64_t)(uintptr_t)code;
    fixture.fstarts = starts; fixture.nfstarts = 1;
}
static void finish(const char *name)
{
    free(selectors.v);
    free(D.fixed);
    munmap(mem, BYTES);
    printf("PASS %s\n", name);
}
static void page(unsigned at_, unsigned reg)
{
    code[at_] = enc_adrp(VA + 0x1000 + 4 * at_, reg, VA + 0x4000);
}
static void far_loads(void)
{
    page(0, 28);
    // Nine calls, as in AutoUnlockDaemon. Their targets are outside this
    // image, so the ABI preserves x28. The two reads are 48 and 101 away.
    for (unsigned i = 4; i <= 36; i += 4) code[i] = 0x94001000;
    code[48] = add(1, 28, strings[0]);
    code[101] = add(2, 28, strings[1]);
}
static void execute(unsigned reg, unsigned site, uint64_t expected, uint64_t branch_input)
{
    hao_cpu cpu = {0};
    cpu.pc = (uint64_t)(uintptr_t)code;
    cpu.x[0] = branch_input;
    for (unsigned step = 0; step < 1024; step++) {
        uint64_t pc = cpu.pc;
        CHECK(pc >= (uint64_t)(uintptr_t)code && pc < (uint64_t)(uintptr_t)(code + WORDS));
        uint32_t insn = *(const uint32_t *)(uintptr_t)pc;
        if (insn == 0xd65f03c0) break;
        if ((insn & 0xfc000000) == 0x94000000) {
            // A native callee may overwrite caller-saved registers.
            for (unsigned r = 0; r <= 17; r++) cpu.x[r] = 0xdead0000 + r;
            cpu.x[30] = pc + 4; cpu.pc = pc + 4;
        } else {
            int rc = a64_step(&cpu);
            CHECK(rc == 0 || rc == HAO_EXIT_BRANCH);
        }
        if (pc == (uint64_t)(uintptr_t)(code + site)) { CHECK(cpu.x[reg] == expected); return; }
    }
    CHECK(!"reader not executed");
}
static void unchanged(const char *name)
{
    uint32_t original[WORDS]; memcpy(original, code, sizeof original);
    scan_code(&fixture, &selectors);
    CHECK(!memcmp(original, code, sizeof original));
    finish(name);
}
int main(void)
{
    for (unsigned split = 0; split < 2; split++) {
        setup(split); far_loads();
        execute(1, 48, strings[0], 0); execute(2, 101, strings[1], 0); // unfixed control
        scan_code(&fixture, &selectors);
        CHECK(D.n_selsites == 2 && D.n_selcode == 2 && D.n_selfar == 2 && !D.n_selleft);
        CHECK(npool == (int)split);
        execute(1, 48, native[0], 0); execute(2, 101, native[1], 0);
        finish(split ? "hoisted reads, split selrefs use one pool page" : "hoisted reads across nine calls, distances 48 and 101");
    }
    setup(false); far_loads(); code[70] = 0xaa1c03e3; // mov x3, x28: non-selector reader
    unchanged("mixed reader preserves the whole ADRP web");
    setup(false); far_loads(); code[70] = 0xf900027c; // str x28, [x19]
    unchanged("storing the page value prevents retargeting");
    setup(false); far_loads(); code[47] = 0xd280001c; // mov x28, #0
    unchanged("register overwrite ends the definition");
    setup(false); page(0, 9); code[4] = 0x94001000; code[48] = add(1, 9, strings[0]);
    unchanged("call clobbers caller-saved page register");
    setup(false); page(0, 28); code[48] = add(1, 28, strings[0]);
    starts[1] = (uint64_t)(uintptr_t)(code + 32); fixture.nfstarts = 2;
    code[31] = 0xd65f03c0;
    unchanged("function boundaries do not inherit a page register");
    setup(false); page(0, 28); code[48] = add(1, 28, strings[0]); code[60] = 0xd61f0200; // br x16
    unchanged("live page at an indirect branch stays unchanged");
    setup(false); far_loads(); code[4] = 0x94000074; // bl to the local outliner at word 120
    starts[1] = (uint64_t)(uintptr_t)(code + 120); fixture.nfstarts = 2;
    code[119] = 0xd65f03c0; code[120] = 0xaa1c03e0; code[121] = 0xd65f03c0;
    unchanged("local outliner reads the saved page register");
    setup(false);
    code[0] = 0xb4000060; // cbz x0, +12
    page(1, 28); code[2] = 0x14000002; page(3, 28);
    code[48] = add(1, 28, strings[0]);
    scan_code(&fixture, &selectors);
    CHECK(D.n_selcode == 1 && !D.n_selleft);
    execute(1, 48, native[0], 0);
    execute(1, 48, native[0], 1);
    finish("two reaching ADRPs are rewritten together");
    setup(false);
    code[0] = 0xb4000060; page(1, 28); code[2] = 0x14000002;
    code[3] = 0xd280001c; code[48] = add(1, 28, strings[0]);
    unchanged("join with another definition is rejected");
    setup(false);
    hao_dsc_sel_lo = (uint64_t)(uintptr_t)(mem + 0x4000); hao_dsc_sel_span = 0x1000;
    CHECK(hao_dsc_sel_map(strings[0]) == native[0]);
    CHECK(hao_dsc_sel_map(native[0]) == native[0]);
    CHECK(hao_dsc_sel_map(1) == 1);
    mem[0x4fff] = 'x';
    CHECK(hao_dsc_sel_map((uint64_t)(uintptr_t)(mem + 0x4fff)) == (uint64_t)(uintptr_t)(mem + 0x4fff));
    finish("msgSend range fallback and unterminated boundary");
    printf("dscselhoist: PASS (%u checks)\n", checks);
    return 0;
}
