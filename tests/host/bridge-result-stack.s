/* A direct-return call from the Swift test image that leaves a stack address
 * in rax incidentally: rax is set, then used, then the call follows (no
 * indirect result). hao must not hand rax to the callee as x8. */
    .text
    .globl _bridge_stack_rax
    .p2align 4
_bridge_stack_rax:              /* uint64_t bridge_stack_rax(const void *entry) */
    pushq %rbp
    movq  %rsp, %rbp
    subq  $32, %rsp
    leaq  16(%rsp), %rax
    movq  %rax, (%rsp)
    callq *%rdi
    leave
    retq

/* Sol's second control: the stack address is assigned to rax right before a
 * direct-return call. An instruction pattern cannot tell this from an
 * indirect result; only the callee's own use of x8 can. */
    .globl _bridge_stack_rax_adjacent
    .p2align 4
_bridge_stack_rax_adjacent:     /* uint64_t bridge_stack_rax_adjacent(const void *entry) */
    pushq %rbp
    movq  %rsp, %rbp
    subq  $32, %rsp
    leaq  16(%rsp), %rax
    callq *%rdi
    leave
    retq
