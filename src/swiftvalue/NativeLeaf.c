/* A native-owned resilient enum uses its defining metadata's tag, projection,
 * injection and ownership witnesses even when constructed by an ARM client.
 * GenEnum.cpp emits case-index exports only for resilient enums. Frozen enums
 * and a guest defining module cannot use this proof. Payload compatibility is
 * checked separately by Common.c before admitting the value. */
#include "NativeLeaf.h"
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { void *fn[8]; uint64_t size,stride; uint32_t flags,extra; } witnesses;
typedef struct cache {
    const void *type;
    const hao_image *head;
    bool admitted;
    struct cache *next;
} cache;
static cache *entries;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void hao_sv_native_leaf_fork_child(void) { lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }
static bool read_at(uint64_t address,void *out,size_t size)
{
    mach_vm_size_t got = 0;
    return address >= 4096 && address <= UINT64_MAX - size &&
        mach_vm_read_overwrite(mach_task_self(),address,size,(mach_vm_address_t)out,&got) == KERN_SUCCESS && got == size;
}
static bool relative(uint64_t address,uint64_t *out)
{
    int32_t rel;
    if (!read_at(address,&rel,4) || !rel) return false;
    *out = address + rel;
    return true;
}
static bool name_at(uint64_t address,char out[256])
{
    for (unsigned k = 0; k < 256; ++k) {
        if (!read_at(address + k,out + k,1)) return false;
        if (!out[k]) return k != 0;
    }
    return false;
}
static bool context_name(uint64_t desc,char name[256])
{
    uint64_t string;
    return relative(desc + 8,&string) && name_at(string,name);
}
static bool module_name(uint64_t desc,char name[256])
{
    for (unsigned depth = 0; depth < 32; ++depth) {
        uint32_t flags;
        if (!read_at(desc,&flags,4)) return false;
        if ((flags & 31) == 0) return context_name(desc,name);
        uint64_t parent;
        if (!relative(desc + 4,&parent)) return false;
        if (parent & 1) {
            if (!read_at(parent & ~UINT64_C(1),&parent,8)) return false;
        }
        desc = parent;
    }
    return false;
}
static bool guest_collision(const hao_image *head,const char *module)
{
    for (const hao_image *image = head; image; image = hao_images_next(image)) {
        if (__atomic_load_n(&image->released,__ATOMIC_ACQUIRE)) continue;
        const char *sections[] = {"__swift5_types","__swift5_types2"};
        for (unsigned s = 0; s < 2; ++s) {
            uint64_t size = 0;
            uint64_t section = hao_image_section((hao_image *)image,NULL,sections[s],&size);
            if (!size) continue;
            if (!section || size % 4 || size > (1u << 24)) return true;
            for (uint64_t offset = 0; offset < size; offset += 4) {
                int32_t record;
                if (!read_at(section + offset,&record,4)) return true;
                unsigned kind = (unsigned)record & 3;
                if (kind >= 2) continue; /* Objective-C records have no Swift nominal identity. */
                uint64_t desc = section + offset + (record & ~3);
                if (kind == 1 && !read_at(desc,&desc,8)) return true;
                char other[256],owner[256];
                if (!context_name(desc,other) || !module_name(desc,owner)) return true;
                if (!strcmp(module,owner)) return true; /* defining-domain code may know layout */
            }
        }
    }
    return false;
}
typedef char *(*demangle_fn)(const char *,size_t,char *,size_t *,uint32_t);
typedef struct {
    demangle_fn demangle;
    char prefix[600];
    char cases[64][256];
    bool seen[64];
    unsigned count;
} case_proof;
static void case_export(void *context,const char *name,const void *address)
{
    (void)address;
    case_proof *p = context;
    size_t length = strlen(name);
    if (length < 4 || strcmp(name + length - 4,"mFWC")) return;
    char *text = p->demangle(name,length,NULL,NULL,0);
    if (!text) return;
    size_t prefix = strlen(p->prefix);
    if (!strncmp(text,p->prefix,prefix)) {
        const char *tail = text + prefix, *end = strchr(tail,'(');
        if (end) for (unsigned i = 0; i < p->count; ++i)
            if ((size_t)(end - tail) == strlen(p->cases[i]) &&
                !memcmp(tail,p->cases[i],(size_t)(end - tail))) p->seen[i] = true;
    }
    free(text);
}
static bool resilient_cases(uint64_t desc,const char *module,const char *name)
{
    uint64_t fields;
    uint32_t layout,count;
    case_proof p = {.demangle = dlsym(RTLD_DEFAULT,"swift_demangle")};
    if (!p.demangle || !relative(desc + 16,&fields) ||
        !read_at(fields + 8,&layout,4) || layout != (12u << 16 | 2) ||
        !read_at(fields + 12,&count,4) || !count || count > 64) return false;
    p.count = count;
    snprintf(p.prefix,sizeof p.prefix,"enum case for %s.%s.",module,name);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t field_name;
        if (!relative(fields + 16 + 12*i + 8,&field_name) ||
            !name_at(field_name,p.cases[i])) return false;
    }
    if (!hao_bridge_native_exports((void *)desc,case_export,&p)) return false;
    for (unsigned i = 0; i < count; ++i) if (!p.seen[i]) return false;
    return true;
}
static bool prove(const void *metadata,const hao_image *head)
{
    if (hao_image_for_addr((uintptr_t)metadata)) return false;
    const void *const *m = metadata;
    if ((uintptr_t)m[0] != 0x201) return false;
    uint64_t desc = (uintptr_t)m[1],parent;
    uint32_t flags,parent_flags;
    if (!read_at(desc,&flags,4) || (flags & 31) != 18 || (flags & 0x80) ||
        hao_image_for_addr(desc) || !relative(desc + 4,&parent) || (parent & 1) ||
        !read_at(parent,&parent_flags,4) || (parent_flags & 31) != 0) return false;
    Dl_info owner = {0},type_owner = {0};
    if (!dladdr((const void *)desc,&owner) || !dladdr(metadata,&type_owner) ||
        !owner.dli_fbase || owner.dli_fbase != type_owner.dli_fbase) return false;
    const witnesses *v = m[-1];
    if (!v || !v->size || v->size > 4096 || v->stride < v->size || (v->flags & 0x01900000)) return false;
    for (unsigned k = 0; k < 14; ++k) {
        if (k >= 8 && k <= 10) continue;
        uint64_t fn = ((const uint64_t *)v)[k];
        Dl_info function = {0};
        if (!fn || hao_image_for_code(fn) || hao_guest_of_entry(fn) || hao_is_trap(fn) ||
            !dladdr((void *)fn,&function) || !function.dli_fbase) return false;
    }
    char name[256],module[256];
    return context_name(desc,name) && context_name(parent,module) &&
        !guest_collision(head,module) && resilient_cases(desc,module,name);
}
bool hao_sv_native_resilient_enum(const void *metadata)
{
    const hao_image *head = hao_images_first();
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (cache *c = entries; c; c = c->next) if (c->type == metadata && c->head == head) {
        bool ok = c->admitted;
        pthread_mutex_unlock(&lock); hao_crit_leave(); return ok;
    }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    bool ok = prove(metadata,head);
    if (head != hao_images_first()) return false;
    cache *c = malloc(sizeof *c);
    if (!c) return false;
    *c = (cache){metadata,head,ok,NULL};
    hao_crit_enter(); pthread_mutex_lock(&lock);
    c->next = entries; entries = c;
    pthread_mutex_unlock(&lock); hao_crit_leave();
    return ok;
}
