.text
#if defined(__x86_64__)
#define BEGIN(name) .globl _ro_##name; .p2align 4; _ro_##name:; movabsq $0x5a5a5a5a5a5a5a5a, %rax
BEGIN(bool)
    testl %edi, %edi
    setne %al
    retq
BEGIN(char)
    movb %dil, %al
    retq
BEGIN(uchar)
    movb %dil, %al
    retq
BEGIN(short)
    movw %di, %ax
    retq
BEGIN(ushort)
    movw %di, %ax
    retq
.globl _ro_callback
.p2align 4
_ro_callback:
    subq $8, %rsp
    movq %rdi, %rax
    movl %esi, %edi
    callq *%rax
    addq $8, %rsp
    testl %eax, %eax
    setg %al
    movb $0x5a, %ah
    retq
// A wrapper whose result is _ro_bool's, unchanged (libMobileGestalt's MobileGestalt_get_X ->
// MGGetBoolAnswer): typed by the callee's record, not by its own result code.
.globl _ro_wrap
.p2align 4
_ro_wrap:
    pushq %rbp
    movq %rsp, %rbp
    popq %rbp
    jmp _ro_bool
#else
.globl _ro_wrap
.p2align 2
_ro_wrap:
    b _ro_bool
.globl _ro_bool
.p2align 2
_ro_bool:
    cmp w0, #0
    cset w0, ne
    ret
.globl _ro_char
.p2align 2
_ro_char:
    sxtb w0, w0
    ret
.globl _ro_uchar
.p2align 2
_ro_uchar:
    uxtb w0, w0
    ret
.globl _ro_short
.p2align 2
_ro_short:
    sxth w0, w0
    ret
.globl _ro_ushort
.p2align 2
_ro_ushort:
    uxth w0, w0
    ret
.globl _ro_callback
.p2align 2
_ro_callback:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x8, x0
    mov w0, w1
    blr x8
    cmp w0, #0
    cset w0, gt
    ldp x29, x30, [sp], #16
    ret
#endif
