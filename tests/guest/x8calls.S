/* Native callers of tests/guest/indirect-x8.sh that set the indirect-result register themselves:
 * x8_call_with(f, p) calls f with p in it (x86_64 rax, as Swift code passes a result's buffer; arm64 x8),
 * x8_call_stale(f, p) with p left in rax as a C caller leaves any value there (arm64: x8 is a private
 * scratch word). Assembled twice: -DSWIFT_IMAGE gives the x8s_ names and a __swift5_types section, so
 * that image counts as one with Swift metadata, as Swift frameworks do; otherwise the x8c_ names. */
#ifdef SWIFT_IMAGE
#define WITH _x8s_call_with
#define STALE _x8s_call_stale
    .section __TEXT,__swift5_types
    .p2align 2
    .long 0
#else
#define WITH _x8c_call_with
#define STALE _x8c_call_stale
#endif
    .text
    .globl WITH
    .globl STALE
#if defined(__x86_64__)
    .p2align 4
WITH:
STALE:
    pushq %rbp
    movq %rsp, %rbp
    movq %rdi, %r11
    movq %rsi, %rax
    callq *%r11
    popq %rbp
    retq
#elif defined(__arm64__)
#if __has_feature(ptrauth_calls)
#define CALL blraaz x16
#else
#define CALL blr x16
#endif
    .p2align 2
WITH:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x16, x0
    mov x8, x1
    CALL
    ldp x29, x30, [sp], #16
    ret
STALE:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    mov x16, x0
    add x8, sp, #16
    CALL
    ldp x29, x30, [sp], #32
    ret
#endif
