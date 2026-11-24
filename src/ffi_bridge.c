/* Darwin arm64 libffi CIFs and closures have a different layout from the
 * x86_64 objects used by system libffi. Keep their lifetimes separate. */
#include "hao/runtime.h"
#include "hao/macho.h"
#include <ffi/ffi.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned abi, nargs;
    ffi_type **args;
    ffi_type *result;
    unsigned bytes, flags, fixed;
} arm_cif;
_Static_assert(sizeof(arm_cif) == 40, "Darwin arm64 CIF");

enum { MAX_ARGS = 128, MAX_NODES = 1024 };
typedef struct {
    ffi_cif cif;
    ffi_type **args;
    ffi_type *result;
    unsigned nargs, nodes;
    char encoding[8192];
    size_t used;
} call_plan;

static ffi_type *scalar(unsigned kind, const char **encoding)
{
#define CASE(tag, name, enc) case FFI_TYPE_##tag: *encoding = enc; return &ffi_type_##name
    switch (kind) {
    CASE(VOID,void,"v"); CASE(INT,sint,"i"); CASE(FLOAT,float,"f"); CASE(DOUBLE,double,"d");
    CASE(UINT8,uint8,"C"); CASE(SINT8,sint8,"c"); CASE(UINT16,uint16,"S"); CASE(SINT16,sint16,"s");
    CASE(UINT32,uint32,"I"); CASE(SINT32,sint32,"i"); CASE(UINT64,uint64,"Q"); CASE(SINT64,sint64,"q");
    CASE(POINTER,pointer,"^v");
    default: return NULL;
    }
#undef CASE
}

static void drop_type(ffi_type *t)
{
    if (!t || t->type != FFI_TYPE_STRUCT) return;
    for (ffi_type **p = t->elements; p && *p; ++p) drop_type(*p);
    free(t->elements);
    free(t);
}

static void drop_plan(call_plan *p)
{
    for (unsigned i = 0; p->args && i < p->nargs; ++i) drop_type(p->args[i]);
    free(p->args);
    drop_type(p->result);
}

static bool emit(call_plan *p, const char *s)
{
    size_t n = strlen(s);
    if (n >= sizeof p->encoding - p->used) return false;
    memcpy(p->encoding + p->used, s, n + 1);
    p->used += n;
    return true;
}

static ffi_type *copy_type(call_plan *p, ffi_type *source, unsigned depth)
{
    if (!source || depth > 24 || ++p->nodes > MAX_NODES) return NULL;
    if (source->type != FFI_TYPE_STRUCT) {
        const char *encoding;
        ffi_type *t = scalar(source->type, &encoding);
        if (!t || t->size != source->size || t->alignment != source->alignment || !emit(p, encoding)) return NULL;
        return t;
    }
    if (!source->elements || !emit(p, "{?=")) return NULL;
    unsigned count = 0;
    while (count < MAX_NODES && source->elements[count]) ++count;
    if (!count || count == MAX_NODES) return NULL;
    ffi_type *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->type = FFI_TYPE_STRUCT;
    t->elements = calloc(count + 1, sizeof *t->elements);
    if (!t->elements) goto bad;
    for (unsigned i = 0; i < count; ++i) {
        t->elements[i] = copy_type(p, source->elements[i], depth + 1);
        if (!t->elements[i] || t->elements[i]->type == FFI_TYPE_VOID) goto bad;
    }
    if (!emit(p, "}") || ffi_get_struct_offsets(FFI_DEFAULT_ABI, t, NULL) != FFI_OK) goto bad;
    if ((source->size && source->size != t->size) ||
        (source->alignment && source->alignment != t->alignment)) goto bad;
    if (!source->size) { source->size = t->size; source->alignment = t->alignment; }
    return t;
bad:
    drop_type(t);
    return NULL;
}

static ffi_status plan_init(call_plan *p, const arm_cif *g)
{
    if (g->abi != 1) return FFI_BAD_ABI;
    if (g->nargs > MAX_ARGS || g->fixed > g->nargs || (g->nargs && !g->args)) return FFI_BAD_TYPEDEF;
    p->nargs = g->nargs;
    p->result = copy_type(p, g->result, 0);
    if (!p->result) return FFI_BAD_TYPEDEF;
    p->args = calloc(g->nargs + 1, sizeof *p->args);
    if (!p->args) return FFI_BAD_TYPEDEF;
    for (unsigned i = 0; i < g->nargs; ++i) {
        p->args[i] = copy_type(p, g->args[i], 0);
        if (!p->args[i] || p->args[i]->type == FFI_TYPE_VOID) return FFI_BAD_TYPEDEF;
    }
    if (g->fixed) return ffi_prep_cif_var(&p->cif, FFI_DEFAULT_ABI, g->fixed, g->nargs, p->result, p->args);
    return ffi_prep_cif(&p->cif, FFI_DEFAULT_ABI, g->nargs, p->result, p->args);
}

/* Return the homogeneous floating-point leaf kind, or zero. */
static unsigned homogeneous(const ffi_type *t)
{
    if (t->type == FFI_TYPE_FLOAT || t->type == FFI_TYPE_DOUBLE) return t->type;
    if (t->type != FFI_TYPE_STRUCT || t->size > 32) return 0;
    unsigned first = homogeneous(t->elements[0]);
    if (!first) return 0;
    for (unsigned i = 1; t->elements[i]; ++i)
        if (homogeneous(t->elements[i]) != first) return 0;
    return first;
}

static unsigned vector_flag(const ffi_type *t)
{
    unsigned kind = homogeneous(t);
    if (!kind) return 0;
    unsigned count = t->size / (kind == FFI_TYPE_FLOAT ? 4 : 8);
    return count && count <= 4 ? (kind == FFI_TYPE_FLOAT ? 12 : 16) - count : 0;
}

static unsigned rounded(unsigned n, unsigned alignment)
{
    return (n + alignment - 1) & ~(alignment - 1);
}

static void guest_fields(arm_cif *g)
{
    ffi_type *r = g->result;
    unsigned bytes = r->type == FFI_TYPE_STRUCT ? 8 : 0;
    switch (r->type) {
    case FFI_TYPE_VOID: g->flags = 0; break;
    case FFI_TYPE_UINT8: g->flags = 20; break;
    case FFI_TYPE_UINT16: g->flags = 22; break;
    case FFI_TYPE_UINT32: g->flags = 24; break;
    case FFI_TYPE_SINT8: g->flags = 26; break;
    case FFI_TYPE_SINT16: g->flags = 28; break;
    case FFI_TYPE_INT: case FFI_TYPE_SINT32: g->flags = 30; break;
    case FFI_TYPE_SINT64: case FFI_TYPE_UINT64: case FFI_TYPE_POINTER: g->flags = 1; break;
    default:
        g->flags = vector_flag(r);
        if (!g->flags) {
            if (r->size > 16) { g->flags = 32; bytes += 8; }
            else g->flags = r->size == 8 ? 1 : r->size == 16 ? 2 : 66;
        }
    }
    for (unsigned i = 0; i < g->nargs; ++i) {
        ffi_type *a = g->args[i];
        bytes = rounded(bytes, a->alignment) + rounded(a->size, 8);
        if (vector_flag(a)) g->flags |= 128;
    }
    g->bytes = rounded(bytes, 16);
}

typedef struct {
    uint64_t tramp[2], cif, fun, data;
} arm_closure;

typedef struct closure_entry {
    struct closure_entry *next;
    arm_closure *guest;
    ffi_closure *native;
    void *code;
    call_plan *plan;
    arm_closure *binding;
    unsigned active;
} closure_entry;

static pthread_mutex_t closure_lock = PTHREAD_MUTEX_INITIALIZER;
static closure_entry *closures;

struct hao_ffi_active {
    closure_entry *entry;
    struct hao_ffi_active *previous;
    int outer_depth;
};

static void lock_registry(void)
{
    hao_crit_enter();
    pthread_mutex_lock(&closure_lock);
}

static void unlock_registry(void)
{
    pthread_mutex_unlock(&closure_lock);
    hao_crit_leave();
}

void hao_ffi_fork_child(void)
{
    closure_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    for (closure_entry *e = __atomic_load_n(&closures, __ATOMIC_ACQUIRE); e; e = __atomic_load_n(&e->next, __ATOMIC_ACQUIRE)) {
        e->active = 0;
        /* Fork can stop a writer between the guest-facing field copy and
         * its release publication. Reconstruct the visible fields from the
         * same complete snapshot that the child's callback will invoke. */
        arm_closure *binding = __atomic_load_n(&e->binding, __ATOMIC_ACQUIRE);
        if (binding) *e->guest = *binding;
        else *e->guest = (arm_closure){0};
    }
    hao_thread *thread = hao_current_peek();
    for (struct hao_ffi_active *a = thread ? thread->ffi_active : NULL; a; a = a->previous)
        ++a->entry->active;
}

void hao_ffi_require_live_frame(hao_thread *thread, int destination)
{
    if (thread->ffi_active && destination <= thread->ffi_active->outer_depth)
        hao_fatal_safe("libffi: nonlocal exit across native closure", NULL, 0);
}

static closure_entry *find_closure(void *address)
{
    for (closure_entry *e = closures; e; e = e->next)
        if (e->guest == address || e->code == address) return e;
    return NULL;
}

static void closure_dispatch(ffi_cif *cif, void *result, void **args, void *ctx)
{
    closure_entry *e = ctx;
    lock_registry();
    arm_closure *ready = __atomic_load_n(&e->binding, __ATOMIC_ACQUIRE);
    if (!ready) {
        unlock_registry();
        hao_fatal_safe("libffi: closure preparation incomplete", NULL, 0);
    }
    hao_thread *thread = hao_current();
    struct hao_ffi_active frame = {e, thread->ffi_active, thread->depth};
    thread->ffi_active = &frame;
    ++e->active;
    arm_closure binding = *ready;
    unlock_registry();
    uint64_t words[] = { binding.cif, (uintptr_t)result, (uintptr_t)args, binding.data };
    hao_call_guest(thread, binding.fun, words, 4);
    lock_registry();
    thread->ffi_active = frame.previous;
    --e->active;
    unlock_registry();
}

static uint64_t allocate_closure(size_t n, void **code)
{
    if (!code || n < sizeof(arm_closure)) return 0;
    closure_entry *e = calloc(1, sizeof *e);
    if (!e) return 0;
    e->guest = calloc(1, n);
    if (e->guest) e->native = ffi_closure_alloc(sizeof *e->native, &e->code);
    if (!e->native) { free(e->guest); free(e); return 0; }
    lock_registry();
    e->next = closures;
    __atomic_store_n(&closures, e, __ATOMIC_RELEASE);
    unlock_registry();
    *code = e->code;
    return (uintptr_t)e->guest;
}

static ffi_status prepare_closure(arm_closure *buf, arm_cif *cif, uint64_t fun, uint64_t data, void *code, bool require_code)
{
    if (!cif || cif->fixed || !hao_is_guest_function(fun)) return FFI_BAD_TYPEDEF;
    call_plan *p = calloc(1, sizeof *p);
    if (!p) return FFI_BAD_TYPEDEF;
    arm_closure *binding = calloc(1, sizeof *binding);
    if (!binding) { free(p); return FFI_BAD_TYPEDEF; }
    binding->cif = (uintptr_t)cif;
    binding->fun = fun;
    binding->data = data;
    ffi_status status = plan_init(p, cif);
    if (status == FFI_OK) {
        lock_registry();
        closure_entry *e = find_closure(buf);
        if (!e || buf != e->guest || (require_code && code != e->code) || e->active)
            status = FFI_BAD_TYPEDEF;
        else if (e->plan) {
            if (strcmp(p->encoding, e->plan->encoding)) status = FFI_BAD_TYPEDEF;
        } else {
            status = ffi_prep_closure_loc(e->native, &p->cif, closure_dispatch, e, e->code);
            if (status == FFI_OK) { e->plan = p; p = NULL; }
        }
        if (status == FFI_OK) {
            arm_closure *old = e->binding;
            *buf = *binding;
            __atomic_store_n(&e->binding, binding, __ATOMIC_RELEASE);
            binding = old;
        }
        unlock_registry();
    }
    if (p) { drop_plan(p); free(p); }
    free(binding);
    return status;
}

static void release_closure(void *buf)
{
    lock_registry();
    closure_entry **slot = &closures;
    while (*slot && (*slot)->guest != buf) slot = &(*slot)->next;
    closure_entry *e = *slot;
    if (!e || e->active) {
        unlock_registry();
        hao_fatal_safe("libffi: invalid or active closure free", NULL, 0);
    }
    __atomic_store_n(slot, e->next, __ATOMIC_RELEASE);
    unlock_registry();
    ffi_closure_free(e->native);
    if (e->plan) { drop_plan(e->plan); free(e->plan); }
    free(e->binding);
    free(e->guest);
    free(e);
}

enum ffi_operation { PREP, PREP_VAR, CALL, ALLOC, FREE, PREP_LOC, PREP_CLOSURE, OFFSETS };

static void ffi_trap(hao_thread *thread, uint64_t operation)
{
    uint64_t *x = thread->cpu.x;
    arm_cif *guest = (void *)(uintptr_t)x[0];
    if (operation == ALLOC) { x[0] = allocate_closure(x[0], (void *)x[1]); return; }
    if (operation == FREE) { release_closure((void *)x[0]); return; }
    if (operation == PREP_LOC || operation == PREP_CLOSURE) {
        x[0] = prepare_closure((void *)x[0], (void *)x[1], x[2], x[3], (void *)x[4], operation == PREP_LOC);
        return;
    }
    call_plan plan = {0};
    if (operation == OFFSETS) {
        ffi_type *source = (void *)x[1];
        ffi_status s = FFI_BAD_ABI;
        if (x[0] == 1) {
            plan.result = copy_type(&plan, source, 0);
            s = plan.result && plan.result->type == FFI_TYPE_STRUCT ?
                ffi_get_struct_offsets(FFI_DEFAULT_ABI, plan.result, (void *)x[2]) : FFI_BAD_TYPEDEF;
        }
        drop_plan(&plan);
        x[0] = s;
        return;
    }
    if (operation == CALL) {
        if (!guest || !x[1] || plan_init(&plan, guest) != FFI_OK)
            hao_fatal_safe("libffi: invalid call interface", NULL, 0);
        uint64_t target = x[1];
        lock_registry();
        closure_entry *e = find_closure((void *)target);
        if (e && (e->code != (void *)target || !__atomic_load_n(&e->binding, __ATOMIC_ACQUIRE) || guest->fixed || strcmp(e->plan->encoding, plan.encoding))) {
            unlock_registry();
            hao_fatal_safe("libffi: closure call signature mismatch", NULL, 0);
        }
        unlock_registry();
        uint64_t original = e ? 0 : hao_guest_of_entry(target);
        if (original) target = original;
        if (!e && hao_image_for_code(target)) {
            target = guest->fixed ? 0 : hao_guest_entry_for_exact_sig(target, plan.encoding);
            if (!target) hao_fatal_safe("libffi: no exact guest call entry", NULL, 0);
        } else if (!e && (hao_is_trap(target) || hao_not_host_code(target))) {
            hao_fatal_safe("libffi: target is not callable host or guest code", NULL, 0);
        }
        ffi_call(&plan.cif, FFI_FN(target), (void *)x[2], (void *)x[3]);
        drop_plan(&plan);
        return;
    }
    bool variadic = operation == PREP_VAR;
    arm_cif input = {.abi = x[1], .nargs = x[variadic ? 3 : 2], .args = (void *)x[variadic ? 5 : 4],
                     .result = (void *)x[variadic ? 4 : 3], .fixed = variadic ? (unsigned)x[2] : 0};
    ffi_status status = !guest || (variadic && !input.fixed) ? FFI_BAD_TYPEDEF : plan_init(&plan, &input);
    if (status == FFI_OK) { guest_fields(&input); *guest = input; }
    drop_plan(&plan);
    x[0] = status;
}

/* Compare the resolved native address, preserving same-named guest exports
 * and application-provided implementations of the libffi API. */
bool hao_ffi_native(const char *name, void *implementation, uint64_t *address)
{
    static const struct { void *function; enum ffi_operation operation; } entries[] = {
        {ffi_prep_cif,PREP}, {ffi_prep_cif_var,PREP_VAR}, {ffi_call,CALL},
        {ffi_closure_alloc,ALLOC}, {ffi_closure_free,FREE}, {ffi_prep_closure_loc,PREP_LOC},
        {ffi_prep_closure,PREP_CLOSURE}, {ffi_get_struct_offsets,OFFSETS}
    };
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; ++i) {
        if (implementation != entries[i].function) continue;
        *address = hao_trap_address(name, ffi_trap, entries[i].operation);
        return true;
    }
    return false;
}
