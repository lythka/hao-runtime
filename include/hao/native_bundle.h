#ifndef HAO_NATIVE_BUNDLE_H
#define HAO_NATIVE_BUNDLE_H
#include <stddef.h>

/* Linked identity shim to runtime. Native dyld stays first for every open. */
typedef void *(*hao_bundle_open_fn)(const char *, int, char *, size_t);
typedef int (*hao_bundle_sym_fn)(void *, const char *, void **);
typedef int (*hao_bundle_close_fn)(void *);
typedef const char *(*hao_bundle_class_fn)(const void *);
typedef void *(*hao_real_dlopen_fn)(const char *, int);
typedef hao_real_dlopen_fn (*hao_bundle_install_fn)(hao_bundle_open_fn, hao_bundle_sym_fn, hao_bundle_close_fn, hao_bundle_class_fn);
/* Version 2 (hao_identity_bundle_hooks2): the interposers also pass their caller's return address.
 * expand: an @rpath/@loader_path/@executable_path name dlopen'd by guest code, as a path from the calling
 * guest image (1, out filled) or 0 (not a guest caller, or no file); open2: as open, and for a guest caller an
 * ARM-only dylib also loads as a guest image; sym2: as sym, and a guest caller may get a guest function's address. A runtime without these keeps version 1. */
typedef int (*hao_bundle_expand_fn)(const char *, const void *, char *, size_t);
typedef void *(*hao_bundle_open2_fn)(const char *, int, const void *, char *, size_t);   /* open, with the caller */
typedef int (*hao_bundle_sym2_fn)(void *, const char *, const void *, void **);
typedef hao_real_dlopen_fn (*hao_bundle_install2_fn)(hao_bundle_open2_fn, hao_bundle_sym2_fn, hao_bundle_close_fn,
                                                     hao_bundle_class_fn, hao_bundle_expand_fn);
void hao_native_bundles_init(void);
#endif
