/* Compiler-lowered Swift reverse signatures emitted by gen_swift.py.
 * This is deliberately separate from abi.c's C/ObjC signatures. */
#ifndef HAO_SWIFT_REVERSE_H
#define HAO_SWIFT_REVERSE_H
#include <stdint.h>

enum { HAO_SR_GP, HAO_SR_FP, HAO_SR_STACK, HAO_SR_SPECIAL };
typedef struct {
    uint16_t arm, native;
    uint8_t arm_kind, native_kind, size, reserved;
} hao_swift_move;
typedef struct {
    uint16_t version, nargs, nrets, stack_bytes;
    hao_swift_move moves[];
} hao_swift_reverse;
#define HAO_SWIFT_REVERSE_VERSION 1
/* hsig allocations are aligned; low bits 10 distinguish Swift from C. */
#define HAO_CB_SWIFT_TAG 2ull
/* Optional compiler semantic companion. This identifies a result type, not
 * permission to reinterpret its representation or transfer its ownership. */
typedef struct {
    uint16_t version, self_arg, witness_arg, result_arg;
    const char *associated_name;
} hao_swift_result_signature;
/* Version 1 remains the 16-byte identity-only prefix. Version 2 adds a
 * compiler-established contract; it does not prove representation equality. */
enum { HAO_SWIFT_RESULT_COPYABLE = 1u };
typedef struct {
    hao_swift_result_signature base;
    uint32_t flags, reserved;
} hao_swift_result_signature_v2;
static inline uint32_t hao_swift_result_contract(const hao_swift_result_signature *s)
{
    return s && s->version == 2 ? ((const hao_swift_result_signature_v2 *)s)->flags : 0;
}
typedef struct hao_swift_result_context hao_swift_result_context;
#define HAO_CB_SWIFT_RESULT_TAG 3ull

const hao_swift_reverse *hao_thunks_reverse(const char *key);
const hao_swift_reverse *hao_swift_reverse_requirement(uint64_t requirement,
    const hao_swift_result_signature **result);
const hao_swift_result_signature *hao_thunks_result(const char *key, const hao_swift_reverse *reverse);
const hao_swift_result_context *hao_swift_result_bind(uint64_t protocol, uint64_t requirement,
    const hao_swift_reverse *reverse, const hao_swift_result_signature *signature);
const hao_swift_reverse *hao_swift_result_reverse(const hao_swift_result_context *context);
void hao_swift_result_check(const hao_swift_result_context *context, const uint64_t *arguments, uint64_t target);
void hao_swift_result_stats(void);
void hao_swift_result_fork_child(void);
#endif
