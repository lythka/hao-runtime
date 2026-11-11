/*
 * Emits every encoder over a sweep of registers/addressing forms and writes
 *   argv[1]: ours.s  — .byte lines produced by hao's emitter
 *   argv[2]: ref.s   — the same instructions as Intel-syntax text
 *   argv[3]: ours.len — the byte length of each of our instructions
 * tools/check_x86.sh assembles both, diffs the disassembly and compares the
 * lengths with the assembler's. Every encoder in include/hao/x86.h is driven
 * here, including the RIP-relative/absolute forms and the exact SSE
 * (prefix, opcode, REX.W) tuples src/jit.c emits.
 */
#include "hao/x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *fo, *fr, *fl;
static uint8_t mem[512];
static xbuf B;

static const char *r64[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
static const char *r32[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
static const char *r16[] = {"ax","cx","dx","bx","sp","bp","si","di","r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
static const char *r8n[] = {"al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
static const char *ccn[] = {"o","no","b","ae","e","ne","be","a","s","ns","p","np","l","ge","le","g"};
static const char *alun[] = {"add","or","adc","sbb","and","sub","xor","cmp"};

static void begin(void) { xb_init(&B, mem, sizeof mem); }
static void end(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void end(const char *fmt, ...)
{
    va_list ap;
    if (B.overflow || B.bad || !xb_len(&B)) { fprintf(stderr, "encoder error at: %s\n", fmt); exit(1); }
    fprintf(fl, "%zu\n", xb_len(&B));
    fprintf(fo, "  .byte ");
    for (size_t i = 0; i < xb_len(&B); i++) fprintf(fo, "%s0x%02x", i ? "," : "", mem[i]);
    fprintf(fo, "\n");
    va_start(ap, fmt);
    fprintf(fr, "  ");
    vfprintf(fr, fmt, ap);
    fprintf(fr, "\n");
    va_end(ap);
}

static char mbuf[128];
static const char *ms(const char *sz, xmem m)
{
    char t[96];
    if (m.base == XRIP)
        snprintf(t, sizeof t, "rip + %d", m.disp);
    else if (m.base == XNONE && m.index >= 0)
        snprintf(t, sizeof t, "%s*%d + %d", r64[m.index], m.scale, m.disp);
    else if (m.base == XNONE)
        snprintf(t, sizeof t, "%d", m.disp);
    else if (m.index >= 0)
        snprintf(t, sizeof t, "%s + %s*%d + %d", r64[m.base], r64[m.index], m.scale, m.disp);
    else
        snprintf(t, sizeof t, "%s + %d", r64[m.base], m.disp);
    snprintf(mbuf, sizeof mbuf, "%s ptr [%s]", sz, t);
    return mbuf;
}

/* Raw filler written to both files (assembled identically). */
static void fill(int n)
{
    fprintf(fo, "  .fill %d, 1, 0xcc\n", n);
    fprintf(fr, "  .fill %d, 1, 0xcc\n", n);
    for (int i = 0; i < n; i++) fprintf(fl, "1\n");
}

/* SSE forms jit.c emits (and the generic table they come from). kind: 0 xmm,xmm;
 * 1 xmm,r (gpr in r/m); 2 r,xmm (gpr in ModRM.reg... i.e. movd/movq r/m, xmm);
 * 3 r32,xmm with the gpr in ModRM.reg (pmovmskb). */
static const char *xmmn(int k) { static char b[4][8]; static int i; i = (i + 1) & 3; snprintf(b[i], 8, "xmm%d", k); return b[i]; }
static const struct { int pfx; uint8_t op; int w; int kind; const char *mn; } sse[] = {
    {0xF2, 0x59, 0, 0, "mulsd"}, {0xF2, 0x5E, 0, 0, "divsd"}, {0xF2, 0x58, 0, 0, "addsd"}, {0xF2, 0x5C, 0, 0, "subsd"},
    {0xF3, 0x59, 0, 0, "mulss"}, {0xF3, 0x5E, 0, 0, "divss"}, {0xF3, 0x58, 0, 0, "addss"}, {0xF3, 0x5C, 0, 0, "subss"},
    {0xF2, 0x51, 0, 0, "sqrtsd"}, {0xF3, 0x51, 0, 0, "sqrtss"}, {0xF2, 0x5A, 0, 0, "cvtsd2ss"}, {0xF3, 0x5A, 0, 0, "cvtss2sd"},
    {0x66, 0x54, 0, 0, "andpd"}, {0x66, 0x57, 0, 0, "xorpd"}, {0x66, 0xEF, 0, 0, "pxor"}, {0x66, 0xDF, 0, 0, "pandn"},
    {0xF3, 0x10, 0, 0, "movss"}, {0xF2, 0x10, 0, 0, "movsd"}, {0x66, 0x2E, 0, 0, "ucomisd"}, {0, 0x2E, 0, 0, "ucomiss"},
    {0x66, 0x2F, 0, 0, "comisd"}, {0, 0x2F, 0, 0, "comiss"}, {0x66, 0x6C, 0, 0, "punpcklqdq"}, {0x66, 0x6D, 0, 0, "punpckhqdq"},
    {0x66, 0x6E, 1, 1, "movq"}, {0x66, 0x6E, 0, 1, "movd"}, {0xF2, 0x2A, 1, 1, "cvtsi2sd"}, {0xF3, 0x2A, 1, 1, "cvtsi2ss"},
    {0x66, 0x7E, 0, 2, "movd"}, {0x66, 0x7E, 1, 2, "movq"},
    /* Advanced SIMD integer forms */
    {0x66, 0xFC, 0, 0, "paddb"}, {0x66, 0xFD, 0, 0, "paddw"}, {0x66, 0xFE, 0, 0, "paddd"}, {0x66, 0xD4, 0, 0, "paddq"},
    {0x66, 0xF8, 0, 0, "psubb"}, {0x66, 0xF9, 0, 0, "psubw"}, {0x66, 0xFA, 0, 0, "psubd"}, {0x66, 0xFB, 0, 0, "psubq"},
    {0x66, 0xDB, 0, 0, "pand"}, {0x66, 0xEB, 0, 0, "por"}, {0x66, 0x74, 0, 0, "pcmpeqb"}, {0x66, 0x75, 0, 0, "pcmpeqw"},
    {0x66, 0x76, 0, 0, "pcmpeqd"}, {0x66, 0x64, 0, 0, "pcmpgtb"}, {0x66, 0x65, 0, 0, "pcmpgtw"}, {0x66, 0x66, 0, 0, "pcmpgtd"},
    {0x66, 0xD8, 0, 0, "psubusb"}, {0x66, 0xD9, 0, 0, "psubusw"}, {0x66, 0xDE, 0, 0, "pmaxub"}, {0x66, 0xDA, 0, 0, "pminub"},
    {0x66, 0xEE, 0, 0, "pmaxsw"}, {0x66, 0xEA, 0, 0, "pminsw"}, {0x66, 0xD5, 0, 0, "pmullw"}, {0x66, 0x60, 0, 0, "punpcklbw"},
    {0x66, 0x62, 0, 0, "punpckldq"}, {0x66, 0x6F, 0, 0, "movdqa"}, {0xF3, 0x7E, 0, 0, "movq"}, {0x66, 0xF6, 0, 0, "psadbw"},
    /* vector floating point, widening, narrowing */
    {0, 0x58, 0, 0, "addps"}, {0, 0x5C, 0, 0, "subps"}, {0, 0x59, 0, 0, "mulps"}, {0, 0x5F, 0, 0, "maxps"},
    {0, 0x5D, 0, 0, "minps"}, {0x66, 0x61, 0, 0, "punpcklwd"}, {0x66, 0x67, 0, 0, "packuswb"}, {0x66, 0x6B, 0, 0, "packssdw"},
    {0x66, 0xD7, 0, 3, "pmovmskb"},
};

static xmem mems[256];
static int nmems;

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    fo = fopen(argv[1], "w");
    fr = fopen(argv[2], "w");
    fl = fopen(argv[3], "w");
    fprintf(fo, ".text\n");
    fprintf(fr, ".intel_syntax noprefix\n.text\n");

    int32_t disps[] = {0, 8, -8, 127, 128, -129, 0x1234};
    for (int bse = 0; bse < 16; bse++)
        for (unsigned d = 0; d < sizeof disps / sizeof *disps; d += (bse % 3 == 0) ? 1 : 3)
            mems[nmems++] = xm(bse, disps[d]);
    mems[nmems++] = xmi(RAX, RCX, 8, 0);
    mems[nmems++] = xmi(R13, R12, 4, 16);
    mems[nmems++] = xmi(RSP, R9, 2, -4);
    mems[nmems++] = xmi(RBP, RAX, 1, 0);
    mems[nmems++] = xmi(R12, R15, 8, 1000);
    mems[nmems++] = xm(XRIP, 0);
    mems[nmems++] = xm(XRIP, -16);
    mems[nmems++] = xm(XRIP, 0x12345);
    mems[nmems++] = xm(XNONE, 0x1234);
    mems[nmems++] = xmi(XNONE, RCX, 8, 0x40);
    mems[nmems++] = xmi(XNONE, R11, 2, -8);

    for (int a = 0; a < 16; a++) {
        for (int c = 0; c < 16; c += 5) {
            begin(); x_mov_rr(&B, 1, a, c); end("mov %s, %s", r64[a], r64[c]);
            begin(); x_mov_rr(&B, 0, a, c); end("mov %s, %s", r32[a], r32[c]);
            for (int op = 0; op < 8; op++) {
                begin(); x_alu_rr(&B, op, 1, a, c); end("%s %s, %s", alun[op], r64[a], r64[c]);
                begin(); x_alu_rr(&B, op, 0, a, c); end("%s %s, %s", alun[op], r32[a], r32[c]);
            }
            begin(); x_test_rr(&B, 1, a, c); end("test %s, %s", r64[a], r64[c]);
            begin(); x_imul_rr(&B, 1, a, c); end("imul %s, %s", r64[a], r64[c]);
            begin(); x_bsr(&B, 0, a, c); end("bsr %s, %s", r32[a], r32[c]);
            begin(); x_bsf(&B, 1, a, c); end("bsf %s, %s", r64[a], r64[c]);
            begin(); x_movzx8_rr(&B, a, c); end("movzx %s, %s", r32[a], r8n[c]);
            begin(); x_movzx16_rr(&B, a, c); end("movzx %s, %s", r32[a], r16[c]);
            begin(); x_movsx_rr(&B, 1, 1, a, c); end("movsx %s, %s", r64[a], r8n[c]);
            begin(); x_movsx_rr(&B, 2, 0, a, c); end("movsx %s, %s", r32[a], r16[c]);
            begin(); x_movsx_rr(&B, 4, 1, a, c); end("movsxd %s, %s", r64[a], r32[c]);
            begin(); x_xchg_rr(&B, 1, a, c); end("xchg %s, %s", r64[a], r64[c]);
            for (int cc = 0; cc < 16; cc += 3) {
                begin(); x_cmov_rr(&B, cc, 1, a, c); end("cmov%s %s, %s", ccn[cc], r64[a], r64[c]);
            }
        }
        uint64_t imms[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull,
                           0xffffffffffffffffull, 0xffffffff80000000ull, 0x123456789abcdef0ull};
        for (unsigned i = 0; i < sizeof imms / sizeof *imms; i++) {
            begin(); x_mov_ri(&B, a, imms[i]);
            /* All of these produce the full 64-bit value; reference uses movabs
             * only when needed, matching our encoding choice. */
            if (imms[i] <= 0xffffffffull) end("mov %s, %llu", r32[a], (unsigned long long)imms[i]);
            else if ((int64_t)imms[i] >= INT32_MIN && (int64_t)imms[i] <= INT32_MAX)
                end("mov %s, %lld", r64[a], (long long)imms[i]);
            else end("movabs %s, %llu", r64[a], (unsigned long long)imms[i]);
        }
        int32_t aimms[] = {1, -1, 127, 128, -129, 0x12345678};
        for (int op = 0; op < 8; op++)
            for (unsigned i = 0; i < 6; i++) {
                begin(); x_alu_ri(&B, op, 1, a, aimms[i]); end("%s %s, %d", alun[op], r64[a], aimms[i]);
                begin(); x_alu_ri(&B, op, 0, a, aimms[i]); end("%s %s, %d", alun[op], r32[a], aimms[i]);
            }
        begin(); x_test_ri(&B, 1, a, 0x100); end("test %s, %d", r64[a], 0x100);
        int shops[] = {XROL, XROR, XSHL, XSHR, XSAR};
        const char *shn[] = {"rol", "ror", "shl", "shr", "sar"};
        for (int s = 0; s < 5; s++) {
            begin(); x_shift_ri(&B, shops[s], 1, a, 13); end("%s %s, 13", shn[s], r64[a]);
            begin(); x_shift_ri(&B, shops[s], 0, a, 1); end("%s %s, 1", shn[s], r32[a]);
            begin(); x_shift_cl(&B, shops[s], 1, a); end("%s %s, cl", shn[s], r64[a]);
        }
        const char *un[] = {0, 0, "not", "neg", "mul", "imul", "div", "idiv"};
        for (int u = 2; u < 8; u++) {
            begin(); x_unary(&B, u, 1, a); end("%s %s", un[u], r64[a]);
        }
        begin(); x_bswap(&B, 1, a); end("bswap %s", r64[a]);
        begin(); x_bswap(&B, 0, a); end("bswap %s", r32[a]);
        for (int cc = 0; cc < 16; cc++) {
            begin(); x_setcc(&B, cc, a); end("set%s %s", ccn[cc], r8n[a]);
        }
        begin(); x_push_r(&B, a); end("push %s", r64[a]);
        begin(); x_pop_r(&B, a); end("pop %s", r64[a]);
        begin(); x_call_r(&B, a); end("call %s", r64[a]);
        begin(); x_jmp_r(&B, a); end("jmp %s", r64[a]);
        begin(); x_test_ri(&B, 0, a, 0x40); end("test %s, %d", r32[a], 0x40);
        for (int c = 0; c < 16; c += 3) {
            begin(); x_shrd_ri(&B, 1, a, c, 9); end("shrd %s, %s, 9", r64[a], r64[c]);
            begin(); x_shrd_ri(&B, 0, a, c, 31); end("shrd %s, %s, 31", r32[a], r32[c]);
        }
        for (unsigned k = 0; k < sizeof sse / sizeof *sse; k++)
            for (int c = 0; c < 16; c += 5) {
                begin(); x_sse_rr(&B, sse[k].pfx, sse[k].op, sse[k].w, a, c);
                const char **g = sse[k].w ? r64 : r32;
                if (sse[k].kind == 0) end("%s %s, %s", sse[k].mn, xmmn(a), xmmn(c));
                else if (sse[k].kind == 1) end("%s %s, %s", sse[k].mn, xmmn(a), g[c]);
                else if (sse[k].kind == 3) end("%s %s, %s", sse[k].mn, g[a], xmmn(c));
                else end("%s %s, %s", sse[k].mn, g[c], xmmn(a));
            }
    }
    for (int a = 0; a < 16; a++) {
        begin(); x_psllq_ri(&B, a, 60); end("psllq xmm%d, 60", a);
        static const struct { uint8_t op; int digit; const char *mn; } sh[] = {
            {0x71, 2, "psrlw"}, {0x71, 4, "psraw"}, {0x71, 6, "psllw"}, {0x72, 2, "psrld"}, {0x72, 4, "psrad"},
            {0x72, 6, "pslld"}, {0x73, 2, "psrlq"}, {0x73, 6, "psllq"},
        };
        for (unsigned k = 0; k < sizeof sh / sizeof *sh; k++) {
            begin(); x_sse_shift_ri(&B, sh[k].op, sh[k].digit, a, (uint8_t)(k + 1)); end("%s xmm%d, %u", sh[k].mn, a, k + 1);
        }
        for (int c = 0; c < 16; c += 5) {
            begin(); x_sse_rri(&B, 0xF2, 0x70, a, c, 0); end("pshuflw %s, %s, 0", xmmn(a), xmmn(c));
            begin(); x_sse_rri(&B, 0x66, 0x70, a, c, 0xEE); end("pshufd %s, %s, 238", xmmn(a), xmmn(c));
            static const uint8_t shuf[] = {0x00, 0x08, 0x55, 0xAA, 0xF5, 0xFF};
            for (unsigned k = 0; k < sizeof shuf; k++) {
                begin(); x_sse_rri(&B, 0x66, 0x70, a, c, shuf[k]); end("pshufd %s, %s, %u", xmmn(a), xmmn(c), shuf[k]);
            }
            static const char *cmpn[] = {"cmpeqps", "cmpltps", "cmpleps"};
            for (unsigned k = 0; k < 3; k++) {
                begin(); x_sse_rri(&B, 0, 0xC2, a, c, (uint8_t)k); end("%s %s, %s", cmpn[k], xmmn(a), xmmn(c));
            }
        }
    }
    int32_t pimms[] = {0, 1, -1, 0x202, 0x7fffffff, (int32_t)0x80000000};
    for (unsigned i = 0; i < sizeof pimms / sizeof *pimms; i++) {
        begin(); x_push_i32(&B, pimms[i]); end("push %d", pimms[i]);   /* we always emit the imm32 form */
    }

    for (int i = 0; i < nmems; i++) {
        xmem m = mems[i];
        for (int a = 0; a < 16; a += 3) {
            begin(); x_mov_ld(&B, 8, a, m); end("mov %s, %s", r64[a], ms("qword", m));
            begin(); x_mov_ld(&B, 4, a, m); end("mov %s, %s", r32[a], ms("dword", m));
            begin(); x_mov_ld(&B, 2, a, m); end("movzx %s, %s", r32[a], ms("word", m));
            begin(); x_mov_ld(&B, 1, a, m); end("movzx %s, %s", r32[a], ms("byte", m));
            begin(); x_movs_ld(&B, 1, 1, a, m); end("movsx %s, %s", r64[a], ms("byte", m));
            begin(); x_movs_ld(&B, 2, 0, a, m); end("movsx %s, %s", r32[a], ms("word", m));
            begin(); x_movs_ld(&B, 4, 1, a, m); end("movsxd %s, %s", r64[a], ms("dword", m));
            begin(); x_mov_st(&B, 8, m, a); end("mov %s, %s", ms("qword", m), r64[a]);
            begin(); x_mov_st(&B, 4, m, a); end("mov %s, %s", ms("dword", m), r32[a]);
            begin(); x_mov_st(&B, 2, m, a); end("mov %s, %s", ms("word", m), r16[a]);
            begin(); x_mov_st(&B, 1, m, a); end("mov %s, %s", ms("byte", m), r8n[a]);
            begin(); x_lea(&B, 1, a, m); end("lea %s, %s", r64[a], ms("qword", m) + 10);
            begin(); x_alu_rm(&B, XADD, 1, a, m); end("add %s, %s", r64[a], ms("qword", m));
            begin(); x_alu_mr(&B, XCMP, 0, m, a); end("cmp %s, %s", ms("dword", m), r32[a]);
            begin(); x_cmov_rm(&B, XCC_LE, 1, a, m); end("cmovle %s, %s", r64[a], ms("qword", m));
            begin(); x_lock_cmpxchg(&B, 8, m, a); end("lock cmpxchg %s, %s", ms("qword", m), r64[a]);
            begin(); x_lock_cmpxchg(&B, 1, m, a); end("lock cmpxchg %s, %s", ms("byte", m), r8n[a]);
            begin(); x_lock_cmpxchg(&B, 2, m, a); end("lock cmpxchg %s, %s", ms("word", m), r16[a]);
            begin(); x_lock_xadd(&B, 4, m, a); end("lock xadd %s, %s", ms("dword", m), r32[a]);
            begin(); x_movdqu_ld(&B, a, m); end("movdqu xmm%d, %s", a, ms("xmmword", m));
            begin(); x_movdqu_st(&B, m, a); end("movdqu %s, xmm%d", ms("xmmword", m), a);
            begin(); x_movq_ld(&B, a, m); end("movq xmm%d, %s", a, ms("qword", m));
            begin(); x_movd_ld(&B, a, m); end("movd xmm%d, %s", a, ms("dword", m));
            begin(); x_movq_st(&B, m, a); end("movq %s, xmm%d", ms("qword", m), a);
            begin(); x_movd_st(&B, m, a); end("movd %s, xmm%d", ms("dword", m), a);
            begin(); x_movdqa_ld(&B, a, m); end("movdqa xmm%d, %s", a, ms("xmmword", m));
            begin(); x_movdqa_st(&B, m, a); end("movdqa %s, xmm%d", ms("xmmword", m), a);
            begin(); x_sse_rm(&B, 0xF2, 0x10, 0, a, m); end("movsd xmm%d, %s", a, ms("qword", m));
            begin(); x_sse_rm(&B, 0x66, 0x2E, 0, a, m); end("ucomisd xmm%d, %s", a, ms("qword", m));
            if (a == 0) {
                begin(); x_stmxcsr(&B, m); end("stmxcsr %s", ms("dword", m));
                begin(); x_ldmxcsr(&B, m); end("ldmxcsr %s", ms("dword", m));
            }
            begin(); x_lock_cmpxchg(&B, 4, m, a); end("lock cmpxchg %s, %s", ms("dword", m), r32[a]);
            begin(); x_lock_xadd(&B, 1, m, a); end("lock xadd %s, %s", ms("byte", m), r8n[a]);
            begin(); x_lock_xadd(&B, 2, m, a); end("lock xadd %s, %s", ms("word", m), r16[a]);
            begin(); x_lock_xadd(&B, 8, m, a); end("lock xadd %s, %s", ms("qword", m), r64[a]);
            begin(); x_xchg_mr(&B, 1, m, a); end("xchg %s, %s", ms("byte", m), r8n[a]);
            begin(); x_xchg_mr(&B, 2, m, a); end("xchg %s, %s", ms("word", m), r16[a]);
            begin(); x_xchg_mr(&B, 4, m, a); end("xchg %s, %s", ms("dword", m), r32[a]);
            begin(); x_xchg_mr(&B, 8, m, a); end("xchg %s, %s", ms("qword", m), r64[a]);
        }
        for (int a = 1; a < 16; a += 4) {   /* the remaining register numbers */
            begin(); x_mov_ld(&B, 8, a, m); end("mov %s, %s", r64[a], ms("qword", m));
            begin(); x_mov_st(&B, 1, m, a); end("mov %s, %s", ms("byte", m), r8n[a]);
            begin(); x_xchg_mr(&B, 1, m, a); end("xchg %s, %s", ms("byte", m), r8n[a]);
            begin(); x_lock_cmpxchg(&B, 1, m, a); end("lock cmpxchg %s, %s", ms("byte", m), r8n[a]);
        }
        begin(); x_call_m(&B, m); end("call %s", ms("qword", m));
        begin(); x_mov_mi(&B, 8, m, -5); end("mov %s, -5", ms("qword", m));
        begin(); x_mov_mi(&B, 4, m, 0x12345); end("mov %s, 74565", ms("dword", m));
        begin(); x_mov_mi(&B, 2, m, 0x1234); end("mov %s, 4660", ms("word", m));
        begin(); x_mov_mi(&B, 1, m, 7); end("mov %s, 7", ms("byte", m));
        begin(); x_alu_mi(&B, XSUB, 1, m, 300); end("sub %s, 300", ms("qword", m));
        begin(); x_alu_mi(&B, XAND, 0, m, -2); end("and %s, -2", ms("dword", m));
        begin(); x_test_mi8(&B, m, 0x80); end("test %s, 128", ms("byte", m));
        begin(); x_push_m(&B, m); end("push %s", ms("qword", m));
        begin(); x_pop_m(&B, m); end("pop %s", ms("qword", m));
        begin(); x_jmp_m(&B, m); end("jmp %s", ms("qword", m));
    }
    /* RIP-relative helpers: target = this instruction's own first byte, so the
     * displacement is minus the instruction length. */
    for (int a = 0; a < 16; a++) {
        begin(); x_mov_ld_rip(&B, 8, a, mem); end("mov %s, qword ptr [rip + %d]", r64[a], -(int)xb_len(&B));
        begin(); x_mov_ld_rip(&B, 4, a, mem + 100); end("mov %s, dword ptr [rip + %d]", r32[a], 100 - (int)xb_len(&B));
        begin(); x_lea_rip(&B, a, mem + 400); end("lea %s, [rip + %d]", r64[a], 400 - (int)xb_len(&B));
    }
    /* Branches, patched to a target past a filler of `gap` bytes; the script
     * compares branch targets relative to the branch instruction. */
    static int lbl;
    for (int g = 0; g < 2; g++) {
        int gap = g ? 300 : 20;
        for (int cc = 0; cc < 16; cc++) {
            begin(); size_t at = x_jcc32(&B, cc);
            if (!x_patch32(&B, at, xb_len(&B) + (size_t)gap)) return 1;
            /* the assembler would relax a near jcc to rel8: force rel32 */
            end("{disp32} j%s L%d", ccn[cc], ++lbl); fill(gap); fprintf(fr, "L%d:\n", lbl);
        }
        begin(); size_t at = x_jmp32(&B); if (!x_patch32(&B, at, xb_len(&B) + (size_t)gap)) return 1;
        end("{disp32} jmp L%d", ++lbl); fill(gap); fprintf(fr, "L%d:\n", lbl);
    }
    for (int cc = 0; cc < 16; cc++) {
        begin(); size_t at = x_jcc8(&B, cc); if (!x_patch8(&B, at, xb_len(&B) + 9)) return 1;
        end("j%s L%d", ccn[cc], ++lbl); fill(9); fprintf(fr, "L%d:\n", lbl);
    }
    {
        begin(); size_t at = x_jmp8(&B); if (!x_patch8(&B, at, xb_len(&B) + 100)) return 1;
        end("jmp L%d", ++lbl); fill(100); fprintf(fr, "L%d:\n", lbl);
        begin(); at = x_jrcxz8(&B); if (!x_patch8(&B, at, xb_len(&B) + 3)) return 1;
        end("jrcxz L%d", ++lbl); fill(3); fprintf(fr, "L%d:\n", lbl);
        /* backward short branch */
        fprintf(fr, "L%d:\n", ++lbl); fill(5);
        begin(); at = x_jcc8(&B, XCC_NE); if (!x_patch8(&B, at, 0)) return 1;
        /* target 0 is the start of this instruction's buffer: 5 filler bytes
         * precede it in the stream, so retarget: emit relative -7 by hand */
        mem[at] = (uint8_t)(int8_t)-(int)(5 + 2);
        end("jne L%d", lbl);
    }
    /* Contract violations must set `bad` and emit nothing. */
    {
        uint8_t t[64]; xbuf b; int fails = 0;
#define MUSTFAIL(stmt) do { xb_init(&b, t, sizeof t); stmt; if (!b.bad || xb_len(&b)) { fprintf(stderr, "not rejected: %s\n", #stmt); fails++; } } while (0)
        MUSTFAIL(x_mov_ld(&b, 16, RAX, xm(RCX, 0)));
        MUSTFAIL(x_mov_ld(&b, 3, RAX, xm(RCX, 0)));
        MUSTFAIL(x_movs_ld(&b, 8, 1, RAX, xm(RCX, 0)));
        MUSTFAIL(x_mov_st(&b, 16, xm(RCX, 0), RAX));
        MUSTFAIL(x_mov_mi(&b, 3, xm(RCX, 0), 1));
        MUSTFAIL(x_lock_cmpxchg(&b, 16, xm(RCX, 0), RAX));
        MUSTFAIL(x_xchg_mr(&b, 0, xm(RCX, 0), RAX));
        MUSTFAIL(x_sse_rr(&b, 1, 0x6E, 1, 0, RAX));
        MUSTFAIL(x_sse_rri(&b, 1, 0x70, 0, 1, 0));
        MUSTFAIL(x_sse_shift_ri(&b, 0x73, 4, 0, 1));
        MUSTFAIL(x_sse_shift_ri(&b, 0x70, 2, 0, 1));
        MUSTFAIL(x_sse_shift_ri(&b, 0x71, 3, 0, 1));
        MUSTFAIL(x_sse_rm(&b, 0xF0, 0x10, 0, 0, xm(RCX, 0)));
        MUSTFAIL(x_mov_rr(&b, 1, 16, RAX));
        MUSTFAIL(x_mov_ri(&b, -3, 1));
        MUSTFAIL(x_mov_ld(&b, 8, RAX, xmi(RCX, RSP, 1, 0)));
        MUSTFAIL(x_mov_ld(&b, 8, RAX, xmi(RCX, RDX, 3, 0)));
        MUSTFAIL(x_mov_ld(&b, 8, RAX, xmi(XRIP, RDX, 1, 0)));
        MUSTFAIL(x_cmov_rr(&b, 16, 1, RAX, RCX));
        MUSTFAIL(x_nopn(&b, 4));
        MUSTFAIL(x_mov_ld_rip(&b, 8, RAX, t + 0x100000000ll));
        MUSTFAIL(x_lea_rip(&b, RAX, t - 0x100000000ll));
        xb_init(&b, t, sizeof t); size_t at = x_jcc8(&b, XCC_E);
        if (x_patch8(&b, at, 200) || !b.bad) { fprintf(stderr, "x_patch8 out of range not rejected\n"); fails++; }
        xb_init(&b, t, sizeof t); at = x_jmp32(&b);
        if (x_patch32(&b, at + 8, 0) || !b.bad) { fprintf(stderr, "x_patch32 outside the buffer not rejected\n"); fails++; }
        xb_init(&b, t, 2); x_mov_ri(&b, RAX, 1);
        if (!b.overflow) { fprintf(stderr, "overflow not reported\n"); fails++; }
        if (fails) return 1;
    }
    begin(); x_mfence(&B); end("mfence");
    begin(); x_int3(&B); end("int3");
    begin(); x_nop(&B); end("nop");
    begin(); x_lahf(&B); end("lahf");
    begin(); x_sahf(&B); end("sahf");
    begin(); x_add_al_i8(&B, 0x7f); end("add al, 127");
    begin(); x_pushfq(&B); end("pushfq");
    begin(); x_popfq(&B); end("popfq");
    begin(); x_cmc(&B); end("cmc");
    begin(); x_stc(&B); end("stc");
    begin(); x_clc(&B); end("clc");
    begin(); x_cqo(&B, 1); end("cqo");
    begin(); x_cqo(&B, 0); end("cdq");
    begin(); x_syscall(&B); end("syscall");
    begin(); x_ud2(&B); end("ud2");
    begin(); x_ret(&B); end("ret");
    begin(); x_nopn(&B, 1); end("nop");
    begin(); x_nopn(&B, 2); end("xchg ax, ax");
    begin(); x_nopn(&B, 3); end("nop dword ptr [rax]");
    fclose(fo);
    fclose(fr);
    return 0;
}
