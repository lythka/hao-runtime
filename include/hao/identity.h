#ifndef HAO_IDENTITY_H
#define HAO_IDENTITY_H
#include <stdbool.h>
#include <stdint.h>
/* Addresses refer to mapped guest segments, not a reserved span or code alias.
 * The loader retains image metadata/path storage after release; readers may
 * retain these pointers as they can retain a native dladdr result. */
typedef struct {
    const char *path;
    const void *header;
    intptr_t slide;
} hao_identity_info;
typedef bool (*hao_identity_lookup_fn)(const void *, hao_identity_info *);
void hao_identity_set_image_lookup(hao_identity_lookup_fn lookup);
bool hao_identity_guest_image(const void *address, hao_identity_info *info);
#endif
