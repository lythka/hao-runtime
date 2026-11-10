/* CSSM_ModuleAttach copies the SDK's CSSM_API_MEMORY_FUNCS into native
 * Security allocator objects. Build the callbacks at run time, not in fixups. */
#include <Security/cssm.h>
#include <Security/cssmapple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
_Static_assert(sizeof(CSSM_API_MEMORY_FUNCS) == 40, "CSSM callback layout");
_Static_assert(offsetof(CSSM_API_MEMORY_FUNCS, AllocRef) == 32, "context is not a callback");
static unsigned allocations, frees, bad;
static unsigned cookie;
static void context(void *p) { if (p != &cookie) bad++; }
static void *allocate(CSSM_SIZE n, void *p) { context(p); allocations++; return malloc(n); }
static void release(void *v, void *p) { context(p); frees++; free(v); }
static void *resize(void *v, CSSM_SIZE n, void *p) { context(p); allocations++; return realloc(v, n); }
static void *zero(uint32 n, CSSM_SIZE s, void *p) { context(p); allocations++; return calloc(n, s); }
static void check(CSSM_RETURN r, const char *op) { if (r) { fprintf(stderr, "%s: %d\n", op, r); exit(1); } }
int main(void)
{
    const CSSM_VERSION ver = {2, 0};
    const CSSM_GUID caller = {0xcab03, 1, 2, {1,2,3,4,5,6,7,8}};
    CSSM_PVC_MODE policy = CSSM_PVC_NONE;
    CSSM_CSP_HANDLE handle;
    CSSM_CC_HANDLE ctx;
    CSSM_API_MEMORY_FUNCS mem;
    /* volatile prevents the compiler replacing this with a static table copy. */
    volatile CSSM_API_MEMORY_FUNCS *m = &mem;
    m->malloc_func = allocate; m->free_func = release;
    m->realloc_func = resize; m->calloc_func = zero; m->AllocRef = &cookie;
    check(CSSM_Init(&ver, CSSM_PRIVILEGE_SCOPE_NONE, &caller, CSSM_KEY_HIERARCHY_NONE, &policy, NULL), "init");
    check(CSSM_ModuleLoad(&gGuidAppleCSP, CSSM_KEY_HIERARCHY_NONE, NULL, NULL), "load");
    check(CSSM_ModuleAttach(&gGuidAppleCSP, &ver, &mem, 0, CSSM_SERVICE_CSP, 0,
          CSSM_KEY_HIERARCHY_NONE, NULL, 0, NULL, &handle), "attach");
    check(CSSM_CSP_CreateDigestContext(handle, CSSM_ALGID_SHA256, &ctx), "context");
    CSSM_DATA in = {3, (uint8 *)"abc"}, out = {0, NULL};
    check(CSSM_DigestData(ctx, &in, 1, &out), "digest");
    printf("digest ");
    for (unsigned i = 0; i < out.Length; i++) printf("%02x", out.Data[i]);
    puts("");
    release(out.Data, &cookie);
    check(CSSM_DeleteContext(ctx), "delete");
    check(CSSM_ModuleDetach(handle), "detach");
    check(CSSM_ModuleUnload(&gGuidAppleCSP, NULL, NULL), "unload");
    check(CSSM_Terminate(), "terminate");
    printf("allocator used=%d freed=%d context=%s\n", allocations > 0, frees > 0, bad ? "BAD" : "OK");
    return !allocations || !frees || bad;
}
