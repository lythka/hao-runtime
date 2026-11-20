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
#include "hao/bridge.h"
#define HAO_SWIFT_VALUE_RUNTIME 1
#include "swiftvalue/Aggregate.h"
#include "swiftvalue/Common.h"
#include "swiftvalue/NativeLeaf.h"
#include "swiftvalue/Tables.h"
#include "swiftvalue/Facts.h"
#include "hao/macho.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    bool invalid;         /* its metadata is invalid (builder invalid): refused even where uncertified values pass */
    unsigned count;
    entry *entries;
    struct hao_swift_value_plan *next;
} plan;
typedef struct {
    kind_fn *kind; count_fn *count; offset_fn *offset; field_fn *field;
    node nodes[NODE_LIMIT]; entry entries[ENTRY_LIMIT];
    unsigned nnode, nentry;
    bool failed, existentials;
    bool invalid;    /* with failed: metadata no valid type has (stride below size, a field outside its parent) */
    bool converts;   /* some node has a nil whose encoding differs (a conversion the walk knows) */
    bool unproven;   /* for one such node the facts table has no case images, or refuses it */
} builder;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t witness_setup_lock = PTHREAD_MUTEX_INITIALIZER;   /* guest calls of witnesses, below */
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;         /* storage destroy hooks, below */
static plan *plans;
static const witnesses *vwt(const void *type) { return ((const witnesses *const *)type)[-1]; }
static bool native_function(uint64_t pc)
{
    return pc && !hao_is_trap(pc) && !hao_image_for_code(pc) && !hao_guest_of_entry(pc);
}
static void storage_fork_child(void);
void hao_swift_value_fork_child(void)
{
    lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    witness_setup_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    storage_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    storage_fork_child();
    hao_sv_native_leaf_fork_child();
    tables_fork_child();
    walk_facts_fork_child();
    recipe_fork_child();
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
/* The survey image's audit (boot argument hao_swift_value_audit=1, every
 * process; HAO_SWIFT_VALUE_AUDIT in an open one), honoured only by a survey
 * build (make SURVEY=1) or a test build; a release build says once that it
 * ignores it and refuses as always. A value hao would refuse goes on as
 * under HAO_SWIFT_VALUE_PASSTHROUGH, accepting the same corruption: a
 * survey image is for listing, not for use. Instead of the survey lines,
 * each innermost refusing type of the value is one line
 *   hao-swift-audit {JSON}
 * the first time its (type, reason) occurs in the process (the reason names
 * the boundary: a result publication, a guest call of a native witness, a
 * lent value, an array element...; "class" groups the reasons). The fields:
 * v (record version, 1), process (its name), reason, class, type (its key in
 * the walk's facts table: the mangled name, private contexts' addresses
 * removed; the table's generator takes these as its input list), kind, size,
 * stride, flags and xi (its value witness table), offset and depth in the
 * refused value, chain (the keys from the value's root down to the parent),
 * walked (the refused value's type), root (the activation's type),
 * caller_image and caller_offset (the guest image's LC_UUID and the call's
 * offset in it), witness_image and witness_offset (the same for the native
 * code called), x86_cache (this process's x86_64 shared cache UUID); in a
 * native call of guest code (a callback: a closure, a witness, an override)
 * also callback_image and callback_offset (the guest function entered: a
 * closure's invoke function), callback_symbol (its own mangled name when the
 * image keeps it: a closure's spells its function type), callback_kind
 * (witness, override or "") and callback_descriptor (the native requirement's
 * or overridden method's descriptor, ...Tq: an override's vtable slot by
 * name). No address and no path. The records go where hao's diagnostics go: with
 * hao_crash=1 the owner-only console.log under /private/var/log/hao (a
 * user's processes: their directory under it), else stderr; from a
 * sandboxed process the unified log (subsystem com.hao, category diag), as
 * private records. */
static const char *refusal_class(const char *why)
{
    if (strstr(why,"misread") || strstr(why,"cannot be converted")) return "known-wrong";
    if (strstr(why,"representation")) return "representation";
    if (strstr(why,"assignment") || strstr(why,"ownership") || strstr(why,"borrowed")) return "ownership";
    if (strstr(why,"allocation")) return "allocation";
    if (strstr(why,"storage") || strstr(why,"extent") || strstr(why,"overlap")) return "storage";
    if (strstr(why,"bound")) return "walk-limit";
    return "abi";
}
static void json_text(FILE *f, const char *text, uint64_t n)
{
    fputc('"',f);
    for (uint64_t i = 0; text && i < n && i < 2048; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') fprintf(f,"\\%c",c);
        else if (c < 0x20 || c >= 0x7f) fprintf(f,"\\u%04x",c);
        else fputc(c,f);
    }
    fputc('"',f);
}
/* TYPE's table key (Facts.h walk_fact_key): the mangled name with every
 * private context's address removed, so a record names no address. */
static void json_mangled(FILE *f, const void *type)
{
    uint64_t n = 0;
    char key[2048];
    const char *m = walk_fact_mangled(type,&n);
    if (!m || !walk_fact_key(m,n,key,sizeof key)) key[0] = 0;
    json_text(f,key,strlen(key));
}
static void json_uuid(FILE *f, const uint8_t *uuid)
{
    fputc('"',f);
    for (unsigned k = 0; uuid && k < 16; ++k) fprintf(f,"%02X",uuid[k]);
    fputc('"',f);
}
/* The LC_UUID of the native image that holds PC, and PC's offset from its header. */
static bool native_image_of(uint64_t pc, uint8_t uuid[16], uint64_t *offset)
{
    Dl_info info;
    if (!pc || !dladdr((const void *)(uintptr_t)pc,&info) || !info.dli_fbase) return false;
    const struct mach_header_64 *h = info.dli_fbase;
    if (h->magic != MH_MAGIC_64 || h->sizeofcmds > (1u << 24)) return false;
    const uint8_t *p = (const uint8_t *)(h + 1), *end = p + h->sizeofcmds;
    for (uint32_t k = 0; k < h->ncmds && p + sizeof(struct load_command) <= end; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > (uint64_t)(end - p)) return false;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            memcpy(uuid,((const struct uuid_command *)(const void *)lc)->uuid,16);
            *offset = pc - (uint64_t)(uintptr_t)info.dli_fbase;
            return true;
        }
        p += lc->cmdsize;
    }
    return false;
}
/* The native function the guest is calling (hao_thread native_target), by
 * name for an audit record: the call map's symbol for it (swiftcall.c), else
 * its exported name, else the method descriptor ("...Tq") whose
 * implementation it is (a vtable call reaches an implementation the library
 * need not export). "" when none is found. */
typedef struct { uint64_t impl; char name[1024]; } callee_search;
static void callee_visit(void *context, const char *name, const void *address)
{
    callee_search *s = context;
    size_t n = strlen(name);
    int32_t rel;
    if (s->name[0] || n < 4 || n >= sizeof s->name || strcmp(name + n - 2,"Tq") || strncmp(name,"_$s",3)) return;
    memcpy(&rel,(const char *)address + 4,4);   /* MethodDescriptor {Flags, Impl (relative direct)} */
    if (rel && (uint64_t)(uintptr_t)address + 4 + (uint64_t)(int64_t)rel == s->impl) memcpy(s->name,name + 1,n);
}
static void json_callee(FILE *f, uint64_t callee)
{
    const char *name = callee ? hao_swiftcall_symbol_at(callee) : NULL;
    Dl_info info;
    callee_search search = {callee, ""};
    if (!name && callee && dladdr((const void *)(uintptr_t)callee,&info) && info.dli_sname &&
        (uint64_t)(uintptr_t)info.dli_saddr == callee) name = info.dli_sname;
    if (!name && callee && hao_bridge_native_exports((const void *)(uintptr_t)callee,callee_visit,&search) && search.name[0])
        name = search.name;
    if (name && name[0] == '_') ++name;
    json_text(f,name ? name : "",name ? strlen(name) : 0);
}
/* The callback fields of an audit record (see above) for the guest function at GUEST. */
static void json_callback(FILE *f, uint64_t guest)
{
    hao_image *img = hao_image_for_addr(guest);
    bool override = false;
    uint64_t d = hao_swiftmeta_callee_descriptor(guest,&override);
    const char *own = hao_guest_symbol_at(guest);
    if (own && own[0] == '_') ++own;
    fprintf(f,",\"callback_image\":");
    json_uuid(f,img && img->has_uuid ? img->uuid : NULL);
    fprintf(f,",\"callback_offset\":%llu,\"callback_symbol\":",(unsigned long long)(img ? guest - img->base : 0));
    json_text(f,own ? own : "",own ? strlen(own) : 0);
    fprintf(f,",\"callback_kind\":\"%s\",\"callback_descriptor\":",d ? (override ? "override" : "witness") : "");
    Dl_info info;
    const char *name = d && dladdr((const void *)(uintptr_t)d,&info) && info.dli_sname &&
                       (uint64_t)(uintptr_t)info.dli_saddr == d ? info.dli_sname : NULL;
    if (name && name[0] == '_') ++name;
    json_text(f,name ? name : "",name ? strlen(name) : 0);
}
/* One audit record (see above). Types by their table keys, code by image
 * UUID and offset: no address, no path. TYPE NULL: a value the call map
 * (swiftcall.c) names by KEY, or a whole call (KEY NULL). */
static void audit_record(const void *type, const char *key, const char *why, uint64_t offset, unsigned depth,
                         const void *const *chain, const void *walked, const void *root, uint64_t lr, uint64_t pc,
                         bool once)
{
    if (once && !first_report(type ? type : key ? (const void *)key : (const void *)(uintptr_t)pc,why)) return;
    FILE *f = HAO_DIAG;
    static const witnesses none;
    const witnesses *v = type ? vwt(type) : &none;
    typedef uintptr_t SWIFT kind_fn(const void *);
    kind_fn *kind = HAO_DLSYM("swift_getMetadataKind");
    hao_image *img = lr ? hao_image_for_addr(lr) : NULL;
    uint8_t witness[16], callee_uuid[16];
    uint64_t witness_offset = 0, callee_offset = 0;
    bool native = native_image_of(pc,witness,&witness_offset);
    hao_thread *t = hao_current_peek();
    uint64_t callee = t && t->native_target ? t->native_target : pc;
    bool callee_native = native_image_of(callee,callee_uuid,&callee_offset);
    extern bool _dyld_get_shared_cache_uuid(unsigned char uuid[16]);
    unsigned char cache[16] = {0};
    (void)_dyld_get_shared_cache_uuid(cache);
    flockfile(f);
    fprintf(f,"hao-swift-audit {\"v\":1,\"process\":");
    json_text(f,getprogname(),strlen(getprogname()));
    fprintf(f,",\"reason\":");
    json_text(f,why,strlen(why));
    fprintf(f,",\"class\":\"%s\",\"type\":",refusal_class(why));
    if (type) json_mangled(f,type);
    else json_text(f,key ? key : "",key ? strlen(key) : 0);
    fprintf(f,",\"kind\":%lu,\"size\":%llu,\"stride\":%llu,\"flags\":%u,\"xi\":%u,\"offset\":%llu,\"depth\":%u,\"chain\":[",
        kind && type ? (unsigned long)kind(type) : 0ul,(unsigned long long)v->size,(unsigned long long)v->stride,v->flags,v->extra,
        (unsigned long long)offset,depth);
    for (unsigned k = 0; chain && k < depth && k < HAO_SV_AUDIT_CHAIN; ++k) {
        if (k) fputc(',',f);
        json_mangled(f,chain[k]);
    }
    fprintf(f,"],\"walked\":");
    if (walked || type) json_mangled(f,walked ? walked : type);
    else json_text(f,key ? key : "",key ? strlen(key) : 0);
    fprintf(f,",\"root\":");
    if (root || type) json_mangled(f,root ? root : type);
    else json_text(f,key ? key : "",key ? strlen(key) : 0);
    fprintf(f,",\"caller_image\":");
    json_uuid(f,img && img->has_uuid ? img->uuid : NULL);
    fprintf(f,",\"caller_offset\":%llu,\"witness_image\":",(unsigned long long)(img ? lr - img->base : 0));
    json_uuid(f,native ? witness : NULL);
    fprintf(f,",\"witness_offset\":%llu,\"callee_symbol\":",(unsigned long long)witness_offset);
    json_callee(f,callee_native ? callee : 0);
    fprintf(f,",\"callee_image\":");
    json_uuid(f,callee_native ? callee_uuid : NULL);
    fprintf(f,",\"callee_offset\":%llu,\"x86_cache\":",(unsigned long long)callee_offset);
    json_uuid(f,cache);
    if (t && t->callback_guest) json_callback(f,t->callback_guest);
    fprintf(f,"}\n");
    funlockfile(f);
}
static void audit(const void *type, const char *why, const void *walked, const void *value, int existentials,
                  const void *root, uint64_t lr, uint64_t pc, bool once)
{
    audit_entry entries[8];
    unsigned found = 0;
    if (walked && value) {
        empty_patch scratch[64];
        (void)collect_audit_patches(walked,value,scratch,64,existentials,entries,8,&found);
    }
    for (unsigned i = 0; i < found; ++i)
        audit_record(entries[i].type,NULL,why,entries[i].offset,entries[i].depth,entries[i].chain,walked,root,lr,pc,once);
    if (!found) audit_record(type,NULL,why,0,0,NULL,walked,root,lr,pc,once);
}

/* Whether WHY means only that no row or rule proves the value either way
 * (v0.0.1 policy, docs/ISSUES.md): the walk's bounds, an active
 * representation it has no rule for, a type not settled at a destruction
 * hao cannot hook. Such a value is reported and passed as found, as before
 * the refusal policy, unless HAO_SWIFT_VALUE_STRICT (tests). Every other
 * reason is a value hao knows native code would misread (a nil it must
 * convert and cannot), malformed storage, or a failed allocation a
 * conversion needs: those stay refused. */
static bool uncertified(const char *why)
{
    static const char *const reasons[] = {
        "type graph past the walk's bounds",
        "guest-defined fields past the walk's bound",
        "box of a type that is not settled cannot be certified at its destruction",
        "array storage of a type that is not settled cannot be certified at its destruction",
        "child task result is copied by native code before it can be certified",
    };
    for (unsigned k = 0; k < sizeof reasons / sizeof reasons[0]; ++k) if (!strcmp(why,reasons[k])) return true;
    return strstr(why,"unresolved") != NULL;   /* "... unresolved active representation ...", "unresolved assignment destination" */
}

/* TYPE and WHY name the refusal; WALKED/VALUE, when given, the value whose
 * walk refused it, ROOT the type of the activation, LR and PC the guest call. */
/* Returns only under HAO_SWIFT_VALUE_PASSTHROUGH, the audit, or for an
 * uncertified reason (report-only). */
static void decline_walk(const void *type, const char *why, const void *walked, const void *value, int existentials,
                         const void *root, uint64_t lr, uint64_t pc)
{
    if (hao_opt.swift_value_audit) { audit(type,why,walked,value,existentials,root,lr,pc,true); return; }
    if (hao_opt.swift_value_audit_ignored && first_report(NULL,"audit ignored")) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG,"hao_swift_value_audit ignored: this libhao is not a survey build (make SURVEY=1)\n");
    }
    bool pass = hao_opt.swift_value_passthrough;
    /* Known-wrong values are reported too: their rules (malformed tags, nils
     * with no encoding, flags from a partial walk) are not yet proven against
     * a crash, and refusing them aborted applications that ran before. */
    bool known_wrong = !strcmp(why,"value native code would misread") ||
                       !strcmp(why,"guest-encoded nil in a shared box cannot be converted") ||
                       !strcmp(why,"child task result has a nil native code would copy in the guest's encoding");
    if (!pass && !hao_opt.swift_value_strict && (uncertified(why) || known_wrong)) {
        if (!first_report(type,why)) return;
        hao_crit_enter();   /* no guest signal handler runs inside the report */
        audit(type,why,walked,value,existentials,root,lr,pc,false);
        type_name n = name_of(type), r = name_of(root);
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG,"Swift value survey: not certified, passed as found (report-only): %s (metadata=%p size=%llu "
            "flags=%#x xi=%#x type=%.*s) root=%.*s guest lr=%#llx pc=%#llx\n",why,type,(unsigned long long)vwt(type)->size,
            vwt(type)->flags,vwt(type)->extra,(int)n.length,n.text,(int)r.length,r.text,(unsigned long long)lr,(unsigned long long)pc);
        hao_crit_leave();
        return;
    }
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
static void decline(const void *type, const char *why, const void *walked, const void *value,
                    const void *root, uint64_t lr, uint64_t pc)
{
    decline_walk(type,why,walked,value,0,root,lr,pc);
}
/* A mapped Swift call (swiftcall.c) the policy refuses: the value of TYPE, or
 * of KEY that does not resolve (TYPE NULL), or the whole call (both NULL),
 * WHY, SYMBOL the call map's function. Refused as decline does, or recorded
 * by the audit; returns only under the audit or HAO_SWIFT_VALUE_PASSTHROUGH. */
void hao_swift_call_refused(const void *type, const char *key, const char *why, const char *symbol, uint64_t lr, uint64_t pc)
{
    if (type) { decline(type,why,NULL,NULL,type,lr,pc); return; }
    if (hao_opt.swift_value_audit) { audit_record(NULL,key,why,0,0,NULL,NULL,NULL,lr,pc,true); return; }
    if (hao_opt.swift_value_audit_ignored && first_report(NULL,"audit ignored")) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG,"hao_swift_value_audit ignored: this libhao is not a survey build (make SURVEY=1); refusing\n");
    }
    bool pass = hao_opt.swift_value_passthrough;
    /* Only a row's REFUSE verdict refuses: a row this runtime cannot apply
     * leaves the call as without a row, reported once (v0.0.1 report-only
     * policy), unless HAO_SWIFT_VALUE_STRICT (tests). */
    bool report_only = !pass && !hao_opt.swift_value_strict && strcmp(why,"a value the call map refuses") &&
                       strcmp(why,"a call the call map refuses");
    if ((pass || report_only) && !first_report(key ? (const void *)key : (const void *)symbol,why)) return;
    hao_diag_prefix(HAO_DIAG);
    fprintf(HAO_DIAG,"Swift value boundary %s: %s (call=%s%s%s guest lr=%#llx)\n",
        pass ? "passed through unconverted (HAO_SWIFT_VALUE_PASSTHROUGH)" :
        report_only ? "not converted, passed as found (report-only)" : "refused",why,symbol ? symbol : "?",
        key ? " type=" : "",key ? key : "",(unsigned long long)lr);
    if (!pass && !report_only) abort();
}

static int certified_patches(const void *type, const void *value, empty_patch *p, unsigned cap, int existentials,
                             int (*known_layout)(const void *), const char *why, const void *root, uint64_t lr, uint64_t pc);
/* A value of TYPE a mapped Swift call (swiftcall.c) passes in registers:
 * VALUE, its words (BYTES) as the sending side's registers held them, in that
 * side's encoding, rewritten to the receiving side's (TO_GUEST: native code
 * made it). Only the walk's certified patches are written; a value it cannot
 * certify, or whose size the words do not hold, is refused (decline). */
void hao_swift_call_value(const void *type, void *value, uint64_t bytes, bool to_guest, const char *symbol,
                          uint64_t lr, uint64_t pc)
{
    if (hao_opt.no_swift_values || !type || !value) return;
    const witnesses *v = vwt(type);
    if (v->size > bytes || v->size + 8 <= bytes) {
        hao_swift_call_refused(type,NULL,"Swift call value's size is not its register words",symbol,lr,pc);
        return;
    }
    empty_patch p[64];
    int n = certified_patches(type,value,p,64,to_guest ? HAO_SV_SOURCE_NATIVE : HAO_SV_SOURCE_GUEST,NULL,
                              to_guest ? "Swift call result has unresolved active representation" :
                                         "Swift call argument has unresolved active representation",type,lr,pc);
    for (int i = 0; i < n; ++i)
        if (p[i].offset + 8 <= bytes) memcpy((char *)value + p[i].offset,to_guest ? &p[i].arm_bits : &p[i].receiver_bits,8);
    if (hao_opt.bridge_check)
    {
        type_name tn = name_of(type);
        fprintf(HAO_DIAG,"hao: Swift call %s %s %.*s: %d patches\n",symbol ? symbol : "?",to_guest ? "result" : "argument",
            (int)tn.length,tn.text,n);
    }
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
static int certified_patches(const void *type, const void *value, empty_patch *p, unsigned cap, int existentials,
                             int (*known_layout)(const void *), const char *why, const void *root, uint64_t lr, uint64_t pc)
{
    const void *unresolved = type;
    unsigned passes = 0;
    if (!value) { decline(type,"null value storage",NULL,NULL,root,lr,pc); return 0; }
    int n = collect_value_patches(type,value,p,cap,&unresolved,existentials,known_layout,&passes);
    if (n >= 0) { pass_note(type,passes); return n; }
    /* The walk stops at its first failure; the partial walk goes on past it
     * and may find a known-wrong part beyond. */
    unsigned rest = 0;
    n = collect_partial_patches(type,value,p,cap,existentials,known_layout,&rest);
    passes |= rest;
    if (passes & HAO_SV_UNCONVERTIBLE) why = "guest-encoded nil in a shared box cannot be converted";   /* known wrong */
    else if (passes & HAO_SV_KNOWN_WRONG) why = "value native code would misread";   /* Recipe.h HAO_SV_KNOWN_WRONG */
    decline_walk(unresolved ? unresolved : type,why,type,value,existentials,root,lr,pc);
    /* The patches come from a walk after the report: reporting names the
     * types it walked, which completes metadata the first walk found
     * incomplete, and the walk after it then finds the nils to convert. A
     * value passed as found with them unconverted is misread by native code
     * (a guest nil where native code reads a count or a pointer). */
    int after = collect_partial_patches(type,value,p,cap,existentials,known_layout,NULL);
    if (after != n && hao_verbose && first_report(type,"partial walk after the report"))
        HAO_LOG(1, "Swift value: %d nils to convert after the report, %d before it (metadata=%p)\n", after, n, type);
    n = after;
    return n < 0 ? 0 : n;
}
/* Whether a value of TYPE needs certification at a boundary: some node of
 * its type graph is not settled (Recipe.h settled_node), its representation
 * not proven the same on both architectures from metadata alone. A graph
 * past the walk's bounds (depth, nodes, fields, size) sets b->failed: such a
 * type is uncertifiable, never unaffected. */
static bool graph(builder *b, const void *type, unsigned depth)
{
    if (!type) { b->failed = b->invalid = true; return false; }
    if (depth > 32 || b->nnode == NODE_LIMIT) { b->failed = true; return false; }
    for (unsigned i = 0; i < b->nnode; ++i) if (b->nodes[i].type == type) return b->nodes[i].affected;
    unsigned at = b->nnode++;
    b->nodes[at].type = type;
    const witnesses *v = vwt(type);
    if (v->stride < v->size) { b->failed = b->invalid = true; return false; }
    if (v->size > 4096) { b->failed = true; return false; }
    if (v->flags & 0x00800000) {   /* IsNonCopyable: settled only by a SAME row */
        walk_fact f;
        if (walk_fact_of(type,&f) && f.verdict == WALK_FACT_SAME) { b->nodes[at].affected = false; return false; }
        b->failed = true; return false;
    }
    uintptr_t kind = b->kind(type);
    /* At a guest call of a native witness, a guest type its own (guest)
     * witnesses handle whole is the walk's owner-opaque class. */
    if (b->existentials && guest_owned_type(type)) { b->nodes[at].affected = false; return false; }
    empty_plan empty;
    bool converts = (b->existentials ? discover_witness_plan(type,&empty) : discover_empty_plan(type,&empty)) &&
                    empty.consumed == 2;
    converts |= kind == 0x202 && translated_nil_type(type);   /* a spare-bit enum XI that differs (Data?) */
    converts &= !xi_native_owned(type);                        /* nils only native witnesses write */
    b->converts |= converts;
    walk_fact fact;
    if (converts && walk_fact_of(type,&fact) &&
        (fact.verdict == WALK_FACT_REFUSE || (fact.verdict == WALK_FACT_CONVERT && !fact.nimages && !fact.guest)))
        b->unproven = true;
    int whole = 0;
    bool settled = settled_node(type,&whole);
    bool affected = converts || !settled;
    if (whole && !converts) {
        /* nothing below needs looking at */
    } else if (kind == 0x200 || kind == 0x301) {
        intptr_t count = b->count(type);
        if (count < 0 || count > HAO_SV_FIELD_LIMIT) { b->failed = true; return false; }
        if (!count && v->size) affected = true;   /* fields not reflected: only the walk can tell (it refuses) */
        for (intptr_t i = 0; i < count; ++i) {
            field_info info = {0};
            const void *child = b->field(type,i,&info);
            intptr_t offset = b->offset(type,i);
            if (info.free_name) info.free_name(info.name);
            if (!child || offset < 0 || (uint64_t)offset > v->size || vwt(child)->size > v->size - (uint64_t)offset) {
                b->failed = b->invalid = true; return false;
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
    b->count = reflect_count_fn();
    b->offset = reflect_offset_fn();
    b->field = reflect_field_fn();
    bool syms = b->kind && b->count && b->offset && b->field;
    bool affected = syms && graph(b,type,0);
    plan *p = calloc(1,sizeof *p);
    if (!p) { free(b); return NULL; }
    p->type = type;
    p->uncertifiable = !syms || b->failed;
    p->invalid = !syms || b->invalid;
    if (!p->uncertifiable && affected && b->nentry) {
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
    if (p->uncertifiable) {
        decline(type,p->invalid ? "type graph has invalid metadata" : "type graph past the walk's bounds",NULL,NULL,type,t->cpu.x[30],0);
        return;
    }
    if (!p->count) return;   /* every node settled, or none with a native witness to intercept */
    *scope = (hao_swift_value_scope){t->swift_values,p,output};
    t->swift_values = scope;
}
/* Only ordinary single-field wrappers with identical storage are transparent.
 * Do not byte-clone the enclosing non-bitwise-borrowable enum. */
static const void *enum_storage(const void *type)
{
    kind_fn *kind = HAO_DLSYM("swift_getMetadataKind");
    count_fn *count = reflect_count_fn();
    offset_fn *offset = reflect_offset_fn();
    field_fn *field = reflect_field_fn();
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
    const void *root = t->swift_values->plan->type;
#define DECLINE(type,why,walked,value) do { decline(type,why,walked,value,root,c->x[30],e->pc); return false; } while (0)
    /* initializeBufferWithCopyOfBuffer(dest, src, type) (ValueWitness.def):
     * a type stored inline in a ValueBuffer is copied as initializeWithCopy
     * does; an out-of-line one (IsNonInline) has a box reference in each
     * buffer, which the witness copies and retains, sharing the box: no
     * value bytes move and none are read, so there is nothing to convert. */
    entry view = *e;
    if (!destroy && (view.slots & (1u << 0))) {
        if (vwt(e->type)->flags & 0x00020000) {
            if (view.slots == (1u << 0)) { pass_note(e->type,HAO_SV_PASS_OWNED); return false; }
        } else view.slots = (view.slots & ~(1u << 0)) | (1u << 2);
    }
    e = &view;
    /* Refuse other buffer and ambiguous aliases before reading any value
     * bytes, including when the later walk has no nil. */
    if ((e->slots & (1u << 0)) || (destroy && (transfer || e->slots != (1u << 1))))
        DECLINE(e->type,"unsupported buffer or ambiguous witness ABI",NULL,NULL);
    void *source = (void *)(uintptr_t)c->x[destroy ? 0 : 1];
    void *destination = (void *)(uintptr_t)c->x[0];
    if (!source) DECLINE(e->type,"null value storage",NULL,NULL);
    empty_patch patches[64];
    int count = certified_patches(e->type,source,patches,64,HAO_SV_SOURCE_GUEST,NULL,"unresolved active representation",root,c->x[30],e->pc);
    bool foreign = false;
    for (int i = 0; i < count; ++i) foreign |= patches[i].bits != patches[i].receiver_bits;
    bool assignment = (e->slots & ((1u << 3) | (1u << 5))) != 0;
    if (assignment) {
        empty_patch old[64];
        if (!destination) DECLINE(e->type,"null assignment destination",NULL,NULL);
        int n = certified_patches(e->type,destination,old,64,0,NULL,"unresolved assignment destination",root,c->x[30],e->pc);
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
    int count = certified_patches(scope->plan->type,scope->output,patches,64,HAO_SV_SOURCE_GUEST,NULL,
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
 * walk cannot prove is refused (certified_patches).
 *
 * The intercepted functions: pc -> the mask of slots (below) it serves in,
 * grown as native types are added; read lock-free on every native call
 * inside [lo, hi]; witness_entries counts (pc, slot) pairs. witness_ready
 * is set once the first set is in, optional_metadata with it. */
static hao_gmap witness_pcs = HAO_GMAP_INIT(2, 1, 8, 16);
static bool witness_ready;
static unsigned witness_entries, witness_dropped;
uint64_t hao_swift_witness_lo = 1, hao_swift_witness_hi = 0;   /* empty until set up; hi read relaxed after lo */
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

/* Whether calls of PC are intercepted as SLOT. */
static bool witness_has(uint64_t pc, unsigned slot)
{
    const uint64_t *e = hao_gmap_get1(&witness_pcs,pc);
    return e && (__atomic_load_n(&e[1],__ATOMIC_RELAXED) >> slot & 1);
}
/* Under witness_setup_lock in a hao critical section: intercept calls of
 * PC as SLOT, widening [*lo, *hi]. A full map drops it, counted and said once. */
static void witness_add_locked(uint64_t pc, unsigned slot, uint64_t *lo, uint64_t *hi)
{
    bool fresh;
    uint64_t *e = hao_gmap_claim1(&witness_pcs,pc,&fresh);
    if (!e) {
        if (!witness_dropped++)
            fprintf(HAO_DIAG,"hao: Swift value witness table full (%u entries): calls of %#llx and later witnesses are not converted\n",
                    witness_entries,(unsigned long long)pc);
        return;
    }
    if (fresh) { e[1] = 1ull << slot; hao_gmap_publish1(&witness_pcs,e,pc); }
    else if (e[1] >> slot & 1) return;
    else __atomic_store_n(&e[1],e[1] | 1ull << slot,__ATOMIC_RELEASE);
    witness_entries++;
    if (pc < *lo) *lo = pc;
    if (pc > *hi) *hi = pc;
}
/* The published range to widen, empty while no witness is added. */
static void witness_range_now(uint64_t *lo, uint64_t *hi)
{
    *lo = witness_entries ? hao_swift_witness_lo : UINT64_MAX;
    *hi = witness_entries ? hao_swift_witness_hi : 0;
}
/* Publish the widened range: hi before lo, which readers load first. */
static void witness_range_publish(uint64_t lo, uint64_t hi)
{
    __atomic_store_n(&hao_swift_witness_hi,hi,__ATOMIC_RELAXED);
    __atomic_store_n(&hao_swift_witness_lo,lo,__ATOMIC_RELEASE);
}

static void witness_setup_locked(void);
static bool payload_witnesses_added;
static void payload_witnesses_locked(void);
void hao_swift_witness_setup(void)
{
    if ((__atomic_load_n(&witness_ready,__ATOMIC_ACQUIRE) && __atomic_load_n(&payload_witnesses_added,__ATOMIC_ACQUIRE)) ||
        hao_opt.no_swift_values) return;
    hao_crit_enter();
    pthread_mutex_lock(&witness_setup_lock);
    if (!witness_ready) witness_setup_locked();
    if (witness_ready && !payload_witnesses_added) payload_witnesses_locked();
    pthread_mutex_unlock(&witness_setup_lock);
    hao_crit_leave();
}

/* The single-payload tag witnesses (getEnumTagSinglePayload, slot 6;
 * storeEnumTagSinglePayload, slot 7) of a payload type whose first extra
 * inhabitant differs between arm64 and x86_64, so that guest generic code
 * asking a type's own witnesses about its Optional's nil (`if case .some =
 * v` for a `T?` with T = Data) gets it in their encoding: Foundation's Data
 * (its representation's spare bits; Recipe.c translated_nil_type). The
 * handler walks the type's Optional, as for an Optional's own witnesses.
 * Added when Foundation is loaded; the readers see a new entry once
 * it is published. */
static void payload_witnesses_locked(void)
{
    const void *data = HAO_DLSYM("$s10Foundation4DataVN");
    if (!data || !optional_metadata) return;   /* Foundation not loaded yet: a later image retries */
    const void *optional = optional_metadata(0,data).metadata;
    if (!optional || !translated_nil_type(optional)) { __atomic_store_n(&payload_witnesses_added,true,__ATOMIC_RELEASE); return; }
    uint64_t lo = hao_swift_witness_lo, hi = hao_swift_witness_hi;
    static const unsigned slots[] = {6,7};
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t pc = (uint64_t)(uintptr_t)((void *const *)vwt(data))[slots[i]];
        if (native_function(pc)) witness_add_locked(pc,slots[i],&lo,&hi);
    }
    witness_range_publish(lo,hi);
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
    /* The opaque existentials' own witnesses (OpaqueExistentialBox<N>,
     * chosen by the number of witness tables): guest code copies and
     * destroys an Any or an `any P` through them, and they hand an inline
     * payload to its type's witnesses (an Any holding a C?? nil). */
    const void *description = HAO_DLSYM("$ss23CustomStringConvertibleMp");
    const void *one_table = existential && description ? existential(1,NULL,1,&description) : NULL;
    if (!one_table) return;
    const void *types[] = {one, two,
        tuple2(0,int_type,any_object2,NULL,NULL).metadata, tuple2(0,int_type,any2,NULL,NULL).metadata,
        tuple2(0,int_type,string_type,NULL,NULL).metadata, tuple2(0,string_type,string_type,NULL,NULL).metadata,
        any, one_table};
    for (unsigned k = 0; k < sizeof types / sizeof *types; ++k) if (!types[k]) return;
    static const unsigned slots[] = {0,1,2,3,4,5,6,7,11,12,13};
    uint64_t lo, hi;
    unsigned before = witness_entries;
    witness_range_now(&lo,&hi);   /* a runtime-laid guest struct's, added before */
    for (unsigned k = 0; k < sizeof types / sizeof *types; ++k)
        for (unsigned i = 0; i < (k < 2 ? 11u : 8u); ++i) {   /* enum witnesses (11-13) only in an enum's table */
            uint64_t pc = (uint64_t)(uintptr_t)((void *const *)vwt(types[k]))[slots[i]];
            if (native_function(pc)) witness_add_locked(pc,slots[i],&lo,&hi);
        }
    /* The runtime's array operations (Array.cpp), which guest code calls
     * from inlined Array code: each element through its type's witnesses. */
    for (unsigned k = 0; k < sizeof array_ops / sizeof *array_ops; ++k) {
        uint64_t pc = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT,array_ops[k].name);
        if (native_function(pc)) witness_add_locked(pc,array_ops[k].slot,&lo,&hi);
    }
    if (witness_entries == before) return;
    __atomic_store_n(&optional_metadata,opt,__ATOMIC_RELEASE);
    __atomic_store_n(&witness_ready,true,__ATOMIC_RELEASE);
    witness_range_publish(lo,hi);
}

/* Whether a value of this Optional may hold an outer nil in a pointer's
 * extra inhabitants anywhere in it (graph() with existential payloads). */
/* What a guest call of a native witness on a value of TYPE needs (graph()
 * with existential payloads), cached: */
enum { WITNESS_CONVERTS = 1,    /* certification, and a nil whose encoding differs may need converting */
       WITNESS_SETTLED = 2,     /* nothing: every node settled */
       WITNESS_UNCERTIFIABLE = 3, /* refusal: the type graph is past the walk's bounds */
       WITNESS_CERTIFY = 4,     /* certification only */
       WITNESS_UNPROVEN = 5,    /* as CONVERTS, but a conversion has no certified rule (graph b->unproven) */
       WITNESS_INVALID = 6 };   /* refusal: invalid metadata (graph b->invalid), or no memory to tell */
/* The refusal of a type in STATE, or NULL. */
static const char *state_refusal(int state)
{
    return state == WITNESS_UNCERTIFIABLE ? "type graph past the walk's bounds" :
           state == WITNESS_INVALID ? "type graph has invalid metadata" : NULL;
}
static int witness_state(const void *type)
{
    const uint64_t *e = hao_gmap_get1(&witness_types,(uint64_t)(uintptr_t)type);
    if (e) return (int)e[1];
    builder *b = calloc(1,sizeof *b);
    if (!b) return WITNESS_INVALID;   /* not cached: a later call tries again */
    b->existentials = true;
    b->kind = HAO_DLSYM("swift_getMetadataKind");
    b->count = reflect_count_fn();
    b->offset = reflect_offset_fn();
    b->field = reflect_field_fn();
    bool ok = b->kind && b->count && b->offset && b->field;
    bool affected = ok && graph(b,type,0);
    int state = !ok || b->invalid ? WITNESS_INVALID : b->failed ? WITNESS_UNCERTIFIABLE : !affected ? WITNESS_SETTLED :
                b->unproven ? WITNESS_UNPROVEN : b->converts ? WITNESS_CONVERTS : WITNESS_CERTIFY;
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
    if (state_refusal(state)) {
        decline(type,state_refusal(state),NULL,NULL,type,lr,pc);
        return false;   /* HAO_SWIFT_VALUE_PASSTHROUGH: unexamined */
    }
    return state != WITNESS_SETTLED;
}

/* The byte ranges in a value of TYPE (at BASE) of the fields whose struct or
 * enum a guest image defines: their value witnesses are guest code, or the
 * runtime's layout-string ones following a layout string guest code
 * compiled, which has guest functions read its enums' cases; either reads
 * the guest's encoding inside them, so a nil of an Optional there keeps the
 * guest's bits. A guest struct whose layout string the runtime built from
 * its fields' metadata (runtime_laid) is walked through like a tuple: the
 * native witnesses read its fields. Through struct and tuple fields only: an Optional
 * (or a native enum) around a guest type is read by its own native
 * witnesses, also where its nil is a word inside the guest type's bytes
 * (`Leaf?`, Leaf a guest struct holding a class reference). -1 when there
 * are more than CAP or the type is deeper than 16. */
typedef struct { uint64_t lo, hi; } byte_range;
/* Guest structs whose layout string the runtime built (hao_swift_struct_layout_done). */
static hao_gmap laid_types = HAO_GMAP_INIT(2, 1, 6, 20);         /* guest struct metadata -> 1 */
static bool runtime_laid(const void *type) { return hao_gmap_get1(&laid_types,(uint64_t)(uintptr_t)type) != NULL; }
static int guest_ranges(const void *type, uint64_t base, byte_range *out, int n, int cap, int depth)
{
    kind_fn *kind = HAO_DLSYM("swift_getMetadataKind");
    count_fn *count = reflect_count_fn();
    offset_fn *offset = reflect_offset_fn();
    field_fn *field = reflect_field_fn();
    if (!kind || !count || !offset || !field || depth > 16) return -1;
    const void *const *m = type;
    uintptr_t k = kind(type);
    if ((k == 0x200 || k == 0x201) && hao_image_for_addr((uint64_t)(uintptr_t)m[1]) && !runtime_laid(type)) {
        if (n >= cap) return -1;
        out[n++] = (byte_range){base, base + vwt(type)->size};
        return n;
    }
    if (k != 0x200 && k != 0x301) return n;   /* an Optional or a native enum: their witnesses are native */
    intptr_t fields = count(type);
    if (fields < 0 || fields > HAO_SV_FIELD_LIMIT) return -1;
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
static bool witness_strip_guest(const void *type, witness_patches *w);
/* Where the guest-defined fields lie is not known (past the walk's bound):
 * with a nil one side must have converted, either side may misread it. */
static const char *strip_refusal(const empty_patch *p, int count)
{
    for (int i = 0; i < count; ++i)
        if (p[i].bits != p[i].receiver_bits) return "nil to convert beside guest-defined fields past the walk's bound";
    return "guest-defined fields past the walk's bound";
}
static void witness_walk(const void *type, const void *value, witness_patches *w, const char *why, uint64_t lr, uint64_t pc)
{
    w->count = certified_patches(type,value,w->p,64,1 | HAO_SV_SOURCE_GUEST,NULL,why,type,lr,pc);   /* guest storage */
    if (w->count > 0 && !witness_strip_guest(type,w)) {   /* which nils lie in guest-defined fields is not known */
        decline(type,strip_refusal(w->p,w->count),NULL,NULL,type,lr,pc);   /* the patches as found: a failed strip keeps them */
        w->count = 0;
    }
}
/* Drop the patches inside guest-defined struct and enum fields (their own
 * guest witnesses read them); false when those fields are past the bound. */
static bool strip_guest(const void *type, empty_patch *p, int *count)
{
    byte_range guest[32];
    int ng = guest_ranges(type,0,guest,0,32,0);
    if (ng < 0) return false;
    int kept = 0;
    for (int i = 0; i < *count; ++i) {
        bool inside = false;
        for (int g = 0; g < ng && !inside; ++g) inside = p[i].offset >= guest[g].lo && p[i].offset < guest[g].hi;
        if (!inside) p[kept++] = p[i];
    }
    *count = kept;
    return true;
}
static bool witness_strip_guest(const void *type, witness_patches *w) { return strip_guest(type,w->p,&w->count); }
/* Rewrite the walked outer nils to the witness's own encoding. */
static void encode(void *value, const empty_patch *p, int count)
{
    for (int i = 0; i < count; ++i) memcpy((char *)value + p[i].offset,&p[i].receiver_bits,8);
}
static void witness_encode(void *value, const witness_patches *w) { encode(value,w->p,w->count); }
/* A word of VALUE at AT that a witness wrote as NATIVE and hao set to the
 * guest's ARM bits (hao_swift_witness_result). */
static void guest_bits_note(hao_thread *t, const void *value, const void *at, uint64_t native, uint64_t arm)
{
    unsigned i = t->swift_made_n++, k = i % HAO_SWIFT_MADE_RING;
    t->swift_made[k].value = (uint64_t)(uintptr_t)value;
    t->swift_made[k].at = (uint64_t)(uintptr_t)at;
    t->swift_made[k].native = native;
    t->swift_made[k].arm = arm;
    t->swift_made[k].idx = i;
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

/* A native struct positively resilient (Tables.h): the generator lists every
 * frozen struct of each image build it covers (frozenstruct_gen.inc), so a
 * struct of a covered build that it lists neither at its descriptor nor by
 * name was not frozen in that module's interface. Absence alone proves
 * nothing: an uncovered image, and any enum (the table lists structs only),
 * are not resilient for hao. */
static hao_gmap resilient_types = HAO_GMAP_INIT(2, 1, 6, 14);    /* nominal descriptor -> 1 resilient, 2 not proven */
int native_resilient_type(const void *type)
{
    const void *const *m = type;
    if (!type || (uintptr_t)m[0] != 0x200 || !m[1]) return 0;
    uint64_t desc = (uint64_t)(uintptr_t)m[1];
    const uint64_t *e = hao_gmap_get1(&resilient_types,desc);
    if (e) return e[1] == 1;
    char uuid[33];
    uint64_t offset;
    bool covered = false, listed = false;
    if (native_type_place(type,uuid,&offset))
        for (size_t k = 0; k < sizeof frozen_structs / sizeof *frozen_structs; ++k)
            if (!strcmp(frozen_structs[k].uuid,uuid)) { covered = true; listed |= frozen_structs[k].offset == offset; }
    bool resilient = covered && !listed && !native_frozen(type);
    hao_crit_enter(); pthread_mutex_lock(&lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&resilient_types,desc,&fresh);
    if (slot && fresh) { slot[1] = resilient ? 1 : 2; hao_gmap_publish1(&resilient_types,slot,desc); }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return resilient;
}

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
 * its witnesses then use Data's arm64 encoding, which only they read.
 * The struct is noted (runtime_laid): its layout string, built by the
 * runtime from the fields' metadata, has the native witnesses read the
 * fields of native types themselves, in the native encoding, so guest calls
 * of its witnesses convert like those of a frozen native struct's. */
static void witness_add_type(const void *type);
void hao_swift_struct_layout_done(void *metadata, uint64_t n, const void *const *fields, const uint8_t *tags)
{
    if (hao_opt.no_swift_values || !metadata || !fields || !tags) return;
    const void *const *m = metadata;
    if ((uintptr_t)m[0] != 0x200 || !hao_image_for_addr((uint64_t)(uintptr_t)m[1])) return;   /* a guest struct */
    hao_crit_enter(); pthread_mutex_lock(&lock);
    bool fresh;
    uint64_t *slot = hao_gmap_claim1(&laid_types,(uint64_t)(uintptr_t)metadata,&fresh);
    if (slot && fresh) { slot[1] = 1; hao_gmap_publish1(&laid_types,slot,(uint64_t)(uintptr_t)metadata); }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    /* Unrecorded, its fields would be taken as read in the guest's encoding. */
    if (!slot) decline(metadata,"layout record allocation",NULL,NULL,metadata,0,0);
    witness_add_type(metadata);
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
    w.count = certified_patches(type,value,w.p,64,1 | HAO_SV_SOURCE_NATIVE,guest_layout,why,type,lr,pc);   /* native-made */
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

/* swift_task_create or swift_asyncLet_begin with FUTURE, the result type
 * (swiftmeta.c; LR the guest call). The child's guest code writes its result
 * into the task, and the concurrency runtime copies and destroys it with the
 * type's native witnesses before any boundary hao sees (a waiting task's
 * copy, the task's release). A type whose values need certification is
 * refused here, the earliest point hao sees: converting the result where the
 * guest writes it is not done. */
void hao_swift_task_result(const void *future, uint64_t lr)
{
    if (hao_opt.no_swift_values || !future) return;
    int state = witness_state(future);
    if (state == WITNESS_SETTLED) return;
    decline(future,state_refusal(state) ? state_refusal(state) : state == WITNESS_CERTIFY ?
            "child task result is copied by native code before it can be certified" :
            "child task result has a nil native code would copy in the guest's encoding",NULL,NULL,future,lr,0);
}

/* Storage guest code creates and native code destroys: array storage
 * (swift_allocObject with _ContiguousArrayStorage<E>, _allocateUninitializedArray<E>)
 * and boxes (swift_allocBox of T). Guest code fills it, and when its last
 * reference goes, wherever that is released, the runtime destroys the
 * contents with the element type's native witnesses: no boundary hao sees.
 * An element type past the walk's bounds is refused when the storage is
 * made. One with a nil whose encoding differs is admitted only as a carrier:
 * each conversion it may need has a certified rule (no unproven node), and
 * the storage's destroy hook can be had (below; hao_swift_storage_made then
 * makes hooking and registering the object conditions of admitting it), so
 * that its final destruction converts the guest's nils first. Native copies
 * of its elements out of the storage are not converted (docs/ISSUES.md). An
 * element type that is only not settled (an enum, an existential) is not
 * refused: refusing every [Any] would stop nearly every program, and its
 * contents are not certified at that destruction (docs/ISSUES.md). */
static void storage_element(const void *element, bool box, const char *why, uint64_t lr)
{
    if (hao_opt.no_swift_values || !element) return;
    int state = witness_state(element);
    bool hook = box ? HAO_DLSYM("swift_allocBox") != NULL : HAO_DLSYM("$ss23_ContiguousArrayStorageCfD") != NULL;
    if (state_refusal(state)) decline(element,state_refusal(state),NULL,NULL,element,lr,0);
    else if (state == WITNESS_UNPROVEN || (state == WITNESS_CONVERTS && !hook)) decline(element,why,NULL,NULL,element,lr,0);
}
void hao_swift_storage_alloc(const void *metadata, uint64_t lr)
{
    const uint64_t *m = metadata;
    if (hao_opt.no_swift_values || !m || m[0] <= 0x7ff || !(m[4] & 3)) return;   /* a Swift class's metadata */
    const void *storage = HAO_DLSYM("$ss23_ContiguousArrayStorageCMn");
    const char *desc = (const char *)(uintptr_t)m[8];   /* ClassMetadata Description */
    if (!storage || desc != storage) return;
    uint32_t positive, immediate;   /* ClassDescriptor: MetadataPositiveSizeInWords, NumImmediateMembers */
    memcpy(&positive,desc + 28,4); memcpy(&immediate,desc + 32,4);
    if (positive <= immediate) return;
    storage_element((const void *)(uintptr_t)m[positive - immediate],false,
                    "array element type has a nil native code would destroy in the guest's encoding",lr);
}
void hao_swift_storage_element(const void *element, bool box, uint64_t lr)
{
    storage_element(element,box,"storage element type has a nil native code would destroy in the guest's encoding",lr);
}

/* ------------------------------------------- final destruction of storage
 *
 * The runtime destroys array storage (_ContiguousArrayStorage<E>'s
 * deallocating deinit calls swift_arrayDestroy, a direct call inside
 * libswiftCore that no symbol interposition reaches) and generic boxes
 * (swift_allocBox's metadata destroys the boxed value) with the element
 * type's native witnesses, wherever the last reference is released. Guest
 * code may have written those elements, an Any holding a C?? nil for one.
 * For an element type that is not settled, hao takes over that storage's
 * destroy function (HeapMetadata's, the word before the value witness table
 * pointer; one per instantiated element type, so every such storage is
 * covered, whoever made it) once guest code makes storage of it. Before the
 * original destroy runs, each live element is walked as a guest call of a
 * native witness would be (certified_patches, guest-defined fields left to
 * their own witnesses) and its nils are rewritten in place to the native
 * encoding: the destruction is exclusive and final, nothing reads them
 * after. A shared existential box is never rewritten (boxed_payload refuses
 * one that would need it). An element the walk cannot certify is refused in
 * storage guest code made (registered below, by object address); in other
 * storage it is destroyed as found, being native-made for all hao knows.
 * Not covered: copies and assignments of elements (the runtime's array
 * operations and witness calls inside native code), Dictionary and Set
 * storage, storage of element types guest code never made storage of. */
typedef void SWIFT heap_destroy_fn(void *__attribute__((swift_context)));
/* The runtime's destroy functions the hooks call on: set once, under
 * storage_lock, from a captured and validated pointer, before any hook that
 * calls them is published. */
static heap_destroy_fn *array_destroy_original, *box_destroy_original;

/* Storage guest code made and the runtime has not destroyed yet: an open-
 * addressed set of object addresses (0 empty, 1 a removed entry, which an
 * insert reuses), rebuilt without removed entries when live and removed ones
 * fill three quarters of it. Read and written under storage_lock only. A
 * rebuild fills a new table, publishes it with one pointer store and only
 * then frees the old one, so a fork child, whatever point the parent was
 * at, holds a whole table (storage_fork_child recounts it). */
typedef struct { size_t cap, live, gone; uint64_t e[]; } storage_set;
static storage_set *guest_storage;
static size_t storage_slot(const storage_set *t, uint64_t object, bool insert)
{
    size_t cap = t->cap, h = (size_t)((object >> 4) * 0x9E3779B97F4A7C15ull) & (cap - 1), reuse = SIZE_MAX;
    for (size_t n = 0; n < cap; ++n, h = (h + 1) & (cap - 1)) {
        if (t->e[h] == object) return h;
        if (t->e[h] == 1 && reuse == SIZE_MAX) reuse = h;
        if (!t->e[h]) return insert && reuse != SIZE_MAX ? reuse : h;
    }
    return insert ? reuse : SIZE_MAX;
}
/* Register OBJECT (LIVE) or retire it; false when it cannot be registered. */
static bool storage_note_locked(uint64_t object, bool live)
{
    storage_set *t = guest_storage;
    if (!live) {
        size_t at = t ? storage_slot(t,object,false) : SIZE_MAX;
        if (at != SIZE_MAX && t->e[at] == object) { t->e[at] = 1; t->live--; t->gone++; }
        return true;
    }
    if (!t || 4 * (t->live + t->gone + 1) > 3 * t->cap) {
        size_t cap = t ? t->cap : 1024, live = t ? t->live : 0;
        while (2 * (live + 1) > cap) cap *= 2;   /* at most half full after the rebuild */
        if (cap > ((size_t)1 << 26)) return false;
        storage_set *n = calloc(1,sizeof *n + cap * sizeof(uint64_t));
        if (!n) return false;
        n->cap = cap;
        for (size_t i = 0; t && i < t->cap; ++i)
            if (t->e[i] > 1) { n->e[storage_slot(n,t->e[i],true)] = t->e[i]; n->live++; }
        __atomic_store_n(&guest_storage,n,__ATOMIC_RELEASE);
        free(t);
        t = n;
    }
    size_t at = storage_slot(t,object,true);
    if (at == SIZE_MAX) return false;
    if (t->e[at] != object) {
        if (t->e[at] == 1) t->gone--;
        t->e[at] = object;
        t->live++;
    }
    return true;
}
/* In a fork child: the published table is whole; its counters are
 * recounted from its entries (the parent may have been between writes). */
static void storage_fork_child(void)
{
    storage_set *t = guest_storage;
    if (!t) return;
    t->live = t->gone = 0;
    for (size_t i = 0; i < t->cap; ++i) { if (t->e[i] == 1) t->gone++; else if (t->e[i]) t->live++; }
}
/* Whether OBJECT is registered, retiring it: its final destruction. */
static bool storage_take(void *object)
{
    hao_crit_enter(); pthread_mutex_lock(&storage_lock);
    storage_set *t = guest_storage;
    size_t at = t ? storage_slot(t,(uint64_t)(uintptr_t)object,false) : SIZE_MAX;
    bool registered = at != SIZE_MAX && t->e[at] == (uint64_t)(uintptr_t)object;
    if (registered) storage_note_locked((uint64_t)(uintptr_t)object,false);
    pthread_mutex_unlock(&storage_lock); hao_crit_leave();
    return registered;
}
/* VALUE of TYPE at its final destruction, P the scratch for CAP patches
 * (NULL: it could not be had). */
static void storage_value(const void *type, char *value, bool guest_made, empty_patch *p, unsigned cap)
{
    const char *why = "stored value has unresolved active representation at its final destruction";
    int count = 0;
    if (!(vwt(type)->flags & 0x10000)) return;
    if (!p) {
        if (guest_made) decline(type,"scratch allocation",NULL,NULL,type,0,0);
        return;
    }
    if (guest_made) {
        count = certified_patches(type,value,p,cap,1 | HAO_SV_SOURCE_GUEST,NULL,why,type,0,0);
        if (count > 0 && !strip_guest(type,p,&count)) {
            decline(type,strip_refusal(p,count),NULL,NULL,type,0,0);
            count = 0;
        }
    } else {
        /* Storage native code made: what the walk proves is converted; a value
         * it finds in the guest's encoding but cannot convert (in a shared
         * existential box) is refused; anything else it cannot read is left
         * as found, native-made for all hao knows (docs/ISSUES.md). */
        const void *unresolved;
        unsigned passes = 0;
        count = collect_value_patches(type,value,p,cap,&unresolved,1,NULL,&passes);
        if (count < 0 && (passes & HAO_SV_UNCONVERTIBLE))
            decline(unresolved ? unresolved : type,"guest-encoded nil in a shared box at its final destruction",type,value,type,0,0);
        if (count > 0 && !strip_guest(type,p,&count)) count = 0;
    }
    unsigned changed = 0;
    for (int i = 0; i < count; ++i) changed += p[i].bits != p[i].receiver_bits;
    encode(value,p,count);
    if (changed && hao_opt.bridge_check)
        fprintf(HAO_DIAG,"hao: Swift storage destroyed by the runtime: %u nils converted (metadata=%p %s)\n",changed,type,
                guest_made ? "guest-made" : "not guest-made");
}
static const void *array_element(const uint64_t *metadata)
{
    const char *desc = (const char *)(uintptr_t)metadata[8];
    uint32_t positive, immediate;
    memcpy(&positive,desc + 28,4); memcpy(&immediate,desc + 32,4);
    return positive > immediate ? (const void *)(uintptr_t)metadata[positive - immediate] : NULL;
}
/* Scratch for the patches of one value at a final destruction: a value of
 * HAO_SV_FIELD_LIMIT fields may have as many, more than a stack can spare
 * on a native caller's thread. */
static empty_patch *storage_scratch(void)
{
    hao_crit_enter();
    empty_patch *p = malloc(HAO_SV_FIELD_LIMIT * sizeof *p);
    hao_crit_leave();
    return p;
}
static void storage_scratch_free(empty_patch *p)
{
    hao_crit_enter();
    free(p);
    hao_crit_leave();
}
static void SWIFT array_storage_destroy(void *__attribute__((swift_context)) object)
{
    const uint64_t *o = object;
    const void *element = array_element((const uint64_t *)(uintptr_t)o[0]);
    bool guest_made = storage_take(object);
    if (element && (int64_t)o[2] > 0 && (vwt(element)->flags & 0x10000)) {
        /* _ContiguousArrayStorage: HeapObject, count, capacity and flags, elements */
        const witnesses *v = vwt(element);
        uint64_t mask = v->flags & 255, at = (32 + mask) & ~mask, n = o[2];
        empty_patch *p = storage_scratch();
        for (uint64_t i = 0; i < n && v->stride; ++i)
            storage_value(element,(char *)object + at + i * v->stride,guest_made,p,HAO_SV_FIELD_LIMIT);
        storage_scratch_free(p);
    }
    array_destroy_original(object);
}
static void SWIFT box_storage_destroy(void *__attribute__((swift_context)) object)
{
    const uint64_t *m = (const uint64_t *)(uintptr_t)*(const uint64_t *)object;   /* GenericBoxHeapMetadata */
    uint32_t offset; memcpy(&offset,(const char *)m + 8,4);
    const void *boxed = (const void *)(uintptr_t)m[2];
    bool guest_made = storage_take(object);
    if (boxed && (vwt(boxed)->flags & 0x10000)) {
        empty_patch *p = storage_scratch();
        storage_value(boxed,(char *)object + offset,guest_made,p,HAO_SV_FIELD_LIMIT);
        storage_scratch_free(p);
    }
    box_destroy_original(object);
}
/* Whether the page holding ADDR may be written (instantiated metadata is;
 * metadata in an image's constant data is not). */
static bool storage_writable(const void *addr)
{
    mach_vm_address_t region = (mach_vm_address_t)(uintptr_t)addr & ~(mach_vm_address_t)(vm_page_size - 1);
    mach_vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(),&region,&bytes,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,
                                      &count,&object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
    return kr == KERN_SUCCESS && region <= (mach_vm_address_t)(uintptr_t)addr && (info.protection & VM_PROT_WRITE);
}
/* Under storage_lock: METADATA's destroy (word -2) is the hook, or becomes
 * it while it is still ORIGINAL. */
static bool storage_hook_locked(uint64_t *metadata, heap_destroy_fn *original, heap_destroy_fn *wrapper)
{
    uint64_t now = __atomic_load_n(&metadata[-2],__ATOMIC_ACQUIRE);
    if (now == (uint64_t)(uintptr_t)wrapper) return true;
    if (!original || now != (uint64_t)(uintptr_t)original) return false;
    if (hao_image_sealed_at((uint64_t)(uintptr_t)&metadata[-2],8))   /* a guest's constant metadata (prespecialized) */
        return hao_image_sealed_cas((uint64_t)(uintptr_t)&metadata[-2],now,(uint64_t)(uintptr_t)wrapper) ||
               __atomic_load_n(&metadata[-2],__ATOMIC_ACQUIRE) == (uint64_t)(uintptr_t)wrapper;
    if (!storage_writable(&metadata[-2])) return false;
    return __atomic_compare_exchange_n(&metadata[-2],&now,(uint64_t)(uintptr_t)wrapper,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) ||
           now == (uint64_t)(uintptr_t)wrapper;
}
/* OBJECT, just made by guest code (swift_allocObject, swift_allocBox,
 * _allocateUninitializedArray): array storage or a generic box. For an
 * element type that is not settled, hooking its metadata and registering
 * the object are conditions of admitting it: either failing refuses it. */
void hao_swift_storage_made(void *object)
{
    if (hao_opt.no_swift_values || !object) return;
    uint64_t *m = (uint64_t *)(uintptr_t)*(const uint64_t *)object;
    if (!m) return;
    const void *element;
    bool box = m[0] == 0x500;   /* HeapGenericLocalVariable: {kind, Offset, BoxedType} */
    if (box) element = (const void *)(uintptr_t)m[2];
    else if (m[0] > 0x7ff && (m[4] & 3) && (const void *)(uintptr_t)m[8] == HAO_DLSYM("$ss23_ContiguousArrayStorageCMn"))
        element = array_element(m);
    else return;
    int state = element ? witness_state(element) : WITNESS_SETTLED;
    if (state == WITNESS_SETTLED) return;
    void *array_original = box ? NULL : HAO_DLSYM("$ss23_ContiguousArrayStorageCfD");
    void *alloc_box = box ? HAO_DLSYM("swift_allocBox") : NULL;
    bool admitted = false;
    hao_crit_enter(); pthread_mutex_lock(&storage_lock);
    if (box) {
        uint64_t now = __atomic_load_n(&m[-2],__ATOMIC_ACQUIRE);
        Dl_info a, b;
        if (!box_destroy_original && now != (uint64_t)(uintptr_t)box_storage_destroy && alloc_box &&
            dladdr((void *)(uintptr_t)now,&a) && dladdr(alloc_box,&b) && a.dli_fbase == b.dli_fbase)
            box_destroy_original = (heap_destroy_fn *)(uintptr_t)now;   /* the runtime's, validated as captured */
        admitted = storage_hook_locked(m,box_destroy_original,box_storage_destroy);
    } else {
        if (!array_destroy_original) array_destroy_original = (heap_destroy_fn *)array_original;
        admitted = storage_hook_locked(m,array_destroy_original,array_storage_destroy);
    }
    admitted = admitted && storage_note_locked((uint64_t)(uintptr_t)object,true);
    pthread_mutex_unlock(&storage_lock); hao_crit_leave();
    if (!admitted && (state == WITNESS_CONVERTS || state == WITNESS_UNPROVEN))   /* a nil to convert: known wrong */
        decline(element,"storage element type has a nil native code would destroy in the guest's encoding",NULL,NULL,element,0,0);
    else if (!admitted)
        decline(element,box ? "box of a type that is not settled cannot be certified at its destruction"
                            : "array storage of a type that is not settled cannot be certified at its destruction",
                NULL,NULL,element,0,0);
}

/* The value witnesses (slots 0-5) of TYPE, a native struct or enum whose
 * values guest code builds inline (a View it lent to a native modifier), or
 * a guest struct the runtime laid out (runtime_laid), so that its later
 * guest calls of them (an outlined copy or destroy) convert like those of an
 * Optional's. */
static void witness_add_type(const void *type)
{
    const void *const *m = type;
    uintptr_t kind = (uintptr_t)m[0];
    if ((kind != 0x200 && kind != 0x201) || (hao_image_for_addr((uint64_t)(uintptr_t)m[1]) && !runtime_laid(type))) return;
    if (witness_has((uint64_t)(uintptr_t)vwt(type)->fn[1],1)) return;
    hao_crit_enter();
    pthread_mutex_lock(&witness_setup_lock);
    uint64_t lo, hi;
    witness_range_now(&lo,&hi);
    for (unsigned slot = 0; slot <= 5; ++slot) {
        uint64_t pc = (uint64_t)(uintptr_t)vwt(type)->fn[slot];
        if (native_function(pc)) witness_add_locked(pc,slot,&lo,&hi);
    }
    witness_range_publish(lo,hi);
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
    int state = witness_state(type);
    if (state == WITNESS_CONVERTS || state == WITNESS_UNPROVEN) witness_add_type(type);   /* the guest's later copies and destroys of it */
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
static uint64_t witness_made_lost;
void hao_swift_witness_result(hao_thread *t, unsigned mark, uint64_t indirect)
{
    const unsigned ring = HAO_SWIFT_MADE_RING;
    unsigned n = t->swift_made_n;
    if (indirect && n != mark) {
        /* this activation's entries [mark, n): those past the ring, or
         * overwritten by a nested callback's, are lost */
        unsigned from = n - mark > ring ? n - ring : mark;
        unsigned restored = 0, lost = from - mark;
        for (unsigned i = from; i != n; ++i) {
            unsigned k = i % ring;
            if (t->swift_made[k].idx != i) { lost++; continue; }
            if (t->swift_made[k].value != indirect) continue;
            uint64_t bits;
            memcpy(&bits,(const void *)(uintptr_t)t->swift_made[k].at,8);
            if (bits != t->swift_made[k].arm) continue;
            memcpy((void *)(uintptr_t)t->swift_made[k].at,&t->swift_made[k].native,8);
            restored++;
        }
        /* a callback's return may be a guest signal handler's: no stdio here, only lines built
         * on the stack for the async-signal-safe diagnostic write, and counters */
        if (restored && hao_opt.bridge_check) {
            char m[120];
            size_t k = 0;
            static const char a[] = "hao: Swift Optional witness nils returned to native: ", b[] = " at 0x";
            memcpy(m,a,sizeof a - 1); k += sizeof a - 1;
            k += hao_diag_fmt_u64(m + k,restored,10);
            memcpy(m + k,b,sizeof b - 1); k += sizeof b - 1;
            k += hao_diag_fmt_u64(m + k,indirect,16);
            m[k++] = '\n';
            hao_diag_write(m,k);
        }
        if (lost && !__atomic_fetch_add(&witness_made_lost,lost,__ATOMIC_RELAXED)) {
            static const char msg[] = "hao: Swift Optional witness nils past the ring were not tracked: a callback "
                                      "result may keep the guest's encoding (count: --stats)\n";
            hao_diag_write(msg,sizeof msg - 1);
        }
    }
    t->swift_made_n = mark;
}

uint64_t hao_swift_witness_lost(void) { return __atomic_load_n(&witness_made_lost,__ATOMIC_RELAXED); }

void hao_swift_witness_stats(void)
{
    uint64_t lost = __atomic_load_n(&witness_made_lost,__ATOMIC_RELAXED);
    if (witness_entries || witness_dropped || lost)
        fprintf(HAO_DIAG,"hao-swift-witness: entries=%u dropped=%u map_kb=%llu nils_lost=%llu\n",witness_entries,witness_dropped,
                (unsigned long long)(witness_pcs.bytes >> 10),(unsigned long long)lost);
}

/* Each element's outer nils in the witness's encoding, in place; their
 * original bits recorded by element index and offset (rec, count *nrec). */
typedef struct { uint64_t at, bits; } witness_bits;
#define WITNESS_NON_BITWISE_TAKABLE 0x00100000u   /* ValueWitnessFlags IsNonBitwiseTakable (swift/ABI/MetadataValues.h) */

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
        /* static values in an image's read-only constant data: destroyed as a converted copy (natively
         * nothing writes there). Only a bitwise-takable type may move (WITNESS_NON_BITWISE_TAKABLE: weak
         * references and some imported C++ values are not); such values need run-time registration and
         * cannot be static constants, so those stay in place */
        if (!(vwt(type)->flags & WITNESS_NON_BITWISE_TAKABLE) && hao_image_sealed_at((uint64_t)(uintptr_t)dest, (size_t)(count * stride))) {
            if (!(scratch = malloc((size_t)(count * stride)))) { decline(type,"scratch allocation",NULL,NULL,type,lr,pc); return false; }
            memcpy(scratch,dest,(size_t)(count * stride));
            dest = scratch;
        }
        if (!witness_encode_elements(type,dest,count,&in,&nin,lr,pc)) { free(scratch); return false; }
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
    const uint64_t *e = hao_gmap_get1(&witness_pcs,pc);
    if (!e) return false;
    for (uint64_t slots = __atomic_load_n(&e[1],__ATOMIC_ACQUIRE); slots; slots &= slots - 1) {
        unsigned slot = (unsigned)__builtin_ctzll(slots);
        const void *type = (const void *)(uintptr_t)c->x[witness_metadata_reg(slot)];
        if (slot >= ARRAY_DESTROY) return type && witness_array(t,pc,slot,type);
        if (!type || (uint64_t)(uintptr_t)((void *const *)vwt(type))[slot] != pc) continue;
        const void *const *tm = type;
        if (slot <= 5 && ((uintptr_t)tm[0] == 0x200 || (uintptr_t)tm[0] == 0x201) &&
            hao_image_for_addr((uint64_t)(uintptr_t)tm[1]) && !runtime_laid(type))
            return false;   /* a guest type whose layout string guest code compiled reads its own encoding */
        /* Single-payload tag witnesses act on an enclosing enum of the
         * type; with one empty case that is the type's own Optional. A
         * tuple's table has no enum witnesses (11-13): only an Optional's
         * pcs carry those slots, and the table check above reads them only
         * for such a pc. */
        const void *walked = type;
        if (slot == 6 || slot == 7) {
            optional_accessor *opt = __atomic_load_n(&optional_metadata,__ATOMIC_ACQUIRE);
            if (c->x[slot == 6 ? 1 : 2] != 1) return false;
            walked = opt ? opt(0,type).metadata : NULL;
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
            if (slot == 1 && in.count > 0 && !(vwt(type)->flags & WITNESS_NON_BITWISE_TAKABLE) &&
                hao_image_sealed_at((uint64_t)(uintptr_t)dest, (size_t)v->size)) {
                /* a static value in read-only constant data: destroyed as a converted copy (ARRAY_DESTROY) */
                scratch = v->size <= sizeof local ? local : malloc((size_t)v->size);
                if (!scratch) { decline(type,"scratch allocation",NULL,NULL,type,lr,pc); return false; }
                memcpy(scratch,dest,(size_t)v->size);
                witness_encode(scratch,&in);
            } else witness_encode(dest,&in);
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
        case 1: ((void2_fn *)f)(scratch ? scratch : dest,type); break;
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
