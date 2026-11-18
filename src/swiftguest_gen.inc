/* Generated from macOS 27.2.1 (26B131) guest images (keyed by LC_UUID, one row per arm64e slice) and the walk table
 * swiftwalk_gen.inc of the same build.
 * Per guest-defined Swift type: the verdict of the value walk, from a reconstruction measured on both architectures
 * whose arm64 layout equals the guest's own metadata. Include once per section, with exactly one of these defined:
 *   SWIFTGUEST_TYPES   {"image LC_UUID", descriptor offset (from the mach header), "generic argument walk keys"
 *                       ("" when not generic; comma-separated walk or guest keys), "mangled name", verdict,
 *                       size, stride, align, arm64 VW flags, arm64 XI count, x86_64 XI count, XI class,
 *                       XI word offset, payload cases, empty cases, first field, field count, first image,
 *                       image count, "slice cdhash", metadata initialization, static metadata offset}
 *                      The row applies only to the slice whose CodeDirectory hash (as hao verified its pages,
 *                      40 hex digits) is this, and only while the descriptor's metadata initialization is the
 *                      row's (SWIFTGUEST_INIT_NONE: static metadata; SWIFTGUEST_INIT_SINGLETON: the runtime
 *                      completes it); a non-generic type only to its canonical metadata: for INIT_NONE the static
 *                      metadata at this offset from the mach header (0 for the others).
 *                      The runtime uses a row only when the live guest metadata has this size, stride, VW flags
 *                      and XI count; otherwise the type has no row. Verdict, XI class and images: as in
 *                      swiftwalk_gen.inc.
 *   SWIFTGUEST_FIELDS  {type index, field index (struct: stored field; enum: case), "native walk key" or NULL,
 *                       guest type index or -1, verdict, indirect}
 *                      No offsets: read them from the live metadata's field offset vector. An enum's empty case
 *                      has NULL and -1. A field whose walk key is not in swiftwalk_gen.inc makes the row
 *                      incomplete: refuse the type.
 *   SWIFTGUEST_IMAGES  {type index, case, "arm64 image", "arm64 mask", "x86_64 image", "x86_64 mask"}: every case of
 *                      each CONVERT row, as SWIFTWALK_IMAGES.
 * A type that is not listed is refused.
 * The generator is not part of this repository. Stamp: include/hao/gen_stamp.h. Do not edit.
 */
#if HAO_GEN_FORMAT != 1
#error "swiftguest_gen.inc is table format 1: include hao/gen_stamp.h first"
#endif
#if defined(SWIFTGUEST_TYPES)
#elif defined(SWIFTGUEST_FIELDS)
#elif defined(SWIFTGUEST_IMAGES)
#else
#error "define one of SWIFTGUEST_TYPES, SWIFTGUEST_FIELDS, SWIFTGUEST_IMAGES"
#endif
