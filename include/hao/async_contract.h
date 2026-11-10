#ifndef HAO_ASYNC_CONTRACT_H
#define HAO_ASYNC_CONTRACT_H
#include <stdbool.h>
#include <stdint.h>

/* A versioned contract provider supplies API/type/receiver policy. The runtime
 * owns only invocation lifetime and the admitted exact-word substitution.
 * No provider pointer or borrowed stack scope is retained across suspension.
 */
#define HAO_ASYNC_CONTRACT_ABI 1u
typedef struct {
    uint32_t abi, size;
    const char *symbol;
    uint64_t target, arguments[8], swift_self, context;
    uint64_t resume_entry, resume_guest, resume_vmaddr;
    uint8_t image_uuid[16];
} hao_async_call;

typedef struct {
    uint32_t size, result_bytes;
    uint64_t result_address, source_word, receiver_word;
    uint32_t throwing, reserved;
} hao_async_rule;

typedef struct {
    uint32_t abi, size;
    const char *identifier;
    bool (*match)(const hao_async_call *, hao_async_rule *);
} hao_async_provider;

/* The explicitly selected own-code contract library exports this function. */
typedef const hao_async_provider *hao_async_provider_getter(void);
#endif
