/* The compiler's reverse recipe and Copyable result companion for SRLib's Source.value getter, in the
 * generator's layout (include/hao/swift_reverse.h): x8 -> rax, x20 -> r13, x0 -> rdi (Self), x1 -> rsi. */
#include <stdint.h>
typedef struct { uint16_t arm, native; uint8_t arm_kind, native_kind, size, reserved; } move;
__attribute__((visibility("default"), aligned(8)))
const struct { uint16_t version, nargs, nrets, stack_bytes; move moves[4]; } hao_rs_ksrvalue =
    {1, 4, 0, 0, {{8, 3, 0, 3, 8, 0}, {20, 1, 0, 3, 8, 0}, {0, 0, 0, 0, 8, 0}, {1, 1, 0, 0, 8, 0}}};
__attribute__((visibility("default"), aligned(8)))
const struct { uint16_t version, self_arg, witness_arg, result_arg; const char *name; uint32_t flags, reserved; }
    hao_ar_ksrvalue = {2, 2, 3, 0, "Value", 1, 0};
