/*
 * hao — guest(arm64) <-> native(x86_64 SysV) call bridges.
 *
 * hao_native_call(fn, cpu, nstack): call native `fn` with the guest's
 * argument registers mapped to SysV:
 *   x0-x5 -> rdi rsi rdx rcx r8 r9,  x6 x7 -> first two stack slots,
 *   guest stack slots [sp, sp+8*nstack) -> following stack slots,
 *   v0-v7 -> xmm0-xmm7, al = 8.
 * Results: rax -> x0, rdx -> x1, xmm0 -> v0, xmm1 -> v1.
 *
 * hao_callback_entry: the fault handler redirects a native call into
 * guest code here with r11 = guest target. Builds a hao_cb_frame and calls
 * hao_callback(frame) in C, then returns the guest's results natively,
 * popping the bytes hao_callback returns beyond the return address (the
 * callee pop of x86_64 swifttailcc; see hao_callback in runtime.c).
 *
 * Every bridge carries call-frame information, so the native unwinder can
 * step through it: exceptions cross the boundary in both directions
 * (src/unwind.c). The .cfi_offset lines are in clang's order, lowest slot
 * first: the assembler derives the compact unwind encoding from that order,
 * not from the offsets.
 */
#define CPU_X(n)   (8*(n))
#define CPU_V(n)   (0x200 + 16*(n))
#define CPU_BRIDGE_RSP 0x1D8
#define CPU_BRIDGE_RBP 0x1E0
#define CPU_BRIDGE_FN  0x1E8
#define CPU_NATIVE_POP 0x1F0
#define HAO_RETURN_MAGIC 0x00007ffffffff000   /* include/hao/runtime.h */

    .text
    .globl _hao_native_call
    .p2align 4
_hao_native_call:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    .cfi_offset %r15, -56
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    movq  %rsi, %rbx            /* cpu */
    movq  %rdi, %r15            /* fn  */
    movq  %rdx, %r10            /* nstack */
    leaq  2(%r10), %rax
    shlq  $3, %rax
    addq  $15, %rax
    andq  $-16, %rax
    subq  %rax, %rsp
    andq  $-16, %rsp
    movq  CPU_X(6)(%rbx), %rax
    movq  %rax, 0(%rsp)
    movq  CPU_X(7)(%rbx), %rax
    movq  %rax, 8(%rsp)
    movq  CPU_X(31)(%rbx), %r11 /* guest sp */
    cmpq  $16, %r10             /* the usual count: 128 bytes in eight vector moves (xmm0-7 are loaded below) */
    jne   5f
    movups   0(%r11), %xmm0
    movups  16(%r11), %xmm1
    movups  32(%r11), %xmm2
    movups  48(%r11), %xmm3
    movups  64(%r11), %xmm4
    movups  80(%r11), %xmm5
    movups  96(%r11), %xmm6
    movups 112(%r11), %xmm7
    movaps %xmm0,  16(%rsp)
    movaps %xmm1,  32(%rsp)
    movaps %xmm2,  48(%rsp)
    movaps %xmm3,  64(%rsp)
    movaps %xmm4,  80(%rsp)
    movaps %xmm5,  96(%rsp)
    movaps %xmm6, 112(%rsp)
    movaps %xmm7, 128(%rsp)
    jmp   2f
5:  xorl  %ecx, %ecx
1:  cmpq  %r10, %rcx
    jae   2f
    movq  (%r11,%rcx,8), %rax
    movq  %rax, 16(%rsp,%rcx,8)
    incq  %rcx
    jmp   1b
2:  movdqu CPU_V(0)(%rbx), %xmm0
    movdqu CPU_V(1)(%rbx), %xmm1
    movdqu CPU_V(2)(%rbx), %xmm2
    movdqu CPU_V(3)(%rbx), %xmm3
    movdqu CPU_V(4)(%rbx), %xmm4
    movdqu CPU_V(5)(%rbx), %xmm5
    movdqu CPU_V(6)(%rbx), %xmm6
    movdqu CPU_V(7)(%rbx), %xmm7
    /* swiftcc: self x20->r13, error x21->r12, async context x22->r14 */
    movq  CPU_X(20)(%rbx), %r13
    movq  CPU_X(21)(%rbx), %r12
    movq  CPU_X(22)(%rbx), %r14
    movq  CPU_X(0)(%rbx), %rdi
    movq  CPU_X(1)(%rbx), %rsi
    movq  CPU_X(2)(%rbx), %rdx
    movq  CPU_X(3)(%rbx), %rcx
    movq  CPU_X(4)(%rbx), %r8
    movq  CPU_X(5)(%rbx), %r9
    /* indirect result: arm64 x8 <-> x86_64 swiftcc rax. (C variadics are
     * bridged through va_list interposers, so al is not needed here.) */
    movq  CPU_X(8)(%rbx), %rax
    movq  %rsp, CPU_BRIDGE_RSP(%rbx)
    movq  %rbp, CPU_BRIDGE_RBP(%rbx)
    movq  %r15, CPU_BRIDGE_FN(%rbx)
    callq *%r15
Lnative_call_ret:               /* return address; see callee_pop in runtime.c */
    /* x86_64 swifttailcc callees pop their argument area (`retq $8`; Swift
     * concurrency). rsp is restored from rbp below, so the pop is absorbed
     * here; record it when this was the guest's tail call (guest lr = the
     * callback sentinel): the guest function is then swifttailcc too, and
     * hao_callback_entry pops for its own native caller (amount: see
     * hao_callback in runtime.c). Any other native call resets it to 0. */
    movq  %rsp, %r10
    subq  CPU_BRIDGE_RSP(%rbx), %r10
    movabsq $HAO_RETURN_MAGIC, %r11
    cmpq  %r11, CPU_X(30)(%rbx)
    movl  $0, %r11d
    cmovneq %r11, %r10
    movq  %r10, CPU_NATIVE_POP(%rbx)
    cmpq  %rsp, CPU_BRIDGE_RSP(%rbx)
    jne   3f
    cmpq  %rbp, CPU_BRIDGE_RBP(%rbx)
    jne   3f
4:
    /* results: up to four GPRs and four vector registers (swiftcc) */
    movq  %rax, CPU_X(0)(%rbx)
    movq  %rdx, CPU_X(1)(%rbx)
    movq  %rcx, CPU_X(2)(%rbx)
    movq  %r8,  CPU_X(3)(%rbx)
    movq  %r12, CPU_X(21)(%rbx)
    movdqu %xmm0, CPU_V(0)(%rbx)
    movdqu %xmm1, CPU_V(1)(%rbx)
    movdqu %xmm2, CPU_V(2)(%rbx)
    movdqu %xmm3, CPU_V(3)(%rbx)
    leaq  -40(%rbp), %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
3:  /* native callee returned with rsp/rbp different from the call site
     * (debug report). rbp is callee-saved and stays as returned; the
     * epilogue at 4 recomputes rsp from it. */
    movq  %rbx, %rdi
    movq  %rsp, %rsi
    movq  %rbp, %rdx
    leaq  -48(%rbp), %rsp       /* below the saved registers, 16-aligned */
    subq  $64, %rsp             /* results: xmm0-3 (getenv may use SSE), rax rdx rcx r8 */
    movdqu %xmm0, 0(%rsp)
    movdqu %xmm1, 16(%rsp)
    movdqu %xmm2, 32(%rsp)
    movdqu %xmm3, 48(%rsp)
    pushq %rax
    pushq %rdx
    pushq %rcx
    pushq %r8
    callq _hao_bridge_unbalanced
    popq  %r8
    popq  %rcx
    popq  %rdx
    popq  %rax
    movdqu 0(%rsp), %xmm0
    movdqu 16(%rsp), %xmm1
    movdqu 32(%rsp), %xmm2
    movdqu 48(%rsp), %xmm3
    jmp   4b
    .cfi_endproc

/* hao_native_call's return address (a label inside a CFI region must be
 * assembler-local, so its address is published as data) */
    .section __DATA,__const
    .p2align 3
    .private_extern _hao_native_call_ret
_hao_native_call_ret:
    .quad Lnative_call_ret
    .text

/*
 * hao_cb_frame layout (see runtime.h):
 *   0x00 gpr[6]       rdi rsi rdx rcx r8 r9  (on return: rax rdx rcx r8)
 *   0x30 target
 *   0x38 stack_args   pointer to caller's stack arguments
 *   0x40 xmm[8]       16 bytes each (on return: xmm0-xmm3)
 *   0xC0 swift[4]     r12 (error) r13 (self) r14 (async) rax (indirect result)
 *                     on return [0] -> r12
 *   0xE0 sig          r10 (hsig* from a signature thunk) or 0; 1 on return: load st0 from xmm[0]
 *   0xE8 ret          return address into the native caller
 *   0xF0 rbp          the native caller's rbp
 *   size 0xF8
 */
    .globl _hao_callback_entry
    .p2align 4
_hao_callback_entry:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    .cfi_offset %r15, -56
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    subq  $0xF8, %rsp
    movq  %rax, 0xD8(%rsp)
    movq  %r10, 0xE0(%rsp)
    movq  8(%rbp), %rax
    movq  %rax, 0xE8(%rsp)
    movq  (%rbp), %rax
    movq  %rax, 0xF0(%rsp)
    movq  %rdi, 0x00(%rsp)
    movq  %rsi, 0x08(%rsp)
    movq  %rdx, 0x10(%rsp)
    movq  %rcx, 0x18(%rsp)
    movq  %r8,  0x20(%rsp)
    movq  %r9,  0x28(%rsp)
    movq  %r11, 0x30(%rsp)
    leaq  16(%rbp), %rax
    movq  %rax, 0x38(%rsp)
    movdqu %xmm0, 0x40(%rsp)
    movdqu %xmm1, 0x50(%rsp)
    movdqu %xmm2, 0x60(%rsp)
    movdqu %xmm3, 0x70(%rsp)
    movdqu %xmm4, 0x80(%rsp)
    movdqu %xmm5, 0x90(%rsp)
    movdqu %xmm6, 0xA0(%rsp)
    movdqu %xmm7, 0xB0(%rsp)
    movq  %r12, 0xC0(%rsp)
    movq  %r13, 0xC8(%rsp)
    movq  %r14, 0xD0(%rsp)
    movq  %rsp, %rdi
    callq _hao_callback         /* returns the bytes to pop for the caller */
    movq  %rax, %r11
    movq  0xC0(%rsp), %rax
    movq  %rax, -16(%rbp)       /* returned swifterror becomes the restored r12 */
    movq  0x00(%rsp), %rax
    movq  0x08(%rsp), %rdx
    movq  0x10(%rsp), %rcx
    movq  0x18(%rsp), %r8
    movdqu 0x40(%rsp), %xmm0
    movdqu 0x50(%rsp), %xmm1
    movdqu 0x60(%rsp), %xmm2
    movdqu 0x70(%rsp), %xmm3
    /* sig == 1 on return: the callee returns long double, in xmm[0] as 80-bit; SysV returns it in st0 */
    cmpq  $1, 0xE0(%rsp)
    jne   1f
    fldt  0x40(%rsp)
1:  leaq  -40(%rbp), %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    testq %r11, %r11
    jnz   2f
    retq
2:  /* the guest (swifttailcc) function ended in a tail call to a native
     * swifttailcc function: its native caller expects a callee pop of r11
     * bytes (`callq *56(%r13); subq $8, %rsp`). retq imm16 is not
     * variable: return by hand. */
    movq  (%rsp), %r10
    leaq  8(%rsp,%r11), %rsp
    jmpq  *%r10
    .cfi_endproc

/* Raw Darwin x86_64 syscall with up to 9 arguments (extras on the stack).
 * long hao_raw_syscall(long num, const uint64_t args[9], uint64_t *rdx_out, int *err_out) */
    .globl _hao_raw_syscall
    .p2align 4
_hao_raw_syscall:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    movq  %rdi, %rax
    movq  %rsi, %rbx            /* args */
    movq  %rdx, %r12            /* rdx_out */
    movq  %rcx, %r13            /* err_out */
    subq  $32, %rsp
    andq  $-16, %rsp
    /* kernel reads stack args at user rsp+8 (as if called through a stub) */
    movq  48(%rbx), %r14
    movq  %r14, 8(%rsp)
    movq  56(%rbx), %r14
    movq  %r14, 16(%rsp)
    movq  64(%rbx), %r14
    movq  %r14, 24(%rsp)
    movq  0(%rbx), %rdi
    movq  8(%rbx), %rsi
    movq  16(%rbx), %rdx
    movq  24(%rbx), %r10
    movq  32(%rbx), %r8
    movq  40(%rbx), %r9
    syscall
    setc  %cl
    movzbl %cl, %ecx
    movl  %ecx, (%r13)
    movq  %rdx, (%r12)
    leaq  -32(%rbp), %rsp
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
    .cfi_endproc

/* void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16],
 *                    const uint64_t *stack, uint64_t nstack,
 *                    uint64_t out[6] (rax rdx xmm0lo xmm1lo, then st0 as 80-bit long double))
 * On entry out[4] != 0 asks for the x87 result: a function returning long
 * double leaves it in st0, which is popped into out[4..5]. (Popping when
 * the callee returned nothing there would unbalance the x87 stack.)
 * A plain SysV call with explicit register/stack images (used for
 * signature-driven calls; caller computed the classification). */
    .globl _hao_sysv_call
    .p2align 4
_hao_sysv_call:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    .cfi_offset %r15, -56
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    movq  %rdi, %r15            /* fn */
    movq  %rsi, %r12            /* gpr */
    movq  %rdx, %r13            /* xmm */
    movq  %r9, %rbx             /* out */
    movq  32(%r9), %r14         /* capture st0? */
    leaq  (,%r8,8), %rax
    addq  $15, %rax
    andq  $-16, %rax
    subq  %rax, %rsp
    andq  $-16, %rsp
    xorl  %eax, %eax
1:  cmpq  %r8, %rax
    jae   2f
    movq  (%rcx,%rax,8), %r10
    movq  %r10, (%rsp,%rax,8)
    incq  %rax
    jmp   1b
2:  movdqu 0x00(%r13), %xmm0
    movdqu 0x10(%r13), %xmm1
    movdqu 0x20(%r13), %xmm2
    movdqu 0x30(%r13), %xmm3
    movdqu 0x40(%r13), %xmm4
    movdqu 0x50(%r13), %xmm5
    movdqu 0x60(%r13), %xmm6
    movdqu 0x70(%r13), %xmm7
    movq  0(%r12), %rdi
    movq  8(%r12), %rsi
    movq  16(%r12), %rdx
    movq  24(%r12), %rcx
    movq  32(%r12), %r8
    movq  40(%r12), %r9
    movl  $8, %eax
    callq *%r15
    movq  %rax, 0(%rbx)
    movq  %rdx, 8(%rbx)
    movq  %xmm0, 16(%rbx)
    movq  %xmm1, 24(%rbx)
    testq %r14, %r14
    jz    3f
    fstpt 32(%rbx)
3:  leaq  -40(%rbp), %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
    .cfi_endproc

/*
 * long hao_eh_guard(void (*fn)(void *), hao_seg *seg, long kind)
 * Calls fn(seg) and returns 0. The frame's personality is
 * hao_eh_personality (unwind.c), which reads seg and kind back from rbx and
 * r12 (callee-saved, live across the call). When the personality stops an
 * exception here it installs hao_eh_guard_pad(), and hao_eh_guard returns
 * 1 instead.
 */
    .globl _hao_eh_guard
    .p2align 4
_hao_eh_guard:
    .cfi_startproc
    .cfi_personality 155, _hao_eh_personality
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %rbx
    pushq %r12
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    movq  %rsi, %rbx            /* seg  */
    movq  %rdx, %r12            /* kind */
    movq  %rdi, %rax
    movq  %rsi, %rdi
    callq *%rax
    xorl  %eax, %eax
Lguard_ret:
    leaq  -16(%rbp), %rsp
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
Lguard_pad:                     /* landing pad: registers restored by the unwinder */
    movl  $1, %eax
    jmp   Lguard_ret
    .cfi_endproc

/* uint64_t hao_eh_guard_pad(void): address of the guard's landing pad */
    .globl _hao_eh_guard_pad
    .p2align 4
_hao_eh_guard_pad:
    leaq  Lguard_pad(%rip), %rax
    retq

/*
 * int hao_sig_call_native(fn, sig, info, uctx): call a native SIGSEGV/SIGBUS
 * handler fn(sig, info, uctx) for the fault handler (signals.c) and report
 * which callee-saved registers it returned changed: bit 0 rbx, 1 rbp,
 * 2 r12, 3 r13, 4 r14, 5 r15. All of them are restored. Natively the
 * handler returns into _sigtramp, which keeps the ucontext in rbx and the
 * sigreturn token in r12: a handler that changes either fails __sigreturn.
 */
    .globl _hao_sig_call_native
    .p2align 4
_hao_sig_call_native:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    .cfi_offset %r15, -56
    .cfi_offset %r14, -48
    .cfi_offset %r13, -40
    .cfi_offset %r12, -32
    .cfi_offset %rbx, -24
    subq  $8, %rsp              /* rsp: rbp - 48, 16-byte aligned */
    movq  %rdi, %rax
    movl  %esi, %edi
    movq  %rdx, %rsi
    movq  %rcx, %rdx
    call  *%rax
    /* the saved copies, relative to rsp (rbp may be the handler's) */
    xorl  %eax, %eax
    cmpq  40(%rsp), %rbx
    je    1f
    orl   $1, %eax
1:  leaq  48(%rsp), %rcx
    cmpq  %rcx, %rbp
    je    2f
    orl   $2, %eax
2:  cmpq  32(%rsp), %r12
    je    3f
    orl   $4, %eax
3:  cmpq  24(%rsp), %r13
    je    4f
    orl   $8, %eax
4:  cmpq  16(%rsp), %r14
    je    5f
    orl   $16, %eax
5:  cmpq  8(%rsp), %r15
    je    6f
    orl   $32, %eax
6:  leaq  48(%rsp), %rbp
    addq  $8, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret
    .cfi_endproc

/*
 * void hao_call_on_stack(void (*fn)(void *), void *arg, void *top): fn(arg)
 * with rsp at top (aligned down to 16), then back on the caller's stack.
 * runtime.c runs a fatal fault's diagnostics on a stack of their own there.
 */
    .globl _hao_call_on_stack
    .private_extern _hao_call_on_stack
    .p2align 4
_hao_call_on_stack:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq  %rsp, %rbp
    .cfi_def_cfa_register %rbp
    andq  $-16, %rdx
    movq  %rdx, %rsp
    movq  %rdi, %rax
    movq  %rsi, %rdi
    callq *%rax
    movq  %rbp, %rsp
    popq  %rbp
    .cfi_def_cfa %rsp, 8
    retq
    .cfi_endproc

/*
 * hao_fault_entry / hao_abort_entry(sig, info, uctx): what the kernel has
 * installed for on_fault (SIGSEGV/SIGBUS) and on_abort (SIGILL/SIGTRAP/
 * SIGABRT under the diagnostics log). _sigtramp calls a handler with the
 * sigreturn token of this delivery in r12 (xnu passes it in r9; _sigtramp
 * keeps it there for __sigreturn), so it is read here, before any compiled
 * code can move it: hao_on_fault/hao_on_abort(sig, info, uctx, token).
 */
    .globl _hao_fault_entry
    .p2align 4
_hao_fault_entry:
    .cfi_startproc
    movq  %r12, %rcx
    jmp   _hao_on_fault
    .cfi_endproc

    .globl _hao_abort_entry
    .p2align 4
_hao_abort_entry:
    .cfi_startproc
    movq  %r12, %rcx
    jmp   _hao_on_abort
    .cfi_endproc

/*
 * void hao_sig_resume_ctx(const hao_resume_blk *b) (noreturn; signals.c
 * hao_sig_resume): leave a signal handler without __sigreturn, resuming the
 * thread in the state b holds. The caller has restored the signal mask and
 * stored the target rip at b->rsp_slot = target rsp - 136, below the
 * interrupted code's 128-byte red zone. Layout (signals.c hao_resume_blk):
 *   0x000 fx[512]     FXSAVE image (x87, MXCSR, xmm0-15), 64-byte aligned
 *   0x200 ymmh[16]    upper halves of ymm0-15
 *   0x300 vec         0 xmm only; 1 also ymmh (AVX mcontext); 2 also k, zmmh, zmm16-31 (AVX-512)
 *   0x310 gpr[15]     r15 r14 r13 r12 r11 r10 r9 r8 rbp rsi rdi rdx rcx rbx rax
 *   0x388 rflags
 *   0x390 rsp_slot
 *   0x3C0 k[8]        opmask registers (kmovq: AVX512BW, checked by the caller)
 *   0x400 zmmh[16]    upper 256 bits of zmm0-15
 *   0x600 zmm[16]     zmm16-31
 * Order: FXRSTOR leaves bits 128 and up alone; the VEX vinsertf128 zeroes
 * bits 256 and up, so the EVEX inserts of the zmm upper halves come after it.
 * rsp moves onto the block before the pops: everything still read is at or
 * above rsp, so a signal taken meanwhile (its frame goes below rsp) cannot
 * overwrite it. `popq %rsp` sets rsp to the slot, and `retq $128` takes the
 * target rip from it and leaves rsp at the target: no register is needed
 * for the jump and the red zone above the slot is never written.
 */
    .globl _hao_sig_resume_ctx
    .private_extern _hao_sig_resume_ctx
    .p2align 4
_hao_sig_resume_ctx:
    fxrstor64 (%rdi)
    cmpq  $0, 0x300(%rdi)
    je    1f
    vinsertf128 $1, 0x200(%rdi), %ymm0, %ymm0
    vinsertf128 $1, 0x210(%rdi), %ymm1, %ymm1
    vinsertf128 $1, 0x220(%rdi), %ymm2, %ymm2
    vinsertf128 $1, 0x230(%rdi), %ymm3, %ymm3
    vinsertf128 $1, 0x240(%rdi), %ymm4, %ymm4
    vinsertf128 $1, 0x250(%rdi), %ymm5, %ymm5
    vinsertf128 $1, 0x260(%rdi), %ymm6, %ymm6
    vinsertf128 $1, 0x270(%rdi), %ymm7, %ymm7
    vinsertf128 $1, 0x280(%rdi), %ymm8, %ymm8
    vinsertf128 $1, 0x290(%rdi), %ymm9, %ymm9
    vinsertf128 $1, 0x2A0(%rdi), %ymm10, %ymm10
    vinsertf128 $1, 0x2B0(%rdi), %ymm11, %ymm11
    vinsertf128 $1, 0x2C0(%rdi), %ymm12, %ymm12
    vinsertf128 $1, 0x2D0(%rdi), %ymm13, %ymm13
    vinsertf128 $1, 0x2E0(%rdi), %ymm14, %ymm14
    vinsertf128 $1, 0x2F0(%rdi), %ymm15, %ymm15
    cmpq  $2, 0x300(%rdi)
    jne   1f
    kmovq 0x3C0(%rdi), %k0
    kmovq 0x3C8(%rdi), %k1
    kmovq 0x3D0(%rdi), %k2
    kmovq 0x3D8(%rdi), %k3
    kmovq 0x3E0(%rdi), %k4
    kmovq 0x3E8(%rdi), %k5
    kmovq 0x3F0(%rdi), %k6
    kmovq 0x3F8(%rdi), %k7
    vinserti64x4 $1, 0x400(%rdi), %zmm0, %zmm0
    vinserti64x4 $1, 0x420(%rdi), %zmm1, %zmm1
    vinserti64x4 $1, 0x440(%rdi), %zmm2, %zmm2
    vinserti64x4 $1, 0x460(%rdi), %zmm3, %zmm3
    vinserti64x4 $1, 0x480(%rdi), %zmm4, %zmm4
    vinserti64x4 $1, 0x4A0(%rdi), %zmm5, %zmm5
    vinserti64x4 $1, 0x4C0(%rdi), %zmm6, %zmm6
    vinserti64x4 $1, 0x4E0(%rdi), %zmm7, %zmm7
    vinserti64x4 $1, 0x500(%rdi), %zmm8, %zmm8
    vinserti64x4 $1, 0x520(%rdi), %zmm9, %zmm9
    vinserti64x4 $1, 0x540(%rdi), %zmm10, %zmm10
    vinserti64x4 $1, 0x560(%rdi), %zmm11, %zmm11
    vinserti64x4 $1, 0x580(%rdi), %zmm12, %zmm12
    vinserti64x4 $1, 0x5A0(%rdi), %zmm13, %zmm13
    vinserti64x4 $1, 0x5C0(%rdi), %zmm14, %zmm14
    vinserti64x4 $1, 0x5E0(%rdi), %zmm15, %zmm15
    vmovdqu64 0x600(%rdi), %zmm16
    vmovdqu64 0x640(%rdi), %zmm17
    vmovdqu64 0x680(%rdi), %zmm18
    vmovdqu64 0x6C0(%rdi), %zmm19
    vmovdqu64 0x700(%rdi), %zmm20
    vmovdqu64 0x740(%rdi), %zmm21
    vmovdqu64 0x780(%rdi), %zmm22
    vmovdqu64 0x7C0(%rdi), %zmm23
    vmovdqu64 0x800(%rdi), %zmm24
    vmovdqu64 0x840(%rdi), %zmm25
    vmovdqu64 0x880(%rdi), %zmm26
    vmovdqu64 0x8C0(%rdi), %zmm27
    vmovdqu64 0x900(%rdi), %zmm28
    vmovdqu64 0x940(%rdi), %zmm29
    vmovdqu64 0x980(%rdi), %zmm30
    vmovdqu64 0x9C0(%rdi), %zmm31
1:  leaq  0x310(%rdi), %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rbp
    popq  %rsi
    popq  %rdi
    popq  %rdx
    popq  %rcx
    popq  %rbx
    popq  %rax
    popfq
    popq  %rsp
    retq  $128

/*
 * bool hao_probe_read8(uint64_t a, uint64_t *out): the 8 bytes at a into
 * *out (true), or false when reading a faults. The load at
 * _hao_probe_read8_load is the only instruction here that can fault: on_fault
 * (runtime.c) recognises a SIGSEGV/SIGBUS at exactly that rip and resumes at
 * _hao_probe_read8_fail with nothing else changed (src/blocks.c, the guest
 * heap block check among native call arguments). Callers make sure the
 * thread does not block SIGSEGV/SIGBUS (a blocked synchronous fault is
 * re-executed forever).
 */
    .globl _hao_probe_read8
    .private_extern _hao_probe_read8
    .globl _hao_probe_read8_load
    .private_extern _hao_probe_read8_load
    .globl _hao_probe_read8_fail
    .private_extern _hao_probe_read8_fail
    .p2align 4
_hao_probe_read8:
_hao_probe_read8_load:
    movq  (%rdi), %rax
    movq  %rax, (%rsi)
    movl  $1, %eax
    retq
_hao_probe_read8_fail:
    xorl  %eax, %eax
    retq
