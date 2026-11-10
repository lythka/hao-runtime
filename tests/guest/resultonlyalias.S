// Imports only the renamed re-export _roa_bool (libroa.dylib: _ro_bool from libro.dylib),
// as programs import CoreGraphics' _CGSServerOperationState for SkyLight's
// _SLSServerOperationState. Keeps the result live through a 32-bit read (resultonlycaller.S).
.text

.globl _rt_alias_bool
.p2align 2
_rt_alias_bool:
    stp x29, x30, [sp, #-16]!
    bl _roa_bool
    mov w9, w0
    mov w0, #0
    mov w0, w9
    ldp x29, x30, [sp], #16
    ret
