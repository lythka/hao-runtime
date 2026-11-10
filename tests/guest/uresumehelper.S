/* Native helper for tests/guest/uresume.cpp: the register-level halves.
 * Offsets: tests/guest/uresume.h (uh_ctx, uh_full).
 *
 * uh_call_full(uh_ctx *c): call c->fn with a0..a9 and v0..v7 from c, every
 * callee-saved register and the FP control (MXCSR + x87 control word, or
 * FPCR + FPSR) from c, the caller-saved registers that are not arguments
 * and the other vector registers (with AVX the ymm upper halves too) set
 * to known values; record what the ABI says survives the call.
 *
 * uh_fault_full(uh_full *f): every register from f (RFLAGS with CF PF AF
 * ZF SF DF OF set, or NZCV), then one load from f->addr into the register
 * that held the address; everything recorded after it. */
#if defined(__x86_64__)
#define C_VEC     0x000
#define C_YHI     0x080
#define C_FN      0x180
#define C_GP      0x188
#define C_KEEP    0x1d8
#define C_FPKEEP  0x230
#define C_FPCTL   0x270
#define C_X87     0x278
#define C_AVX     0x27c
#define C_RET     0x280
#define C_KEEPA   0x288
#define C_SPB     0x320
#define C_SPA     0x328
#define C_FPCTLA  0x330
#define C_X87A    0x338
#define C_FLAGSA  0x33c

    .text
    .globl _uh_call_full
    .p2align 4
_uh_call_full:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    pushq %rbx
    .cfi_def_cfa_offset 24
    pushq %r12
    .cfi_def_cfa_offset 32
    pushq %r13
    .cfi_def_cfa_offset 40
    pushq %r14
    .cfi_def_cfa_offset 48
    pushq %r15
    .cfi_def_cfa_offset 56
    subq  $56, %rsp
    .cfi_def_cfa_offset 112
    .cfi_offset %r15, -56
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    .cfi_offset %rbp, -16
    /* [rsp+0..31] stack arguments a6..a9, [rsp+32] c, [rsp+40] fn */
    movq  %rdi, 32(%rsp)
    movq  C_FN(%rdi), %rax
    movq  %rax, 40(%rsp)
    movq  C_GP+48(%rdi), %rax
    movq  %rax, 0(%rsp)
    movq  C_GP+56(%rdi), %rax
    movq  %rax, 8(%rsp)
    movq  C_GP+64(%rdi), %rax
    movq  %rax, 16(%rsp)
    movq  C_GP+72(%rdi), %rax
    movq  %rax, 24(%rsp)
    movq  %rsp, C_SPB(%rdi)
    ldmxcsr C_FPCTL(%rdi)
    fldcw C_X87(%rdi)
    cmpl  $0, C_AVX(%rdi)
    je    1f
    vinsertf128 $1, C_YHI+0x00(%rdi), %ymm0, %ymm0
    vinsertf128 $1, C_YHI+0x10(%rdi), %ymm1, %ymm1
    vinsertf128 $1, C_YHI+0x20(%rdi), %ymm2, %ymm2
    vinsertf128 $1, C_YHI+0x30(%rdi), %ymm3, %ymm3
    vinsertf128 $1, C_YHI+0x40(%rdi), %ymm4, %ymm4
    vinsertf128 $1, C_YHI+0x50(%rdi), %ymm5, %ymm5
    vinsertf128 $1, C_YHI+0x60(%rdi), %ymm6, %ymm6
    vinsertf128 $1, C_YHI+0x70(%rdi), %ymm7, %ymm7
    vinsertf128 $1, C_YHI+0x80(%rdi), %ymm8, %ymm8
    vinsertf128 $1, C_YHI+0x90(%rdi), %ymm9, %ymm9
    vinsertf128 $1, C_YHI+0xa0(%rdi), %ymm10, %ymm10
    vinsertf128 $1, C_YHI+0xb0(%rdi), %ymm11, %ymm11
    vinsertf128 $1, C_YHI+0xc0(%rdi), %ymm12, %ymm12
    vinsertf128 $1, C_YHI+0xd0(%rdi), %ymm13, %ymm13
    vinsertf128 $1, C_YHI+0xe0(%rdi), %ymm14, %ymm14
    vinsertf128 $1, C_YHI+0xf0(%rdi), %ymm15, %ymm15
1:  /* legacy SSE loads keep the upper halves */
    movdqu C_VEC+0x00(%rdi), %xmm0
    movdqu C_VEC+0x10(%rdi), %xmm1
    movdqu C_VEC+0x20(%rdi), %xmm2
    movdqu C_VEC+0x30(%rdi), %xmm3
    movdqu C_VEC+0x40(%rdi), %xmm4
    movdqu C_VEC+0x50(%rdi), %xmm5
    movdqu C_VEC+0x60(%rdi), %xmm6
    movdqu C_VEC+0x70(%rdi), %xmm7
    movq  C_FPKEEP+0x00(%rdi), %xmm8
    movq  C_FPKEEP+0x08(%rdi), %xmm9
    movq  C_FPKEEP+0x10(%rdi), %xmm10
    movq  C_FPKEEP+0x18(%rdi), %xmm11
    movq  C_FPKEEP+0x20(%rdi), %xmm12
    movq  C_FPKEEP+0x28(%rdi), %xmm13
    movq  C_FPKEEP+0x30(%rdi), %xmm14
    movq  C_FPKEEP+0x38(%rdi), %xmm15
    movq  C_KEEP+0x00(%rdi), %rbx
    movq  C_KEEP+0x08(%rdi), %r13
    movq  C_KEEP+0x10(%rdi), %r12
    movq  C_KEEP+0x18(%rdi), %r14
    movq  C_KEEP+0x20(%rdi), %r15
    movq  C_KEEP+0x50(%rdi), %rbp
    movq  C_GP+0x08(%rdi), %rsi
    movq  C_GP+0x10(%rdi), %rdx
    movq  C_GP+0x18(%rdi), %rcx
    movq  C_GP+0x20(%rdi), %r8
    movq  C_GP+0x28(%rdi), %r9
    movabsq $0x5a5a10105a5a1010, %r10
    movabsq $0x5a5a11115a5a1111, %r11
    movl  $8, %eax
    movq  C_GP+0x00(%rdi), %rdi
    cld
    callq *40(%rsp)
    movq  32(%rsp), %r11
    movq  %rax, C_RET(%r11)
    movq  %rbx, C_KEEPA+0x00(%r11)
    movq  %r13, C_KEEPA+0x08(%r11)
    movq  %r12, C_KEEPA+0x10(%r11)
    movq  %r14, C_KEEPA+0x18(%r11)
    movq  %r15, C_KEEPA+0x20(%r11)
    movq  %rbp, C_KEEPA+0x50(%r11)
    movq  %rsp, C_SPA(%r11)
    stmxcsr C_FPCTLA(%r11)
    fnstcw C_X87A(%r11)
    pushfq
    popq  %rax
    movl  %eax, C_FLAGSA(%r11)
    cmpl  $0, C_AVX(%r11)
    je    2f
    vzeroupper
2:  addq  $56, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
    .cfi_endproc

#define F_VIN     0x000
#define F_VOUT    0x200
#define F_YHIN    0x400
#define F_YHOUT   0x500
#define F_GIN     0x600
#define F_GOUT    0x700
#define F_ADDR    0x800
#define F_SPIN    0x808
#define F_SPOUT   0x810
#define F_FLIN    0x818
#define F_FLOUT   0x820
#define F_FPCTLIN 0x828
#define F_X87IN   0x830
#define F_AVX     0x834
#define F_FPCTLOUT 0x838
#define F_X87OUT  0x840

#define VLD(n) movdqu F_VIN+16*n(%rdi), %xmm##n
#define YLD(n) vmovdqu F_VIN+16*n(%rdi), %xmm##n; vinsertf128 $1, F_YHIN+16*n(%rdi), %ymm##n, %ymm##n
#define VST(n) movdqu %xmm##n, F_VOUT+16*n(%rax)
#define YST(n) vextractf128 $1, %ymm##n, F_YHOUT+16*n(%rax)

    .globl _uh_fault_full
    .p2align 4
_uh_fault_full:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    pushq %rbx
    .cfi_def_cfa_offset 24
    pushq %r12
    .cfi_def_cfa_offset 32
    pushq %r13
    .cfi_def_cfa_offset 40
    pushq %r14
    .cfi_def_cfa_offset 48
    pushq %r15
    .cfi_def_cfa_offset 56
    subq  $8, %rsp
    .cfi_def_cfa_offset 64
    .cfi_offset %r15, -56
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    .cfi_offset %rbp, -16
    movq  %rdi, (%rsp)
    movq  %rsp, F_SPIN(%rdi)
    ldmxcsr F_FPCTLIN(%rdi)
    fldcw F_X87IN(%rdi)
    cmpl  $0, F_AVX(%rdi)
    jne   1f
    VLD(0); VLD(1); VLD(2); VLD(3); VLD(4); VLD(5); VLD(6); VLD(7)
    VLD(8); VLD(9); VLD(10); VLD(11); VLD(12); VLD(13); VLD(14); VLD(15)
    jmp   2f
1:  YLD(0); YLD(1); YLD(2); YLD(3); YLD(4); YLD(5); YLD(6); YLD(7)
    YLD(8); YLD(9); YLD(10); YLD(11); YLD(12); YLD(13); YLD(14); YLD(15)
2:  pushq F_FLIN(%rdi)
    popfq                       /* nothing below changes RFLAGS until pushfq */
    movq  F_GIN+0x00(%rdi), %rax
    movq  F_GIN+0x08(%rdi), %rcx
    movq  F_GIN+0x10(%rdi), %rdx
    movq  F_GIN+0x18(%rdi), %rbx
    movq  F_GIN+0x28(%rdi), %rbp
    movq  F_GIN+0x30(%rdi), %rsi
    movq  F_GIN+0x40(%rdi), %r8
    movq  F_GIN+0x48(%rdi), %r9
    movq  F_GIN+0x50(%rdi), %r10
    movq  F_GIN+0x58(%rdi), %r11
    movq  F_GIN+0x60(%rdi), %r12
    movq  F_GIN+0x68(%rdi), %r13
    movq  F_GIN+0x70(%rdi), %r14
    movq  F_GIN+0x78(%rdi), %r15
    movq  F_ADDR(%rdi), %rdi
    movq  (%rdi), %rdi          /* faults; the handler makes the page readable */
    pushfq
    pushq %rax
    movq  16(%rsp), %rax        /* f */
    movq  %rcx, F_GOUT+0x08(%rax)
    movq  %rdx, F_GOUT+0x10(%rax)
    movq  %rbx, F_GOUT+0x18(%rax)
    movq  %rbp, F_GOUT+0x28(%rax)
    movq  %rsi, F_GOUT+0x30(%rax)
    movq  %rdi, F_GOUT+0x38(%rax)
    movq  %r8,  F_GOUT+0x40(%rax)
    movq  %r9,  F_GOUT+0x48(%rax)
    movq  %r10, F_GOUT+0x50(%rax)
    movq  %r11, F_GOUT+0x58(%rax)
    movq  %r12, F_GOUT+0x60(%rax)
    movq  %r13, F_GOUT+0x68(%rax)
    movq  %r14, F_GOUT+0x70(%rax)
    movq  %r15, F_GOUT+0x78(%rax)
    popq  F_GOUT+0x00(%rax)
    popq  F_FLOUT(%rax)
    cld
    movq  %rsp, F_SPOUT(%rax)
    stmxcsr F_FPCTLOUT(%rax)
    fnstcw F_X87OUT(%rax)
    VST(0); VST(1); VST(2); VST(3); VST(4); VST(5); VST(6); VST(7)
    VST(8); VST(9); VST(10); VST(11); VST(12); VST(13); VST(14); VST(15)
    cmpl  $0, F_AVX(%rax)
    je    3f
    YST(0); YST(1); YST(2); YST(3); YST(4); YST(5); YST(6); YST(7)
    YST(8); YST(9); YST(10); YST(11); YST(12); YST(13); YST(14); YST(15)
    vzeroupper
3:  addq  $8, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
    .cfi_endproc

#elif defined(__arm64__) || defined(__aarch64__)
/* scalar fields of uh_ctx relative to S = c + 0x180 */
#define S_FN      0x000
#define S_GP      0x008
#define S_KEEP    0x058
#define S_FPKEEP  0x0b0
#define S_FPCTL   0x0f0
#define S_FPSTAT  0x0f4
#define S_RET     0x100
#define S_KEEPA   0x108
#define S_FPKEEPA 0x160
#define S_SPB     0x1a0
#define S_SPA     0x1a8
#define S_FPCTLA  0x1b0
#define S_FPSTATA 0x1b4

    .text
    .globl _uh_call_full
    .p2align 2
_uh_call_full:
    .cfi_startproc
    stp   x29, x30, [sp, #-16]!
    stp   x27, x28, [sp, #-16]!
    stp   x25, x26, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   d14, d15, [sp, #-16]!
    stp   d12, d13, [sp, #-16]!
    stp   d10, d11, [sp, #-16]!
    stp   d8, d9, [sp, #-16]!
    sub   sp, sp, #48
    .cfi_def_cfa_offset 208
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    .cfi_offset w28, -24
    .cfi_offset w27, -32
    .cfi_offset w26, -40
    .cfi_offset w25, -48
    .cfi_offset w24, -56
    .cfi_offset w23, -64
    .cfi_offset w22, -72
    .cfi_offset w21, -80
    .cfi_offset w20, -88
    .cfi_offset w19, -96
    .cfi_offset b15, -104
    .cfi_offset b14, -112
    .cfi_offset b13, -120
    .cfi_offset b12, -128
    .cfi_offset b11, -136
    .cfi_offset b10, -144
    .cfi_offset b9, -152
    .cfi_offset b8, -160
    /* [sp+0,8] stack arguments a8 a9, [sp+16] S, [sp+24] fn */
    add   x10, x0, #0x180
    str   x10, [sp, #16]
    ldr   x9, [x10, #S_FN]
    str   x9, [sp, #24]
    ldr   x9, [x10, #S_GP+64]
    str   x9, [sp]
    ldr   x9, [x10, #S_GP+72]
    str   x9, [sp, #8]
    mov   x9, sp
    str   x9, [x10, #S_SPB]
    ldr   w9, [x10, #S_FPCTL]
    msr   fpcr, x9
    ldr   w9, [x10, #S_FPSTAT]
    msr   fpsr, x9
    ldp   q0, q1, [x0, #0x00]
    ldp   q2, q3, [x0, #0x20]
    ldp   q4, q5, [x0, #0x40]
    ldp   q6, q7, [x0, #0x60]
    ldp   q16, q17, [x0, #0x80]
    ldp   q18, q19, [x0, #0xa0]
    ldp   q20, q21, [x0, #0xc0]
    ldp   q22, q23, [x0, #0xe0]
    ldp   d8, d9, [x10, #S_FPKEEP+0x00]
    ldp   d10, d11, [x10, #S_FPKEEP+0x10]
    ldp   d12, d13, [x10, #S_FPKEEP+0x20]
    ldp   d14, d15, [x10, #S_FPKEEP+0x30]
    ldp   x19, x20, [x10, #S_KEEP+0x00]
    ldp   x21, x22, [x10, #S_KEEP+0x10]
    ldp   x23, x24, [x10, #S_KEEP+0x20]
    ldp   x25, x26, [x10, #S_KEEP+0x30]
    ldp   x27, x28, [x10, #S_KEEP+0x40]
    ldr   x29, [x10, #S_KEEP+0x50]
    ldp   x1, x2, [x10, #S_GP+0x08]
    ldp   x3, x4, [x10, #S_GP+0x18]
    ldp   x5, x6, [x10, #S_GP+0x28]
    ldr   x7, [x10, #S_GP+0x38]
    ldr   x0, [x10, #S_GP+0x00]
    ldr   x9, [sp, #24]
    mov   x10, #0x1010
    movk  x10, #0x5a5a, lsl #16
    mov   x11, #0x1111
    movk  x11, #0x5a5a, lsl #16
    blr   x9
    ldr   x9, [sp, #16]
    str   x0, [x9, #S_RET]
    stp   x19, x20, [x9, #S_KEEPA+0x00]
    stp   x21, x22, [x9, #S_KEEPA+0x10]
    stp   x23, x24, [x9, #S_KEEPA+0x20]
    stp   x25, x26, [x9, #S_KEEPA+0x30]
    stp   x27, x28, [x9, #S_KEEPA+0x40]
    str   x29, [x9, #S_KEEPA+0x50]
    stp   d8, d9, [x9, #S_FPKEEPA+0x00]
    stp   d10, d11, [x9, #S_FPKEEPA+0x10]
    stp   d12, d13, [x9, #S_FPKEEPA+0x20]
    stp   d14, d15, [x9, #S_FPKEEPA+0x30]
    mov   x10, sp
    str   x10, [x9, #S_SPA]
    mrs   x10, fpcr
    str   w10, [x9, #S_FPCTLA]
    mrs   x10, fpsr
    str   w10, [x9, #S_FPSTATA]
    add   sp, sp, #48
    ldp   d8, d9, [sp], #16
    ldp   d10, d11, [sp], #16
    ldp   d12, d13, [sp], #16
    ldp   d14, d15, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x23, x24, [sp], #16
    ldp   x25, x26, [sp], #16
    ldp   x27, x28, [sp], #16
    ldp   x29, x30, [sp], #16
    ret
    .cfi_endproc

/* uh_full scalars relative to T = f + 0x800 */
#define T_ADDR    0x00
#define T_SPIN    0x08
#define T_SPOUT   0x10
#define T_FLIN    0x18
#define T_FLOUT   0x20
#define T_FPCTLIN 0x28
#define T_FPSTATIN 0x2c
#define T_FPCTLOUT 0x38
#define T_FPSTATOUT 0x3c

    .globl _uh_fault_full
    .p2align 2
_uh_fault_full:
    .cfi_startproc
    stp   x29, x30, [sp, #-16]!
    stp   x27, x28, [sp, #-16]!
    stp   x25, x26, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   d14, d15, [sp, #-16]!
    stp   d12, d13, [sp, #-16]!
    stp   d10, d11, [sp, #-16]!
    stp   d8, d9, [sp, #-16]!
    sub   sp, sp, #16
    .cfi_def_cfa_offset 176
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    str   x0, [sp]
    add   x16, x0, #0x800
    mov   x9, sp
    str   x9, [x16, #T_SPIN]
    ldr   w9, [x16, #T_FPCTLIN]
    msr   fpcr, x9
    ldr   w9, [x16, #T_FPSTATIN]
    msr   fpsr, x9
    ldr   x9, [x16, #T_FLIN]
    msr   nzcv, x9
    ldp   q0, q1, [x0, #0x000]
    ldp   q2, q3, [x0, #0x020]
    ldp   q4, q5, [x0, #0x040]
    ldp   q6, q7, [x0, #0x060]
    ldp   q8, q9, [x0, #0x080]
    ldp   q10, q11, [x0, #0x0a0]
    ldp   q12, q13, [x0, #0x0c0]
    ldp   q14, q15, [x0, #0x0e0]
    ldp   q16, q17, [x0, #0x100]
    ldp   q18, q19, [x0, #0x120]
    ldp   q20, q21, [x0, #0x140]
    ldp   q22, q23, [x0, #0x160]
    ldp   q24, q25, [x0, #0x180]
    ldp   q26, q27, [x0, #0x1a0]
    ldp   q28, q29, [x0, #0x1c0]
    ldp   q30, q31, [x0, #0x1e0]
    add   x17, x0, #0x600
    ldr   x9, [x16, #T_ADDR]
    ldp   x0, x1, [x17, #0x00]
    ldp   x2, x3, [x17, #0x10]
    ldp   x4, x5, [x17, #0x20]
    ldp   x6, x7, [x17, #0x30]
    ldr   x8, [x17, #0x40]
    ldp   x10, x11, [x17, #0x50]
    ldp   x12, x13, [x17, #0x60]
    ldp   x14, x15, [x17, #0x70]
    ldp   x19, x20, [x17, #0x98]
    ldp   x21, x22, [x17, #0xa8]
    ldp   x23, x24, [x17, #0xb8]
    ldp   x25, x26, [x17, #0xc8]
    ldp   x27, x28, [x17, #0xd8]
    ldp   x29, x30, [x17, #0xe8]
    ldr   x16, [x17, #0x80]
    ldr   x17, [x17, #0x88]
    ldr   x9, [x9]              /* faults; the handler makes the page readable */
    stp   x0, x1, [sp, #-16]!
    mrs   x0, nzcv
    ldr   x1, [sp, #16]         /* f */
    add   x1, x1, #0x700
    stp   x2, x3, [x1, #0x10]
    stp   x4, x5, [x1, #0x20]
    stp   x6, x7, [x1, #0x30]
    stp   x8, x9, [x1, #0x40]
    stp   x10, x11, [x1, #0x50]
    stp   x12, x13, [x1, #0x60]
    stp   x14, x15, [x1, #0x70]
    stp   x16, x17, [x1, #0x80]
    stp   x19, x20, [x1, #0x98]
    stp   x21, x22, [x1, #0xa8]
    stp   x23, x24, [x1, #0xb8]
    stp   x25, x26, [x1, #0xc8]
    stp   x27, x28, [x1, #0xd8]
    stp   x29, x30, [x1, #0xe8]
    add   x2, x1, #0x100        /* T */
    str   x0, [x2, #T_FLOUT]
    mrs   x0, fpcr
    str   w0, [x2, #T_FPCTLOUT]
    mrs   x0, fpsr
    str   w0, [x2, #T_FPSTATOUT]
    ldp   x3, x4, [sp], #16
    stp   x3, x4, [x1, #0x00]
    mov   x0, sp
    str   x0, [x2, #T_SPOUT]
    ldr   x0, [sp]
    stp   q0, q1, [x0, #0x200]
    stp   q2, q3, [x0, #0x220]
    stp   q4, q5, [x0, #0x240]
    stp   q6, q7, [x0, #0x260]
    stp   q8, q9, [x0, #0x280]
    stp   q10, q11, [x0, #0x2a0]
    stp   q12, q13, [x0, #0x2c0]
    stp   q14, q15, [x0, #0x2e0]
    stp   q16, q17, [x0, #0x300]
    stp   q18, q19, [x0, #0x320]
    stp   q20, q21, [x0, #0x340]
    stp   q22, q23, [x0, #0x360]
    stp   q24, q25, [x0, #0x380]
    stp   q26, q27, [x0, #0x3a0]
    stp   q28, q29, [x0, #0x3c0]
    stp   q30, q31, [x0, #0x3e0]
    add   sp, sp, #16
    ldp   d8, d9, [sp], #16
    ldp   d10, d11, [sp], #16
    ldp   d12, d13, [sp], #16
    ldp   d14, d15, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x23, x24, [sp], #16
    ldp   x25, x26, [sp], #16
    ldp   x27, x28, [sp], #16
    ldp   x29, x30, [sp], #16
    ret
    .cfi_endproc
#endif
