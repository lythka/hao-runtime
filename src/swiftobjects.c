/* Closures stored by guest code in Swift heap objects can reach native code
 * without ever being arguments themselves (AnySequence/AnyIterator boxes).
 * Learn fields from the native runtime's reflection metadata at allocation,
 * then prepare those exact fields when the object crosses a native boundary.
 * Source: Swift ABI Metadata.h, ReflectionMirror.cpp and SwiftShims/Reflection.h.
 * No object names or fixed field offsets are used. This supplies the existing
 * register bridge, not a new claim about Swift reverse ABI compatibility. */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc/malloc.h>
#include "hao/dlsym_once.h"

#define SWIFTCC __attribute__((swiftcall))
typedef struct { const char *name; void (*free_name)(const char *); bool strong, var; } field_info;
static uintptr_t (SWIFTCC *metadata_kind)(const void *);
static intptr_t (SWIFTCC *field_count)(const void *);
static intptr_t (SWIFTCC *field_offset)(const void *, intptr_t);
static const void *(SWIFTCC *field_type)(const void *, intptr_t, field_info *);
static void *native_alloc;
static void *native_observe;

/* Callback result contracts must come from the API, never from a value that
 * merely looks like a code address. A Swift thick closure result is (code,
 * context), in x0/x1 on arm64 and rax/rdx on x86_64. */
enum { FACTORY_SLOTS = 4096 };
static uint64_t factories[FACTORY_SLOTS];
static unsigned factory_count;

enum { MAX_FIELDS = 64 };
typedef struct recipe {
    uint64_t metadata;
    uint32_t n;
    uint32_t offsets[MAX_FIELDS];
} recipe;
/* Under `lock`, readers too; both grow with use (hao/gmap.h). */
static hao_gmap recipes = HAO_GMAP_INIT(2, 1, 8, 13);             /* metadata -> recipe * (4096 at most, as before) */
static hao_gmap objects = HAO_GMAP_INIT_LOAD(3, 1, 7, 16, 512);   /* object -> size, recipe *: 32768 at most, as before */
typedef struct { uint64_t object, size; const recipe *fields; } object_info;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t observed, prepared, factory_results, dropped;
static unsigned object_count;
/* Inclusive bounds of published keys, widened under lock and never reset.
 * Publish each expansion before its new key; retained maps/fork snapshots
 * may have conservative extra space, never a bound excluding an old key. */
static uint64_t object_min = UINT64_MAX, object_max;
static size_t hash(uint64_t p, size_t mask) { return (size_t)((p >> 4) * 0x9e3779b97f4a7c15ull >> 32) & mask; }

static bool read_memory(const hao_thread *t, uint64_t a, void *p, size_t n)
{
    /* x86 movq also reads an unaligned word. Only admit that extra case
     * within one page; arbitrary/cross-page spans keep the exact Mach read.
     * The shared reader retains signal, address and unreadable-page guards. */
    bool word = n == 8 && (a & 4095) <= 4096 - 8;
    if (t && a && (!(a & 7) || word) && n && !(n & 7) && n <= UINT32_MAX && a <= UINT64_MAX - n)
        return hao_probe_read_words(t, a, p, (unsigned)(n / 8));
    mach_vm_size_t got = 0;
    return a && mach_vm_read_overwrite(mach_task_self(), a, n,
        (mach_vm_address_t)(uintptr_t)p, &got) == KERN_SUCCESS && got == n;
}

/* Only structural aggregates are traversed: enum cases, existential payloads,
 * class references and raw pointers need separate lifetime/type information. */
static void fields_of(recipe *r, const void *type, uint64_t base, unsigned depth)
{
    if (!type || base > UINT32_MAX || depth > 8 || r->n == MAX_FIELDS) return;
    uintptr_t kind = metadata_kind(type);
    if (kind == 0x302) {                            /* Function metadata */
        uint64_t flags = ((const uint64_t *)type)[1];
        if ((flags & 0xff0000u) || (flags & 0x20000000u)) return; /* Swift, synchronous */
        if (base <= UINT32_MAX && !(base & 7)) r->offsets[r->n++] = (uint32_t)base;
        return;
    }
    if (kind != 0x200 && kind != 0x301 && !(kind == 0 && depth == 0)) return; /* struct, tuple, root class */
    intptr_t count = field_count(type);
    if (count < 0 || count > MAX_FIELDS) return;
    for (intptr_t i = 0; i < count; i++) {
        field_info info = {0};
        const void *child = field_type(type, i, &info);
        intptr_t offset = field_offset(type, i);
        if (info.free_name) info.free_name(info.name);
        if (offset < 0 || (uint64_t)offset > UINT32_MAX - base) continue;
        fields_of(r, child, base + (uint64_t)offset, depth + 1);
    }
}

static const recipe *recipe_for(uint64_t metadata)
{
    pthread_mutex_lock(&lock);
    const uint64_t *e = hao_gmap_get1(&recipes, metadata);
    pthread_mutex_unlock(&lock);
    if (e) return (const recipe *)(uintptr_t)e[1];
    if (hao_current()->swift_objects_building) return NULL;                      /* reflection can run guest accessors */
    hao_current()->swift_objects_building++;
    recipe *p = calloc(1, sizeof *p);
    if (p) { p->metadata = metadata; fields_of(p, (const void *)(uintptr_t)metadata, 0, 0); }
    hao_current()->swift_objects_building--;
    if (!p) return NULL;
    bool fresh;
    pthread_mutex_lock(&lock);
    uint64_t *slot = metadata ? hao_gmap_claim1(&recipes, metadata, &fresh) : NULL;
    if (slot && fresh) { slot[1] = (uint64_t)(uintptr_t)p; hao_gmap_publish1(&recipes, slot, metadata); pthread_mutex_unlock(&lock); return p; }
    const recipe *old = slot ? (const recipe *)(uintptr_t)slot[1] : NULL;
    pthread_mutex_unlock(&lock);
    free(p);
    if (!old) __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
    return old;
}

static void note_object(uint64_t a, uint64_t metadata, uint64_t size)
{
    if (!a || size < 16 || hao_current()->swift_objects_building || hao_opt.no_swift_thunks) return;
    const recipe *r = recipe_for(metadata);
    if (!r || !r->n) return;
    bool fresh;
    pthread_mutex_lock(&lock);
    /* Exhaustion (32768 objects) leaves the fault route. */
    uint64_t *p = hao_gmap_claim1(&objects, a, &fresh);
    if (p) {
        /* an address seen before (freed natively, then reused) gets the new object's size and fields */
        p[1] = size;
        p[2] = (uint64_t)(uintptr_t)r;
        if (fresh) {
            if (a < __atomic_load_n(&object_min, __ATOMIC_RELAXED))
                __atomic_store_n(&object_min, a, __ATOMIC_RELEASE);
            if (a > __atomic_load_n(&object_max, __ATOMIC_RELAXED))
                __atomic_store_n(&object_max, a, __ATOMIC_RELEASE);
            hao_gmap_publish1(&objects, p, a);
            __atomic_fetch_add(&object_count, 1, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&lock);
    __atomic_fetch_add(p ? &observed : &dropped, 1, __ATOMIC_RELAXED);
}

static void prepare(hao_thread *t, uint64_t a)
{
    if (!a || (a & 7) || hao_opt.no_swift_thunks || !__atomic_load_n(&object_count, __ATOMIC_RELAXED)) return;
    if (a < __atomic_load_n(&object_min, __ATOMIC_ACQUIRE) ||
        a > __atomic_load_n(&object_max, __ATOMIC_ACQUIRE)) return;
    if (!t) t = hao_current();
    /* No malloc or reflection while executing a guest signal handler. */
    if (t->swift_objects_building || t->sig_nest) return;
    /* Keys are atomically published and never removed; grown tables remain
     * mapped. A miss needs no mutable payload. Hits still repeat the lookup
     * under the lock: native allocation/reentry may have replaced the object
     * size and recipe at the same address. */
    if (!hao_gmap_get1(&objects, a)) return;
    object_info found = {0};
    pthread_mutex_lock(&lock);
    const uint64_t *p = hao_gmap_get1(&objects, a);
    if (p) found = (object_info){p[0], p[1], (const recipe *)(uintptr_t)p[2]};
    pthread_mutex_unlock(&lock);
    if (!found.object) return;
    malloc_zone_t *zone = malloc_zone_from_ptr((const void *)(uintptr_t)a);
    size_t live_size = zone ? zone->size(zone, (const void *)(uintptr_t)a) : 0;
    if (live_size < found.size) return;
    /* This snapshot belongs only to this invocation. Coalesce metadata and
     * bounded closure-code reads without retaining object contents between
     * calls. Large/sparse recipes and failed snapshots keep the old read path. */
    enum { SNAPSHOT_BYTES = 512 };
    uint64_t snapshot[SNAPSHOT_BYTES / sizeof(uint64_t)];
    size_t span = sizeof(uint64_t);
    bool bounded = true;
    for (uint32_t k = 0; k < found.fields->n; k++) {
        uint64_t offset = found.fields->offsets[k];
        if (offset > found.size - 16) continue;
        if (offset > sizeof(snapshot) - sizeof(uint64_t)) { bounded = false; break; }
        if (offset + sizeof(uint64_t) > span) span = offset + sizeof(uint64_t);
    }
    bool captured = bounded && read_memory(t, a, snapshot, span);
    uint64_t metadata;
    /* Native destruction is not interposed. An old registry address is only
     * a candidate: revalidate its metadata before inspecting any field. */
    if (captured) metadata = snapshot[0];
    else if (!read_memory(t, a, &metadata, 8)) return;
    if (metadata != found.fields->metadata) return;
    for (uint32_t k = 0; k < found.fields->n; k++) {
        uint64_t offset = found.fields->offsets[k], fn;
        if (offset > found.size - 16) continue;
        if (captured) memcpy(&fn, (const char *)snapshot + offset, sizeof fn);
        else if (!read_memory(t, a + offset, &fn, sizeof fn)) continue;
        if (!hao_is_guest_function(fn)) continue;
        uint64_t entry = hao_thunk_for(fn);
        if (entry != fn && __atomic_compare_exchange_n((uint64_t *)(uintptr_t)(a + offset), &fn, entry,
                false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            __atomic_fetch_add(&prepared, 1, __ATOMIC_RELAXED);
    }
}

void hao_swift_object_prepare(uint64_t a)
{
    /* The shared fast rejection runs before a standalone caller needs TLS. */
    prepare(NULL, a);
}

void hao_swift_objects_args(hao_thread *t)
{
    if (!__atomic_load_n(&object_count, __ATOMIC_RELAXED)) return;
    for (int k = 0; k < 8; k++) prepare(t, t->cpu.x[k]);
    prepare(t, t->cpu.x[20]);         /* native swiftself is outside the C argument mask */
}

void hao_swift_objects_result(hao_thread *t, uint64_t target, uint64_t indirect)
{
    if (__atomic_load_n(&factory_count, __ATOMIC_RELAXED)) {
        size_t h = hash(target, FACTORY_SLOTS - 1);
        for (size_t k = 0; k < FACTORY_SLOTS; k++) {
            uint64_t f = __atomic_load_n(&factories[(h + k) & (FACTORY_SLOTS - 1)], __ATOMIC_ACQUIRE);
            if (!f) break;
            if (f == target) {
                uint64_t fn = t->cpu.x[0];
                if (hao_is_guest_function(fn)) {
                    t->cpu.x[0] = hao_thunk_for(fn);
                    if (t->cpu.x[0] != fn) __atomic_fetch_add(&factory_results, 1, __ATOMIC_RELAXED);
                }
                break;
            }
        }
    }
    if (!__atomic_load_n(&object_count, __ATOMIC_RELAXED)) return;
    hao_swift_objects_args(t);
    uint64_t first;
    if (read_memory(t, indirect, &first, sizeof first)) prepare(t, first);
}

static void note_factory(uint64_t f)
{
    uint64_t guest = hao_guest_of_entry(f);
    if (guest) f = guest;
    if (!hao_is_guest_function(f)) return;
    size_t h = hash(f, FACTORY_SLOTS - 1);
    for (size_t k = 0; k < FACTORY_SLOTS; k++) {
        uint64_t *p = &factories[(h + k) & (FACTORY_SLOTS - 1)];
        uint64_t old = 0;
        if (__atomic_compare_exchange_n(p, &old, f, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&factory_count, 1, __ATOMIC_RELAXED);
            return;
        }
        if (old == f) return;
    }
    __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
}

static void trap_observe(hao_thread *t, uint64_t unused)
{
    (void)unused;
    /* Observation.swiftinterface: withObservationTracking<T>(_: () -> T,
     * onChange: @autoclosure () -> @Sendable () -> Void) -> T. The second
     * thick closure is x2/x3; only its result has this closure contract.
     * The apply callback's generic result must remain untouched. */
    note_factory(t->cpu.x[2]);
    hao_thunk_native_args(t);
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)native_observe, &t->cpu, 16);
}

static void trap_alloc(hao_thread *t, uint64_t unused)
{
    (void)unused;
    uint64_t metadata = t->cpu.x[0], size = t->cpu.x[1];
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)native_alloc, &t->cpu, 16);
    if (!t->sig_nest) note_object(t->cpu.x[0], metadata, size);
}

/* withObservationTracking(_:onChange:) and swift_allocObject */
static const char *const objects_traps[] = {"_$s11Observation04withA8Tracking_8onChangexxyXE_yyYbcyXKtlF", "_swift_allocObject"};
static hao_name_index objects_ix = HAO_NAME_INDEX(objects_traps, NULL);

void hao_swift_objects_interpose_names(hao_name_fn cb, void *ctx) { hao_name_each(&objects_ix, cb, ctx); }

bool hao_swift_objects_interpose(const char *name, uint64_t *address)
{
    if (hao_opt.no_swift_thunks) return false;
    long k = hao_name_find(&objects_ix, name);
    if (k < 0) return false;
    if (k == 0) {
        native_observe = dlsym(RTLD_DEFAULT, name + 1);
        if (!native_observe) return false;
        *address = hao_trap_address(name, trap_observe, 0);
        return true;
    }
    if (!native_alloc) {
        native_alloc = HAO_DLSYM("swift_allocObject");
        metadata_kind = HAO_DLSYM("swift_getMetadataKind");
        field_count = HAO_DLSYM("swift_reflectionMirror_recursiveCount");
        field_type = HAO_DLSYM("swift_reflectionMirror_recursiveChildMetadata");
        field_offset = HAO_DLSYM("swift_reflectionMirror_recursiveChildOffset");
    }
    if (!native_alloc || !metadata_kind || !field_count || !field_type || !field_offset) return false;
    *address = hao_trap_address(name, trap_alloc, 0);
    return true;
}
void hao_swift_objects_fork_child(void) { lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }
void hao_swift_objects_stats(void)
{
    fprintf(stderr, "hao-swift-objects: observed=%llu prepared=%llu factories=%u factory_results=%llu dropped=%llu\n",
        (unsigned long long)observed, (unsigned long long)prepared, factory_count,
        (unsigned long long)factory_results, (unsigned long long)dropped);
}
