/* Shared declarations for tests/guest/aggalias.c and aggaliashelper.c (tests/guest/aggalias.sh). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* MDPlistObject's layout (MetadataUtilities, tools/aggspi.txt): 24 bytes, MEMORY class on x86_64
 * (on the stack), by reference to a caller-made copy on arm64 (AAPCS64 B.4) */
typedef struct { const void *bytes; uint64_t aux; uint32_t off; uint8_t type; } aa_obj;
/* CGRect's layout: an HFA of four doubles (d0-d3 on arm64), 32 bytes on the x86_64 stack */
typedef struct { struct { double x, y; } origin; struct { double w, h; } size; } aa_rect;

/* libaggaliasimpl.dylib's own names (the defining library) */
const void *aai_data(aa_obj o, size_t *len);
uint8_t aai_type(aa_obj o);
aa_obj aai_index(aa_obj o, uint32_t i);
double aai_area(aa_rect r, double scale);
bool aai_flag(int v);
const void *aai_dl(aa_obj o, size_t *len);
/* libaggaliaspub.dylib's renamed re-exports of them (aap_X -> aai_X) */
const void *aap_data(aa_obj o, size_t *len);
uint8_t aap_type(aa_obj o);
aa_obj aap_index(aa_obj o, uint32_t i);
double aap_area(aa_rect r, double scale);
bool aap_flag(int v);
const void *aap_dl(aa_obj o, size_t *len);
