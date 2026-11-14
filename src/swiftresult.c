/* Compiler-described associated results. Version1 provides type identity;
 * version2 may additionally prove Copyable. Representation admission and
 * ownership operations remain the separate swiftvalue policy. */
#include "hao/runtime.h"
#include "hao/swift_reverse.h"
#include "hao/swiftvalue.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "hao/dlsym_once.h"

struct hao_swift_result_context {
    const hao_swift_reverse *reverse;
    const hao_swift_result_signature *signature;
    uint64_t protocol, requirement, base, associated;
    struct hao_swift_result_context *next;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static hao_swift_result_context *contexts;
static uint64_t bound, checked, refused;
void hao_swift_result_fork_child(void) { lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }

static bool read_at(uint64_t a, void *out, size_t n)
{
    mach_vm_size_t got = 0;
    return a >= 4096 && a <= UINT64_MAX - n &&
        mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)out, &got)
        == KERN_SUCCESS && got == n;
}

static bool relative(uint64_t field, bool indirect, uint64_t *out)
{
    int32_t r;
    if (!read_at(field, &r, 4) || !r) return false;
    bool deref = indirect && (r & 1);
    uint64_t a = field + (int64_t)(deref ? r & ~1 : r);
    if (deref) return read_at(a, out, 8);
    *out = a;
    return true;
}

const hao_swift_result_context *hao_swift_result_bind(uint64_t protocol, uint64_t requirement,
    const hao_swift_reverse *reverse, const hao_swift_result_signature *signature)
{
    if (!signature || !reverse || (!hao_opt.bridge_check && !(hao_swift_result_contract(signature) & HAO_SWIFT_RESULT_COPYABLE))) return NULL;
    uint32_t head[6], flags;
    if (!read_at(protocol, head, sizeof head) || (head[0] & 31) != 3 ||
        head[3] > 4096 || !head[4] || head[4] > 4096) goto refuse;
    uint64_t start = protocol + 24 + 12ull * head[3];
    if (requirement < start || (requirement - start) % 8 ||
        (requirement - start) / 8 >= head[4] || !read_at(requirement, &flags, 4) ||
        (flags & 0xffff) != 0x13) goto refuse; /* synchronous instance getter */
    uint64_t names;
    if (!relative(protocol + 20, false, &names)) goto refuse;
    char text[4096];
    size_t len;
    for (len = 0; len < sizeof text; len++) {
        if (!read_at(names + len, text + len, 1)) goto refuse;
        if (!text[len]) break;
    }
    if (!len || len == sizeof text) goto refuse;
    char *name = text;
    uint64_t associated = 0;
    for (uint32_t k = 0; k < head[4]; k++) {
        uint64_t r = start + 8ull * k;
        if (!read_at(r, &flags, 4)) goto refuse;
        if ((flags & 15) != 7) continue; /* AssociatedTypeAccessFunction */
        if (!name || !*name) goto refuse;
        char *end = strchr(name, ' ');
        if (end) *end = 0;
        if (!strcmp(name, signature->associated_name)) {
            if (associated) goto refuse;
            associated = r;
        }
        name = end ? end + 1 : NULL;
    }
    if (name || !associated) goto refuse;
    hao_swift_result_context *fresh = malloc(sizeof *fresh);
    if (!fresh) goto refuse;
    *fresh = (hao_swift_result_context){reverse, signature, protocol, requirement, start - 8, associated, NULL};
    hao_crit_enter();
    pthread_mutex_lock(&lock);
    for (hao_swift_result_context *c = contexts; c; c = c->next) {
        if (c->protocol == protocol && c->requirement == requirement &&
            c->reverse == reverse && c->signature == signature) {
            pthread_mutex_unlock(&lock);
            hao_crit_leave();
            free(fresh);
            return c;
        }
    }
    fresh->next = contexts;
    contexts = fresh;
    pthread_mutex_unlock(&lock);
    hao_crit_leave();
    __atomic_fetch_add(&bound, 1, __ATOMIC_RELAXED);
    return fresh;
refuse:
    __atomic_fetch_add(&refused, 1, __ATOMIC_RELAXED);
    return NULL;
}

const hao_swift_reverse *hao_swift_result_reverse(const hao_swift_result_context *c) { return c->reverse; }

typedef struct { const void *metadata; uintptr_t state; } metadata_response;
typedef metadata_response __attribute__((swiftcall)) associated_fn(uintptr_t, const void *, const void *, const void *, const void *);
static associated_fn *associated;
static pthread_once_t runtime_once = PTHREAD_ONCE_INIT;
static void runtime_init(void) { associated = (associated_fn *)HAO_DLSYM("swift_getAssociatedTypeWitness"); }

void hao_swift_result_prepare(const hao_swift_result_context *context, const uint64_t *args, uint64_t target, hao_swift_value_scope *scope)
{
    const hao_swift_result_signature *s = context->signature;
    uint64_t self = args[s->self_arg], table = args[s->witness_arg], output = args[s->result_arg];
    uint64_t conformance, protocol, witness;
    uint32_t kind;
    hao_thread *t = hao_current();
    /* An absolute witness table starts with its conformance descriptor. A
     * relative table, unrelated conformance or invalid argument is declined.
     * Do not call a witness slot as a function: it may hold cached metadata
     * or a tagged mangled type reference instead. */
    if (!self || !output || (table & 7) || t->swift_results_resolving ||
        !read_at(table, &conformance, 8) || !relative(conformance, true, &protocol) ||
        protocol != context->protocol || !read_at(self, &kind, 4) ||
        !read_at(table + (context->associated - context->base), &witness, 8) || !witness) goto refuse;
    pthread_once(&runtime_once, runtime_init);
    if (!associated) goto refuse;
    /* Resolution may reenter guest metadata accessors. No plan/loader lock
     * is held; preserve the pending callback's full register activation. */
    hao_cpu saved = t->cpu;
    t->swift_results_resolving++;
    metadata_response response = associated(0, (const void *)(uintptr_t)table, (const void *)(uintptr_t)self,
        (const void *)(uintptr_t)context->base, (const void *)(uintptr_t)context->associated);
    t->swift_results_resolving--;
    t->cpu = saved;
    if (!response.metadata || response.state) goto refuse;
    uint64_t vwt, size;
    if (!read_at((uint64_t)(uintptr_t)response.metadata - 8, &vwt, 8) || !read_at(vwt + 64, &size, 8)) goto refuse;
    if (scope && (hao_swift_result_contract(s) & HAO_SWIFT_RESULT_COPYABLE)) {
        t->swift_results_resolving++;
        hao_swift_value_begin(t,scope,response.metadata,(void *)(uintptr_t)output);
        t->swift_results_resolving--;
        t->cpu = saved;
    }
    __atomic_fetch_add(&checked, 1, __ATOMIC_RELAXED);
    if (!hao_opt.bridge_check) return;
    hao_image *image = hao_image_for_code(target);
    fprintf(HAO_DIAG, "hao: associated result %s metadata=%#llx size=%llu output=%#llx self=%#llx witness=%#llx target=%#llx vmaddr=%#llx\n",
        s->associated_name, (unsigned long long)(uintptr_t)response.metadata, (unsigned long long)size,
        (unsigned long long)output, (unsigned long long)self, (unsigned long long)table,
        (unsigned long long)target, (unsigned long long)(image ? target - image->slide : 0));
    return;
refuse:
    __atomic_fetch_add(&refused, 1, __ATOMIC_RELAXED);
}

void hao_swift_result_check(const hao_swift_result_context *context, const uint64_t *args, uint64_t target)
{
    hao_swift_result_prepare(context,args,target,NULL);
}

void hao_swift_result_stats(void)
{
    if (hao_opt.bridge_check && (bound || checked || refused))
        fprintf(HAO_DIAG, "hao: associated results: bound=%llu checked=%llu refused=%llu (metadata resolutions)\n",
            (unsigned long long)bound, (unsigned long long)checked, (unsigned long long)refused);
}
