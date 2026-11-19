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
    int guest;                 /* a guest-defined type's row (src/swiftguest_gen.inc): its index + 1; 0 a native row */
} walk_fact;
/* TYPE's row in this process (the generated table, or under the test switch
 * HAO_SWIFT_WALK_FACTS a fixture file's: tests/guest/svfacts.facts), 0 when
 * it has none. A guest-defined struct or enum with no such row gets its row
 * of src/swiftguest_gen.inc (or under HAO_SWIFT_GUEST_FACTS a fixture's:
 * Facts.c), found by identity, never by name: its nominal descriptor lies in
 * a loaded guest image of the row's LC_UUID at the row's offset, its generic
 * arguments are the row's (native ones by walk key and native row, guest ones
 * by their own rows), its live value witness table has the row's size,
 * stride, flags and XI count, and its fields (an enum's cases) are the row's,
 * each a native type with a row of its key or a guest type of the row named,
 * through every nested guest row. A row that matches by identity but whose
 * fields do not is REFUSE. */
int walk_fact_of(const void *type, walk_fact *out);
/* A guest row's verdict for field (an enum's case) INDEX; 0 if not listed. */
int walk_fact_field(const walk_fact *f, unsigned index, char *verdict);
/* The payload type of case INDEX of the enum TYPE (an indirect case: NULL,
 * *INDIRECT 1), from its field descriptor; 0 if it has no such payload case. */
int walk_fact_payload(const void *type, unsigned index, const void **payload, int *indirect);
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
/* How the table answered for each metadata the walk met (Facts.c key_met),
 * once per metadata per process: a native row by its verdict; a guest row
 * (GUEST_INCOMPLETE: its fields are not the row's, refused), or an Optional
 * of a guest row's type with the same XIs; NONE no row for the key; LAYOUT a
 * row for the key whose size, stride, flags or XI count is not the live
 * type's; IDENTITY a SAME row the native identity check declined;
 * OTHER_CACHE the table is not for this process's x86_64 cache. Counted
 * always; written as "hao-swift-key" records under HAO_SWIFT_KEYS. */
enum { WALK_KEY_SAME, WALK_KEY_DYNAMIC, WALK_KEY_CONVERT, WALK_KEY_REFUSE, WALK_KEY_GUEST, WALK_KEY_GUEST_INCOMPLETE,
       WALK_KEY_GUEST_OPTIONAL, WALK_KEY_NONE, WALK_KEY_LAYOUT, WALK_KEY_IDENTITY, WALK_KEY_OTHER_CACHE, WALK_KEY_NROWS };
/* The counts by answer, by where the metadata lies (0 the x86_64 shared
 * cache, 1 a guest image, 2 elsewhere: instantiated at run time), and the
 * metadata not counted because the map of those met was full. */
void walk_keys_counts(uint64_t rows[WALK_KEY_NROWS], uint64_t where[3], uint64_t *dropped);
const char *walk_key_row_name(int row);
/* A test build only: the fixture at PATH (format above), before the first
 * lookup, for tests that load libhao into a native program. */
int walk_facts_load(const char *path);
