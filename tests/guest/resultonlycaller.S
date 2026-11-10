// Keep the result live through a 32-bit read, then overwrite x0.
// Moving w0 through w9 preserves dirty low-32 bits for the negative control.
.text

.globl _rt_bool
.p2align 2
_rt_bool:
    stp x29, x30, [sp, #-16]!
    bl _ro_bool
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret

.globl _rt_char
.p2align 2
_rt_char:
    stp x29, x30, [sp, #-16]!
    bl _ro_char
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret

.globl _rt_uchar
.p2align 2
_rt_uchar:
    stp x29, x30, [sp, #-16]!
    bl _ro_uchar
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret

.globl _rt_short
.p2align 2
_rt_short:
    stp x29, x30, [sp, #-16]!
    bl _ro_short
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret

.globl _rt_ushort
.p2align 2
_rt_ushort:
    stp x29, x30, [sp, #-16]!
    bl _ro_ushort
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret

.globl _rt_callback
.p2align 2
_rt_callback:
    stp x29, x30, [sp, #-16]!
    bl _ro_callback
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret

.globl _rt_wrap
.p2align 2
_rt_wrap:
    stp x29, x30, [sp, #-16]!
    bl _ro_wrap
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret
