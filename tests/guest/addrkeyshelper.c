/* libaddrkeysimpl.dylib for tests/guest/addrkeys.sh: functions whose x86_64 code returns a
 * narrow result with junk above it, each exported under two names at one address (the link
 * line's -alias: an alias the library itself exports, not a re-export). The interface unit
 * types one name of each; the guest calls the other.
 *   akx_flag / akx_same   bool, x86_64 leaves 0x5a above al      (the unit names akx_flag)
 *   akx_rev  / akx_revb   bool, the same                         (the unit names akx_revb)
 *   akx_sh   / akx_sh2    int16_t, x86_64 leaves 0x5a5a above ax (the unit names akx_sh)
 * Apple arm64 callers read the whole of w0 (the callee extends a narrow result to 32 bits);
 * SysV defines only al/ax, so without the type the guest sees the junk. */
#include <stdbool.h>
#include <stdint.h>

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _akx_flag\n_akx_flag:\n  movabsq $0x5a5a5a5a5a5a5a5a, %rax\n  testl %edi, %edi\n  setg %al\n  ret\n"
        ".globl _akx_rev\n_akx_rev:\n  movabsq $0x5a5a5a5a5a5a5a5a, %rax\n  cmpl $3, %edi\n  setl %al\n  ret\n"
        ".globl _akx_sh\n_akx_sh:\n  movl $0x5a5a5a5a, %eax\n  movw %di, %ax\n  negw %ax\n  ret\n");
#else
bool akx_flag(int v) { return v > 0; }
bool akx_rev(int v) { return v < 3; }
int16_t akx_sh(int v) { return (int16_t)-v; }
#endif
