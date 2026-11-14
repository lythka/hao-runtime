#ifndef HAO_SWIFTVALUE_H
#define HAO_SWIFTVALUE_H
#include <stdbool.h>
#include <stdint.h>
#include "hao/swift_reverse.h"
struct hao_thread;
struct hao_swift_value_plan;
typedef struct hao_swift_value_scope {
    struct hao_swift_value_scope *previous;
    const struct hao_swift_value_plan *plan;
    void *output;
} hao_swift_value_scope;
void hao_swift_value_begin(struct hao_thread *, hao_swift_value_scope *, const void *metadata, void *output);
void hao_swift_value_end(struct hao_thread *, hao_swift_value_scope *);
bool hao_swift_value_native(struct hao_thread *, uint64_t pc);
/* Guest calls of libswiftCore's Optional value witnesses (swiftvalue.c):
 * set up once the Swift runtime is loaded (per guest image, idempotent); a
 * call target inside [lo, hi] may be one. */
void hao_swift_witness_setup(void);
bool hao_swift_witness_native(struct hao_thread *, uint64_t pc);
extern uint64_t hao_swift_witness_lo, hao_swift_witness_hi;
/* A callback's guest code returns its indirect result INDIRECT (0: none) to
 * native code: the nils hao wrote in the guest's encoding into a value made
 * there since MARK (t->swift_made_n at the callback's entry) get the native
 * encoding back; the ring is then reset to MARK. */
void hao_swift_witness_result(struct hao_thread *, unsigned mark, uint64_t indirect);
/* VALUE of TYPE, which native code made in guest storage (a native generic
 * function's result): its outer nils in the guest's encoding where guest code
 * reads them inline. True if a word changed. */
bool hao_swift_native_result_to_guest(const void *type, void *value);
void hao_swift_value_fork_child(void);
void hao_swift_result_prepare(const hao_swift_result_context *, const uint64_t *, uint64_t, hao_swift_value_scope *);
#endif
