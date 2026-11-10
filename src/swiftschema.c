/* Bounded diagnostic of reachable initialized Swift metadata, independent of
 * the value adapter's admission rules. Inactive enum payloads are included.
 * No value bytes, user strings, object fields, or case discriminators are read.
 * ABI sources: Swift Metadata.h/ReflectionRecords.h and ReflectionMirror.cpp. */
#include "hao/swiftschema.h"
#include "hao/macho.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define SWIFT __attribute__((swiftcall))
#define LIMIT 256
typedef struct { const char *text; uintptr_t length; } type_name;
typedef struct { const char *name; void (*free_name)(const char *); bool strong, var; } field_info;
typedef type_name SWIFT name_fn(const void *, bool);
typedef uintptr_t SWIFT kind_fn(const void *);
typedef intptr_t SWIFT count_fn(const void *);
typedef intptr_t SWIFT offset_fn(const void *, intptr_t);
typedef const void *SWIFT field_fn(const void *, intptr_t, field_info *);
typedef const void *SWIFT resolve_fn(const char *, size_t, const void *, const void *const *);
typedef struct { void *fn[8]; uint64_t size, stride; uint32_t flags, extra; } witnesses;
typedef struct {
    FILE *out; const void *types[LIMIT]; unsigned used, truncated, unresolved;
    name_fn *name; kind_fn *kind; count_fn *count; offset_fn *offset;
    field_fn *field; resolve_fn *resolve;
} survey;

/* Absent in standalone native fixtures. Resolve guest descriptors and
 * thunk/island witnesses before native dladdr when running inside hao. */
typedef hao_image *image_fn(uint64_t);
typedef uint64_t target_fn(uint64_t);
static image_fn *image_lookup;
static target_fn *thunk_target, *sig_target, *island_target;
static pthread_once_t owners_once = PTHREAD_ONCE_INIT;
static void owners_init(void)
{
    image_lookup = dlsym(RTLD_DEFAULT,"hao_image_for_addr");
    thunk_target = dlsym(RTLD_DEFAULT,"hao_thunk_target");
    sig_target = dlsym(RTLD_DEFAULT,"hao_sig_thunk_target");
    island_target = dlsym(RTLD_DEFAULT,"hao_swiftmeta_island_target");
}

static bool read_at(const void *p, void *out, size_t n)
{
    mach_vm_size_t got = 0;
    uintptr_t a = (uintptr_t)p;
    return a >= 4096 && a <= UINTPTR_MAX - n &&
        mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}
static const char *relative(const char *p)
{
    int32_t delta;
    return read_at(p,&delta,4) && delta ? p + delta : NULL;
}
static void quoted(FILE *out, const char *text, size_t n)
{
    fputc('"',out);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c;
        if (!read_at(text + i,&c,1) || !c) break;
        if (c == '"' || c == '\\') fprintf(out,"\\%c",c);
        else if (c < 32) fprintf(out,"\\u%04x",c);
        else fputc(c,out);
    }
    fputc('"',out);
}
static void owner(FILE *out, const void *address)
{
    uint64_t a = (uint64_t)(uintptr_t)address, guest = 0;
    pthread_once(&owners_once,owners_init);
    if (thunk_target) guest = thunk_target(a);
    if (!guest && sig_target) guest = sig_target(a);
    if (!guest && island_target) guest = island_target(a);
    if (guest) a = guest;
    hao_image *im = image_lookup ? image_lookup(a) : NULL;
    if (im) {
        fputs("{\"domain\":\"guest\",\"path\":",out); quoted(out,im->path,sizeof im->path);
        fprintf(out,",\"offset\":%llu,\"vmaddr\":%llu,\"through_thunk\":%s,\"uuid\":",
            (unsigned long long)(a - im->base),(unsigned long long)(a - im->slide),guest ? "true" : "false");
        if (im->has_uuid) {
            fputc('"',out); for (unsigned k = 0; k < 16; ++k) fprintf(out,"%02x",im->uuid[k]); fputc('"',out);
        } else fputs("null",out);
        fputc('}',out); return;
    }
    Dl_info d = {0};
    if (!address || !dladdr(address,&d) || !d.dli_fbase) { fputs("null",out); return; }
    fputs("{\"domain\":\"native\",\"path\":",out); quoted(out,d.dli_fname,1024);
    fprintf(out,",\"offset\":%llu,\"uuid\":",(unsigned long long)((uintptr_t)address - (uintptr_t)d.dli_fbase));
    struct mach_header_64 h;
    bool found = false;
    if (read_at(d.dli_fbase,&h,sizeof h) && h.magic == MH_MAGIC_64 && h.sizeofcmds < (1u << 20)) {
        size_t at = sizeof h, end = at + h.sizeofcmds;
        for (unsigned i = 0; i < h.ncmds && at + sizeof(struct load_command) <= end; ++i) {
            struct load_command lc;
            if (!read_at((char *)d.dli_fbase + at,&lc,sizeof lc) || lc.cmdsize < sizeof lc || lc.cmdsize > end - at) break;
            if (lc.cmd == LC_UUID && lc.cmdsize >= sizeof(struct uuid_command)) {
                struct uuid_command u;
                if (read_at((char *)d.dli_fbase + at,&u,sizeof u)) {
                    fputc('"',out); for (unsigned k = 0; k < 16; ++k) fprintf(out,"%02x",u.uuid[k]); fputc('"',out);
                    found = true;
                }
                break;
            }
            at += lc.cmdsize;
        }
    }
    if (!found) fputs("null",out);
    fputc('}',out);
}
static int add(survey *s, const void *m)
{
    if (!m) return -1;
    for (unsigned i = 0; i < s->used; ++i) if (s->types[i] == m) return (int)i;
    if (s->used == LIMIT) { ++s->truncated; return -1; }
    s->types[s->used] = m; return (int)s->used++;
}
static void gap(survey *s, unsigned parent, const char *why)
{
    ++s->unresolved;
    fprintf(s->out,"hao-schema {\"event\":\"unresolved\",\"parent\":%u,\"reason\":\"%s\"}\n",parent,why);
}
static void edge(survey *s, unsigned parent, const char *kind, unsigned index, const void *child, intptr_t offset, unsigned flags)
{
    int id = add(s,child);
    fprintf(s->out,"hao-schema {\"event\":\"edge\",\"parent\":%u,\"role\":\"%s\",\"index\":%u,\"child\":%d,\"offset\":%lld,\"flags\":%u}\n",
        parent,kind,index,id,(long long)offset,flags);
    if (!child && strcmp(kind,"empty_case")) gap(s,parent,"unresolved child metadata");
}
static void enum_fields(survey *s, unsigned id, const void *m, const char *desc)
{
    uint32_t flags;
    if (!s->resolve || !read_at(desc,&flags,4) || (flags & 31) != 18) { gap(s,id,"enum descriptor unavailable"); return; }
    const char *fields = relative(desc + 16);
    struct { int32_t type, superclass; uint16_t kind, record_size; uint32_t count; } h;
    if (!fields || !read_at(fields,&h,sizeof h) || (h.kind != 2 && h.kind != 3) || h.record_size < 12 || h.record_size > 256 || h.count > 64) {
        gap(s,id,"enum field descriptor unavailable or exceeds bounds"); return;
    }
    for (unsigned i = 0; i < h.count; ++i) {
        const char *record = fields + 16 + h.record_size * i;
        uint32_t rf;
        if (!read_at(record,&rf,4)) { gap(s,id,"unreadable enum case"); continue; }
        const char *name = relative(record + 4);
        if (!name) { edge(s,id,"empty_case",i,NULL,-1,rf); continue; }
        size_t n = 0; unsigned char c = 0;
        while (n < 4096 && read_at(name + n,&c,1) && c) {
            ++n;
            if (c <= 0x17) n += 4;
            else if (c <= 0x1f) n += sizeof(void *);
        }
        if (n >= 4096 || c || !read_at(name + n,&c,1)) { gap(s,id,"unreadable symbolic type reference"); continue; }
        const void *child = s->resolve(name,n,desc,(flags & 0x80) ? (const void *const *)m + 2 : NULL);
        edge(s,id,(rf & 1) ? "indirect_enum_payload" : "enum_payload",i,child,-1,rf);
    }
}

void hao_swift_schema_dump(FILE *out, const void *root, const void *enclosing,
                           const void *unresolved, uint64_t guest_lr, uint64_t witness)
{
    static atomic_flag active = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&active)) { fputs("hao-schema {\"event\":\"concurrent_or_reentrant_capture_skipped\"}\n",out); return; }
    survey s = {.out=out, .name=dlsym(RTLD_DEFAULT,"swift_getTypeName"), .kind=dlsym(RTLD_DEFAULT,"swift_getMetadataKind"),
        .count=dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveCount"), .offset=dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildOffset"),
        .field=dlsym(RTLD_DEFAULT,"swift_reflectionMirror_recursiveChildMetadata"), .resolve=dlsym(RTLD_DEFAULT,"swift_getTypeByMangledNameInContext2")};
    int r = add(&s,root), e = add(&s,enclosing), u = add(&s,unresolved);
    fprintf(out,"hao-schema {\"event\":\"begin\",\"version\":1,\"root\":%d,\"enclosing\":%d,\"unresolved\":%d,\"guest_lr\":%llu,\"witness\":%llu,\"witness_owner\":",
        r,e,u,(unsigned long long)guest_lr,(unsigned long long)witness);
    owner(out,(const void *)(uintptr_t)witness); fputs("}\n",out);
    if (!s.kind || !s.name || !s.count || !s.offset || !s.field) { gap(&s,0,"reflection API unavailable"); goto done; }
    for (unsigned i = 0; i < s.used; ++i) {
        const void *m = s.types[i], *vptr, *desc = NULL; witnesses v;
        if (!read_at((const char *)m - 8,&vptr,8) || !read_at(vptr,&v,sizeof v)) { gap(&s,i,"unreadable metadata witnesses"); continue; }
        uintptr_t kind = s.kind(m);
        if (kind == 0x200 || kind == 0x201 || kind == 0x202) read_at((const char *)m + 8,&desc,8);
        type_name name = s.name(m,true);
        fprintf(out,"hao-schema {\"event\":\"node\",\"id\":%u,\"metadata\":%llu,\"kind\":%llu,\"name\":",i,(unsigned long long)(uintptr_t)m,(unsigned long long)kind);
        quoted(out,name.text,name.length < 1024 ? name.length : 1024);
        fprintf(out,",\"name_truncated\":%s,\"size\":%llu,\"stride\":%llu,\"flags\":%u,\"extra\":%u,\"descriptor_owner\":",
            name.length > 1024 ? "true" : "false",(unsigned long long)v.size,(unsigned long long)v.stride,v.flags,v.extra);
        owner(out,desc); fputs(",\"destroy_owner\":",out); owner(out,v.fn[1]); fputs("}\n",out);
        if (kind == 0x201 || kind == 0x202) { enum_fields(&s,i,m,desc); continue; }
        if (kind == 0x200 || kind == 0x301) {
            intptr_t n = s.count(m);
            if (n < 0 || n > 64) { gap(&s,i,"field count exceeds bounds"); continue; }
            for (intptr_t k = 0; k < n; ++k) {
                field_info info = {0};
                const void *child = s.field(m,k,&info);
                edge(&s,i,"field",(unsigned)k,child,s.offset(m,k),(info.strong ? 1u : 0u) | (info.var ? 2u : 0u));
                if (info.free_name) info.free_name(info.name);
            }
        } else if (kind == 0x302 || kind == 0x303 || kind == 0x307)
            gap(&s,i,"closure captures or existential payload require a separate value contract");
    }
done:
    fprintf(out,"hao-schema {\"event\":\"end\",\"nodes\":%u,\"unresolved_count\":%u,\"truncated\":%u,\"scope\":\"declarations only; no compatibility or active-case verdict\"}\n",s.used,s.unresolved,s.truncated);
    fflush(out); atomic_flag_clear(&active);
}
