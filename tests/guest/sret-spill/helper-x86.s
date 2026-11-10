.text
.globl _spill_bounds
.p2align 4
_spill_bounds:
    pushq %rbp
    movq %rsp, %rbp
    subq $0x10, %rsp
    movq %rdi, -0x8(%rbp)
    movq %rsi, %rdi
    movq -0x8(%rbp), %rax
    movupd (%rdi), %xmm0
    movupd 0x10(%rdi), %xmm1
    movupd %xmm0, (%rax)
    movupd %xmm1, 0x10(%rax)
    leave
    retq
