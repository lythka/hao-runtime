/* Active-payload ownership. Do NOT clone the whole enum: inactive
 * cases may make its type non-bitwise-borrowable. The active payload itself
 * must pass Swift's exact bitwise-borrowable gate before a borrowed byte copy.
 * Admission belongs to the caller; these helpers do not support assignment or inout. */
#include "Aggregate.h"
#include <stdlib.h>
#include <string.h>
#define SWIFT __attribute__((swiftcall))
typedef void *SWIFT copy_fn(void *, const void *, const void *);
typedef void SWIFT destroy_fn(void *, const void *);
typedef struct {
    void *functions[8]; uint64_t size, stride; uint32_t flags, extra;
} witnesses;
static const witnesses *vwt(const void *type) { return ((const witnesses *const *)type)[-1]; }

static void *borrow_payload(const void *source, const enum_case_plan *plan,
                            empty_patch *patches, int *count)
{
    /* IsNonBitwiseTakable | IsNonBitwiseBorrowable | IsNonCopyable.
     * The last bit is only a refusal check: caller's T: Copyable contract
     * supplies positive admission, including conditional conformances. */
    if (!plan->payload || (plan->payload_flags & 0x01900000)) return NULL;
    *count = collect_empty_patches(plan->payload, source, patches, 64);
    if (*count <= 0) return NULL;
    size_t alignment = (plan->payload_flags & 255) + 1;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    void *scratch = NULL;
    if (posix_memalign(&scratch, alignment, (size_t)plan->payload_size)) return NULL;
    memcpy(scratch, source, (size_t)plan->payload_size);
    #ifndef DISABLE_EMPTY_ADAPTATION
    for (int i = 0; i < *count; ++i) {
        uint64_t bits = patches[i].receiver_bits;
        memcpy((char *)scratch + patches[i].offset, &bits, 8);
    }
    #endif
    return scratch;
}

static int transfer(void *destination, const void *source, const void *type, int preserve, unsigned slot)
{
    enum_case_plan plan;
    if (!destination || !discover_enum_case(type, source, &plan)) return 0;
    uintptr_t d = (uintptr_t)destination, s = (uintptr_t)source;
    if ((d >= s ? d - s : s - d) < plan.size) return 0;
    empty_patch patches[64]; int count;
    void *scratch = borrow_payload(source, &plan, patches, &count);
    if (!scratch) return 0;
    copy_fn *copy = (copy_fn *)vwt(plan.payload)->functions[slot];
    /* Copy live sibling ownership from the borrowed view. No destructor is
     * run on scratch. With slot4, caller consumes the original source's
     * ownership; the destination inherits it without an extra retain. */
    copy(destination, scratch, plan.payload);
    if (preserve) for (int i = 0; i < count; ++i)
        memcpy((char *)destination + patches[i].offset, &patches[i].bits, 8);
    ((unsigned char *)destination)[plan.tag_offset] = (unsigned char)plan.tag;
    free(scratch);
    return 1;
}

int copy_enum_empty(void *destination, const void *source, const void *type, int preserve)
{
    return transfer(destination, source, type, preserve, 2);
}

int take_enum_empty(void *destination, void *source, const void *type, int preserve)
{
    return transfer(destination, source, type, preserve, 4);
}

int destroy_enum_empty(void *value, const void *type)
{
    enum_case_plan plan;
    if (!discover_enum_case(type, value, &plan)) return 0;
    empty_patch patches[64]; int count;
    void *scratch = borrow_payload(value, &plan, patches, &count);
    if (!scratch) return 0;
    destroy_fn *destroy = (destroy_fn *)vwt(plan.payload)->functions[1];
    /* Caller relinquishes the original value's ownership exactly once. Its
     * raw storage remains but is no longer an initialized Swift value. */
    destroy(scratch, plan.payload);
    free(scratch);
    return 1;
}
