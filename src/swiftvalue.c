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
#include "hao/gmap.h"
#include "hao/gen_stamp.h"
#include "hao/dlsym_once.h"
#define HAO_SWIFT_VALUE_RUNTIME 1
#include "swiftvalue/Aggregate.h"
#include "swiftvalue/Common.h"
#include "swiftvalue/NativeLeaf.h"
#include "swiftvalue/Tables.h"
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
    bool uncertifiable;   /* the type graph is past the walk's bounds: every value of it is refused */
    unsigned count;
    entry *entries;
    struct hao_swift_value_plan *next;
} plan;
typedef struct {
    kind_fn *kind; count_fn *count; offset_fn *offset; field_fn *field;
    node nodes[NODE_LIMIT]; entry entries[ENTRY_LIMIT];
    unsigned nnode, nentry;
    bool failed, existentials;
    bool converts;   /* some node has a nil whose encoding differs (a conversion the walk knows) */
} builder;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t witness_setup_lock = PTHREAD_MUTEX_INITIALIZER;   /* guest calls of witnesses, below */
static plan *plans;
static const witnesses *vwt(const void *type) { return ((const witnesses *const *)type)[-1]; }
static bool native_function(uint64_t pc)
{
    return pc && !hao_is_trap(pc) && !hao_image_for_code(pc) && !hao_guest_of_entry(pc);
}
void hao_swift_value_fork_child(void)
{
    lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    witness_setup_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    hao_sv_native_leaf_fork_child();
    tables_fork_child();
}
typedef struct { const char *text; uintptr_t length; } type_name;
static type_name name_of(const void *type)
{
    typedef type_name SWIFT name_fn(const void *, bool);
    name_fn *name = HAO_DLSYM("swift_getTypeName");
    type_name n = type && name ? name(type,true) : (type_name){"?",1};
    if (n.length > 4096) n.length = 4096;
    return n;
}
static void refuse(const void *type, const char *why)
{
    hao_diag_prefix(HAO_DIAG);
    type_name n = name_of(type);
    fprintf(HAO_DIAG,"Swift value boundary refused: %s (metadata=%p size=%llu flags=%#x type=%.*s)\n",
        why,type,(unsigned long long)vwt(type)->size,vwt(type)->flags,(int)n.length,n.text);
    abort();
}

/* The boundary policy. A value crossing to native code is converted (its
 * nils given the receiving encoding) or passed as found only where the walk
 * certifies every part of it: each part then is a patched nil or in one of
 * the pass classes of Recipe.h (HAO_SV_PASS_IDENTICAL, _INACTIVE, _OWNED),
 * which pass_note names under HAO_BRIDGE_CHECK. Everything else is refused
 * (decline): an active representation the walk has no rule for, a layout,
 * spare-bit or extra-inhabitant rule that differs by architecture, an
 * ambiguous witness ABI or ownership, an unresolved assignment destination,
 * invalid or overlapping storage, and a failed allocation a required
 * conversion needs. Passing such a value on is not "at worst a low-address
 * crash": a nil of one encoding can be another valid value or case of the
 * other, and a wrong case releases what its owner releases again.
 *
 * Refused, hao prints the value's innermost refusing types and its schema
 * under the prefix "Swift value survey:", then "Swift value boundary
 * refused:" and aborts. HAO_SWIFT_VALUE_PASSTHROUGH=1 (open tier; for
 * triage only, accepting memory corruption) restores the earlier pass-
 * through: such a value goes on with the patches of the parts the walk could
 * certify, reported the first time per (type, reason). */
#define SEEN_LIMIT 1024
static struct { const void *type; const char *why; } seen[SEEN_LIMIT];
static unsigned nseen;
static bool first_report(const void *type, const char *why)
{
    bool first = true, full = false;
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < nseen && first; ++i) first = seen[i].type != type || seen[i].why != why;
    if (first) {
        if (nseen < SEEN_LIMIT) { seen[nseen].type = type; seen[nseen].why = why; nseen++; full = nseen == SEEN_LIMIT; }
        else first = false;
    }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    if (full) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG,"Swift value survey: %u types reported; later ones are not\n",SEEN_LIMIT); }
    return first;
}
/* TYPE and WHY name the refusal; WALKED/VALUE, when given, the value whose
 * walk refused it, ROOT the type of the activation, LR and PC the guest call. */
/* Returns only under HAO_SWIFT_VALUE_PASSTHROUGH. */
static void decline(const void *type, const char *why, const void *walked, const void *value,
                    const void *root, uint64_t lr, uint64_t pc)
{
    bool pass = hao_opt.swift_value_passthrough;
    if (pass && !first_report(type,why)) return;
    if (walked && value) {
        empty_patch scratch[64];
        const void *unresolved;
        (void)collect_survey_patches(walked,value,scratch,64,&unresolved);
        hao_swift_schema_dump(HAO_DIAG,walked,root,type,lr,pc);
    }
    if (!pass) refuse(type,why);
    type_name n = name_of(type), r = name_of(root);
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG,"Swift value survey: passed through unpatched (HAO_SWIFT_VALUE_PASSTHROUGH): %s (metadata=%p size=%llu "
        "flags=%#x xi=%#x type=%.*s) root=%.*s guest lr=%#llx pc=%#llx\n",why,type,(unsigned long long)vwt(type)->size,
        vwt(type)->flags,vwt(type)->extra,(int)n.length,n.text,(int)r.length,r.text,(unsigned long long)lr,(unsigned long long)pc);
}
/* Under HAO_BRIDGE_CHECK, the pass classes by which a certified value of
 * TYPE goes on with parts as found, once per (type, class) in a process. */
static void pass_note(const void *type, unsigned passes)
{
    static const char *const names[] = {"identical-representation", "inactive-payload", "owner-opaque"};
    if (!hao_opt.bridge_check) return;
    for (unsigned k = 0; k < 3; ++k) {
        if (!(passes & (1u << k)) || !first_report(type,names[k])) continue;
        type_name n = name_of(type);
        fprintf(HAO_DIAG,"hao: Swift value passed as found: %s (metadata=%p type=%.*s)\n",names[k],type,(int)n.length,n.text);
    }
}
/* The patches of VALUE of TYPE for a conversion; with EXISTENTIALS the walk
 * of a value a guest hands to a native witness. Where the walk cannot
 * certify the value it is refused for WHY; under the experimental pass-
 * through the patches of the parts it could certify are returned instead,
 * so that a refusing field does not drop its siblings' patches. */
static int certified_patches(const void *type, const void *value, empty_patch *p, int existentials,
                             int (*known_layout)(const void *), const char *why, const void *root, uint64_t lr, uint64_t pc)
{
    const void *unresolved = type;
    unsigned passes = 0;
    if (!value) { decline(type,"null value storage",NULL,NULL,root,lr,pc); return 0; }
    int n = collect_value_patches(type,value,p,64,&unresolved,existentials,known_layout,&passes);
    if (n >= 0) { pass_note(type,passes); return n; }
    decline(unresolved ? unresolved : type,why,type,value,root,lr,pc);
    n = collect_partial_patches(type,value,p,64,existentials,known_layout);
    return n < 0 ? 0 : n;
}
/* Whether a value of TYPE needs certification at a boundary: some node of
 * its type graph is not settled (Recipe.h settled_node), its representation
 * not proven the same on both architectures from metadata alone. A graph
 * past the walk's bounds (depth, nodes, fields, size) sets b->failed: such a
 * type is uncertifiable, never unaffected. */
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
    bool converts = (b->existentials ? discover_witness_plan(type,&empty) : discover_empty_plan(type,&empty)) &&
                    empty.consumed == 2;
    converts |= kind == 0x202 && translated_nil_type(type);   /* a spare-bit enum XI that differs (Data?) */
    b->converts |= converts;
    int whole = 0;
    bool settled = settled_node(type,&whole);
    bool affected = converts || !settled;
    if (whole && !converts) {
        /* nothing below needs looking at */
    } else if (kind == 0x200 || kind == 0x301) {
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
/* The plan of TYPE (cached), NULL only when it cannot be built at all. */
static const plan *prepare(const void *type)
{
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (plan *p = plans; p; p = p->next) if (p->type == type) {
        pthread_mutex_unlock(&lock); hao_crit_leave(); return p;
    }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    builder *b = calloc(1,sizeof *b);
    if (!b) return NULL;
    b->kind = HAO_DLSYM("swift_getMetadataKind");
    b->count = HAO_DLSYM("swift_reflectionMirror_recursiveCount");
    b->offset = HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset");
    b->field = HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata");
    bool ok = b->kind && b->count && b->offset && b->field && graph(b,type,0) && !b->failed;
    plan *p = calloc(1,sizeof *p);
    if (!p) { free(b); return NULL; }
    p->type = type;
    p->uncertifiable = !ok;
    if (ok && b->nentry) {
        p->entries = malloc(b->nentry * sizeof(entry));
        if (!p->entries) { free(b); free(p); return NULL; }   /* not cached: a later call tries again */
        p->count = b->nentry;
        memcpy(p->entries,b->entries,p->count * sizeof(entry));
        qsort(p->entries,p->count,sizeof(entry),entry_order);
    }
    free(b);
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (plan *old = plans; old; old = old->next) if (old->type == type) {
        pthread_mutex_unlock(&lock); hao_crit_leave();
        free(p->entries); free(p); return old;
    }
    p->next = plans; plans = p;
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return p;
}
void hao_swift_value_begin(hao_thread *t, hao_swift_value_scope *scope, const void *type, void *output)
{
    if (!scope || hao_opt.no_swift_values) return;
    const plan *p = prepare(type);
    if (!p) { decline(type,"scratch allocation",NULL,NULL,type,t->cpu.x[30],0); return; }
    if (p->uncertifiable) { decline(type,"type graph past the walk's bounds",NULL,NULL,type,t->cpu.x[30],0); return; }
    if (!p->count) return;   /* every node settled, or none with a native witness to intercept */
    *scope = (hao_swift_value_scope){t->swift_values,p,output};
    t->swift_values = scope;
}
/* Only ordinary single-field wrappers with identical storage are transparent.
 * Do not byte-clone the enclosing non-bitwise-borrowable enum. */
static const void *enum_storage(const void *type)
{
    kind_fn *kind = HAO_DLSYM("swift_getMetadataKind");
    count_fn *count = HAO_DLSYM("swift_reflectionMirror_recursiveCount");
    offset_fn *offset = HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata");
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
    const void *root = t->swift_values->plan->type;
#define DECLINE(type,why,walked,value) do { decline(type,why,walked,value,root,c->x[30],e->pc); return false; } while (0)
    if ((e->slots & (1u << 0)) || (destroy && (transfer || e->slots != (1u << 1))))
        DECLINE(e->type,"unsupported buffer or ambiguous witness ABI",NULL,NULL);
    void *source = (void *)(uintptr_t)c->x[destroy ? 0 : 1];
    void *destination = (void *)(uintptr_t)c->x[0];
    if (!source) DECLINE(e->type,"null value storage",NULL,NULL);
    empty_patch patches[64];
    int count = certified_patches(e->type,source,patches,0,NULL,"unresolved active representation",root,c->x[30],e->pc);
    bool foreign = false;
    for (int i = 0; i < count; ++i) foreign |= patches[i].bits != patches[i].receiver_bits;
    bool assignment = (e->slots & ((1u << 3) | (1u << 5))) != 0;
    if (assignment) {
        empty_patch old[64];
        if (!destination) DECLINE(e->type,"null assignment destination",NULL,NULL);
        int n = certified_patches(e->type,destination,old,0,NULL,"unresolved assignment destination",root,c->x[30],e->pc);
        for (int i = 0; i < n; ++i) foreign |= old[i].bits != old[i].receiver_bits;
    }
    if (!foreign) return false;   /* every nil already in the receiving encoding: identical */
    if (assignment || (!destroy && (e->slots & ~transfers))) DECLINE(e->type,"unsupported assignment or ownership alias",NULL,NULL);
    empty_plan empty;
    bool nil = discover_empty_plan(e->type,&empty);
    if (nil) {
        uint64_t bits; memcpy(&bits,(const char *)source + empty.offset,8);
        nil = empty.consumed == 1 ? bits == 0 : (bits == 1 || bits == 2);
    }
    if (!destroy) {
        uintptr_t d = (uintptr_t)destination, s = (uintptr_t)source;
        if (!d || (d >= s ? d - s : s - d) < vwt(e->type)->size) DECLINE(e->type,"overlapping initialization",NULL,NULL);
    }
    if (nil) {
        if (!destroy) memcpy(destination,source,(size_t)vwt(e->type)->size);
    } else {
        const void *storage = enum_storage(e->type);
        enum_case_plan active;
        unsigned slot = destroy ? 1 : e->slots == (1u << 2) ? 2 : e->slots == (1u << 4) ? 4 : 0;
        if (!slot) DECLINE(e->type,"ambiguous payload ownership",NULL,NULL);
        if (storage && discover_enum_case(storage,source,&active)) {
            if (!active.payload || !native_function((uint64_t)(uintptr_t)vwt(active.payload)->fn[slot]))
                DECLINE(e->type,"non-native payload ownership",NULL,NULL);
            int done = destroy ? destroy_enum_empty(source,storage) : slot == 2 ?
                copy_enum_empty(destination,source,storage,1) : take_enum_empty(destination,source,storage,1);
            if (!done) DECLINE(e->type,"active payload ownership unavailable",NULL,NULL);
        } else {
            /* Ordinary BB structs/tuples can borrow their complete storage.
             * The full active walk above supplies the live-field contract. */
            kind_fn *kind = HAO_DLSYM("swift_getMetadataKind");
            uintptr_t k = kind(e->type);
            const witnesses *v = vwt(e->type);
            if ((k != 0x200 && k != 0x301) || (v->flags & 0x01900000))
                DECLINE(e->type,"non-borrowable aggregate ownership",NULL,NULL);
            void *scratch = NULL;
            size_t alignment = (v->flags & 255) + 1;
            if (alignment < sizeof(void *)) alignment = sizeof(void *);
            if (posix_memalign(&scratch,alignment,(size_t)v->size)) DECLINE(e->type,"scratch allocation",NULL,NULL);
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
#undef DECLINE
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
    int count = certified_patches(scope->plan->type,scope->output,patches,0,NULL,
                                  "result publication has unresolved active representation",scope->plan->type,saved.x[30],0);
    t->cpu = saved;
    unsigned changed = 0;
    for (int i = 0; i < count; ++i) {
        uint64_t bits = patches[i].receiver_bits;
        changed += bits != patches[i].bits;
        memcpy((char *)scope->output + patches[i].offset,&bits,8);
    }
    if (hao_opt.bridge_check) fprintf(HAO_DIAG,"hao: Swift value published metadata=%p patches=%d changed=%u\n",scope->plan->type,count,changed);
    t->swift_values = scope->previous;
}

/* ------------------------------ guest calls of Optional and tuple witnesses
 *
 * Guest code that is not specialized (generic code, outlined copies and
 * destroys through metadata from a mangled name) handles an Optional through
 * its value witnesses, which under hao are libswiftCore's x86_64 ones: every
 * Optional instantiation shares them ($sSqwCP, wxx, wcp, wca, wtk, wta, wet,
 * wst, wug, wup, wui; measured on x86_64), and every tuple one of a few
 * tuple_* sets that hand each element to its own witnesses. A nested Optional whose outer nil
 * sits in a pointer's extra inhabitants (`C??`, `[Int]??`, `(any P)??`) is
 * written by guest code inline with the arm64 encoding, inhabitant 1 = the
 * value 1, and the x86_64 witness reads its own, 2: it takes the guest's
 * outer nil for a live payload and copies or destroys garbage, e.g. destroying
 * a lazy `(any P)?` property that was never read.
 *
 * Such a call is recognized by its target, one of those witnesses, and by
 * its metadata argument, whose own table must hold that target in the slot
 * the call is made through. The witness then gets the value with each outer
 * nil in its own encoding (receiver_empty_bits asks the type's witnesses, so
 * a guest payload type's arm64 extra inhabitants are kept): a scratch copy of
 * a borrowed source, the storage itself when the callee owns it. A value it
 * produces gets back the source's bits at those offsets, so a value keeps the
 * encoding it came with (a native-made value passing through guest generic
 * code stays x86_64); a nil it makes from nothing (store or inject a tag) gets
 * the guest's, since guest code made the call. Only the bounded outer-nil
 * words of collect_witness_patches change, never a live payload. A value the
 * walk cannot prove is refused (certified_patches). */
#define WITNESS_MAX 160
static struct { uint64_t pc; unsigned slot; } witness_pc[WITNESS_MAX];
static unsigned witness_count;
uint64_t hao_swift_witness_lo = 1, hao_swift_witness_hi = 0;   /* empty until set up */
static hao_gmap witness_types = HAO_GMAP_INIT(2, 1, 6, 14);       /* metadata -> 1 affected, 2 not */
typedef struct { const void *metadata; uintptr_t state; } SWIFT_RESPONSE;
typedef SWIFT_RESPONSE SWIFT optional_accessor(uintptr_t, const void *);
static optional_accessor *optional_metadata;

/* Array operations get pseudo-slots: (begin, count, T) to destroy,
 * (dest, src, count, T) for the others. */
enum { ARRAY_DESTROY = 20, ARRAY_INIT_COPY, ARRAY_INIT_TAKE, ARRAY_ASSIGN_COPY, ARRAY_ASSIGN_TAKE };
static const struct { const char *name; unsigned slot; } array_ops[] = {
    {"swift_arrayDestroy",ARRAY_DESTROY},
    {"swift_arrayInitWithCopy",ARRAY_INIT_COPY},
    {"swift_arrayInitWithTakeNoAlias",ARRAY_INIT_TAKE},
    {"swift_arrayInitWithTakeFrontToBack",ARRAY_INIT_TAKE},
    {"swift_arrayInitWithTakeBackToFront",ARRAY_INIT_TAKE},
    {"swift_arrayAssignWithCopyNoAlias",ARRAY_ASSIGN_COPY},
    {"swift_arrayAssignWithCopyFrontToBack",ARRAY_ASSIGN_COPY},
    {"swift_arrayAssignWithCopyBackToFront",ARRAY_ASSIGN_COPY},
    {"swift_arrayAssignWithTake",ARRAY_ASSIGN_TAKE},
};
/* The metadata argument of each slot (ValueWitness.def): x1 for destroy,
 * getEnumTag and destructiveProjectEnumData, x3 for
 * storeEnumTagSinglePayload, x2 otherwise; x2 or x3 for an array operation. */
static unsigned witness_metadata_reg(unsigned slot)
{
    if (slot >= ARRAY_DESTROY) return slot == ARRAY_DESTROY ? 2 : 3;
    return slot == 1 || slot == 11 || slot == 12 ? 1 : slot == 7 ? 3 : 2;
}

static void witness_setup_locked(void);
static bool payload_witnesses_added;
static void payload_witnesses_locked(void);
void hao_swift_witness_setup(void)
{
    if ((__atomic_load_n(&witness_count,__ATOMIC_ACQUIRE) && __atomic_load_n(&payload_witnesses_added,__ATOMIC_ACQUIRE)) ||
        hao_opt.no_swift_values) return;
    pthread_mutex_lock(&witness_setup_lock);
    if (!witness_count) witness_setup_locked();
    if (witness_count && !payload_witnesses_added) payload_witnesses_locked();
    pthread_mutex_unlock(&witness_setup_lock);
}

/* The single-payload tag witnesses (getEnumTagSinglePayload, slot 6;
 * storeEnumTagSinglePayload, slot 7) of a payload type whose first extra
 * inhabitant differs between arm64 and x86_64, so that guest generic code
 * asking a type's own witnesses about its Optional's nil (`if case .some =
 * v` for a `T?` with T = Data) gets it in their encoding: Foundation's Data
 * (its representation's spare bits; Recipe.c translated_nil_type). The
 * handler walks the type's Optional, as for an Optional's own witnesses.
 * Added when Foundation is loaded; the readers see a new entry once
 * witness_count includes it. */
static void payload_witnesses_locked(void)
{
    const void *data = HAO_DLSYM("$s10Foundation4DataVN");
    if (!data || !optional_metadata) return;   /* Foundation not loaded yet: a later image retries */
    const void *optional = optional_metadata(0,data).metadata;
    if (!optional || !translated_nil_type(optional)) { __atomic_store_n(&payload_witnesses_added,true,__ATOMIC_RELEASE); return; }
    unsigned n = witness_count;
    uint64_t lo = hao_swift_witness_lo, hi = hao_swift_witness_hi;
    static const unsigned slots[] = {6,7};
    for (unsigned i = 0; i < 2 && n < WITNESS_MAX; ++i) {
        uint64_t pc = (uint64_t)(uintptr_t)((void *const *)vwt(data))[slots[i]];
        if (!native_function(pc)) continue;
        witness_pc[n++] = (typeof(witness_pc[0])){pc,slots[i]};
        if (pc < lo) lo = pc;
        if (pc > hi) hi = pc;
    }
    hao_swift_witness_hi = hi;
    __atomic_store_n(&witness_count,n,__ATOMIC_RELEASE);
    __atomic_store_n(&hao_swift_witness_lo,lo,__ATOMIC_RELEASE);
    __atomic_store_n(&payload_witnesses_added,true,__ATOMIC_RELEASE);
}
static void witness_setup_locked(void)
{
    optional_accessor *opt = HAO_DLSYM("$sSqMa");
    const void *int_type = HAO_DLSYM("$sSiN");
    if (!opt || !int_type) return;   /* no Swift runtime yet: a later image retries */
    typedef SWIFT_RESPONSE SWIFT tuple2_fn(uintptr_t, const void *, const void *, const char *, const void *);
    tuple2_fn *tuple2 = HAO_DLSYM("swift_getTupleTypeMetadata2");
    const void *string_type = HAO_DLSYM("$sSSN");
    if (!tuple2 || !string_type) return;
    const void *one = opt(0,int_type).metadata, *two = one ? opt(0,one).metadata : NULL;
    /* Tuple witnesses (Metadata.cpp tuple_*) are chosen by the tuple's
     * layout (POD, inline storage, bitwise take): take them from the shapes
     * this path converts, a nested Optional of AnyObject (fits a buffer)
     * and of Any (does not), and from String pairs of both sizes. */
    typedef const void *SWIFT existential_fn(uintptr_t, const void *, size_t, const void *);
    existential_fn *existential = HAO_DLSYM("swift_getExistentialTypeMetadata");
    const void *any_object = existential ? existential(0,NULL,0,NULL) : NULL;   /* ProtocolClassConstraint::Class */
    const void *any = existential ? existential(1,NULL,0,NULL) : NULL;          /* ::Any */
    if (!one || !two || !any_object || !any) return;
    const void *any_object2 = opt(0,opt(0,any_object).metadata).metadata, *any2 = opt(0,opt(0,any).metadata).metadata;
    const void *types[] = {one, two,
        tuple2(0,int_type,any_object2,NULL,NULL).metadata, tuple2(0,int_type,any2,NULL,NULL).metadata,
        tuple2(0,int_type,string_type,NULL,NULL).metadata, tuple2(0,string_type,string_type,NULL,NULL).metadata};
    for (unsigned k = 0; k < sizeof types / sizeof *types; ++k) if (!types[k]) return;
    static const unsigned slots[] = {0,1,2,3,4,5,6,7,11,12,13};
    unsigned n = 0; uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned k = 0; k < sizeof types / sizeof *types; ++k)
        for (unsigned i = 0; i < (k < 2 ? 11u : 8u); ++i) {   /* enum witnesses (11-13) only in an enum's table */
            uint64_t pc = (uint64_t)(uintptr_t)((void *const *)vwt(types[k]))[slots[i]];
            if (!native_function(pc)) continue;
            bool dup = false;
            for (unsigned j = 0; j < n; ++j) dup |= witness_pc[j].pc == pc && witness_pc[j].slot == slots[i];
            if (dup || n == WITNESS_MAX) continue;
            witness_pc[n++] = (typeof(witness_pc[0])){pc,slots[i]};
            if (pc < lo) lo = pc;
            if (pc > hi) hi = pc;
        }
    /* The runtime's array operations (Array.cpp), which guest code calls
     * from inlined Array code: each element through its type's witnesses. */
    for (unsigned k = 0; k < sizeof array_ops / sizeof *array_ops; ++k) {
        uint64_t pc = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT,array_ops[k].name);
        if (!native_function(pc) || n == WITNESS_MAX) continue;
        witness_pc[n++] = (typeof(witness_pc[0])){pc,array_ops[k].slot};
        if (pc < lo) lo = pc;
        if (pc > hi) hi = pc;
    }
    if (!n) return;
    optional_metadata = opt;
    hao_swift_witness_hi = hi;
    __atomic_store_n(&witness_count,n,__ATOMIC_RELEASE);
    __atomic_store_n(&hao_swift_witness_lo,lo,__ATOMIC_RELEASE);
}

/* Whether a value of this Optional may hold an outer nil in a pointer's
 * extra inhabitants anywhere in it (graph() with existential payloads). */
/* What a guest call of a native witness on a value of TYPE needs (graph()
 * with existential payloads), cached: */
enum { WITNESS_CONVERTS = 1,    /* certification, and a nil whose encoding differs may need converting */
       WITNESS_SETTLED = 2,     /* nothing: every node settled */
       WITNESS_UNCERTIFIABLE = 3, /* refusal: the type graph is past the walk's bounds */
       WITNESS_CERTIFY = 4 };   /* certification only */
static int witness_state(const void *type)
{
    const uint64_t *e = hao_gmap_get1(&witness_types,(uint64_t)(uintptr_t)type);
    if (e) return (int)e[1];
    builder *b = calloc(1,sizeof *b);
    if (!b) return WITNESS_UNCERTIFIABLE;   /* not cached: a later call tries again */
    b->existentials = true;
    b->kind = HAO_DLSYM("swift_getMetadataKind");
    b->count = HAO_DLSYM("swift_reflectionMirror_recursiveCount");
    b->offset = HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset");
    b->field = HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata");
    bool ok = b->kind && b->count && b->offset && b->field;
    bool affected = ok && graph(b,type,0);
    int state = !ok || b->failed ? WITNESS_UNCERTIFIABLE : !affected ? WITNESS_SETTLED :
                b->converts ? WITNESS_CONVERTS : WITNESS_CERTIFY;
    free(b);
    hao_crit_enter(); pthread_mutex_lock(&lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&witness_types,(uint64_t)(uintptr_t)type,&fresh);
    if (slot && fresh) { slot[1] = (uint64_t)state; hao_gmap_publish1(&witness_types,slot,(uint64_t)(uintptr_t)type); }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return state;
}
/* Whether a value of TYPE is walked at a guest call of a native witness (LR
 * and PC name it). A type past the walk's bounds is refused here. */
static bool witness_affected(const void *type, uint64_t lr, uint64_t pc)
{
    int state = witness_state(type);
    if (state == WITNESS_UNCERTIFIABLE) {
        decline(type,"type graph past the walk's bounds",NULL,NULL,type,lr,pc);
        return false;   /* HAO_SWIFT_VALUE_PASSTHROUGH: unexamined */
    }
    return state != WITNESS_SETTLED;
}

/* The byte ranges in a value of TYPE (at BASE) of the fields whose struct or
 * enum a guest image defines: their value witnesses are guest code, which
 * reads its own encoding inside them, so a nil of an Optional there keeps
 * the guest's bits. Through struct and tuple fields only: an Optional
 * (or a native enum) around a guest type is read by its own native
 * witnesses, also where its nil is a word inside the guest type's bytes
 * (`Leaf?`, Leaf a guest struct holding a class reference). -1 when there
 * are more than CAP or the type is deeper than 16. */
typedef struct { uint64_t lo, hi; } byte_range;
static int guest_ranges(const void *type, uint64_t base, byte_range *out, int n, int cap, int depth)
{
    kind_fn *kind = HAO_DLSYM("swift_getMetadataKind");
    count_fn *count = HAO_DLSYM("swift_reflectionMirror_recursiveCount");
    offset_fn *offset = HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset");
    field_fn *field = HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata");
    if (!kind || !count || !offset || !field || depth > 16) return -1;
    const void *const *m = type;
    uintptr_t k = kind(type);
    if ((k == 0x200 || k == 0x201) && hao_image_for_addr((uint64_t)(uintptr_t)m[1])) {
        if (n >= cap) return -1;
        out[n++] = (byte_range){base, base + vwt(type)->size};
        return n;
    }
    if (k != 0x200 && k != 0x301) return n;   /* an Optional or a native enum: their witnesses are native */
    intptr_t fields = count(type);
    if (fields < 0 || fields > 256) return -1;
    for (intptr_t i = 0; i < fields; ++i) {
        field_info info = {0};
        const void *child = field(type,i,&info);
        if (info.free_name) info.free_name(info.name);
        intptr_t at = offset(type,i);
        if (!child || at < 0) return -1;
        if ((n = guest_ranges(child,base + (uint64_t)at,out,n,cap,depth + 1)) < 0) return -1;
    }
    return n;
}

typedef struct { int count; empty_patch p[64]; } witness_patches;
/* The outer nils of a value native witnesses read. A value the walk cannot
 * prove is refused (certified_patches; LR and PC name the guest call). A nil
 * inside a field of a guest-defined struct or enum is left out: native
 * witnesses hand that field to its own (guest) ones. */
static void witness_walk(const void *type, const void *value, witness_patches *w, const char *why, uint64_t lr, uint64_t pc)
{
    w->count = certified_patches(type,value,w->p,1,NULL,why,type,lr,pc);
    if (w->count <= 0) return;
    byte_range guest[32];
    int ng = guest_ranges(type,0,guest,0,32,0);
    if (ng < 0) {   /* which nils lie in guest-defined fields is not known: none may be rewritten */
        decline(type,"guest-defined fields past the walk's bound",NULL,NULL,type,lr,pc);
        w->count = 0;
        return;
    }
    int kept = 0;
    for (int i = 0; i < w->count; ++i) {
        bool inside = false;
        for (int g = 0; g < ng && !inside; ++g) inside = w->p[i].offset >= guest[g].lo && w->p[i].offset < guest[g].hi;
        if (!inside) w->p[kept++] = w->p[i];
    }
    w->count = kept;
}
/* Rewrite the walked outer nils to the witness's own encoding. */
static void witness_encode(void *value, const witness_patches *w)
{
    for (int i = 0; i < w->count; ++i) memcpy((char *)value + w->p[i].offset,&w->p[i].receiver_bits,8);
}
/* A word of VALUE at AT that a witness wrote as NATIVE and hao set to the
 * guest's ARM bits (hao_swift_witness_result). */
static void guest_bits_note(hao_thread *t, const void *value, const void *at, uint64_t native, uint64_t arm)
{
    unsigned k = t->swift_made_n++ % (sizeof t->swift_made / sizeof *t->swift_made);
    t->swift_made[k].value = (uint64_t)(uintptr_t)value;
    t->swift_made[k].at = (uint64_t)(uintptr_t)at;
    t->swift_made[k].native = native;
    t->swift_made[k].arm = arm;
}
/* A produced value (destination of a copy or take, a projected payload):
 * the source's guest bits again, noted where they differ from the witness's. */
static void witness_restore_produced(hao_thread *t, void *value, const witness_patches *w)
{
    for (int i = 0; i < w->count; ++i) {
        char *at = (char *)value + w->p[i].offset;
        if (w->p[i].bits != w->p[i].receiver_bits) guest_bits_note(t,value,at,w->p[i].receiver_bits,w->p[i].bits);
        memcpy(at,&w->p[i].bits,8);
    }
}
/* A struct whose layout guest code was compiled against: one a guest image
 * defines (its nominal descriptor lies in one). A native module's struct may
 * be resilient (libswiftos OSSignposter): guest code reads it only through its
 * native witnesses, and so does native code holding the enclosing value. */
/* Native structs whose layout their module freezes (@frozen or @_fixed_layout in its interface), so guest
 * code compiled against it may read them inline (frozenstruct_gen.inc). Matched by qualified nominal name:
 * freezing is permanent ABI in a library-evolution module, so a name listed for one build holds for any;
 * the UUID and offset columns are the generator's join and are not used here. */
static const struct { const char *uuid; uint64_t offset; const char *name; } frozen_structs[] = {
#include "frozenstruct_gen.inc"
};
static hao_gmap frozen_types = HAO_GMAP_INIT(2, 1, 6, 14);       /* nominal descriptor -> 1 frozen, 2 not */

static bool native_frozen(const void *type)
{
    const void *const *m = type;
    uint64_t desc = (uint64_t)(uintptr_t)m[1];
    const uint64_t *e = hao_gmap_get1(&frozen_types,desc);
    if (e) return e[1] == 1;
    typedef struct { const char *text; uintptr_t length; } type_name;
    typedef type_name SWIFT name_fn(const void *, bool);
    name_fn *name = HAO_DLSYM("swift_getTypeName");
    type_name n = name ? name(type,true) : (type_name){NULL,0};
    char nominal[512];                                 /* the qualified name without generic arguments */
    size_t w = 0;
    int depth = 0;
    bool ok = n.text && n.length < sizeof nominal;
    for (uintptr_t i = 0; ok && i < n.length; ++i) {
        char ch = n.text[i];
        if (ch == '<') depth++;
        else if (ch == '>') { if (--depth < 0) ok = false; }
        else if (!depth) nominal[w++] = ch;
    }
    bool frozen = false;
    if (ok && !depth) {
        nominal[w] = 0;
        for (size_t k = 0; k < sizeof frozen_structs / sizeof *frozen_structs && !frozen; ++k)
            frozen = !strcmp(frozen_structs[k].name,nominal);
    }
    hao_crit_enter(); pthread_mutex_lock(&lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&frozen_types,desc,&fresh);
    if (slot && fresh) { slot[1] = frozen ? 1 : 2; hao_gmap_publish1(&frozen_types,slot,desc); }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return frozen;
}

static int guest_layout(const void *type)
{
    const void *const *m = type;
    if ((uintptr_t)m[0] != 0x200) return 1;           /* tuples, Optionals: by their elements */
    if (hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return 1;   /* the guest's own struct */
    return native_frozen(type);                       /* a native one: only if its module froze it */
}
/* For the walk (Tables.h): a listed type guest code lays out is a frozen one. */
int native_frozen_type(const void *type) { return native_frozen(type); }

/* A guest struct built with layout-string value witnesses whose layout
 * depends on a resilient field (Foundation's UUID or Date) is laid out at
 * run time: its metadata completion function passes every field's metadata
 * to swift_cvw_initStructMetadataWithLayoutString, a frozen Data's too,
 * and the native runtime sets the struct's extra inhabitant count to its
 * fields' maximum, Data's x86_64 252. The struct's guest-compiled
 * getEnumTagSinglePayload and storeEnumTagSinglePayload witnesses pick the
 * field whose count equals the one they are handed: Data's inline encoding
 * for arm64's 12, else the resilient fields' witnesses, so with 252 they ask
 * a Date for 252 empty cases and read and write a tag byte past the struct.
 * An Optional of the struct is then nil or present at random, and copies
 * and destroys through it release what they never retained. The count is
 * set to arm64's (Recipe.c arm_struct_xi) before the metadata is complete;
 * its witnesses then use Data's arm64 encoding, which only they read. */
void hao_swift_struct_layout_done(void *metadata, uint64_t n, const void *const *fields, const uint8_t *tags)
{
    if (hao_opt.no_swift_values || !metadata || !fields || !tags) return;
    const void *const *m = metadata;
    if ((uintptr_t)m[0] != 0x200 || !hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return;   /* a guest struct */
    witnesses *v = (witnesses *)vwt(metadata);
    uint32_t arm;
    if (!v || !arm_struct_xi(fields, tags, (size_t)n, &arm) || arm == v->extra) return;
    if (hao_opt.bridge_check || hao_verbose)
        fprintf(HAO_DIAG,"hao: Swift struct metadata=%p extra inhabitants %u -> %u (arm64 field layout)\n",
                metadata,v->extra,arm);
    v->extra = arm;
}

/* A nil the witness made: the guest's encoding (the walk's arm64 bits:
 * extra inhabitant consumed - 1 as the value consumed - 1 for a pointer
 * word, arm64's XI of a spare-bit enum for Data?), where guest code can read
 * it inline. Inside a struct of a native module (resilient for all hao knows)
 * the native encoding stays: its readers are native witnesses, also those
 * reached from native code that holds an enclosing guest struct, which no
 * conversion sees. */
static void witness_to_guest(hao_thread *t, const void *type, void *value, const char *why, uint64_t lr, uint64_t pc)
{
    witness_patches w;
    w.count = certified_patches(type,value,w.p,1,guest_layout,why,type,lr,pc);
    for (int i = 0; i < w.count; ++i) {
        char *at = (char *)value + w.p[i].offset;
        if (w.p[i].arm_bits != w.p[i].bits) guest_bits_note(t,value,at,w.p[i].bits,w.p[i].arm_bits);
        memcpy(at,&w.p[i].arm_bits,8);
    }
}

/* A value native code made in guest storage, as the result of a native generic
 * function the guest called (swiftcc.c, native generic results): its outer
 * nils get the guest's encoding where guest code reads them inline, the rule
 * of a nil a witness makes. Only outer-nil words change; a live payload keeps
 * its bits, also one guest code made (a child task's result). */
bool hao_swift_native_result_to_guest(const void *type, void *value)
{
    if (hao_opt.no_swift_values || !type || !value) return false;
    hao_swift_witness_setup();
    hao_thread *t = hao_current();
    if (!t || !optional_metadata || !witness_affected(type,t->cpu.x[30],0)) return false;
    unsigned before = t->swift_made_n;
    witness_to_guest(t,type,value,"native result has unresolved active representation",t->cpu.x[30],0);
    return t->swift_made_n != before;
}

/* The value witnesses (slots 0-5) of TYPE, a native struct or enum whose
 * values guest code builds inline (a View it lent to a native modifier), so
 * that its later guest calls of them (an outlined copy or destroy) convert
 * like those of an Optional's. */
static void witness_add_type(const void *type)
{
    const void *const *m = type;
    uintptr_t kind = (uintptr_t)m[0];
    if ((kind != 0x200 && kind != 0x201) || hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return;
    uint64_t destroy = (uint64_t)(uintptr_t)vwt(type)->fn[1];
    unsigned n = __atomic_load_n(&witness_count,__ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; ++i) if (witness_pc[i].pc == destroy && witness_pc[i].slot == 1) return;
    hao_crit_enter();
    pthread_mutex_lock(&witness_setup_lock);
    n = witness_count;
    uint64_t lo = hao_swift_witness_lo, hi = hao_swift_witness_hi;
    for (unsigned slot = 0; slot <= 5; ++slot) {
        uint64_t pc = (uint64_t)(uintptr_t)vwt(type)->fn[slot];
        if (!native_function(pc)) continue;
        bool dup = false;
        for (unsigned j = 0; j < n; ++j) dup |= witness_pc[j].pc == pc && witness_pc[j].slot == slot;
        if (dup || n == WITNESS_MAX) continue;
        witness_pc[n++] = (typeof(witness_pc[0])){pc,slot};
        if (pc < lo) lo = pc;
        if (pc > hi) hi = pc;
    }
    hao_swift_witness_hi = hi;
    __atomic_store_n(&witness_count,n,__ATOMIC_RELEASE);
    __atomic_store_n(&hao_swift_witness_lo,lo,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&witness_setup_lock);
    hao_crit_leave();
}

/* A native function guest code calls for the first time (bridge.c, a bare
 * address with no exported name): one of the value witnesses (slots 0-5) of
 * a native struct whose module freezes it, called through that struct's
 * metadata, if its metadata argument (x1 for destroy, x2 for the others)
 * is a struct's metadata whose table holds this very function in that slot.
 * Guest code compiled against a frozen struct builds its values inline
 * (an optimized build inlining View.overlay makes `_OverlayModifier<P?>`
 * itself), its nils in the arm64 encoding, and calls the struct's own
 * witnesses through an outlined copy or destroy: those witnesses are added
 * to the ones whose calls convert (hao_swift_witness_native), so this call
 * and later ones, with any metadata whose table holds them, get the value's
 * outer nils in the native encoding. A resilient struct's values are made
 * only by its witnesses, in their own encoding, and are left alone. */
void hao_swift_witness_probe(hao_thread *t, uint64_t pc)
{
    if (hao_opt.no_swift_values || !t || !hao_probe_safe(t) || !native_function(pc)) return;
    hao_cpu *c = &t->cpu;
    for (unsigned reg = 1; reg <= 2; ++reg) {
        uint64_t m = c->x[reg], w[3], fn[6];
        if (m < 0x1000 || (m & 7) || !hao_probe_addr_ok(m - 8)) continue;
        if (!hao_probe_read_words(t,m - 8,w,3) || w[1] != 0x200 || !w[0] || !w[2]) continue;   /* {vwt, kind, descriptor} */
        if ((w[0] & 7) || !hao_probe_addr_ok(w[0]) || !hao_probe_read_words(t,w[0],fn,6)) continue;
        bool slot = false;
        for (unsigned s = 0; s <= 5 && !slot; ++s) slot = fn[s] == pc && (s == 1) == (reg == 1);
        if (!slot || hao_image_for_addr(w[2])) continue;   /* not this witness, or the guest's own struct */
        const void *type = (const void *)(uintptr_t)m;
        if (!native_frozen(type)) return;
        hao_swift_witness_setup();
        witness_add_type(type);
        if (hao_opt.bridge_check)
            fprintf(HAO_DIAG,"hao: Swift value witness of a frozen native struct called by guest code: %#llx metadata=%p\n",
                    (unsigned long long)pc,type);
        return;
    }
}

/* VALUE of TYPE, made by guest code and lent to a native function for the
 * call (the indirect self of a native protocol extension method; swiftcc.c,
 * View modifiers): NULL when it needs no conversion, else a converted copy to
 * lend instead, its outer nils in the native witnesses' encoding (the walk of
 * a value guest code hands to a native witness), for the caller to free after
 * the call. The guest's own value is never written. A copy is a valid borrow
 * only for a bitwise-takable value of at most 16-byte alignment; any other is
 * lent as the guest wrote it, as is one the walk cannot prove. */
void *hao_swift_borrow_to_native(const void *type, const void *value)
{
    if (hao_opt.no_swift_values || !type || !value) return NULL;
    hao_swift_witness_setup();
    if (!optional_metadata || !witness_affected(type,0,0)) return NULL;
    const witnesses *v = vwt(type);
    if (witness_state(type) == WITNESS_CONVERTS) witness_add_type(type);   /* the guest's later copies and destroys of it */
    witness_patches w;
    witness_walk(type,value,&w,"lent value has unresolved active representation",0,0);
    unsigned changed = 0;
    for (int i = 0; i < w.count; ++i) changed += w.p[i].bits != w.p[i].receiver_bits;
    if (hao_opt.bridge_check)
        fprintf(HAO_DIAG,"hao: Swift value lent to native metadata=%p patches=%d changed=%u\n",type,w.count,changed);
    if (w.count <= 0 || !changed) return NULL;   /* its nils already the native encoding: identical */
    if ((v->flags & 0x00100000) || (v->flags & 0xff) > 15 || !v->size) {   /* not bitwise-takable, alignment */
        decline(type,"lent value needs conversion and cannot be borrowed as a copy",NULL,NULL,type,0,0);
        return NULL;
    }
    void *copy = malloc((size_t)v->size);
    if (!copy) { decline(type,"scratch allocation",NULL,NULL,type,0,0); return NULL; }
    memcpy(copy,value,(size_t)v->size);
    witness_encode(copy,&w);
    return copy;
}

/* Guest code that made such a nil may hand the value to native code, which
 * reads it with its witnesses: a closure native code calls (View.overlay's
 * content: () -> V with V an Optional) builds its indirect result in place
 * with the Optional's storeEnumTagSinglePayload. The callback's return is
 * the boundary: each nil made in a value at that address in this activation,
 * still holding the guest bits hao wrote, gets back the bits the native
 * witness wrote. A value made elsewhere and copied there by guest code is
 * not tracked (a copy is the guest's own memcpy, not a witness call). */
void hao_swift_witness_result(hao_thread *t, unsigned mark, uint64_t indirect)
{
    const unsigned ring = sizeof t->swift_made / sizeof *t->swift_made;
    unsigned n = t->swift_made_n;
    if (indirect && n != mark) {
        unsigned from = n - mark > ring ? n - ring : mark;
        unsigned restored = 0;
        for (unsigned i = from; i != n; ++i) {
            unsigned k = i % ring;
            if (t->swift_made[k].value != indirect) continue;
            uint64_t bits;
            memcpy(&bits,(const void *)(uintptr_t)t->swift_made[k].at,8);
            if (bits != t->swift_made[k].arm) continue;
            memcpy((void *)(uintptr_t)t->swift_made[k].at,&t->swift_made[k].native,8);
            restored++;
        }
        if (restored && hao_opt.bridge_check)
            fprintf(HAO_DIAG,"hao: Swift Optional witness nils returned to native: %u at %#llx\n",restored,(unsigned long long)indirect);
    }
    t->swift_made_n = mark;
}

/* Each element's outer nils in the witness's encoding, in place; their
 * original bits recorded by element index and offset (rec, count *nrec). */
typedef struct { uint64_t at, bits; } witness_bits;
static bool witness_encode_elements(const void *type, char *base, uint64_t count, witness_bits **rec, size_t *nrec,
                                    uint64_t lr, uint64_t pc)
{
    uint64_t stride = vwt(type)->stride;
    size_t cap = 0;
    *rec = NULL; *nrec = 0;
    for (uint64_t i = 0; i < count; ++i) {
        witness_patches w;
        witness_walk(type,base + i * stride,&w,"array element has unresolved active representation",lr,pc);
        if (w.count <= 0) continue;
        if (*nrec + (size_t)w.count > cap) {
            size_t ncap = cap ? cap * 2 : 16;
            while (ncap < *nrec + (size_t)w.count) ncap *= 2;
            witness_bits *grown = realloc(*rec,ncap * sizeof **rec);
            /* Earlier elements are already in the witness's encoding: refused, not handed back half converted. */
            if (!grown) { decline(type,"scratch allocation",NULL,NULL,type,lr,pc); free(*rec); *rec = NULL; return false; }
            *rec = grown; cap = ncap;
        }
        for (int k = 0; k < w.count; ++k) (*rec)[(*nrec)++] = (witness_bits){i * stride + w.p[k].offset,w.p[k].bits};
        witness_encode(base + i * stride,&w);
    }
    return true;
}

static bool witness_array(hao_thread *t, uint64_t pc, unsigned slot, const void *type)
{
    hao_cpu *c = &t->cpu;
    if (!witness_affected(type,c->x[30],pc)) return false;
    char *dest = (char *)(uintptr_t)c->x[0], *src = (char *)(uintptr_t)c->x[1];
    uint64_t count = slot == ARRAY_DESTROY ? c->x[1] : c->x[2], stride = vwt(type)->stride, lr = c->x[30];
    if (!count) return false;   /* no element: nothing to convert */
    if (!stride || count > (UINT64_MAX >> 1) / stride) {
        decline(type,"array extent out of range",NULL,NULL,type,lr,pc);
        return false;
    }
    witness_bits *in = NULL, *old = NULL;
    size_t nin = 0, nold = 0;
    char *scratch = NULL;
    if (slot == ARRAY_DESTROY) {
        if (!witness_encode_elements(type,dest,count,&in,&nin,lr,pc)) return false;
    } else {
        if (slot == ARRAY_INIT_COPY || slot == ARRAY_ASSIGN_COPY) {   /* borrowed source: a converted copy */
            scratch = malloc((size_t)(count * stride));
            if (!scratch) { decline(type,"scratch allocation",NULL,NULL,type,lr,pc); return false; }
            memcpy(scratch,src,(size_t)(count * stride));
            src = scratch;
        }
        if ((slot == ARRAY_ASSIGN_COPY || slot == ARRAY_ASSIGN_TAKE) &&
            !witness_encode_elements(type,dest,count,&old,&nold,lr,pc)) { free(scratch); return false; }
        if (!witness_encode_elements(type,src,count,&in,&nin,lr,pc)) { free(scratch); free(old); return false; }
    }
    typedef void SWIFT destroy_fn(void *, size_t, const void *);
    typedef void SWIFT move_fn(void *, void *, size_t, const void *);
    if (slot == ARRAY_DESTROY) ((destroy_fn *)(uintptr_t)pc)(dest,(size_t)count,type);
    else {
        ((move_fn *)(uintptr_t)pc)(dest,src,(size_t)count,type);
        for (size_t k = 0; k < nin; ++k) memcpy(dest + in[k].at,&in[k].bits,8);   /* each source's encoding */
    }
    if (hao_opt.bridge_check)
        fprintf(HAO_DIAG,"hao: Swift array witness op=%u metadata=%p count=%llu patches=%zu\n",slot,type,(unsigned long long)count,nin);
    free(scratch); free(in); free(old);
    t->native_calls++;
    c->pc = c->x[30];
    return true;
}

bool hao_swift_witness_native(hao_thread *t, uint64_t pc)
{
    hao_cpu *c = &t->cpu;
    for (unsigned i = 0; i < witness_count; ++i) {
        if (witness_pc[i].pc != pc) continue;
        unsigned slot = witness_pc[i].slot;
        const void *type = (const void *)(uintptr_t)c->x[witness_metadata_reg(slot)];
        if (slot >= ARRAY_DESTROY) return type && witness_array(t,pc,slot,type);
        if (!type || (uint64_t)(uintptr_t)((void *const *)vwt(type))[slot] != pc) continue;
        /* Single-payload tag witnesses act on an enclosing enum of the
         * type; with one empty case that is the type's own Optional. A
         * tuple's table has no enum witnesses (11-13): only an Optional's
         * pcs carry those slots, and the table check above reads them only
         * for such a pc. */
        const void *walked = type;
        if (slot == 6 || slot == 7) {
            if (c->x[slot == 6 ? 1 : 2] != 1) return false;
            walked = optional_metadata(0,type).metadata;
            if (!walked) { decline(type,"Optional metadata unavailable",NULL,NULL,type,c->x[30],pc); return false; }
        }
        if (!witness_affected(walked,c->x[30],pc)) return false;
        const witnesses *v = vwt(walked);
        if (slot == 0 && (vwt(type)->flags & 0x00020000)) {
            /* An out-of-line buffer holds a box pointer: copying the buffer
             * retains the box, which only the native runtime and the
             * value's own witnesses touch. */
            pass_note(type,HAO_SV_PASS_OWNED);
            return false;
        }
        void *dest = (void *)(uintptr_t)c->x[0], *src = (void *)(uintptr_t)c->x[1];
        bool borrowed_src = slot == 0 || slot == 2 || slot == 3;
        bool two = slot <= 5 && slot != 1;
        _Alignas(16) unsigned char local[512];
        void *scratch = NULL;
        witness_patches in, old;
        in.count = old.count = 0;
        const char *why = "unresolved active representation (guest call of a native witness)";
        uint64_t lr = c->x[30];
        if (slot == 6 || slot == 11) {                        /* borrowed value: x0 */
            witness_walk(walked,dest,&in,why,lr,pc);
            if (in.count > 0) {
                scratch = v->size <= sizeof local ? local : malloc((size_t)v->size);
                if (!scratch) { decline(walked,"scratch allocation",NULL,NULL,walked,lr,pc); return false; }
                memcpy(scratch,dest,(size_t)v->size);
                witness_encode(scratch,&in);
            }
        } else if (two) {
            witness_walk(type,src,&in,why,lr,pc);
            if (slot == 3 || slot == 5) witness_walk(type,dest,&old,"unresolved assignment destination",lr,pc);
            if (in.count > 0 && borrowed_src) {
                scratch = v->size <= sizeof local ? local : malloc((size_t)v->size);
                if (!scratch) { decline(type,"scratch allocation",NULL,NULL,type,lr,pc); return false; }
                memcpy(scratch,src,(size_t)v->size);
                witness_encode(scratch,&in);
            } else witness_encode(src,&in);                    /* owned by the callee */
            witness_encode(dest,&old);                         /* after the last refusal point: never half converted */
        } else if (slot == 1 || slot == 12) {
            witness_walk(type,dest,&in,why,lr,pc);
            witness_encode(dest,&in);
        }
        typedef void *SWIFT ptr3_fn(void *, void *, const void *);
        typedef void SWIFT void2_fn(void *, const void *);
        typedef unsigned SWIFT tag_fn(const void *, const void *);
        typedef unsigned SWIFT single_get_fn(const void *, unsigned, const void *);
        typedef void SWIFT single_store_fn(void *, unsigned, unsigned, const void *);
        typedef void SWIFT inject_fn(void *, unsigned, const void *);
        void *f = (void *)(uintptr_t)pc;
        uint64_t result = c->x[0];
        switch (slot) {
        case 0: case 2: case 3: case 4: case 5:
            result = (uint64_t)(uintptr_t)((ptr3_fn *)f)(dest,scratch ? scratch : src,type);
            witness_restore_produced(t,dest,&in);              /* the source's encoding */
            break;
        case 1: ((void2_fn *)f)(dest,type); break;
        case 6: result = ((single_get_fn *)f)(scratch ? scratch : dest,(unsigned)c->x[1],type); break;
        case 7:   /* case 0 is the payload: no nil is made, its encoding stays */
            ((single_store_fn *)f)(dest,(unsigned)c->x[1],(unsigned)c->x[2],type);
            if ((unsigned)c->x[1]) witness_to_guest(t,walked,dest,"made nil has unresolved active representation",c->x[30],pc);
            break;
        case 11: result = ((tag_fn *)f)(scratch ? scratch : dest,type); break;
        case 12: ((void2_fn *)f)(dest,type); witness_restore_produced(t,dest,&in); break;
        case 13:
            ((inject_fn *)f)(dest,(unsigned)c->x[1],type);
            if ((unsigned)c->x[1]) witness_to_guest(t,type,dest,"made nil has unresolved active representation",c->x[30],pc);
            break;
        default: return false;
        }
        if (scratch && scratch != local) free(scratch);
        if (hao_opt.bridge_check)
            fprintf(HAO_DIAG,"hao: Swift Optional witness slot=%u metadata=%p patches=%d\n",slot,type,in.count);
        c->x[0] = result;
        t->native_calls++;
        c->pc = c->x[30];
        return true;
    }
    return false;
}
