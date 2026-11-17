#pragma once
#include "Prefix.h"
#include <stdint.h>
/* Per-type facts of the Swift value walk (Facts.c), from the generated
 * table src/swiftwalk_gen.inc (format 1; its generator is not part of this
 * repository): for one OS build's cache pair, per fully instantiated Swift
 * type keyed by its mangled name, both architectures' layouts compared by a
 * native comparison (metadata instantiated by name in an arm64 and an x86_64
 * process, value witness tables and each case's byte image dumped), and a
 * verdict:
 *   SAME     every value has the same representation and ownership on both
 *            architectures: passed as found, nothing in it walked;
 *   DYNAMIC  the type's own layout is the same, but a part's representation
 *            is a dynamic type's (an existential's payload, a field holding
 *            one): the walk's rules certify it, the existential ABI rule for
 *            the contained value;
 *   CONVERT  the empty cases' images differ (SWIFTWALK_IMAGES, with each
 *            case's defined-bit masks): an empty case found under its mask
 *            is rewritten to the receiving encoding (Recipe.c case_images);
 *   REFUSE   refused, even where a rule would admit it.
 * A row applies only in a process whose x86_64 shared cache is the table's,
 * and only while the live value witness table has the row's x86_64 size,
 * stride, flags and XI count. A type without a row (every guest-defined
 * type, and native types the table does not list) gets the walk's rules,
 * which refuse what they cannot certify. */
enum { WALK_FACT_SAME = 'S', WALK_FACT_DYNAMIC = 'D', WALK_FACT_CONVERT = 'C', WALK_FACT_REFUSE = 'R' };
/* A row's XI class, for an enum around its type that the table does not
 * list (an Optional a guest instantiates): NONE no XIs; SAME identical XIs;
 * POINTER the XIs differ only in the 8-byte word at XI_OFFSET, a pointer
 * word's (pointer XI p: p on arm64, 2p on x86_64; Recipe.c pointer_word_xi);
 * OTHER anything else (spare bits: the mpenum table's masks). */
enum { WALK_XI_NONE = 'n', WALK_XI_SAME = 's', WALK_XI_POINTER = 'p', WALK_XI_OTHER = 'o' };
enum { WALK_FACT_IMAGE_BYTES = 4096 };
typedef struct {
    char verdict;
    char xi_class;
    int xi_offset;             /* POINTER: the word's offset in the value */
    unsigned nimages;          /* CONVERT: case images, at most 64 */
    int first_image;           /* opaque to callers: walk_fact_image's first index */
    int fixture;
} walk_fact;
/* TYPE's row in this process (the generated table, or under the test switch
 * HAO_SWIFT_WALK_FACTS a fixture file's: tests/guest/svfacts.facts), 0 when
 * it has none. */
int walk_fact_of(const void *type, walk_fact *out);
/* Case image I of a CONVERT row (every case is listed): its case index, each
 * architecture's image (the bytes its destructiveInjectEnumTag writes over
 * zeros) and mask (1 for each bit the injection defines: the same over a
 * zeros and a ones background), the value's size each (at most
 * WALK_FACT_IMAGE_BYTES). A payload case's all-zero mask means it has no
 * image of its own: a value is in it when it is in no empty case. */
int walk_fact_image(const walk_fact *f, unsigned i, unsigned *which, unsigned char *arm, unsigned char *arm_mask,
                    unsigned char *x86, unsigned char *x86_mask, uint64_t size);
/* TYPE's mangled name from the native runtime (swift_getMangledTypeName;
 * the runtime keeps the text), length into *LENGTH; NULL if unavailable. */
const char *walk_fact_mangled(const void *type, uint64_t *length);
/* The table's key of a mangled name: each private context's address
 * removed ("<digits>$<hex>yXZ" -> "$yXZ"), as the generator writes it.
 * Into OUT (CAP bytes, NUL-terminated); 0 when it does not fit. */
int walk_fact_key(const char *mangled, uint64_t n, char *out, uint64_t cap);
void walk_facts_fork_child(void);
/* A test build only: the fixture at PATH (format above), before the first
 * lookup, for tests that load libhao into a native program. */
int walk_facts_load(const char *path);
