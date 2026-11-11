/*
 * objcreg — load an arm64/arm64e Mach-O image with hao and register its
 * Objective-C metadata with the native x86_64 runtime, without running any
 * of the image's code: no initializers, no +load, no main, no guest dylibs
 * (HAO_GUEST_ROOT points nowhere) and hao_no_guest_code makes any entry into
 * guest code abort.
 *   objcreg [-v N] --om IMAGE   dump the objcmeta.m classes (objcdump.h)
 *   objcreg [-v N] IMAGE        register, then walk every class of the image
 *                               through the runtime API; print a summary
 *   objcreg [-v N] --root DIR IMAGE   the same, with the arm64e-only
 *                               libraries of the arm64e shared cache under DIR
 *                               loaded as guest images (bound, not run) and
 *                               registered first; a summary line per library
 * Built and run by tests/objc/run.sh and tools/objcreg27.sh.
 */
#include "hao/runtime.h"
#include "objcdump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint64_t isa, superclass, cache, vtable, data; } class64;
typedef struct { uint32_t flags, instanceStart, instanceSize, reserved; uint64_t ivarLayout, name; } ro64;

/* methods walked, and those whose selector is not a native one (the
 * registered string's own selector) or that have no type encoding */
static size_t nmethods, nbad;

static void walk(Class c)
{
    unsigned n = 0;
    for (int meta = 0; meta < 2; meta++) {
        Class k = meta ? object_getClass((id)c) : c;
        Method *m = class_copyMethodList(k, &n);
        for (unsigned j = 0; j < n; j++) {
            SEL s = method_getName(m[j]);
            const char *t = method_getTypeEncoding(m[j]);
            nmethods++;
            if (!s || sel_registerName(sel_getName(s)) != s || !t || !*t) nbad++;
        }
        free(m);
        objc_property_t *p = class_copyPropertyList(k, &n);
        for (unsigned j = 0; j < n; j++) (void)property_getAttributes(p[j]);
        free(p);
        Protocol **pl = class_copyProtocolList(k, &n);
        for (unsigned j = 0; j < n; j++) (void)protocol_getName(pl[j]);
        free(pl);
    }
    Ivar *iv = class_copyIvarList(c, &n);
    for (unsigned j = 0; j < n; j++) (void)ivar_getOffset(iv[j]);
    free(iv);
    (void)class_getInstanceSize(c);
}

int main(int argc, char **argv)
{
    int om = 0, a = 1;
    const char *root = NULL;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "--om")) om = 1;
        else if (!strcmp(argv[a], "--root") && a + 1 < argc) root = argv[++a];
        else if (!strcmp(argv[a], "-v") && a + 1 < argc) hao_verbose = atoi(argv[++a]);
        else { fprintf(stderr, "usage: objcreg [-v N] [--om] [--root DIR] IMAGE\n"); return 64; }
    }
    if (a + 1 != argc) { fprintf(stderr, "usage: objcreg [-v N] [--om] [--root DIR] IMAGE\n"); return 64; }
    setenv("HAO_GUEST_ROOT", root ? root : "/var/empty/hao-objcreg-no-guest-dylibs", 1);
    hao_no_guest_code = true;
    hao_install_fault_handler();
    hao_jit_init();
    char err[512];
    hao_image *img = hao_load_image(argv[a], hao_resolve_import, err, sizeof err);
    if (!img) { fprintf(stderr, "objcreg: %s\n", err); return 2; }
    hao_main_image = img;

    /* the image's own classes and their names, read before registration */
    uint64_t n = 0, cl = hao_image_section(img, NULL, "__objc_classlist", &n);
    size_t ncls = n / 8;
    class64 **cls = calloc(ncls + 1, sizeof *cls);
    char **names = calloc(ncls + 1, sizeof *names);
    for (size_t k = 0; k < ncls; k++) {
        cls[k] = *(class64 **)(uintptr_t)(cl + 8 * k);
        ro64 *ro = (ro64 *)(uintptr_t)(cls[k]->data & 0x00007ffffffffff8ull);
        names[k] = strdup(ro && ro->name ? (const char *)(uintptr_t)ro->name : "?");
    }
    uint64_t ncat = 0;
    hao_image_section(img, NULL, "__objc_catlist", &ncat);

    /* shared-cache libraries (loaded while the image bound): dependencies
     * are listed after the images that loaded them */
    hao_image *dl[256];
    int ndl = 0;
    for (hao_image *im = hao_images; im && ndl < 256; im = im->next) if (im->dsc) dl[ndl++] = im;
    for (int k = ndl - 1; k >= 0; k--) {
        uint64_t m = 0, l = hao_image_section(dl[k], NULL, "__objc_classlist", &m);
        char **dn = calloc(m / 8 + 1, sizeof *dn);
        for (uint64_t j = 0; j < m / 8; j++) {
            class64 *c = *(class64 **)(uintptr_t)(l + 8 * j);
            ro64 *ro = (ro64 *)(uintptr_t)(c->data & 0x00007ffffffffff8ull);
            dn[j] = strdup(ro && ro->name ? (const char *)(uintptr_t)ro->name : "?");
        }
        hao_objc_read_image(dl[k]);
        size_t r = 0, ab = 0;
        nmethods = nbad = 0;
        for (uint64_t j = 0; j < m / 8; j++) {
            Class c = objc_getClass(dn[j]);
            if (c == *(Class *)(uintptr_t)(l + 8 * j)) { r++; walk(c); }
            else { ab++; if (hao_verbose) fprintf(stderr, "objcreg: class %s of %s not registered\n", dn[j], dl[k]->path); }
            free(dn[j]);
        }
        free(dn);
        uint64_t nc = 0;
        hao_image_section(dl[k], NULL, "__objc_catlist", &nc);
        printf("cache %s classes=%llu registered=%zu absent=%zu categories=%llu methods=%zu bad=%zu\n", dl[k]->path,
               (unsigned long long)(m / 8), r, ab, (unsigned long long)(nc / 8), nmethods, nbad);
        fflush(stdout);   /* a later abort (guest code reached) keeps it */
    }

    hao_objc_read_image(img);

    if (om) { om_dump(); return 0; }
    size_t reg = 0, other = 0, absent = 0;
    for (size_t k = 0; k < ncls; k++) {
        Class c = objc_getClass(names[k]);
        if (c == (Class)cls[k]) { reg++; walk(c); }
        else if (c) other++;
        else { absent++; if (hao_verbose) fprintf(stderr, "objcreg: class %s not registered\n", names[k]); }
    }
    printf("classes=%zu registered=%zu other=%zu absent=%zu categories=%llu unresolved=%llu\n", ncls, reg, other,
           absent, (unsigned long long)(ncat / 8), (unsigned long long)img->n_unresolved);
    return 0;
}
