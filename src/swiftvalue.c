/* Native fast-path Swift values. Admission starts at a compiler-certified
 * Copyable associated result, never by guessing pointer arguments of arbitrary
 * calls. Metadata comes from the native runtime's complete response. Native
 * allocation/RC and ordinary compiler schemas are the fast path's contracts.
 * The shared policy supplies the narrower representation proofs.
 */
#include "hao/runtime.h"
#include "hao/swiftvalue.h"
#include "hao/swiftschema.h"
#include "hao/opts.h"
#define HAO_SWIFT_VALUE_RUNTIME 1
#include "swiftvalue/Aggregate.h"
#include "swiftvalue/Common.h"
#include "swiftvalue/NativeLeaf.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SWIFT __attribute__((swiftcall))
#define NODE_LIMIT 1024
#define ENTRY_LIMIT (NODE_LIMIT * 6)
typedef struct { void *fn[8]; uint64_t size, stride; uint32_t flags, extra; } witnesses;
typedef struct { const char *name; void (*free_name)(const char *); bool strong, var; } field_info;
typedef uintptr_t SWIFT kind_fn(const void *);
typedef intptr_t SWIFT count_fn(const void *);
typedef intptr_t SWIFT offset_fn(const void *, intptr_t);
typedef const void *SWIFT field_fn(const void *, intptr_t, field_info *);
typedef struct { const void *type; bool affected; } node;
typedef struct { uint64_t pc; const void *type; unsigned slots; } entry;
typedef struct hao_swift_value_plan {
    const void *type;
    unsigned count;
    entry *entries;
    struct hao_swift_value_plan *next;
} plan;
typedef struct {
    kind_fn *kind; count_fn *count; offset_fn *offset; field_fn *field;
    node nodes[NODE_LIMIT]; entry entries[ENTRY_LIMIT];
    unsigned nnode, nentry;
    bool failed;
} builder;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static plan *plans;
static const witnesses *vwt(const void *type) { return ((const witnesses *const *)type)[-1]; }
static bool native_function(uint64_t pc)
{
    return pc && !hao_is_trap(pc) && !hao_image_for_code(pc) && !hao_guest_of_entry(pc);
}
void hao_swift_value_fork_child(void)
{
    lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    hao_sv_native_leaf_fork_child();
}
static void refuse(const void *type, const char *why)
{
    hao_diag_prefix(HAO_DIAG);
    typedef struct { const char *text; uintptr_t length; } type_name;
    typedef type_name SWIFT name_fn(const void *, bool);
    name_fn *name = dlsym(RTLD_DEFAULT,"swift_getTypeName");
    type_name n = name ? name(type,true) : (type_name){"?",1};
    fprintf(HAO_DIAG,"Swift value boundary refused: %s (metadata=%p size=%llu flags=%#x type=%.*s)\n",
        why,type,(unsigned long long)vwt(type)->size,vwt(type)->flags,(int)(n.length > 4096 ? 4096 : n.length),n.text);
    abort();
}
static bool graph(builder *b, const void *type, unsigned depth)
{
    if (!type || depth > 32 || b->nnode == NODE_LIMIT) { b->failed = true; return false; }
    for (unsigned i = 0; i < b->nnode; ++i) if (b->nodes[i].type == type) return b->nodes[i].affected;
    unsigned at = b->nnode++;
    b->nodes[at].type = type;
    const witnesses *v = vwt(type);
    if (v->size > 4096 || v->stride < v->size || (v->flags & 0x00800000)) { b->failed = true; return false; }
    uintptr_t kind = b->kind(type);
    empty_plan empty;
    bool affected = discover_empty_plan(type,&empty) && empty.consumed == 2;
    if (kind == 0x200 || kind == 0x301) {
        intptr_t count = b->count(type);
        if (count < 0 || count > 64) { b->failed = true; return false; }
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = b->field(type,i,&info);
            intptr_t offset = b->offset(type,i);
            if (info.free_name) info.free_name(info.name);
            if (!child || offset < 0 || (uint64_t)offset > v->size || vwt(child)->size > v->size - (uint64_t)offset) {
                b->failed = true; return false;
            }
            affected |= graph(b,child,depth + 1);
        }
    } else if (kind == 0x202) {
        affected |= graph(b,((const void *const *)type)[2],depth + 1);
    } else if (kind == 0x201) {
        const void *payloads[HAO_SV_MAX_ENUM_PAYLOADS]; uint64_t extent;
        int count = enum_payloads(type,payloads,&extent);
        if (count)
            for (int i = 0; i < count; ++i) affected |= graph(b,payloads[i],depth + 1);
    }
    b->nodes[at].affected = affected;
    if (!affected) return false;
    /* Include aliases in every functional slot, including buffer, assignment
     * and enum witnesses. Numeric layout words8..10 are never code pointers. */
    for (unsigned slot = 0; slot <= 5; ++slot) {
        uint64_t pc = (uint64_t)(uintptr_t)v->fn[slot];
        if (!native_function(pc)) continue;
        unsigned bits = 0;
        for (unsigned k = 0; k < 8; ++k) if (v->fn[k] == (void *)(uintptr_t)pc) bits |= 1u << k;
        if (kind == 0x201 || kind == 0x202)
            for (unsigned k = 11; k <= 13; ++k)
                if (((void *const *)v)[k] == (void *)(uintptr_t)pc) bits |= 1u << k;
        bool duplicate = false;
        for (unsigned i = 0; i < b->nentry; ++i)
            if (b->entries[i].pc == pc && b->entries[i].type == type) duplicate = true;
        if (duplicate) continue;
        if (b->nentry == ENTRY_LIMIT) { b->failed = true; return false; }
        b->entries[b->nentry++] = (entry){pc,type,bits};
    }
    return affected;
}
static int entry_order(const void *a, const void *b)
{
    uint64_t x = ((const entry *)a)->pc, y = ((const entry *)b)->pc;
    return (x > y) - (x < y);
}
static const plan *prepare(const void *type)
{
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (plan *p = plans; p; p = p->next) if (p->type == type) {
        pthread_mutex_unlock(&lock); hao_crit_leave(); return p->count ? p : NULL;
    }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    builder *b = calloc(1,sizeof *b);
    if (!b) return NULL;
    b->kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    b->count = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount");
    b->offset = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset");
    b->field = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata");
    bool ok = b->kind && b->count && b->offset && b->field && graph(b,type,0) && !b->failed;
    plan *p = calloc(1,sizeof *p);
    if (!p) { free(b); return NULL; }
    p->type = type;
    if (ok && b->nentry) {
        p->entries = malloc(b->nentry * sizeof(entry));
        if (p->entries) {
            p->count = b->nentry;
            memcpy(p->entries,b->entries,p->count * sizeof(entry));
            qsort(p->entries,p->count,sizeof(entry),entry_order);
        }
    }
    free(b);
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (plan *old = plans; old; old = old->next) if (old->type == type) {
        pthread_mutex_unlock(&lock); hao_crit_leave();
        free(p->entries); free(p); return old->count ? old : NULL;
    }
    p->next = plans; plans = p;
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return p->count ? p : NULL;
}
void hao_swift_value_begin(hao_thread *t, hao_swift_value_scope *scope, const void *type, void *output)
{
    if (!scope || hao_opt.no_swift_values) return;
    const plan *p = prepare(type);
    if (!p) return;
    *scope = (hao_swift_value_scope){t->swift_values,p,output};
    t->swift_values = scope;
}
/* Only ordinary single-field wrappers with identical storage are transparent.
 * Do not byte-clone the enclosing non-bitwise-borrowable enum. */
static const void *enum_storage(const void *type)
{
    kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
    count_fn *count = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount");
    offset_fn *offset = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata");
    for (unsigned depth = 0; depth < 16; ++depth) {
        if (kind(type) == 0x201) return type;
        if (kind(type) != 0x200 || count(type) != 1 || offset(type,0)) return NULL;
        field_info info = {0};
        const void *child = field(type,0,&info);
        if (info.free_name) info.free_name(info.name);
        if (!child || vwt(child)->size != vwt(type)->size || vwt(child)->stride != vwt(type)->stride ||
            (vwt(child)->flags & 255) != (vwt(type)->flags & 255)) return NULL;
        type = child;
    }
    return NULL;
}
static bool handle(hao_thread *t, const entry *e)
{
    hao_cpu *c = &t->cpu;
    unsigned transfers = (1u << 2) | (1u << 4);
    unsigned writes = transfers | (1u << 0) | (1u << 3) | (1u << 5);
    bool destroy = (e->slots & (1u << 1)) && c->x[1] == (uint64_t)(uintptr_t)e->type;
    bool transfer = (e->slots & writes) && c->x[2] == (uint64_t)(uintptr_t)e->type;
    if (!destroy && !transfer) return false;
    /* A buffer witness has a different storage ABI. Refuse aliases before
     * reading any value bytes, including when the later walk has no nil. */
    if ((e->slots & (1u << 0)) || (destroy && (transfer || e->slots != (1u << 1))))
        refuse(e->type,"unsupported buffer or ambiguous witness ABI");
    void *source = (void *)(uintptr_t)c->x[destroy ? 0 : 1];
    void *destination = (void *)(uintptr_t)c->x[0];
    if (!source) refuse(e->type,"null value storage");
    empty_patch patches[64];
    const void *unresolved;
    int count = collect_empty_patches_detail(e->type,source,patches,64,&unresolved);
    if (count < 0) {
        hao_swift_schema_dump(HAO_DIAG,e->type,t->swift_values->plan->type,unresolved,c->x[30],e->pc);
        refuse(unresolved,"unresolved active representation");
    }
    bool foreign = false;
    for (int i = 0; i < count; ++i) foreign |= patches[i].bits != patches[i].receiver_bits;
    bool assignment = (e->slots & ((1u << 3) | (1u << 5))) != 0;
    if (assignment) {
        empty_patch old[64];
        int n = collect_empty_patches_detail(e->type,destination,old,64,&unresolved);
        if (n < 0) {
            hao_swift_schema_dump(HAO_DIAG,e->type,t->swift_values->plan->type,unresolved,c->x[30],e->pc);
            refuse(unresolved,"unresolved assignment destination");
        }
        for (int i = 0; i < n; ++i) foreign |= old[i].bits != old[i].receiver_bits;
    }
    if (!foreign) return false;
    if (assignment || (!destroy && (e->slots & ~transfers))) refuse(e->type,"unsupported assignment or ownership alias");
    empty_plan empty;
    bool nil = discover_empty_plan(e->type,&empty);
    if (nil) {
        uint64_t bits; memcpy(&bits,(const char *)source + empty.offset,8);
        nil = empty.consumed == 1 ? bits == 0 : (bits == 1 || bits == 2);
    }
    if (!destroy) {
        uintptr_t d = (uintptr_t)destination, s = (uintptr_t)source;
        if (!d || (d >= s ? d - s : s - d) < vwt(e->type)->size) refuse(e->type,"overlapping initialization");
    }
    if (nil) {
        if (!destroy) memcpy(destination,source,(size_t)vwt(e->type)->size);
    } else {
        const void *storage = enum_storage(e->type);
        enum_case_plan active;
        unsigned slot = destroy ? 1 : e->slots == (1u << 2) ? 2 : e->slots == (1u << 4) ? 4 : 0;
        if (!slot) refuse(e->type,"ambiguous payload ownership");
        if (storage && discover_enum_case(storage,source,&active)) {
            if (!active.payload || !native_function((uint64_t)(uintptr_t)vwt(active.payload)->fn[slot]))
                refuse(e->type,"non-native payload ownership");
            int done = destroy ? destroy_enum_empty(source,storage) : slot == 2 ?
                copy_enum_empty(destination,source,storage,1) : take_enum_empty(destination,source,storage,1);
            if (!done) refuse(e->type,"active payload ownership unavailable");
        } else {
            /* Ordinary BB structs/tuples can borrow their complete storage.
             * The full active walk above supplies the live-field contract. */
            kind_fn *kind = dlsym(RTLD_DEFAULT,"swift_getMetadataKind");
            uintptr_t k = kind(e->type);
            const witnesses *v = vwt(e->type);
            if ((k != 0x200 && k != 0x301) || (v->flags & 0x01900000))
                refuse(e->type,"non-borrowable aggregate ownership");
            void *scratch = NULL;
            size_t alignment = (v->flags & 255) + 1;
            if (alignment < sizeof(void *)) alignment = sizeof(void *);
            if (posix_memalign(&scratch,alignment,(size_t)v->size)) refuse(e->type,"scratch allocation");
            memcpy(scratch,source,(size_t)v->size);
            for (int i = 0; i < count; ++i) {
                uint64_t bits = patches[i].receiver_bits;
                memcpy((char *)scratch + patches[i].offset,&bits,8);
            }
            typedef void SWIFT destroy_fn(void *, const void *);
            typedef void *SWIFT transfer_fn(void *, const void *, const void *);
            if (destroy) ((destroy_fn *)v->fn[1])(scratch,e->type);
            else {
                ((transfer_fn *)v->fn[slot])(destination,scratch,e->type);
                for (int i = 0; i < count; ++i)
                    memcpy((char *)destination + patches[i].offset,&patches[i].bits,8);
            }
            free(scratch); /* borrowed view: original ownership moved/consumed once */
        }
    }
    if (hao_opt.bridge_check) fprintf(HAO_DIAG,"hao: Swift value ownership slot=%u metadata=%p patches=%d\n",
        destroy ? 1 : e->slots == (1u << 4) ? 4 : 2,e->type,count);
    return true;
}
bool hao_swift_value_native(hao_thread *t, uint64_t pc)
{
    hao_swift_value_scope *scope = t->swift_values;
    if (!scope) return false;
    /* Preserve pending ABI across native reflection/tag callbacks. Index only
     * this activation; a nested typed callback owns its own complete plan. */
    hao_cpu saved = t->cpu;
    const plan *p = scope->plan;
    unsigned lo = 0, hi = p->count;
    while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (p->entries[mid].pc < pc) lo = mid + 1; else hi = mid; }
    bool done = false;
    for (unsigned i = lo; i < p->count && p->entries[i].pc == pc; ++i) {
        t->cpu = saved;
        if (handle(t,p->entries + i)) { done = true; break; }
    }
    t->cpu = saved;
    if (done) { t->native_calls++; t->cpu.pc = saved.x[30]; }
    return done;
}
void hao_swift_value_end(hao_thread *t, hao_swift_value_scope *scope)
{
    if (!scope->plan) return;
    if (t->swift_values != scope) refuse(scope->plan->type,"activation lifetime mismatch");
    hao_cpu saved = t->cpu;
    empty_patch patches[64];
    const void *unresolved;
    int count = collect_empty_patches_detail(scope->plan->type,scope->output,patches,64,&unresolved);
    t->cpu = saved;
    if (count < 0) {
        hao_swift_schema_dump(HAO_DIAG,scope->plan->type,scope->plan->type,unresolved,saved.x[30],0);
        refuse(unresolved,"result publication has unresolved active representation");
    }
    unsigned changed = 0;
    for (int i = 0; i < count; ++i) {
        uint64_t bits = patches[i].receiver_bits;
        changed += bits != patches[i].bits;
        memcpy((char *)scope->output + patches[i].offset,&bits,8);
    }
    if (hao_opt.bridge_check) fprintf(HAO_DIAG,"hao: Swift value published metadata=%p patches=%d changed=%u\n",scope->plan->type,count,changed);
    t->swift_values = scope->previous;
}
