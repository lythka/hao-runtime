/*
 * hao — Swift's __VaListBuilder for arm64 guest code.
 *
 * libswiftCore implements `__VaListBuilder` (stdlib VarArgs.swift: withVaList,
 * getVaList, String(format:) with CVarArg arguments) per architecture. On
 * Apple arm64 a va_list is a pointer to a buffer of words, and the class is
 * a class holding a word buffer: in the 27.x arm64e libswiftCore its
 * allocated word count is at offset 32 and its storage pointer at 40, and the
 * instance is 48 bytes (alignment mask 7). Its init, append, appendWords, allocStorage and
 * deinit are @inlinable, so guest code allocates the object and writes those
 * fields itself; it imports only the metadata accessor, va_list() and
 * deallocStorage(wordCount:storage:). On x86_64 the class has another layout
 * (a SysV register-save header, register counts, a ContiguousArray) and no
 * deallocStorage at all.
 *
 * Bound to x86_64 libswiftCore, a guest builder got the x86_64 metadata,
 * va_list() read the x86_64 fields of an object laid out for arm64, the
 * import of deallocStorage stayed unresolved, and releasing the object ran the
 * x86_64 destroyer over the arm64 fields. hao answers the three imports with
 * the arm64 semantics instead:
 *   - va_list(): storage, or the address of an empty word when there is none;
 *     that is an arm64 va_list, as the bridge converts it for native calls;
 *   - deallocStorage(wordCount:storage:): swift_slowDealloc(storage,
 *     wordCount * 8, 7), the pair of the inlined allocStorage's swift_slowAlloc;
 *   - the metadata accessor: a copy of the x86_64 class metadata, made once,
 *     with instance size 48, alignment mask 7 and a destroyer that frees
 *     storage as the arm64 deinit does and then the object. The x86_64 class
 *     itself is untouched: native code keeps using it.
 * The copy is not registered with the ObjC runtime; the builder is internal to
 * the stdlib and is not sent ObjC messages. getVaList(_:) is not inlinable: its
 * whole body runs in x86_64 libswiftCore and returns an x86_64 va_list, which
 * this does not convert.
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Read off the 27.x arm64e libswiftCore: va_list() loads storage from [x20, #40]; the deallocating
 * deinit frees swift_slowDealloc(storage = [x20, #40], allocated = [x20, #32] * 8, 7) and then
 * swift_deallocClassInstance(self, 48, 7); deallocStorage is swift_slowDealloc(storage, wordCount * 8, 7). */
#define VB_ALLOCATED 32
#define VB_STORAGE 40
#define VB_SIZE 48
#define VB_ALIGN_MASK 7

static const char *const vb_names[] = {
    "_$ss15__VaListBuilderCMa",                                              /* metadata accessor */
    "_$ss15__VaListBuilderC7va_lists03CVaB7PointerVyF",                      /* va_list() */
    "_$ss15__VaListBuilderC14deallocStorage9wordCount7storageySi_SpySiGtF",  /* deallocStorage(wordCount:storage:) */
};
static hao_name_index vb_ix = HAO_NAME_INDEX(vb_names, NULL);

typedef struct { const void *metadata; uint64_t state; } md_response;
static md_response (*native_accessor)(uint64_t request);
static void (*slow_dealloc)(void *ptr, size_t size, size_t align_mask);
static void (*dealloc_class_instance)(void *object, size_t size, size_t align_mask);
static uint64_t empty_word;              /* alignedStorageForEmptyVaLists */
static const void *metadata;             /* the arm64-layout copy */
static pthread_once_t once = PTHREAD_ONCE_INIT;
uint64_t hao_vb_metadata_calls, hao_vb_va_lists, hao_vb_deallocs, hao_vb_destroyed;

/* HeapObjectDestroyer: SWIFT_CC(swift) void (SWIFT_CONTEXT HeapObject *) */
__attribute__((swiftcall)) static void vb_destroy(__attribute__((swift_context)) void *object)
{
    uint8_t *o = object;
    uint64_t allocated, storage;
    memcpy(&allocated, o + VB_ALLOCATED, 8);
    memcpy(&storage, o + VB_STORAGE, 8);
    if (storage) slow_dealloc((void *)(uintptr_t)storage, allocated * 8, VB_ALIGN_MASK);
    __atomic_fetch_add(&hao_vb_destroyed, 1, __ATOMIC_RELAXED);
    dealloc_class_instance(object, VB_SIZE, VB_ALIGN_MASK);
}

/* Swift class metadata (x86_64, ObjC interop) around its address point M:
 * M[-2] destroyer, M[-1] value witnesses; M+48 instance size (u32) and
 * alignment mask (u16); M+56 class size and class address point (u32 each):
 * the whole object is class size bytes starting class address point bytes
 * before M. */
static void vb_init(void)
{
    native_accessor = dlsym(RTLD_DEFAULT, vb_names[0] + 1);
    slow_dealloc = dlsym(RTLD_DEFAULT, "swift_slowDealloc");
    dealloc_class_instance = dlsym(RTLD_DEFAULT, "swift_deallocClassInstance");
    if (!native_accessor || !slow_dealloc || !dealloc_class_instance) return;
    const uint8_t *m = native_accessor(0).metadata;
    if (!m) return;
    uint32_t class_size, address_point;
    memcpy(&class_size, m + 56, 4);
    memcpy(&address_point, m + 60, 4);
    if (address_point < 16 || class_size < address_point + 80 || class_size > 4096) return;
    uint8_t *copy = calloc(1, class_size);
    if (!copy) return;
    memcpy(copy, m - address_point, class_size);
    uint8_t *mc = copy + address_point;
    __attribute__((swiftcall)) void (*destroy)(__attribute__((swift_context)) void *) = vb_destroy;
    memcpy(mc - 16, &destroy, 8);
    uint32_t size = VB_SIZE;
    uint16_t align = VB_ALIGN_MASK;
    memcpy(mc + 48, &size, 4);
    memcpy(mc + 52, &align, 2);
    __atomic_store_n(&metadata, mc, __ATOMIC_RELEASE);
}

static void trap_vb(hao_thread *t, uint64_t k)
{
    hao_cpu *c = &t->cpu;
    if (k == 0) {                              /* metadata accessor: MetadataResponse in x0, x1 */
        __atomic_fetch_add(&hao_vb_metadata_calls, 1, __ATOMIC_RELAXED);
        c->x[0] = (uint64_t)(uintptr_t)__atomic_load_n(&metadata, __ATOMIC_ACQUIRE);
        c->x[1] = 0;                           /* complete */
    } else if (k == 1) {                       /* va_list(): self in x20 */
        __atomic_fetch_add(&hao_vb_va_lists, 1, __ATOMIC_RELAXED);
        uint64_t storage;
        memcpy(&storage, (const uint8_t *)(uintptr_t)c->x[20] + VB_STORAGE, 8);
        c->x[0] = storage ? storage : (uint64_t)(uintptr_t)&empty_word;
    } else {                                   /* deallocStorage(wordCount: x0, storage: x1) */
        __atomic_fetch_add(&hao_vb_deallocs, 1, __ATOMIC_RELAXED);
        if (c->x[1]) slow_dealloc((void *)(uintptr_t)c->x[1], c->x[0] * 8, VB_ALIGN_MASK);
    }
}

void hao_valist_builder_interpose_names(hao_name_fn cb, void *ctx) { hao_name_each(&vb_ix, cb, ctx); }

bool hao_valist_builder_interpose(const char *name, uint64_t *addr)
{
    long k = hao_name_find(&vb_ix, name);
    if (k < 0) return false;
    hao_crit_enter();
    pthread_once(&once, vb_init);
    hao_crit_leave();
    if (!metadata) return false;               /* not this layout: the native binding stands */
    *addr = hao_trap_address(name, trap_vb, (uint64_t)k);
    return true;
}
