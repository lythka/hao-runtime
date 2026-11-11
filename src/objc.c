/*
 * hao — register a guest arm64e image's Objective-C metadata with the
 * native x86_64 runtime, so guest classes/categories live in the same
 * object model as the native frameworks. Struct layouts (class_t,
 * class_ro_t, method lists, category_t, protocol_t) are identical on
 * LP64 arm64 and x86_64; arm64e pointer signatures were stripped by the
 * loader. Guest IMPs stay arm64 addresses: native objc_msgSend reaches
 * them through the non-executable-page callback path.
 */
#include "hao/bridge.h"
#include "hao/runtime.h"
#include "hao/vafmt.h"
#include "hao/bridge.h"
#include "hao/opts.h"
#include <dlfcn.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct objc_image_info { uint32_t version, flags; };
extern Class objc_readClassPair(Class cls, const struct objc_image_info *info);

typedef struct { uint32_t entsize_flags, count; } method_list_t;
typedef struct { uint64_t isa, superclass, cache, vtable, data; } class_t;
typedef struct {
    uint32_t flags, instanceStart, instanceSize, reserved;
    uint64_t ivarLayout, name, baseMethods, baseProtocols, ivars, weakIvarLayout, baseProperties;
    uint64_t swiftMetadataInitializer;   /* only with RO_HAS_SWIFT_INITIALIZER */
} class_ro_t;
typedef struct {
    uint64_t name, cls, instanceMethods, classMethods, protocols, instanceProperties, classProperties;
} category_t;
typedef struct {
    uint64_t isa, mangledName, protocols, instanceMethods, classMethods, optInstanceMethods, optClassMethods,
             instanceProperties;
    uint32_t size, flags;
    uint64_t extendedMethodTypes, demangledName, classProperties;
} protocol_t;
typedef struct { uint64_t count; uint64_t list[]; } protocol_list_t;

/* method_list_t flags (objc4 objc-runtime-new.h, dyld ObjCVisitor.h): small
 * (relative) entries; in shared-cache images also selectors and types as
 * uint32 offsets from the cache's selector buffer (direct) */
#define ML_SMALL 0x80000000u
#define ML_DIRECT_SELS 0x40000000u
#define ML_DIRECT_TYPES 0x20000000u

/* iterate a method list (big, relative/small, or a shared-cache image's
 * direct relative format) */
typedef void (*method_cb)(SEL sel, const char *types, uint64_t imp, void *ctx);

static void for_each_method(uint64_t mlp, method_cb cb, void *ctx)
{
    if (!mlp || (mlp & 1)) return;   /* a list of lists: dsc_methods flattens it */
    method_list_t *ml = (method_list_t *)(uintptr_t)mlp;
    uint32_t fl = ml->entsize_flags, es = fl & 0xfffc;
    bool small = fl & ML_SMALL;
    uint64_t sb = 0;
    if (small && (fl & (ML_DIRECT_SELS | ML_DIRECT_TYPES)) && !(sb = hao_dsc_selector_base())) return;
    uint8_t *e = (uint8_t *)(ml + 1);
    for (uint32_t k = 0; k < ml->count; k++, e += es) {
        SEL sel;
        const char *types;
        uint64_t imp;
        if (small) {
            int32_t *f = (int32_t *)e;
            if (fl & ML_DIRECT_SELS)
                sel = sel_registerName((const char *)(uintptr_t)(sb + (uint32_t)f[0]));
            else {
                /* name -> selector reference (already uniqued) */
                uint64_t selref = (uint64_t)(uintptr_t)&f[0] + (int64_t)f[0];
                sel = *(SEL *)(uintptr_t)selref;
            }
            types = fl & ML_DIRECT_TYPES ? (const char *)(uintptr_t)(sb + (uint32_t)f[1])
                                         : (const char *)((uintptr_t)&f[1] + (intptr_t)f[1]);
            imp = f[2] ? (uint64_t)(uintptr_t)&f[2] + (int64_t)f[2] : 0;
        } else {
            uint64_t *f = (uint64_t *)e;
            sel = sel_registerName((const char *)(uintptr_t)f[0]);
            types = (const char *)(uintptr_t)f[1];
            imp = f[2];
        }
        cb(sel, types, imp, ctx);
    }
}

/* ---- properties (property_list_t: entsize, count, {name, attributes}...) */
typedef void (*property_cb)(const char *name, const char *attrs, void *ctx);
static void for_each_property(uint64_t plp, property_cb cb, void *ctx)
{
    if (!plp) return;
    const method_list_t *pl = (const method_list_t *)(uintptr_t)plp;
    uint32_t es = pl->entsize_flags & ~3u;
    if (es < 16) return;
    const uint8_t *e = (const uint8_t *)(pl + 1);
    for (uint32_t k = 0; k < pl->count; k++, e += es) {
        const uint64_t *f = (const uint64_t *)e;
        if (f[0] && f[1]) cb((const char *)(uintptr_t)f[0], (const char *)(uintptr_t)f[1], ctx);
    }
}

/* "T@\"NSString\",C,N,V_name" -> {T, @"NSString"}, {C, ""}, ... (the runtime
 * rebuilds the same string from the pairs) */
#define MAX_PROP_ATTRS 24
typedef struct {
    objc_property_attribute_t a[MAX_PROP_ATTRS];
    char names[MAX_PROP_ATTRS][2];
    char buf[1024];
    unsigned n;
} prop_attrs;
static bool parse_prop_attrs(const char *s, prop_attrs *p)
{
    size_t len = strlen(s);
    if (len >= sizeof p->buf) return false;
    memcpy(p->buf, s, len + 1);
    p->n = 0;
    for (char *q = p->buf; *q;) {
        if (p->n == MAX_PROP_ATTRS) return false;
        p->names[p->n][0] = *q;
        p->names[p->n][1] = 0;
        char *v = ++q;
        bool quoted = false;
        while (*q && (quoted || *q != ',')) { if (*q == '"') quoted = !quoted; q++; }
        if (*q) *q++ = 0;
        p->a[p->n].name = p->names[p->n];
        p->a[p->n].value = v;
        p->n++;
    }
    return true;
}

/* A category property the class already has keeps the class's attributes
 * (natively the category's copy is found first): class_replaceProperty would
 * rewrite the existing property_t in place, and a native class's lies in the
 * read-only shared cache. */
static void cb_class_property(const char *name, const char *attrs, void *ctx)
{
    prop_attrs pa;
    if (parse_prop_attrs(attrs, &pa)) class_addProperty((Class)ctx, name, pa.a, pa.n);
}

/* ---- protocols */
static Protocol *native_protocol(protocol_t *p);

static void cb_proto_method(SEL sel, const char *types, uint64_t imp, void *ctx)
{
    (void)imp;
    uint64_t *f = ctx;   /* f[0] = Protocol*, f[1] = required, f[2] = instance */
    protocol_addMethodDescription((Protocol *)(uintptr_t)f[0], sel, types, f[1], f[2]);
}

struct proto_prop_ctx { Protocol *p; bool instance; };
static void cb_proto_property(const char *name, const char *attrs, void *ctx)
{
    struct proto_prop_ctx *c = ctx;
    prop_attrs pa;
    if (parse_prop_attrs(attrs, &pa)) protocol_addProperty(c->p, name, pa.a, pa.n, YES, c->instance);
}

/* A runtime-allocated copy: method descriptions and properties, but no
 * extended method types (used only if in-place registration is impossible) */
static Protocol *copy_protocol(protocol_t *p)
{
    const char *name = (const char *)(uintptr_t)p->mangledName;
    Protocol *np = objc_allocateProtocol(name);
    if (!np) return objc_getProtocol(name);
    if (p->protocols) {
        protocol_list_t *pl = (protocol_list_t *)(uintptr_t)p->protocols;
        for (uint64_t k = 0; k < pl->count; k++) protocol_addProtocol(np, native_protocol((protocol_t *)(uintptr_t)pl->list[k]));
    }
    uint64_t ctx[3];
    ctx[0] = (uint64_t)(uintptr_t)np;
    ctx[1] = 1; ctx[2] = 1; for_each_method(p->instanceMethods, cb_proto_method, ctx);
    ctx[1] = 1; ctx[2] = 0; for_each_method(p->classMethods, cb_proto_method, ctx);
    ctx[1] = 0; ctx[2] = 1; for_each_method(p->optInstanceMethods, cb_proto_method, ctx);
    ctx[1] = 0; ctx[2] = 0; for_each_method(p->optClassMethods, cb_proto_method, ctx);
    /* the runtime keeps one instance and one class property list per
     * protocol (optional ones carry '?' in their attributes) */
    struct proto_prop_ctx pc = {np, true};
    for_each_property(p->instanceProperties, cb_proto_property, &pc);
    pc.instance = false;
    if (p->size >= offsetof(protocol_t, classProperties) + 8) for_each_property(p->classProperties, cb_proto_property, &pc);
    objc_registerProtocol(np);
    return np;
}

/* The Protocol for a guest protocol_t, or for a Protocol object (native, or
 * a guest one already registered): found by name, else the guest's own
 * protocol_t is registered in place. It lives in writable guest data and
 * keeps everything a runtime copy would lose: extended method types
 * (NSXPCInterface needs them for every method with a block parameter),
 * properties and the demangled name. objc_registerProtocol accepts it once
 * its isa is __IncompleteProtocol, which is what objc_allocateProtocol sets;
 * that isa also marks a protocol whose sub-protocols are being resolved. */
static Protocol *native_protocol(protocol_t *p)
{
    static Class incomplete;
    static int have_incomplete;
    if (!have_incomplete) { incomplete = objc_getClass("__IncompleteProtocol"); have_incomplete = 1; }
    if (p->isa) return (Protocol *)p;   /* a Protocol object already (or one being registered) */
    const char *name = (const char *)(uintptr_t)p->mangledName;
    Protocol *np = objc_getProtocol(name);
    if (np) return np;
    if (!incomplete) return copy_protocol(p);
    p->isa = (uint64_t)(uintptr_t)incomplete;
    if (p->protocols) {
        protocol_list_t *pl = (protocol_list_t *)(uintptr_t)p->protocols;
        for (uint64_t k = 0; k < pl->count; k++)
            pl->list[k] = (uint64_t)(uintptr_t)native_protocol((protocol_t *)(uintptr_t)pl->list[k]);
    }
    objc_registerProtocol((Protocol *)p);
    np = objc_getProtocol(name);
    return np ? np : (Protocol *)p;
}

/* map guest protocol_t* -> native Protocol*, via the image's protolist */
typedef struct { uint64_t guest; Protocol *native; } pmap_t;
static pmap_t *pmap;
static size_t npmap;
static uint64_t map_proto(uint64_t g)
{
    for (size_t k = 0; k < npmap; k++) if (pmap[k].guest == g) return (uint64_t)(uintptr_t)pmap[k].native;
    return g;
}

static void fix_protocol_list(uint64_t plp)
{
    if (!plp) return;
    protocol_list_t *pl = (protocol_list_t *)(uintptr_t)plp;
    for (uint64_t k = 0; k < pl->count; k++) pl->list[k] = map_proto(pl->list[k]);
}

/* ---- shared-cache images (dsc.c)
 *
 * The cache builder pre-optimises the ObjC metadata of cache images for the
 * arm64e runtime, which knows the cache: method lists are direct relative
 * lists (selector and type offsets from the cache's selector buffer, which
 * the native runtime would read as selector-reference offsets), protocol
 * references point at canonical copies in libobjc's region of the cache,
 * and a class's method, protocol and property lists can be a list of lists
 * with the categories the builder attached. Before such an image's classes
 * and protocols reach the native runtime they get what a non-optimised
 * image has: big method lists of native selectors, the class's own lists
 * only (categories are attached from the image's catlist, as for any
 * image), and protocol lists of native Protocol objects. */

static bool is_guest_data(hao_image *img, uint64_t a);

/* list addresses the image's categories own (excluded from a class's list of lists) */
static struct { uint64_t *v; size_t n; } catl;
static bool catl_has(uint64_t a)
{
    for (size_t k = 0; k < catl.n; k++) if (catl.v[k] == a) return true;
    return false;
}
static void catl_build(hao_image *img)
{
    uint64_t n, a = hao_image_section(img, NULL, "__objc_catlist", &n);
    free(catl.v);
    catl.v = hao_alloc_check(calloc(n / 8 * 5 + 1, sizeof *catl.v), "ObjC category list");
    catl.n = 0;
    for (uint64_t k = 0; k < n / 8; k++) {
        category_t *cat = *(category_t **)(uintptr_t)(a + 8 * k);
        uint64_t l[5] = {cat->instanceMethods, cat->classMethods, cat->protocols, cat->instanceProperties, cat->classProperties};
        for (int j = 0; j < 5; j++) if (l[j]) catl.v[catl.n++] = l[j];
    }
}

/* The image's own lists in a list field: the list itself, or the entries of
 * a list of lists (relative_list_list_t: entsize 8, count, then {imageIndex:16,
 * listOffset:48 from the entry}) that lie in the image and are no category's. */
static int own_lists(hao_image *img, uint64_t p, uint64_t *out, int max)
{
    if (!p) return 0;
    if (!(p & 1)) { out[0] = p; return 1; }
    const method_list_t *ll = (const method_list_t *)(uintptr_t)(p & ~1ull);
    const uint8_t *e = (const uint8_t *)(ll + 1);
    int n = 0;
    for (uint32_t k = 0; k < ll->count && n < max; k++) {
        int64_t ent;
        memcpy(&ent, e + 8 * k, 8);
        uint64_t l = (uint64_t)(uintptr_t)(e + 8 * k) + (uint64_t)(ent >> 16);
        if (is_guest_data(img, l) && !catl_has(l)) out[n++] = l;
    }
    return n;
}

struct big_ctx { uint64_t *e; uint32_t n; };
static void cb_big(SEL sel, const char *types, uint64_t imp, void *ctx)
{
    struct big_ctx *b = ctx;
    uint64_t *e = b->e + 3 * b->n++;
    e[0] = (uint64_t)(uintptr_t)sel;
    e[1] = (uint64_t)(uintptr_t)types;
    e[2] = imp;
}

/* a big method list of native selectors with the image's own methods of field p */
static uint64_t dsc_methods(hao_image *img, uint64_t p)
{
    uint64_t lists[16];
    int n = own_lists(img, p, lists, 16);
    uint32_t total = 0;
    for (int k = 0; k < n; k++) total += ((const method_list_t *)(uintptr_t)lists[k])->count;
    if (!n) return 0;
    method_list_t *ml = hao_alloc_check(calloc(1, sizeof *ml + 24 * (size_t)total), "ObjC method list");
    struct big_ctx b = {(uint64_t *)(ml + 1), 0};
    for (int k = 0; k < n; k++) for_each_method(lists[k], cb_big, &b);
    ml->entsize_flags = 24;   /* not uniqued, not sorted: the runtime fixes it up */
    ml->count = b.n;
    return (uint64_t)(uintptr_t)ml;
}

static void cb_native_method(SEL sel, const char *types, uint64_t imp, void *ctx)
{
    /* An absolute method list in a generic class pattern can have passed
     * through fnslots before Swift allocates the class metadata. Upgrade
     * that plain entry just as bind_guest_imps does for late classes. */
    uint64_t guest = hao_thunk_target(imp);
    if (!guest) guest = imp;
    if (hao_image_for_code(guest)) imp = hao_guest_entry_for_sig(guest, types);
    cb_big(sel, types, imp, ctx);
}

/* Swift generic classes have no entry in __objc_classlist. Their metadata
 * allocator copies the pattern's class/metaclass RO data, including raw guest
 * method lists; register_class never sees these instantiated classes.
 * Metadata.cpp initializeClassMetadataFromPattern installs both RO pointers
 * before returning from swift_allocateGenericClassMetadata. Replace only the
 * copied RO pointers, before realization, with complete typed method lists.
 * The shared pattern and its relative pointers remain untouched. */
void hao_objc_prepare_generic_class(uint64_t metadata)
{
    class_t *cls = (class_t *)(uintptr_t)metadata;
    if (!cls) return;
    class_t *classes[2] = {cls, (class_t *)(uintptr_t)cls->isa};
    for (unsigned k = 0; k < 2; k++) {
        if (!classes[k]) continue;
        class_ro_t *ro = (class_ro_t *)(uintptr_t)(classes[k]->data & ~7ull);
        if (!ro || (ro->flags & (1u << 31)) || !hao_image_for_addr(ro->baseMethods)) continue;
        if (ro->baseMethods & 1) continue;
        const method_list_t *old = (const method_list_t *)(uintptr_t)ro->baseMethods;
        method_list_t *ml = hao_alloc_check(calloc(1, sizeof *ml + 24ull * old->count), "ObjC method list");
        struct big_ctx b = {(uint64_t *)(ml + 1), 0};
        for_each_method(ro->baseMethods, cb_native_method, &b);
        ml->entsize_flags = 24;
        ml->count = b.n;
        ro->baseMethods = (uint64_t)(uintptr_t)ml;
    }
}

/* property lists: only a list of lists needs a new list */
static uint64_t dsc_properties(hao_image *img, uint64_t p)
{
    if (!(p & 1)) return p;
    uint64_t lists[16];
    int n = own_lists(img, p, lists, 16);
    uint32_t total = 0;
    for (int k = 0; k < n; k++) total += ((const method_list_t *)(uintptr_t)lists[k])->count;
    if (!n) return 0;
    method_list_t *pl = hao_alloc_check(calloc(1, sizeof *pl + 16 * (size_t)total), "ObjC property list");
    uint64_t *d = (uint64_t *)(pl + 1);
    for (int k = 0; k < n; k++) {
        const method_list_t *l = (const method_list_t *)(uintptr_t)lists[k];
        uint32_t es = l->entsize_flags & ~3u;
        for (uint32_t j = 0; j < l->count && es >= 16; j++) {
            memcpy(d, (const uint8_t *)(l + 1) + (size_t)j * es, 16);
            d += 2;
            pl->count++;
        }
    }
    pl->entsize_flags = 16;
    return (uint64_t)(uintptr_t)pl;
}

static Protocol *dsc_protocol(uint64_t p);

/* a protocol_list_t of native Protocol objects for field p */
static uint64_t dsc_protocols(hao_image *img, uint64_t p)
{
    uint64_t lists[16];
    int n = own_lists(img, p, lists, 16);
    uint64_t total = 0;
    for (int k = 0; k < n; k++) total += ((const protocol_list_t *)(uintptr_t)lists[k])->count;
    if (!n) return 0;
    protocol_list_t *nl = hao_alloc_check(calloc(1, sizeof *nl + 8 * (size_t)total), "ObjC protocol list");
    for (int k = 0; k < n; k++) {
        const protocol_list_t *l = (const protocol_list_t *)(uintptr_t)lists[k];
        for (uint64_t j = 0; j < l->count; j++) {
            Protocol *q = dsc_protocol(l->list[j]);
            if (q) nl->list[nl->count++] = (uint64_t)(uintptr_t)q;
        }
    }
    return (uint64_t)(uintptr_t)nl;
}

#define PROTOCOL_FIXED_UP_MASK ((1u << 31) | (1u << 30))
#define PROTOCOL_IS_CANONICAL (1u << 29)

/* The native Protocol for a protocol_t anywhere in the cache: by name when
 * the runtime has one; else registered in place when a guest image owns it
 * (its method lists rebuilt with native selectors, extended method types
 * kept: they follow the same method order); else, for a canonical copy in a
 * library hao does not load as guest code, a runtime copy of its methods
 * and sub-protocols, read through the slide info. */
static Protocol *dsc_protocol(uint64_t p)
{
    static Class incomplete;
    if (!incomplete) incomplete = objc_getClass("__IncompleteProtocol");
    if (!p) return NULL;
    bool foreign = hao_dsc_foreign(p);
    hao_image *img = foreign ? NULL : hao_image_for_addr(p);
    if (!foreign && !img) return (Protocol *)(uintptr_t)p;   /* a native Protocol already */
    if (img && !img->dsc) return native_protocol((protocol_t *)(uintptr_t)p);
    /* a guest image's pointers are rewritten; a foreign copy's are read through the slide info */
    const char *name = foreign ? (const char *)(uintptr_t)hao_dsc_load_ptr(p + offsetof(protocol_t, mangledName))
                               : (const char *)(uintptr_t)((const protocol_t *)(uintptr_t)p)->mangledName;
    if (!name) return NULL;
    Protocol *np = objc_getProtocol(name);
    if (np) return np;
    if (img && incomplete) {
        protocol_t *pt = (protocol_t *)(uintptr_t)p;
        if (pt->isa == (uint64_t)(uintptr_t)incomplete) return (Protocol *)pt;   /* being registered (a cycle) */
        hao_dsc_writable(p, sizeof *pt);
        pt->isa = (uint64_t)(uintptr_t)incomplete;
        pt->flags &= ~(PROTOCOL_FIXED_UP_MASK | PROTOCOL_IS_CANONICAL);
        pt->protocols = dsc_protocols(img, pt->protocols);
        pt->instanceMethods = dsc_methods(img, pt->instanceMethods);
        pt->classMethods = dsc_methods(img, pt->classMethods);
        pt->optInstanceMethods = dsc_methods(img, pt->optInstanceMethods);
        pt->optClassMethods = dsc_methods(img, pt->optClassMethods);
        objc_registerProtocol((Protocol *)pt);
        np = objc_getProtocol(name);
        return np ? np : (Protocol *)pt;
    }
    np = objc_allocateProtocol(name);
    if (!np) return objc_getProtocol(name);
    uint64_t pl = hao_dsc_load_ptr(p + offsetof(protocol_t, protocols));
    uint64_t count = pl && hao_dsc_foreign(pl) ? *(const uint64_t *)(uintptr_t)pl : 0;
    for (uint64_t k = 0; k < count && k < 4096; k++) {
        Protocol *q = dsc_protocol(hao_dsc_load_ptr(pl + 8 + 8 * k));
        if (q) protocol_addProtocol(np, q);
    }
    static const struct { size_t off; uint64_t req, inst; } ml[] = {
        {offsetof(protocol_t, instanceMethods), 1, 1}, {offsetof(protocol_t, classMethods), 1, 0},
        {offsetof(protocol_t, optInstanceMethods), 0, 1}, {offsetof(protocol_t, optClassMethods), 0, 0}};
    for (int k = 0; k < 4; k++) {
        uint64_t l = hao_dsc_load_ptr(p + ml[k].off);
        /* relative lists only: a big list's pointers are not rewritten here */
        if (!l || !(((const method_list_t *)(uintptr_t)l)->entsize_flags & ML_SMALL)) continue;
        uint64_t ctx[3] = {(uint64_t)(uintptr_t)np, ml[k].req, ml[k].inst};
        for_each_method(l, cb_proto_method, ctx);
    }
    objc_registerProtocol(np);
    return np;
}

/* A class pointer into a library hao does not load as guest code that the
 * slide-info binding could not name: the native class of the same name. */
static uint64_t dsc_class(uint64_t c)
{
    if (!c || !hao_dsc_foreign(c)) return c;
    uint64_t ro = hao_dsc_load_ptr(c + offsetof(class_t, data)) & ~7ull;
    const char *name = ro ? (const char *)(uintptr_t)hao_dsc_load_ptr(ro + offsetof(class_ro_t, name)) : NULL;
    Class k = name && hao_dsc_foreign((uint64_t)(uintptr_t)name) ? objc_getClass(name) : Nil;
    HAO_LOG(1, "objc: cache class %#llx (%s) -> %p\n", (unsigned long long)c, name ? name : "?", (void *)k);
    return (uint64_t)(uintptr_t)k;
}

static void dsc_set(uint64_t *field, uint64_t v)
{
    if (*field == v) return;
    hao_dsc_writable((uint64_t)(uintptr_t)field, 8);
    *field = v;
}

/* a class_ro_t of a cache image, before the runtime sees it */
static void dsc_fix_ro(hao_image *img, class_ro_t *ro)
{
    hao_dsc_writable((uint64_t)(uintptr_t)ro, sizeof *ro);
    ro->baseMethods = dsc_methods(img, ro->baseMethods);
    ro->baseProtocols = dsc_protocols(img, ro->baseProtocols);
    ro->baseProperties = dsc_properties(img, ro->baseProperties);
}

static void dsc_fix_class_refs(hao_image *img, const char *sect)
{
    uint64_t n, a = hao_image_section(img, NULL, sect, &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        uint64_t *ref = (uint64_t *)(uintptr_t)(a + 8 * k);
        dsc_set(ref, dsc_class(*ref));
    }
}

/* ---- classes */
static bool is_guest_data(hao_image *img, uint64_t a) { return hao_image_extent(img, a) != 0; }

/* Sets of class addresses (hao/gmap.h: they grow with use; 0 is no class).
 * Written at image registration, as before. */
typedef hao_gmap class_set;
#define CLASS_SET_INIT HAO_GMAP_INIT(1, 1, 9, 17)
static bool set_add(class_set *st, uint64_t c)   /* false: already present (or the set is full) */
{
    bool fresh;
    uint64_t *e = c ? hao_gmap_claim1(st, c, &fresh) : NULL;
    if (!e || !fresh) return false;
    hao_gmap_publish1(st, e, c);
    return true;
}
static bool set_has(const class_set *st, uint64_t c) { return hao_gmap_get1(st, c) != NULL; }

/* classes already handed to the native runtime (their `data` now points at
 * runtime-owned class_rw_t, so it must not be read as class_ro_t again) */
static class_set seen_classes = CLASS_SET_INIT;
/* guest classes that were not registered (missing superclass, or the runtime
 * refused them); like the runtime's ignored classes, references to them
 * become nil and their categories and +load are skipped */
static class_set ignored_classes = CLASS_SET_INIT;
static int nignored;
static bool class_ignored(uint64_t c) { return nignored && set_has(&ignored_classes, c); }

#define RO_ROOT (1u << 1)
#define RO_HAS_SWIFT_INITIALIZER (1u << 6)

/* Why a guest class cannot be registered, or NULL. The native runtime ignores
 * a class whose superclass chain ends in a missing weak import instead of
 * reaching a root class; hao binds every missing class import to nil (see
 * hao_resolve_import), so a class whose superclass lives in a library without
 * an x86_64 slice takes the same path. objc_readClassPair dereferences the
 * superclass (its data word, to check that it is realized), so it must never
 * see a class from this list. */
static bool class_missing(uint64_t c) { return !c || hao_is_trap(c); }   /* nil, or bound to an unresolved-import trap */
static bool super_missing(uint64_t super, const class_ro_t *ro) { return super ? hao_is_trap(super) : !(ro->flags & RO_ROOT); }
static const char *class_unusable(class_t *cls, class_ro_t *ro, class_t *meta, class_ro_t *mro)
{
    if (!ro) return "no class_ro_t";
    if (!meta || !mro) return "no metaclass";
    if (super_missing(cls->superclass, ro)) return "superclass missing";
    if (cls->superclass && class_ignored(cls->superclass)) return "superclass not registered";
    if (class_missing(meta->isa)) return "root metaclass missing";
    if (super_missing(meta->superclass, mro)) return "metaclass superclass missing";
    if (meta->superclass && class_ignored(meta->superclass)) return "metaclass superclass not registered";
    return NULL;
}

static void ignore_class(class_t *cls, class_ro_t *ro, const char *why)
{
    if (set_add(&ignored_classes, (uint64_t)(uintptr_t)cls)) nignored++;
    if (cls->isa && set_add(&ignored_classes, cls->isa)) nignored++;   /* its metaclass */
    HAO_LOG(1, "objc class %s ignored: %s\n", ro && ro->name ? (const char *)(uintptr_t)ro->name : "?", why);
}

/* each class's own +load, read from its class_ro_t before registration: a
 * lookup after the categories are attached would find a category's +load */
static struct { uint64_t cls, imp; } *class_loads;
static size_t nclass_loads, cap_class_loads;
static uint64_t find_load(uint64_t mlp);
static void note_class_load(class_t *cls, uint64_t imp)
{
    if (nclass_loads == cap_class_loads) {
        cap_class_loads = cap_class_loads ? 2 * cap_class_loads : 64;
        class_loads = hao_alloc_check(realloc(class_loads, cap_class_loads * sizeof *class_loads), "ObjC +load list");
    }
    class_loads[nclass_loads].cls = (uint64_t)(uintptr_t)cls;
    class_loads[nclass_loads].imp = imp;
    nclass_loads++;
}
static uint64_t class_load(uint64_t cls)
{
    for (size_t k = 0; k < nclass_loads; k++) if (class_loads[k].cls == cls) return class_loads[k].imp;
    return 0;
}

/* The image's classes, superclasses first, prepared from their class_ro_t
 * before any of them is registered. Registering one can run guest code (a
 * Swift metadata initializer), and that code can realize other classes of
 * the image through the runtime (field types, superclasses): their `data`
 * then points at runtime-owned class_rw_t, not at the class_ro_t. */
typedef struct { class_t *cls; class_ro_t *ro; bool deferred; } pending_class;
typedef struct { pending_class *v; size_t n, cap; } pending_list;   /* per image: registration can nest */

#define RO_REALIZED (1u << 31)   /* also RW_REALIZED: class_rw_t's flags (objc4) */

static void prepare_class(hao_image *img, class_t *cls, pending_list *pl, int depth)
{
    if (depth > 64 || !set_add(&seen_classes, (uint64_t)(uintptr_t)cls)) return;
    class_ro_t *ro = (class_ro_t *)(uintptr_t)(cls->data & ~7ull);
    if (ro && (ro->flags & RO_REALIZED)) goto add;   /* the runtime's own already: only named */
    if (img->dsc) dsc_set(&cls->superclass, dsc_class(cls->superclass));
    class_t *super = (class_t *)(uintptr_t)cls->superclass;
    if (ro && super && is_guest_data(img, (uint64_t)(uintptr_t)super)) prepare_class(img, super, pl, depth + 1);
    class_t *meta = (class_t *)(uintptr_t)cls->isa;
    if (img->dsc && meta) {
        dsc_set(&meta->isa, dsc_class(meta->isa));
        dsc_set(&meta->superclass, dsc_class(meta->superclass));
    }
    class_ro_t *mro = meta ? (class_ro_t *)(uintptr_t)(meta->data & ~7ull) : NULL;
    const char *why = class_unusable(cls, ro, meta, mro);
    if (why) { ignore_class(cls, ro, why); return; }
    if (img->dsc) {
        dsc_fix_ro(img, ro);
        dsc_fix_ro(img, mro);
    }
    fix_protocol_list(ro->baseProtocols);
    fix_protocol_list(mro->baseProtocols);
    uint64_t load = find_load(mro->baseMethods);
    if (load) note_class_load(cls, load);
add:
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? 2 * pl->cap : 256;
        pl->v = hao_alloc_check(realloc(pl->v, pl->cap * sizeof *pl->v), "ObjC pending classes");
    }
    pl->v[pl->n++] = (pending_class){cls, ro && (ro->flags & RO_REALIZED) ? NULL : ro};
}

/* give native objc_msgSend x86 entry points for a realized class's guest methods */
static void bind_guest_imps(Class c)
{
    for (int m = 0; m < 2; m++) {
        Class k = m ? object_getClass((id)c) : c;
        unsigned n = 0;
        Method *ms = class_copyMethodList(k, &n);
        for (unsigned j = 0; j < n; j++) {
            uint64_t imp = (uint64_t)(uintptr_t)method_getImplementation(ms[j]);
            /* fnslots can have installed a plain entry before this late
             * class was realized. Its method encoding now supplies the ABI. */
            uint64_t guest = hao_thunk_target(imp);
            if (!guest) guest = imp;
            if (hao_image_for_code(guest))
                method_setImplementation(ms[j], (IMP)(uintptr_t)hao_guest_entry_for_sig(guest, method_getTypeEncoding(ms[j])));
        }
        free(ms);
    }
}

static void register_class(class_t *cls, class_ro_t *ro, const struct objc_image_info *info)
{
    /* a superclass the runtime refused while this class was pending */
    if (cls->superclass && class_ignored(cls->superclass)) { ignore_class(cls, ro, "superclass not registered"); return; }
    /* realized meanwhile by guest code (see pending_class): only name it */
    bool realized = !ro || (*(const uint32_t *)(uintptr_t)(cls->data & 0x00007ffffffffff8ull) & RO_REALIZED) != 0;
    Class c;
    if (realized) {
        HAO_LOG(2, "objc class %s was realized by guest code first\n", class_getName((Class)cls));
        if (!(c = objc_readClassPair((Class)cls, info))) {
            HAO_LOG(0, "objc_readClassPair failed for %s\n", class_getName((Class)cls));
            ignore_class(cls, ro, "objc_readClassPair failed");
            return;
        }
    } else if ((ro->flags & RO_HAS_SWIFT_INITIALIZER) && hao_image_for_code(ro->swiftMetadataInitializer) && !hao_no_guest_code) {
        /* A Swift class whose layout is completed at run time (stored
         * properties of resilient types such as os.Logger or URL). It must
         * not be realized from its compile-time class_ro_t: the Swift
         * metadata initializer computes the field offsets, writes them to
         * the ivar offset globals and the class_ro_t, and realizes the class
         * (swift_updateClassMetadata2 -> _objc_realizeClassFromSwift).
         * objc_readClassPair realizes immediately, which left every ivar at
         * its placeholder offset and swift_updateClassMetadata2 reading the
         * runtime's class_rw_t as the class_ro_t. Natively, map_images only
         * names the class and libobjc runs the initializer when the class
         * is first realized; hao runs it here, then objc_readClassPair names
         * the already-realized class (the Swift path does not) and leaves
         * it as it is. (objcreg, which runs no guest code, keeps the plain
         * registration.) */
        uint64_t args[2] = {(uint64_t)(uintptr_t)cls, 0};
        c = (Class)(uintptr_t)hao_call_guest(hao_current(), ro->swiftMetadataInitializer, args, 2);
        if (c != (Class)cls || !(c = objc_readClassPair(c, info))) {
            HAO_LOG(0, "swift metadata initializer failed for %s\n", (const char *)(uintptr_t)ro->name);
            ignore_class(cls, ro, "swift metadata initializer failed");
            return;
        }
    } else if (!(c = objc_readClassPair((Class)cls, info))) {
        HAO_LOG(0, "objc_readClassPair failed for %s\n", (const char *)(uintptr_t)ro->name);
        ignore_class(cls, ro, "objc_readClassPair failed");
        return;
    }
    bind_guest_imps(c);
    HAO_LOG(2, "objc class %s registered\n", class_getName(c));
}

/* class and super references to ignored classes read nil, as the runtime's
 * remapClassRef makes them for dyld-loaded images */
static void remap_ignored_refs(hao_image *img, const char *sect)
{
    uint64_t n, a = hao_image_section(img, NULL, sect, &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        uint64_t *ref = (uint64_t *)(uintptr_t)(a + 8 * k);
        if (*ref && class_ignored(*ref)) *ref = 0;
    }
}

static void cb_cat(SEL sel, const char *types, uint64_t imp, void *ctx)
{
    (void)sel;
    class_replaceMethod((Class)ctx, sel, (IMP)(uintptr_t)(hao_image_for_code(imp) ? hao_guest_entry_for_sig(imp, types) : imp), types);
}

/* a category's methods, properties and (already mapped) protocols, on a
 * class the runtime has realized */
static void attach_category(const category_t *cat, Class cls, bool class_props)
{
    for_each_method(cat->instanceMethods, cb_cat, (void *)cls);
    for_each_method(cat->classMethods, cb_cat, (void *)object_getClass((id)cls));
    for_each_property(cat->instanceProperties, cb_class_property, (void *)cls);
    if (class_props) for_each_property(cat->classProperties, cb_class_property, (void *)object_getClass((id)cls));
    if (cat->protocols) {
        protocol_list_t *pl = (protocol_list_t *)(uintptr_t)cat->protocols;
        for (uint64_t j = 0; j < pl->count; j++) class_addProtocol(cls, (Protocol *)(uintptr_t)pl->list[j]);
    }
}

/* ------------------------------------- classes the Swift runtime realizes
 * A Swift class whose metadata is completed at run time (singleton metadata
 * initialization: a generic or resilient superclass, whose superclass word
 * is 0 in the image) is not in __objc_classlist and has no
 * RO_HAS_SWIFT_INITIALIZER. libobjc first meets it after the guest's
 * metadata accessor has completed it (swift_getSingletonMetadata -> the
 * guest completion -> swift_initClassMetadata2): the 26A428 libswiftCore
 * realizes it there (swift_instantiateObjCClass), the 27.2 one leaves it to
 * libobjc's first use. A category on such a class (an @objc extension:
 * 27.0 FindMy's FMReachabilityOperation, Shortcuts' window controllers,
 * Reminders' cell views) cannot be attached at image registration: objc4
 * keeps it in unattachedCategories and methodizeClass attaches it when the
 * class is realized. hao called class_replaceMethod on the unrealized class,
 * whose class_ro_t libobjc read as class_rw_t, and faulted in
 * prepareMethodLists before main. hao keeps such categories here and attaches
 * them when the guest's swift_initClassMetadata2 (or a sibling) returns with
 * the class realized, or when its swift_getSingletonMetadata returns the
 * class complete, realizing it then (class_copyMethodList: no +initialize).
 * The class's own guest methods get x86 entry points at that point, as
 * register_class gives listed classes.
 * Swift class stubs (isa 1..15: classes with resilient ancestry) are named
 * only through the stub, which the class's metadata does not point back to;
 * a category on one is reported and not attached (none in 26A428). */
typedef struct { const category_t *cat; uint64_t cls; bool class_props; } pending_cat;
static pending_cat *pend_cats;
static size_t npend_cats, cap_pend_cats;
static size_t npend_live;   /* atomic: entries not yet attached */
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
void hao_objc_fork_child(void) { pend_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; }
static uint64_t st_deferred, st_attached_late, st_late_classes;
/* guest classes realized by the Swift runtime after registration, seen by
 * class_realized (written under pend_lock) */
static class_set late_classes = CLASS_SET_INIT;

#define FAST_DATA_MASK 0x00007ffffffffff8ull
#define FAST_IS_SWIFT_STABLE 2u
static bool is_stub_class(uint64_t c) { uint64_t isa = ((const class_t *)(uintptr_t)c)->isa; return isa >= 1 && isa < 16; }
static bool data_realized(uint64_t c)
{
    uint64_t d = ((const class_t *)(uintptr_t)c)->data & FAST_DATA_MASK;
    return d && (*(const uint32_t *)(uintptr_t)d & RO_REALIZED);
}
/* A resilient Swift class can have native-heap metadata even though its
 * descriptor and methods belong to a guest image. Stable Swift class
 * metadata (FAST_IS_SWIFT_STABLE, objc4) carries Description at word 8,
 * after the class size/address-point fields; see callbacks.c's metadata
 * allocator handling. The metadata address alone does not identify its
 * owner. Do not treat native framework classes as guest classes. */
static bool guest_class_metadata(uint64_t c)
{
    if (hao_image_for_addr(c)) return true;
    const class_t *cls = (const class_t *)(uintptr_t)c;
    return (cls->data & FAST_IS_SWIFT_STABLE) && hao_image_for_addr(((const uint64_t *)(uintptr_t)c)[8]);
}
/* a guest class the native runtime does not know yet: not registered by
 * hao (seen_classes) and not realized since */
static bool guest_class_unknown(uint64_t c)
{
    if (!c || set_has(&seen_classes, c) || !hao_image_for_addr(c)) return false;
    return is_stub_class(c) || !data_realized(c);
}

static void defer_category(const category_t *cat, bool class_props)
{
    hao_crit_enter();
    pthread_mutex_lock(&pend_lock);
    if (npend_cats == cap_pend_cats) {
        cap_pend_cats = cap_pend_cats ? 2 * cap_pend_cats : 16;
        pend_cats = hao_alloc_check(realloc(pend_cats, cap_pend_cats * sizeof *pend_cats), "ObjC pending categories");
    }
    pend_cats[npend_cats++] = (pending_cat){cat, cat->cls, class_props};
    __atomic_fetch_add(&npend_live, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&pend_lock);
    hao_crit_leave();
    __atomic_fetch_add(&st_deferred, 1, __ATOMIC_RELAXED);
    HAO_LOG(1, "objc category %s on %s deferred until the Swift runtime realizes the class\n",
                cat->name ? (const char *)(uintptr_t)cat->name : "?", class_getName((Class)(uintptr_t)cat->cls));
}

/* After the native runtime may have realized guest class c (force: c's
 * metadata is complete, so realize it now if the runtime has not). */
static void class_realized(uint64_t c, bool force)
{
    /* class metadata starts with its isa; any other metadata with its kind (< 0x800) */
    if (!c || (c & 7) || set_has(&late_classes, c) || set_has(&seen_classes, c) ||
        ((const class_t *)(uintptr_t)c)->isa < 0x800 || !guest_class_metadata(c) || (!force && !data_realized(c)))
        return;
    /* late_classes claims the class: one thread attaches its categories */
    hao_crit_enter();
    pthread_mutex_lock(&pend_lock);
    bool fresh = set_add(&late_classes, c);
    pending_cat *mine = NULL;
    size_t n = 0;
    for (size_t k = 0; fresh && k < npend_cats; k++) {
        if (pend_cats[k].cls != c || !pend_cats[k].cat) continue;
        pending_cat *m = hao_alloc_check(realloc(mine, (n + 1) * sizeof *mine), "ObjC pending categories");
        mine = m;
        mine[n++] = pend_cats[k];
        pend_cats[k].cat = NULL;
    }
    __atomic_fetch_sub(&npend_live, n, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&pend_lock);
    hao_crit_leave();
    if (!fresh) { free(mine); return; }
    __atomic_fetch_add(&st_late_classes, 1, __ATOMIC_RELAXED);
    bind_guest_imps((Class)(uintptr_t)c);   /* class_copyMethodList realizes it (no +initialize) */
    if (!data_realized(c)) {
        HAO_LOG(0, "objc class %s not realized after its Swift metadata completed: %zu categories not attached\n",
                class_getName((Class)(uintptr_t)c), n);
        free(mine);
        return;
    }
    for (size_t k = 0; k < n; k++) attach_category(mine[k].cat, (Class)(uintptr_t)c, mine[k].class_props);
    free(mine);
    __atomic_fetch_add(&st_attached_late, n, __ATOMIC_RELAXED);
    if (hao_verbose > 1 || (hao_verbose && n))
        HAO_LOG(0, "objc class %s realized by the Swift runtime: %zu deferred categories attached\n",
                class_getName((Class)(uintptr_t)c), n);
}

/* swift_initClassMetadata2, swift_updateClassMetadata2 (MetadataDependency
 * results) and the void/Class-returning swift_initClassMetadata and
 * swift_updateClassMetadata: x0 is the class's metadata.
 * swift_getSingletonMetadata, which the guest's metadata accessor calls for
 * every request, returns the metadata in x0 and its state in x1 (0:
 * complete); a class already handled costs two lock-free probes. */
#define NSWIFT_CLS 5
static void *swift_cls_fn[NSWIFT_CLS];
static const char *const swift_cls_names[NSWIFT_CLS] = {
    "_swift_initClassMetadata2", "_swift_updateClassMetadata2", "_swift_initClassMetadata", "_swift_updateClassMetadata",
    "_swift_getSingletonMetadata",
};
static void trap_swift_class_init(hao_thread *t, uint64_t k)
{
    hao_cpu *c = &t->cpu;
    uint64_t cls = c->x[0];
    hao_thunk_native_args(t);
    if (k == 4) cls = 0;   /* the result names it */
    t->native_calls++;
    hao_native_call((uint64_t)(uintptr_t)swift_cls_fn[k], c, 16);
    if (k == 4) class_realized(c->x[1] == 0 ? c->x[0] : 0, true);
    else class_realized(cls, false);
}
static hao_name_index swift_cls_ix = HAO_NAME_INDEX(swift_cls_names, NULL);
static bool objc_swift_interpose(const char *name, uint64_t *addr)
{
    long k = hao_name_find(&swift_cls_ix, name);
    if (k < 0) return false;
    if (!swift_cls_fn[k]) swift_cls_fn[k] = dlsym(RTLD_DEFAULT, name + 1);
    if (!swift_cls_fn[k]) return false;
    *addr = hao_trap_address(name, trap_swift_class_init, (uint64_t)k);
    return true;
}

void hao_objc_late_stats(uint64_t *deferred, uint64_t *attached, uint64_t *classes)
{
    *deferred = __atomic_load_n(&st_deferred, __ATOMIC_RELAXED);
    *attached = __atomic_load_n(&st_attached_late, __ATOMIC_RELAXED);
    *classes = __atomic_load_n(&st_late_classes, __ATOMIC_RELAXED);
}

/* the guest IMP of +load in a method list, or 0 */
struct load_ctx { SEL load; uint64_t imp; };
static void cb_load(SEL sel, const char *types, uint64_t imp, void *ctx)
{
    (void)types;
    struct load_ctx *x = ctx;
    if (sel == x->load && !x->imp) x->imp = imp;
}
static uint64_t find_load(uint64_t mlp)
{
    struct load_ctx x = {sel_registerName("load"), 0};
    for_each_method(mlp, cb_load, &x);
    return x.imp;
}

static void call_load(Class cls, uint64_t imp)
{
    if (!imp || !hao_image_for_code(imp)) return;
    uint64_t args[2] = {(uint64_t)(uintptr_t)cls, (uint64_t)(uintptr_t)sel_registerName("load")};
    hao_call_guest(hao_current(), imp, args, 2);
}

void hao_objc_read_image(hao_image *img)
{
    uint64_t n, a;
    const struct objc_image_info *info = (const void *)(uintptr_t)hao_image_section(img, NULL, "__objc_imageinfo", &n);
    if (!info) return;

    /* 1. selectors */
    a = hao_image_section(img, NULL, "__objc_selrefs", &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        uint64_t *ref = (uint64_t *)(uintptr_t)(a + 8 * k);
        *ref = (uint64_t)(uintptr_t)sel_registerName((const char *)(uintptr_t)*ref);
    }
    /* 2. protocols */
    a = hao_image_section(img, NULL, "__objc_protolist", &n);
    pmap = hao_alloc_check(realloc(pmap, (npmap + n / 8 + 1) * sizeof *pmap), "ObjC protocol map");
    for (uint64_t k = 0; k < n / 8; k++) {
        protocol_t *p = *(protocol_t **)(uintptr_t)(a + 8 * k);
        pmap[npmap].guest = (uint64_t)(uintptr_t)p;
        pmap[npmap].native = img->dsc ? dsc_protocol((uint64_t)(uintptr_t)p) : native_protocol(p);
        npmap++;
    }
    a = hao_image_section(img, NULL, "__objc_protorefs", &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        uint64_t *ref = (uint64_t *)(uintptr_t)(a + 8 * k);
        if (img->dsc) dsc_set(ref, (uint64_t)(uintptr_t)dsc_protocol(*ref));   /* canonical copies, by name */
        else *ref = map_proto(*ref);
    }
    if (img->dsc) {
        catl_build(img);
        dsc_fix_class_refs(img, "__objc_classrefs");
        dsc_fix_class_refs(img, "__objc_superrefs");
    }
    /* 3. classes (superclass first) */
    a = hao_image_section(img, NULL, "__objc_classlist", &n);
    pending_list pl = {NULL, 0, 0};
    for (uint64_t k = 0; k < n / 8; k++) prepare_class(img, *(class_t **)(uintptr_t)(a + 8 * k), &pl, 0);
    /* objc4 _read_images publishes every class name with readClass before
     * realizing classes. objc_readClassPair also realizes, so do not apply it
     * to incomplete Swift metadata just to publish a name. Register ordinary
     * classes first: Swift field metadata can resolve a later ObjC class by
     * name. A class with a pending Swift-initialized ancestor must wait too.
     * prepare_class already orders each superclass before its subclasses;
     * propagate that dependency before either phase can run guest code. */
    for (size_t k = 0; k < pl.n; k++) {
        pending_class *p = &pl.v[k];
        p->deferred = p->ro && (p->ro->flags & RO_HAS_SWIFT_INITIALIZER);
        for (size_t j = 0; j < k; j++) {
            if ((uint64_t)(uintptr_t)pl.v[j].cls == p->cls->superclass) {
                p->deferred |= pl.v[j].deferred;
                break;
            }
        }
    }
    for (size_t k = 0; k < pl.n; k++)
        if (!pl.v[k].deferred) register_class(pl.v[k].cls, pl.v[k].ro, info);
    for (size_t k = 0; k < pl.n; k++)
        if (pl.v[k].deferred) register_class(pl.v[k].cls, pl.v[k].ro, info);
    free(pl.v);
    if (nignored) {
        remap_ignored_refs(img, "__objc_classrefs");
        remap_ignored_refs(img, "__objc_superrefs");
    }
    HAO_LOG(2, "objc: classes done\n");
    /* 4. categories */
    a = hao_image_section(img, NULL, "__objc_catlist", &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        category_t *cat = *(category_t **)(uintptr_t)(a + 8 * k);
        if (img->dsc) dsc_set(&cat->cls, dsc_class(cat->cls));
        Class cls = (Class)(uintptr_t)cat->cls;
        if (class_missing(cat->cls) || class_ignored(cat->cls)) continue;   /* category on a missing class: ignored, as natively */
        if (cat->protocols) {
            if (img->dsc) dsc_set(&cat->protocols, dsc_protocols(img, cat->protocols));
            fix_protocol_list(cat->protocols);
        }
        bool class_props = (info->flags & (1u << 6)) != 0;   /* HasCategoryClassProperties: category_t has classProperties */
        if (guest_class_unknown(cat->cls)) {
            if (is_stub_class(cat->cls))
                HAO_LOG(0, "objc category %s on a Swift class stub (%s) is not attached\n",
                        cat->name ? (const char *)(uintptr_t)cat->name : "?", img->path);
            else defer_category(cat, class_props);
            continue;
        }
        attach_category(cat, cls, class_props);
    }
    HAO_LOG(2, "objc: categories done\n");
}

/* +load: classes, then categories */
void hao_objc_run_loads(hao_image *img)
{
    uint64_t n, a;
    if (!hao_image_section(img, NULL, "__objc_imageinfo", &n)) return;
    a = hao_image_section(img, NULL, "__objc_nlclslist", &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        uint64_t c = *(uint64_t *)(uintptr_t)(a + 8 * k);
        if (!class_ignored(c)) call_load((Class)(uintptr_t)c, class_load(c));
    }
    a = hao_image_section(img, NULL, "__objc_nlcatlist", &n);
    for (uint64_t k = 0; k < n / 8; k++) {
        category_t *cat = *(category_t **)(uintptr_t)(a + 8 * k);
        if (class_missing(cat->cls) || class_ignored(cat->cls) || guest_class_unknown(cat->cls)) continue;
        call_load((Class)(uintptr_t)cat->cls, find_load(cat->classMethods));
    }
}

void hao_objc_register_image(hao_image *img)
{
    hao_objc_read_image(img);
    hao_objc_run_loads(img);
}

/* ------------------------------------------------------- objc_msgSend */

typedef struct { id receiver; Class super_class; } objc_super_t;

/*
 * Variadic methods. Apple arm64 passes every variadic argument on the stack in
 * 8-byte slots, and a method's type encoding does not say it is variadic, so
 * the plain register bridge would hand the native method garbage. Known
 * selectors are in va_sels. Any other selector is taken as variadic only when
 * a keyword ending in "Format:"/"format:" takes an object and the class
 * answers <selector>arguments: (initWithFormat:locale:, expressionWithFormat:,
 * raise:format:, ...); that sibling gets an x86 va_list over the guest's
 * stack slots (an exhausted register save area makes va_arg read the
 * overflow area, in 8-byte slots, which is the arm64 layout). A keyword name
 * alone proves nothing: propertyList:isValidForFormat: takes an enum and
 * setPositiveFormat: a plain string. For the VA_FORMAT_LAST entries the
 * format is the last argument; it is expanded here and the method gets
 * @"%@" and the string.
 *
 * Every other variadic method, the format methods of NSString,
 * NSMutableString and NSAssertionHandler among them, goes through
 * msgsend_varsniff below: the native IMP's va_start prologue gives its fixed
 * arguments and the SDK's NS_FORMAT_FUNCTION types the slots from the format
 * (tests/guest/objcva.sh, section nsformat).
 */
enum { VA_ARRAY = 1, VA_DICT, VA_FORMAT_ARGS, VA_FORMAT_LAST };
#define VA_LIST_MAX ((size_t)1 << 24)   /* objects of a nil-terminated list read at most (128 MB of guest stack) */
/* The SDK's declaring class and method kind are part of each contract.
 * A selector is shared globally: CalendarFoundation's fixed-argument NSDate
 * instance method localizedStringWithFormat: is not NSString's class API.
 * Subclasses retain the declared API (including Foundation class clusters);
 * unrelated classes and the other method kind must use ordinary dispatch.
 *
 * Why these stay named: the collection methods take a nil-terminated object
 * list of any length, which these recipes walk to the nil, while
 * msgsend_varsniff passes a method without an SDK format only its first 16
 * stack slots (varsniff.c VS_MIN); NSAlert's informativeTextWithFormat: is
 * AppKit's and is not exercised on a development host (no GUI), so its
 * recipe is kept until a target-system test shows varsniff's equal.
 * (enum_methods below and callbacks.c's tables are hand-written too; both
 * encode SDK declarations, the methods typed NSTextAlignment or
 * NSImageResizingMode and the structs of callback pointers an API takes,
 * so a scan of the SDK headers could generate them as fmtattr_gen.inc is.) */
static const struct { const char *sel; int kind; const char *vsel; const char *owner; bool class_method; } va_sels[] = {
    {"arrayWithObjects:", VA_ARRAY, "arrayWithObjects:count:", "NSArray", true},
    {"initWithObjects:", VA_ARRAY, "initWithObjects:count:", "NSArray", false},
    {"initWithObjects:", VA_ARRAY, "initWithObjects:count:", "NSSet", false},
    {"initWithObjects:", VA_ARRAY, "initWithObjects:count:", "NSOrderedSet", false},
    {"setWithObjects:", VA_ARRAY, "setWithObjects:count:", "NSSet", true},
    {"orderedSetWithObjects:", VA_ARRAY, "orderedSetWithObjects:count:", "NSOrderedSet", true},
    {"dictionaryWithObjectsAndKeys:", VA_DICT, "dictionaryWithObjects:forKeys:count:", "NSDictionary", true},
    {"initWithObjectsAndKeys:", VA_DICT, "initWithObjects:forKeys:count:", "NSDictionary", false},
    {"alertWithMessageText:defaultButton:alternateButton:otherButton:informativeTextWithFormat:", VA_FORMAT_LAST, NULL, "NSAlert", true},
};
static SEL va_sel_cache[sizeof va_sels / sizeof *va_sels];

typedef struct { unsigned gp, fp; void *overflow, *reg; } x86_va[1];
static void va_over(x86_va va, const void *stack) { va->gp = 48; va->fp = 176; va->overflow = (void *)stack; va->reg = NULL; }

static id msg0(id r, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(r, sel_registerName(sel)); }

/* [[NSString alloc] initWithFormat:fmt locale:loc arguments:va], retained */
static id format_string(id fmt, id loc, x86_va va)
{
    id s = msg0((id)objc_getClass("NSString"), "alloc");
    return ((id (*)(id, SEL, id, id, void *))objc_msgSend)(s, sel_registerName("initWithFormat:locale:arguments:"), fmt, loc, va);
}

/* imp(recv, sel, a[0..n-1], last) for n <= 6 integer arguments */
static uint64_t call_n(IMP imp, id recv, SEL sel, const uint64_t *a, int n, uint64_t last)
{
    typedef uint64_t u;
    switch (n) {
    case 0: return ((u (*)(id, SEL, u))imp)(recv, sel, last);
    case 1: return ((u (*)(id, SEL, u, u))imp)(recv, sel, a[0], last);
    case 2: return ((u (*)(id, SEL, u, u, u))imp)(recv, sel, a[0], a[1], last);
    case 3: return ((u (*)(id, SEL, u, u, u, u))imp)(recv, sel, a[0], a[1], a[2], last);
    case 4: return ((u (*)(id, SEL, u, u, u, u, u))imp)(recv, sel, a[0], a[1], a[2], a[3], last);
    case 5: return ((u (*)(id, SEL, u, u, u, u, u, u))imp)(recv, sel, a[0], a[1], a[2], a[3], a[4], last);
    default: return ((u (*)(id, SEL, u, u, u, u, u, u, u))imp)(recv, sel, a[0], a[1], a[2], a[3], a[4], a[5], last);
    }
}
/* the same as a variadic call: imp(recv, sel, a[0..n-1], @"%@", str) */
static uint64_t call_n_fmt(IMP imp, id recv, SEL sel, const uint64_t *a, int n, id fmt, id str)
{
    typedef uint64_t u;
    typedef u (*fv)(id, SEL, ...);
    switch (n) {
    case 0: return ((fv)imp)(recv, sel, fmt, str);
    case 1: return ((fv)imp)(recv, sel, a[0], fmt, str);
    case 2: return ((fv)imp)(recv, sel, a[0], a[1], fmt, str);
    case 3: return ((fv)imp)(recv, sel, a[0], a[1], a[2], fmt, str);
    case 4: return ((fv)imp)(recv, sel, a[0], a[1], a[2], a[3], fmt, str);
    default: return ((fv)imp)(recv, sel, a[0], a[1], a[2], a[3], a[4], fmt, str);
    }
}

/* the 0-based argument index (after self and _cmd) of each keyword ending in
 * "Format:"/"format:", as a bit mask */
static unsigned format_keywords(const char *s)
{
    unsigned mask = 0;
    int arg = 0;
    for (const char *p = s, *kw = s; *p; p++) {
        if (*p != ':') continue;
        if (p - kw >= 6 && !strncmp(p - 5, "ormat", 5) && (p[-6] == 'F' || p[-6] == 'f') && arg < 32) mask |= 1u << arg;
        arg++;
        kw = p + 1;
    }
    return mask;
}

/* integer and object named arguments only (a float or aggregate one means
 * this is not a method the generic path can forward), and an object at
 * every format keyword */
static bool generic_format_args(Method m, unsigned fmt_mask)
{
    unsigned n = m ? method_getNumberOfArguments(m) : 0;
    if (n < 3) return false;
    bool fmt_obj = false;
    for (unsigned k = 2; k < n; k++) {
        char t[64];
        method_getArgumentType(m, k, t, sizeof t);
        const char *q = t;
        while (*q && strchr("rnNoORV", *q)) q++;   /* qualifiers */
        if (strchr("fdD{(", *q)) return false;
        if (k - 2 < 32 && (fmt_mask >> (k - 2) & 1)) { if (*q != '@') return false; fmt_obj = true; }
    }
    return fmt_obj;
}

static bool msgsend_variadic(hao_cpu *c, id recv, Class cls, SEL sel, IMP imp)
{
    if (!va_sel_cache[0]) for (size_t k = 0; k < sizeof va_sels / sizeof *va_sels; k++) va_sel_cache[k] = sel_registerName(va_sels[k].sel);
    const char *name = sel_getName(sel);
    int kind = 0;
    const char *vsel = NULL;
    size_t k;
    for (k = 0; k < sizeof va_sels / sizeof *va_sels; k++)
        if (va_sel_cache[k] == sel && class_isMetaClass(cls) == va_sels[k].class_method) {
            Class owner = objc_getClass(va_sels[k].owner);
            if (!owner) continue;
            if (va_sels[k].class_method) owner = object_getClass((id)owner);
            Class ancestor = cls;
            while (ancestor && ancestor != owner) ancestor = class_getSuperclass(ancestor);
            if (!ancestor) continue;
            /* Replacing a call by its sibling/allocation recipe must not
             * bypass a native override. Inherited implementations compare
             * equal; overrides keep their own dispatch/variadic inference. */
            Method contract = class_getInstanceMethod(owner, sel);
            if (!contract || method_getImplementation(contract) != imp) continue;
            kind = va_sels[k].kind;
            vsel = va_sels[k].vsel;
            break;
        }
    char sibling[512];
    if (!kind) {
        unsigned fm = format_keywords(name);
        if (!fm || strlen(name) + 11 > sizeof sibling) return false;
        snprintf(sibling, sizeof sibling, "%sarguments:", name);
        if (!class_respondsToSelector(cls, sel_registerName(sibling))) return false;
        if (!generic_format_args(class_getInstanceMethod(cls, sel), fm)) return false;
        kind = VA_FORMAT_ARGS;
        vsel = sibling;
    }
    int named = 0;
    for (const char *p = name; *p; p++) named += *p == ':';
    if (named < 1 || named > 6) return false;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    const uint64_t *a = &c->x[2];   /* named arguments */
    x86_va va;
    va_over(va, sp);
    switch (kind) {
    case VA_ARRAY: case VA_DICT: {
        /* nil-terminated, first element in the last named argument */
        size_t n = 0, cap = 64;
        uint64_t *v = hao_alloc_check(malloc(cap * sizeof *v), "ObjC variadic collection");
        for (uint64_t e = a[named - 1]; ; e = sp[n - 1]) {
            if (kind == VA_ARRAY ? !e : (!(n & 1) && !e)) break;
            if (n == cap) { cap *= 2; v = hao_alloc_check(realloc(v, cap * sizeof *v), "ObjC variadic collection"); }
            v[n++] = e;
            if (n == VA_LIST_MAX) {   /* no nil: the guest's list is unterminated; it is cut here, and said once */
                static int said;
                if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
                    HAO_LOG(0, "objc: %c[%s %s]: no nil among the first %zu variadic objects; the list is cut there\n",
                            class_isMetaClass(cls) ? '+' : '-', class_getName(cls), name, n);
                break;
            }
        }
        SEL vs = sel_registerName(vsel);
        IMP vi = class_getMethodImplementation(cls, vs);
        if (kind == VA_ARRAY) {
            c->x[0] = ((uint64_t (*)(id, SEL, const uint64_t *, unsigned long))vi)(recv, vs, v, n);
        } else {   /* objects and keys alternate */
            size_t pairs = n / 2;
            uint64_t *objs = hao_alloc_check(malloc((pairs + 1) * sizeof *objs), "ObjC variadic collection"),
                     *keys = hao_alloc_check(malloc((pairs + 1) * sizeof *keys), "ObjC variadic collection");
            for (size_t j = 0; j < pairs; j++) { objs[j] = v[2 * j]; keys[j] = v[2 * j + 1]; }
            c->x[0] = ((uint64_t (*)(id, SEL, const uint64_t *, const uint64_t *, unsigned long))vi)(recv, vs, objs, keys, pairs);
            free(objs);
            free(keys);
        }
        free(v);
        return true;
    }
    /* every format the native method gets has the guest's long double conversions made double (vafmt.c) */
    case VA_FORMAT_ARGS: {
        SEL vs = sel_registerName(vsel);
        unsigned fm = format_keywords(name);
        uint64_t b[6];
        hao_fmtfix fx[6];
        for (int j = 0; j < named; j++) b[j] = fm >> j & 1 ? hao_fmt_fix('C', a[j], &fx[j]) : a[j];
        c->x[0] = call_n(class_getMethodImplementation(cls, vs), recv, vs, b, named, (uint64_t)(uintptr_t)va);
        for (int j = 0; j < named; j++) if (fm >> j & 1) hao_fmt_done(&fx[j]);
        return true;
    }
    case VA_FORMAT_LAST: {
        hao_fmtfix fx;
        id fmt = (id)(uintptr_t)hao_fmt_fix('C', a[named - 1], &fx);
        id str = format_string(fmt, nil, va);
        hao_fmt_done(&fx);
        id pct = ((id (*)(id, SEL, const char *))objc_msgSend)(msg0((id)objc_getClass("NSString"), "alloc"),
                                                              sel_registerName("initWithUTF8String:"), "%@");
        c->x[0] = call_n_fmt(imp, recv, sel, a, named - 1, pct, str);
        msg0(str, "release");
        msg0(pct, "release");
        return true;
    }
    }
    return false;
}

/*
 * Native variadic methods that neither va_sels nor the sibling rule names:
 * -[OSLogTermDumper format:] (a C format), -[PQLConnection execute:],
 * +[NSString stringWithValidatedFormat:validFormatSpecifiers:error:], ...,
 * reached by objc_msgSend, objc_msgSendSuper or objc_msgSendSuper2. The
 * encoding never says a method is variadic; the IMP's own code does: a SysV
 * variadic function starts with the va_start register save, which gives its
 * fixed GPR and FP argument counts (varsniff.c, the verdict kept per IMP).
 * The encoding lists exactly the fixed arguments, so it must agree, scalar
 * by scalar (an aggregate argument or result, or a count that differs,
 * declines: the call stays on the register bridge and is counted). The
 * call then goes through the varsniff bridge: fixed arguments in registers,
 * the guest's 8-byte stack slots as SysV variadic arguments, typed from the
 * format the SDK declares for the selector (fmtattr_gen.inc, NS_FORMAT_FUNCTION)
 * or else from the last fixed argument when that is a format constant in a
 * guest image. A method without a format passes every slot as an integer,
 * which is exact for the object and integer lists such methods take and
 * counted as untyped (HAO_BRIDGE_CHECK=1 names it).
 */
uint64_t hao_objcva_imps, hao_objcva_calls, hao_objcva_untyped, hao_objcva_declined;

static void objcva_note(hao_thread *t, Class cls, SEL sel, uint64_t imp, const char *what)
{
    Dl_info d;
    hao_crit_enter();          /* dyld's lock: no guest signal handler meanwhile */
    bool named = dladdr((void *)(uintptr_t)imp, &d) && d.dli_fname;
    hao_crit_leave();
    const char *b = named ? strrchr(d.dli_fname, '/') : NULL;
    char where[1100];
    HAO_LOG(0, "bridge: objc variadic %c[%s %s] (%s+%#llx): %s, lr=%#llx%s\n", class_isMetaClass(cls) ? '+' : '-',
            class_getName(cls), sel_getName(sel), named ? (b ? b + 1 : d.dli_fname) : "?",
            (unsigned long long)(named ? imp - (uint64_t)(uintptr_t)d.dli_fbase : imp), what,
            (unsigned long long)t->cpu.x[30], hao_diag_guest_where(t->cpu.x[30], where, sizeof where));
}

/* The bridge's plan for a native IMP whose prologue says variadic with g fixed GPR and f fixed FP
 * arguments, for a method of encoding enc and selector sel: false (declined) when the encoding disagrees;
 * else the fixed GPR count, and the format's index and SDK family (fmt = last fixed, kind 0, when the
 * SDK declares none for sel). An offline scanner, not part of this
 * repository, runs it over whole libraries. */
bool hao_objcva_plan(int g, int f, const char *enc, const char *sel, int *ngp, int *fmt, char *fk)
{
    int eg = -1, ef = -1;
    if (!enc || !hao_sig_scalar_args(enc, &eg, &ef) || ef != f) return false;
    /* Six or more fixed integer arguments leave no GPR for va_start to save: the decoder reports 6 for
     * a prologue that saves none, and fewer when spills of fixed arguments land where a save area would
     * be (-[NUAssertionHandler handleFailureInMethod:object:file:lineNumber:currentlyExecutingJobName:
     * description:] spills r8 and r9 there). The XMM save still proves the method variadic, and the
     * encoding gives the count, up to the eight in x0-x7. */
    if (eg >= 6 && eg <= 8 && g < eg) g = eg;
    if (eg != g) return false;
    /* the SDK's format for this selector, when its first variadic argument is where the code says */
    const hao_fmtattr *fa = hao_fmtattr_sel(sel);
    *ngp = g;
    *fmt = g - 1;
    *fk = 0;
    if (fa && fa->first >= 0 && fa->first + 2 == g && fa->fmt + 2 < g) *fmt = fa->fmt + 2, *fk = fa->kind;
    return true;
}

static bool msgsend_varsniff(hao_thread *t, Class cls, SEL sel, IMP imp, const char *enc)
{
    if (hao_opt.no_varsniff) return false;
    uint64_t fn = (uint64_t)(uintptr_t)imp;
    bool sig = hao_bridge_in_signal_handler(t), fresh;
    int g, nfp, ngp, fmt;
    char fk;
    if (!hao_varsniff_imp(fn, !sig, &g, &nfp, &fresh)) return false;
    bool report = (hao_verbose || hao_opt.bridge_check) && !sig;
    char what[200];
    if (!hao_objcva_plan(g, nfp, enc, sel_getName(sel), &ngp, &fmt, &fk)) {
        if (hao_varsniff_imp_note(fn, 0)) {
            __atomic_fetch_add(&hao_objcva_declined, 1, __ATOMIC_RELAXED);
            if (report) {
                snprintf(what, sizeof what, "declined: the prologue has %d fixed GPR and %d FP arguments, the encoding is %s",
                         g, nfp, enc ? enc : "(none)");
                objcva_note(t, cls, sel, fn, what);
            }
        }
        return false;
    }
    if (fresh) {
        __atomic_fetch_add(&hao_objcva_imps, 1, __ATOMIC_RELAXED);
        if (report) {
            snprintf(what, sizeof what, "%d fixed GPR, %d fixed FP arguments, format %s", ngp, nfp, fk ? "the SDK's" : "the last fixed");
            objcva_note(t, cls, sel, fn, what);
        }
    }
    __atomic_fetch_add(&hao_objcva_calls, 1, __ATOMIC_RELAXED);
    if (!hao_varsniff_call(t, fn, ngp, nfp, hao_narrowret_kind_for_enc(enc), fmt, fk)) {
        __atomic_fetch_add(&hao_objcva_untyped, 1, __ATOMIC_RELAXED);
        if (hao_opt.bridge_check && !sig && hao_varsniff_imp_note(fn, 1))
            objcva_note(t, cls, sel, fn, "variadic slots untyped (no format): passed as integers");
    }
    return true;
}

void hao_note_native_block(uint64_t block);   /* blocks.c */

/* After a native method returned through the register bridge: extend a
 * BOOL/char/short result ('c' is BOOL on x86_64) as arm64 callers expect,
 * and learn the signature of a native block it returned. */
static void msgsend_result(hao_cpu *c, const char *enc)
{
    char nk = hao_narrowret_kind_for_enc(enc);
    if (nk) c->x[0] = hao_narrowret_extend(c->x[0], nk);
    else if (enc && enc[0] == '@' && enc[1] == '?' && c->x[0]) hao_note_native_block(c->x[0]);
}

/* A function pointer a native method returns that is one of hao's entry points for a guest function
 * (the thunk an earlier '^?' argument got: -[NSPointerFunctions hashFunction] after setHashFunction:)
 * goes back to guest code as the guest function's own address, which it compares with its own. */
static void msgsend_fnret(hao_cpu *c)
{
    uint64_t g = c->x[0] ? hao_guest_of_entry(c->x[0]) : 0;
    if (g) c->x[0] = g;
}

/* -[NSInvocation setArgument:atIndex:] copies the argument's bytes into the invocation, which native
 * code invokes later (-invoke, NSTimer, -performSelectorOnMainThread:...): a guest function stored
 * for an argument the invocation's signature types '^?' is stored as its native entry point, from a
 * copy in *buf, as objc_msgSend passes one (trap_msgsend). */
static SEL sel_setarg, sel_methsig, sel_argtype, sel_nargs;
static Class cls_invocation;
static void msgsend_init(void)   /* before the first guest objc_msgSend: its import's binding */
{
    sel_methsig = sel_registerName("methodSignature");
    sel_argtype = sel_registerName("getArgumentTypeAtIndex:");
    sel_nargs = sel_registerName("numberOfArguments");
    cls_invocation = objc_getClass("NSInvocation");
    __atomic_store_n(&sel_setarg, sel_registerName("setArgument:atIndex:"), __ATOMIC_RELEASE);
}
static void msgsend_invocation_arg(hao_thread *t, Class cls, id inv, uint64_t *buf)
{
    hao_cpu *c = &t->cpu;
    if (!c->x[2]) return;
    if (!cls_invocation) cls_invocation = objc_getClass("NSInvocation");   /* Foundation loaded after the binding */
    Class k = cls;
    while (k && k != cls_invocation) k = class_getSuperclass(k);
    if (!k) return;                             /* another class's setArgument:atIndex: */
    id sig = ((id (*)(id, SEL))objc_msgSend)(inv, sel_methsig);
    long idx = (long)c->x[3];
    if (!sig || idx < 0 || (unsigned long)idx >= ((unsigned long (*)(id, SEL))objc_msgSend)(sig, sel_nargs))
        return;
    const char *ty = ((const char *(*)(id, SEL, unsigned long))objc_msgSend)(sig, sel_argtype, (unsigned long)idx);
    while (ty && *ty && strchr("rnNoORV", *ty)) ty++;
    if (!ty || ty[0] != '^' || ty[1] != '?') return;
    uint64_t g;
    memcpy(&g, (const void *)(uintptr_t)c->x[2], 8);   /* a '^?' argument: 8 bytes there */
    if (!hao_is_guest_function(g)) return;
    *buf = hao_thunk_for(g);
    c->x[2] = (uint64_t)(uintptr_t)buf;
}

static void trap_msgsend(hao_thread *t, uint64_t kind)
{
    hao_cpu *c = &t->cpu;
    /* a selector string of the arm64e cache that a cache image built in code and hao did not patch (dsc.c) */
    if (__builtin_expect(c->x[1] - hao_dsc_sel_lo < hao_dsc_sel_span, 0)) c->x[1] = hao_dsc_sel_map(c->x[1]);
    SEL sel = (SEL)(uintptr_t)c->x[1];
    id recv;
    Class cls;
    if (kind == 0) {
        recv = (id)(uintptr_t)c->x[0];
        if (!recv) {   /* messages to nil return zero in x0/x1/v0-v3 */
            c->x[0] = c->x[1] = 0;
            memset(&c->v[0], 0, 4 * sizeof c->v[0]);
            return;
        }
        cls = object_getClass(recv);
    } else {
        objc_super_t *su = (objc_super_t *)(uintptr_t)c->x[0];
        recv = su->receiver;
        cls = kind == 1 ? class_getSuperclass(su->super_class) : su->super_class;
    }
    IMP imp = class_getMethodImplementation(cls, sel);
    HAO_LOG(3, "msgSend [%s %s]\n", class_getName(cls), sel_getName(sel));
    c->x[0] = (uint64_t)(uintptr_t)recv;
    uint64_t g = hao_guest_of_entry((uint64_t)(uintptr_t)imp);
    if (g) { c->pc = g; return; }               /* guest method: stay in translated code */
    hao_thunk_native_blocks(t);                 /* guest blocks passed as arguments */
    if (msgsend_variadic(c, recv, cls, sel, imp)) return;
    Method m = class_getInstanceMethod(cls, sel);
    const char *enc = m ? method_getTypeEncoding(m) : NULL;
    /* guest functions in the slots the encoding types as code ('^?': -[NSArray
     * sortedArrayUsingFunction:context:]'s comparator) get native entry points; untyped slots keep
     * their values (hao_thunk_native_blocks) */
    unsigned codeslots = 0;
    bool fnret = false, simple = !enc || hao_sig_msgsend(enc, &codeslots, &fnret);
    if (codeslots) hao_thunk_native_code_slots(t, codeslots);
    uint64_t invarg;                            /* -[NSInvocation setArgument:atIndex:]'s copy (msgsend_invocation_arg) */
    if (__builtin_expect(sel == sel_setarg && sel, 0)) msgsend_invocation_arg(t, cls, recv, &invarg);
    if (msgsend_varsniff(t, cls, sel, imp, enc)) return;
    /* native methods taking a va_list ([1{__va_list_tag=...}] in the x86
     * encoding): the guest passes an arm64 va_list (pointer to its stack
     * slots); hand the native method an equivalent x86 va_list */
    if (enc && strstr(enc, "__va_list_tag")) {
        x86_va va;
        int argi = 0;
        const char *p = enc;
        /* walk: return type, then args; count top-level args until the va_list */
        int depth = 0, k = -1;
        for (; *p; p++) {
            if (*p == '[' || *p == '{' || *p == '(') { if (depth++ == 0) { k++; if (strncmp(p, "[1{__va_list_tag", 16) == 0) { argi = k - 1; break; } } }
            else if (*p == ']' || *p == '}' || *p == ')') depth--;
            else if (depth == 0 && !(*p >= '0' && *p <= '9') && !strchr("rnNoORVA^\"", *p)) {
                if (*p == '^') continue;
                k++;
                if (*p == '@' && p[1] == '?') p++;
            }
        }
        if (argi > 0 && argi < 8) {
            va->gp = 48; va->fp = 176; va->overflow = (void *)(uintptr_t)c->x[argi]; va->reg = NULL;
            c->x[argi] = (uint64_t)(uintptr_t)va;
            /* the format the SDK declares for this selector (NS_FORMAT_FUNCTION(F, 0)) */
            const hao_fmtattr *fa = hao_fmtattr_sel(sel_getName(sel));
            hao_fmtfix fx = {0};
            if (fa && fa->first == -1 && fa->fmt + 2 < argi) c->x[fa->fmt + 2] = hao_fmt_fix(fa->kind, c->x[fa->fmt + 2], &fx);
            hao_native_call((uint64_t)(uintptr_t)imp, c, 16);
            hao_fmt_done(&fx);
            msgsend_result(c, enc);
            return;
        }
    }
    unsigned enum_result = hao_objc_enum_args(c, 0, false);
    /* the signature path converts the result by its encoding, narrow types included */
    if (!simple && hao_call_native_sig(c, (uint64_t)(uintptr_t)imp, enc)) {
        hao_objc_enum_result(c, enum_result);
        if (fnret) msgsend_fnret(c);
        return;
    }
    uint64_t arg = c->x[2];
    hao_native_call((uint64_t)(uintptr_t)imp, c, 16);
    msgsend_result(c, enc);
    hao_objc_enum_result(c, enum_result);
    if (fnret) msgsend_fnret(c);
    /* -methodForSelector: / +instanceMethodForSelector: hand out an IMP: its type now, from its method */
    static SEL mfs, imfs;
    if (!mfs) { imfs = sel_registerName("instanceMethodForSelector:"); mfs = sel_registerName("methodForSelector:"); }
    if ((sel == mfs || sel == imfs) && arg && c->x[0]) {
        Class k = sel == imfs ? (Class)recv : object_getClass(recv);
        Method m = k ? class_getInstanceMethod(k, (SEL)(uintptr_t)arg) : NULL;
        if (m && (uint64_t)(uintptr_t)method_getImplementation(m) == c->x[0])
            hao_bridge_note_imp(c->x[0], method_getTypeEncoding(m), arg);
    }
}

/* class_addMethod, class_replaceMethod and method_setImplementation with a
 * guest IMP: bind it as read_class does for declared methods, through a
 * signature thunk when the encoding is not register-exact (a struct
 * returned in memory, say). The generic argument pass would give it a plain
 * thunk, which takes the x86 sret pointer for self. */
enum { IMP_ADD, IMP_REPLACE, IMP_SET, IMP_GET, IMP_GET_STRET, IMP_OF_METHOD };
static uint64_t guest_imp_entry(uint64_t imp, const char *types)
{
    /* an IMP loaded from a slot fnslots.c rewrote is a plain thunk */
    if (hao_thunk_target(imp)) imp = hao_thunk_target(imp);
    if (!imp || !hao_image_for_code(imp)) return imp;
    return types ? hao_guest_entry_for_sig(imp, types) : hao_thunk_for(imp);
}
static void trap_set_imp(hao_thread *t, uint64_t kind)
{
    hao_cpu *c = &t->cpu;
    switch (kind) {
    case IMP_ADD: case IMP_REPLACE: {
        Class cls = (Class)(uintptr_t)c->x[0];
        SEL sel = (SEL)(uintptr_t)c->x[1];
        const char *types = (const char *)(uintptr_t)c->x[3];
        IMP imp = (IMP)(uintptr_t)guest_imp_entry(c->x[2], types);
        if (kind == IMP_ADD) c->x[0] = class_addMethod(cls, sel, imp, types);
        else {
            /* the old IMP: the method's type, recorded now, since no method holds it any more */
            Method was = class_getInstanceMethod(cls, sel);
            const char *oenc = was ? method_getTypeEncoding(was) : types;
            c->x[0] = (uint64_t)(uintptr_t)class_replaceMethod(cls, sel, imp, types);
            hao_bridge_note_imp(c->x[0], oenc, (uint64_t)(uintptr_t)sel);
        }
        break;
    }
    case IMP_SET: {
        Method m = (Method)(uintptr_t)c->x[0];
        const char *enc = m ? method_getTypeEncoding(m) : NULL;
        IMP imp = (IMP)(uintptr_t)guest_imp_entry(c->x[1], enc);
        c->x[0] = (uint64_t)(uintptr_t)method_setImplementation(m, imp);
        hao_bridge_note_imp(c->x[0], enc, m ? (uint64_t)(uintptr_t)method_getName(m) : 0);
        break;
    }
    case IMP_GET: case IMP_GET_STRET: {
        /* the IMP's type now, from its method: its first call may come with a nil receiver */
        Class cls = (Class)(uintptr_t)c->x[0];
        SEL sel = (SEL)(uintptr_t)c->x[1];
        IMP r = kind == IMP_GET ? class_getMethodImplementation(cls, sel) : class_getMethodImplementation_stret(cls, sel);
        c->x[0] = (uint64_t)(uintptr_t)r;
        Method m = cls && sel ? class_getInstanceMethod(cls, sel) : NULL;
        if (m && method_getImplementation(m) == r) hao_bridge_note_imp(c->x[0], method_getTypeEncoding(m), (uint64_t)(uintptr_t)sel);
        break;
    }
    case IMP_OF_METHOD: {
        Method m = (Method)(uintptr_t)c->x[0];
        c->x[0] = (uint64_t)(uintptr_t)method_getImplementation(m);
        if (m) hao_bridge_note_imp(c->x[0], method_getTypeEncoding(m), (uint64_t)(uintptr_t)method_getName(m));
        break;
    }
    }
}

static const struct { const char *name; uint64_t kind; } imp_traps[] = {
    {"_class_addMethod", IMP_ADD}, {"_class_replaceMethod", IMP_REPLACE}, {"_method_setImplementation", IMP_SET},
    {"_class_getMethodImplementation", IMP_GET}, {"_class_getMethodImplementation_stret", IMP_GET_STRET},
    {"_method_getImplementation", IMP_OF_METHOD},
};

static hao_name_index imp_traps_ix = HAO_NAME_INDEX(imp_traps, NULL);

bool hao_objc_interpose_imp(const char *name, uint64_t *addr)
{
    long k = hao_name_find(&imp_traps_ix, name);
    if (k >= 0) { *addr = hao_trap_address(name, trap_set_imp, imp_traps[k].kind); return true; }
    return objc_swift_interpose(name, addr);
}

/* objc_retain_xN / objc_release_xN: src/regvar.c */

/* trap_msgsend's kinds: 0 objc_msgSend, 1 objc_msgSendSuper2, 2 objc_msgSendSuper */
static const char *const msgsend_names[] = {"_objc_msgSend", "_objc_msgSendSuper2", "_objc_msgSendSuper"};
static hao_name_index msgsend_ix = HAO_NAME_INDEX(msgsend_names, NULL);

void hao_objc_interpose_names(hao_name_fn cb, void *ctx)
{
    if (!hao_opt.no_msgsend) hao_name_each(&msgsend_ix, cb, ctx);   /* interpose.c asks hao_objc_interpose only then */
    hao_name_each(&imp_traps_ix, cb, ctx);
    hao_name_each(&swift_cls_ix, cb, ctx);
}

bool hao_objc_interpose(const char *name, uint64_t *addr)
{
    long k = hao_name_find(&msgsend_ix, name);
    if (k < 0) return false;
    if (!sel_setarg) msgsend_init();
    *addr = hao_trap_address(name, trap_msgsend, (uint64_t)k);
    return true;
}

/* ----------------------------------------------- bare native IMPs
 * Guest code that calls a native method's IMP directly (from
 * class_getMethodImplementation, method_getImplementation,
 * -methodForSelector:, a swizzled or cached IMP: MenuBarAgent's gated layer
 * class tail-calls the captured -[CALayer setBounds:] with the CGRect in
 * d0-d3) reaches an address dladdr cannot name, so no table types it. The
 * call names its method itself: x0 is the receiver and x1 the selector. The
 * method is the one, at or above the receiver's class, that has this IMP for
 * that selector, and its encoding is the native runtime's, which
 * trap_msgsend marshals the same method by. Method lists are read with
 * class_copyMethodList (no +resolveInstanceMethod:, no +initialize); every
 * read of a register's target is guarded, since the call need not be a
 * method call at all; an IMP that does not match is no answer. */
static bool rd_guarded(uint64_t a, void *buf, size_t n)
{
    mach_vm_size_t got = 0;
    return a >= 0x1000 && a < 0x0000800000000000ull &&
           mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)buf, &got) == KERN_SUCCESS &&
           got == n;
}

/* a readable NUL-terminated string within 256 bytes at a */
static bool cstring_at(uint64_t a)
{
    char buf[256];
    size_t n = 0;
    while (n < sizeof buf) {
        uint64_t at = a + n;
        size_t chunk = 4096 - (size_t)(at & 4095);
        if (chunk > sizeof buf - n) chunk = sizeof buf - n;
        if (!rd_guarded(at, buf + n, chunk)) return false;
        if (memchr(buf + n, 0, chunk)) return true;
        n += chunk;
    }
    return false;
}

/* x86_64 objc4 (runtime/isa.h): ISA_MASK; objc-internal.h: tagged pointers have bit 0 set */
#define X86_ISA_MASK 0x00007ffffffffff8ull

/* k is a class or metaclass: its isa chain reaches a root metaclass that is its own isa */
static bool class_ptr(uint64_t k)
{
    uint64_t m, r, rr;
    if (!k || (k & 7) || !rd_guarded(k, &m, 8)) return false;
    m &= X86_ISA_MASK;
    if (!rd_guarded(m, &r, 8)) return false;
    r &= X86_ISA_MASK;
    if (!rd_guarded(r, &rr, 8)) return false;
    return (rr & X86_ISA_MASK) == r;
}

/* NSText.h and NSImage.h deliberately assign different *meanings* to the
 * same NSInteger on ARM and Intel. ObjC encodings erase the enum typedef.
 * Match the SDK-declared class and selector, never an arbitrary integer or
 * every method named alignment (NSStackView's is NSLayoutAttribute).
 * The two maps are involutions, so native callbacks use the same conversion. */
static struct enum_method {
    const char *owner, *name;
    unsigned slot, kind;       /* slot 0: result; otherwise ARM argument register */
    SEL sel;
} enum_methods[] = {
#define ALIGN(C) {C,"alignment",0,1,0}, {C,"setAlignment:",2,1,0}
    ALIGN("NSControl"), ALIGN("NSCell"), ALIGN("NSText"), ALIGN("NSParagraphStyle"),
    {"NSTextTab","alignment",0,1,0},
    {"NSTextTab","initWithTextAlignment:location:options:",2,1,0},
    {"NSForm","setTitleAlignment:",2,1,0}, {"NSForm","setTextAlignment:",2,1,0},
    {"NSFormCell","titleAlignment",0,1,0}, {"NSFormCell","setTitleAlignment:",2,1,0},
    {"NSSegmentedControl","setAlignment:forSegment:",2,1,0},
    {"NSSegmentedControl","alignmentForSegment:",0,1,0},
    {"NSTextView","setAlignment:range:",2,1,0},
    {"NSMutableAttributedString","setAlignment:range:",2,1,0},
    {"NSImage","resizingMode",0,2,0}, {"NSImage","setResizingMode:",2,2,0},
#undef ALIGN
};
static pthread_once_t enum_once = PTHREAD_ONCE_INIT;
static void enum_init(void)
{
    for (unsigned i = 0; i < sizeof enum_methods / sizeof *enum_methods; i++)
        enum_methods[i].sel = sel_registerName(enum_methods[i].name);
}
static uint64_t enum_convert(uint64_t value, unsigned kind)
{
    if (kind == 1 && (value == 1 || value == 2)) return 3 - value;
    if (kind == 2 && value <= 1) return 1 - value;
    return value;
}
void hao_objc_enum_result(hao_cpu *c, unsigned token)
{
    if (token) c->x[0] = enum_convert(c->x[0], token);
}
unsigned hao_objc_enum_args(hao_cpu *c, uint64_t target, bool callback)
{
    pthread_once(&enum_once, enum_init);
    SEL sel = (SEL)(uintptr_t)c->x[1];
    bool candidate = false;
    for (unsigned i = 0; i < sizeof enum_methods / sizeof *enum_methods; i++)
        if (sel == enum_methods[i].sel) { candidate = true; break; }
    if (!candidate || !c->x[0]) return 0;
    uint64_t isa;
    if ((c->x[0] & 7) || !rd_guarded(c->x[0], &isa, 8) || !class_ptr(isa & X86_ISA_MASK)) return 0;
    Class cls = object_getClass((id)(uintptr_t)c->x[0]);
    if (callback) {
        bool found = false;
        /* Include super implementations: a native super call may target an
         * overridden guest method below the receiver's most-derived method. */
        for (Class k = cls; k && !found; k = class_getSuperclass(k)) {
            unsigned n = 0;
            Method *methods = class_copyMethodList(k, &n);
            for (unsigned j = 0; j < n; j++) {
                if (method_getName(methods[j]) != sel) continue;
                uint64_t imp = (uint64_t)(uintptr_t)method_getImplementation(methods[j]);
                if (imp == target || hao_guest_of_entry(imp) == target) found = true;
            }
            free(methods);
        }
        if (!found) return 0;
    } else if (target && !hao_objc_imp_types(c->x[0], c->x[1], target)) return 0;
    for (unsigned i = 0; i < sizeof enum_methods / sizeof *enum_methods; i++) {
        const struct enum_method *m = &enum_methods[i];
        if (m->sel != sel) continue;
        for (Class k = cls; k; k = class_getSuperclass(k)) {
            if (strcmp(class_getName(k), m->owner)) continue;
            if (!m->slot) return m->kind;
            c->x[m->slot] = enum_convert(c->x[m->slot], m->kind);
            return 0;
        }
    }
    return 0;
}

const char *hao_objc_imp_types(uint64_t self, uint64_t sel, uint64_t imp)
{
    if (!self || !imp || !cstring_at(sel) || !sel_isMapped((SEL)(uintptr_t)sel)) return NULL;
    if (imp == (uint64_t)(uintptr_t)_objc_msgForward || imp == (uint64_t)(uintptr_t)_objc_msgForward_stret)
        return NULL;   /* its ABI is each forwarded method's */
    Class cls;
    if (self & 1) cls = object_getClass((id)(uintptr_t)self);
    else {
        uint64_t isa;
        if ((self & 7) || !rd_guarded(self, &isa, 8) || !class_ptr(isa & X86_ISA_MASK)) return NULL;
        cls = object_getClass((id)(uintptr_t)self);
        if ((uint64_t)(uintptr_t)cls != (isa & X86_ISA_MASK)) return NULL;
    }
    /* the method for this selector first; then any with this IMP (method_exchangeImplementations
     * moved it under another selector, whose encoding a swizzle shares) */
    for (int pass = 0; pass < 2; pass++)
        for (Class k = cls; k && k != (Class)0; k = class_getSuperclass(k)) {
            unsigned n = 0;
            Method *ms = class_copyMethodList(k, &n);
            const char *enc = NULL;
            for (unsigned j = 0; j < n && !enc; j++)
                if ((pass || method_getName(ms[j]) == (SEL)(uintptr_t)sel) &&
                    (uint64_t)(uintptr_t)method_getImplementation(ms[j]) == imp)
                    enc = method_getTypeEncoding(ms[j]);
            free(ms);
            if (enc) return enc;
        }
    return NULL;
}

/* A native variadic method's IMP called bare (-[OSLogTermDumper format:]
 * from -methodForSelector:): its encoding lists only the fixed arguments, so
 * it is bridged as trap_msgsend bridges the same method, from its va_start
 * prologue and the selector's format (msgsend_varsniff), whether or not
 * dladdr names the IMP. */
static void trap_bare_variadic(hao_thread *t, uint64_t imp)
{
    hao_cpu *c = &t->cpu;
    id recv = (id)(uintptr_t)c->x[0];
    SEL sel = (SEL)(uintptr_t)c->x[1];
    const char *enc = hao_objc_imp_types(c->x[0], c->x[1], imp);
    Class cls = recv ? object_getClass(recv) : Nil;
    hao_thunk_native_blocks(t);
    if (msgsend_varsniff(t, cls, sel, (IMP)(uintptr_t)imp, enc)) return;
    hao_native_call(imp, c, 16);
    msgsend_result(c, enc);
}

uint64_t hao_objc_variadic_imp_trap(uint64_t imp, bool decode)
{
    int g, f;
    bool fresh;
    if (hao_opt.no_varsniff || !hao_varsniff_imp(imp, decode, &g, &f, &fresh)) return 0;
    return hao_trap_address("(native variadic IMP)", trap_bare_variadic, imp);
}
