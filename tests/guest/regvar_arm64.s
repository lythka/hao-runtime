// tests/guest/regvar.sh: one caller per register-variant entry point (src/regvar_gen.inc).
// rv_<sym>(obj): x19-x28 and x1-x15 get sentinels, xREG = obj, x0 = 0x7777 (unless REG is 0),
// bl SYM; returns a mask of what AAPCS64 (and the variant's arm64 contract) promises and the
// callee broke: bit 0 the retain result in x0 is not obj, bits 1-10 x19-x28 changed (xREG: not
// obj), bit 11 sp moved. regvar_cases.s (written by regvar.sh from the .inc) instantiates
// RV SYM, REG, ISRET, IDX for each record and lists them in _rv_table.
        .macro RV sym, reg, isret
        .text
        .p2align 2
        .globl _rv_\sym
_rv_\sym:
        sub     sp, sp, #112
        stp     x29, x30, [sp, #96]
        add     x29, sp, #96
        stp     x19, x20, [sp, #0]
        stp     x21, x22, [sp, #16]
        stp     x23, x24, [sp, #32]
        stp     x25, x26, [sp, #48]
        stp     x27, x28, [sp, #64]
        str     x0, [sp, #80]
        .irp    r, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,19,20,21,22,23,24,25,26,27,28
        mov     x\r, #(0x100 * \r + \r)
        .endr
        .if \reg != 0
        mov     x\reg, x0
        mov     x0, #0x7777
        .endif
        bl      _\sym
        mov     x9, #0
        ldr     x10, [sp, #80]
        .if \isret
        cmp     x0, x10
        cset    x11, ne
        orr     x9, x9, x11
        .endif
        .irp    r, 19,20,21,22,23,24,25,26,27,28
        .if \r == \reg
        cmp     x\r, x10
        .else
        mov     x11, #(0x100 * \r + \r)
        cmp     x\r, x11
        .endif
        cset    x11, ne
        orr     x9, x9, x11, lsl #(\r - 18)
        .endr
        add     x11, sp, #96
        cmp     x11, x29
        cset    x11, ne
        orr     x0, x9, x11, lsl #11
        ldp     x19, x20, [sp, #0]
        ldp     x21, x22, [sp, #16]
        ldp     x23, x24, [sp, #32]
        ldp     x25, x26, [sp, #48]
        ldp     x27, x28, [sp, #64]
        ldp     x29, x30, [sp, #96]
        add     sp, sp, #112
        ret
        .data
        .p2align 3
Lent_\sym:
        .quad   _rv_\sym@AUTH(ia,0)
        .quad   Lname_\sym
        .quad   \isret
        .cstring
Lname_\sym:
        .asciz  "\sym"
        .endm
