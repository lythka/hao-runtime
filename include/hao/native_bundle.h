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
void hao_native_bundles_init(void);
#endif
