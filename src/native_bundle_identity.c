/* Native Foundation/CFBundle must be able to open an ARM-only guest bundle.
 * dyld owns native handles; the runtime owns only explicitly recognized guest
 * handles. The hook is enabled after the guest main image has been verified. */
#include "hao/native_bundle.h"
#include <dlfcn.h>
#include <pthread.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hao_bundle_open_fn guest_open;
static hao_bundle_sym_fn guest_sym;
static hao_bundle_close_fn guest_close;
static hao_bundle_class_fn guest_class;
static atomic_bool enabled;
static pthread_key_t error_key;
static pthread_once_t error_once = PTHREAD_ONCE_INIT;
struct error_state { int pending; char text[1024]; };
static void error_init(void) { pthread_key_create(&error_key, free); }
static struct error_state *error_state(void)
{
    pthread_once(&error_once, error_init);
    struct error_state *s = pthread_getspecific(error_key);
    if (!s) { s = calloc(1, sizeof *s); if (s) pthread_setspecific(error_key, s); }
    return s;
}
static void set_error(const char *text)
{
    struct error_state *s = error_state();
    if (s) { s->pending = text != NULL; if (text) snprintf(s->text, sizeof s->text, "%s", text); }
}
hao_real_dlopen_fn hao_identity_bundle_hooks(hao_bundle_open_fn o, hao_bundle_sym_fn s, hao_bundle_close_fn c, hao_bundle_class_fn i)
{
    guest_open = o; guest_sym = s; guest_close = c; guest_class = i;
    atomic_store_explicit(&enabled, true, memory_order_release);
    return dlopen; /* references inside this interposing image are the original */
}
static void *bundle_dlopen(const char *path, int mode)
{
    if (!atomic_load_explicit(&enabled, memory_order_acquire)) return dlopen(path, mode);
    set_error(NULL);
    void *h = dlopen(path, mode);
    if (h || !path) return h;
    const char *native = dlerror();
    char original[1024], why[1024] = "";
    snprintf(original, sizeof original, "%s", native ? native : "dlopen failed");
    h = guest_open(path, mode, why, sizeof why);
    if (!h) set_error(why[0] ? why : original);
    else (void)dlerror();
    return h;
}
static void *bundle_dlsym(void *handle, const char *name)
{
    if (!atomic_load_explicit(&enabled, memory_order_acquire)) return dlsym(handle, name);
    set_error(NULL);
    (void)dlerror();
    void *address = NULL;
    if (handle == RTLD_DEFAULT) {
        address = dlsym(handle, name);
        if (address) return address;
        if (guest_sym(handle, name, &address) > 0) { (void)dlerror(); return address; }
        return NULL; /* preserve native lookup error */
    }
    int owned = guest_sym(handle, name, &address);
    if (owned < 0) return dlsym(handle, name);
    set_error(owned ? NULL : "dlsym: guest bundle symbol missing or untyped function export unsupported");
    return address;
}
static int bundle_dlclose(void *handle)
{
    if (!atomic_load_explicit(&enabled, memory_order_acquire)) return dlclose(handle);
    set_error(NULL);
    (void)dlerror();
    int owned = guest_close(handle);
    return owned < 0 ? dlclose(handle) : owned;
}

static char *bundle_dlerror(void)
{
    if (atomic_load_explicit(&enabled, memory_order_acquire)) {
        struct error_state *s = error_state();
        if (s && s->pending) { s->pending = 0; return s->text; }
    }
    return dlerror();
}
static const char *bundle_class_getImageName(Class cls)
{
    if (atomic_load_explicit(&enabled, memory_order_acquire)) {
        const char *path = guest_class(cls);
        if (path) return path;
    }
    return class_getImageName(cls);
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } interposers[] = {
    {(void *)bundle_dlopen, (void *)dlopen}, {(void *)bundle_dlsym, (void *)dlsym},
    {(void *)bundle_class_getImageName, (void *)class_getImageName},
    {(void *)bundle_dlclose, (void *)dlclose}, {(void *)bundle_dlerror, (void *)dlerror},
};
const void *hao_bundle_original(const void *fn)
{
    for (size_t k = 0; k < sizeof interposers / sizeof *interposers; k++)
        if (interposers[k].replacement == fn) return interposers[k].original;
    return NULL;
}
