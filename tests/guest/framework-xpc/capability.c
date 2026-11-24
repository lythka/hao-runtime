// Only checks whether dyld accepts a callback table in this process.
// No image, path, service, or privilege is registered.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
struct callbacks_v1 { uintptr_t version; void (*unused[6])(void); };
/* dyld_priv.h v1: version, dispose, init, deinit, symbols, address, unwind. */
_Static_assert(sizeof(struct callbacks_v1) == 7 * sizeof(uintptr_t), "callback table ABI");
__attribute__((constructor))
static void measure_callback_provider(void) {
    static const struct callbacks_v1 table = { .version = 1 };
    void *(*register_table)(const struct callbacks_v1 *) =
        dlsym(RTLD_DEFAULT, "_dyld_pseudodylib_register_callbacks");
    void (*deregister_table)(void *) =
        dlsym(RTLD_DEFAULT, "_dyld_pseudodylib_deregister_callbacks");
    if (!register_table || !deregister_table) {
        fprintf(stderr, "dyld callback-provider missing export: register=%d deregister=%d\n",
                register_table != NULL, deregister_table != NULL);
        return;
    }
    void *handle = register_table(&table);
    fprintf(stderr, "dyld callback-provider registration %s\n", handle ? "available" : "unavailable");
    if (handle) deregister_table(handle);
}
