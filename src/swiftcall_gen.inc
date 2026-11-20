/* Generated from macOS 27.2.1 (26B131):
 * x86_64 cache 954AA5DE-1C5D-3D47-9921-AF7AF681D333. Swift calls: where each value sits on arm64 and on x86_64, and its type in
 * swiftwalk_gen.inc. A call that is not listed keeps the current behaviour: uncertified Swift values are
 * refused. Include once per section, with exactly one of these defined:
 *   SWIFTCALL_FUNCS   {"mangled symbol", "x86_64 library LC_UUID", kind, verdict, first value, value count}
 *   SWIFTCALL_VALUES  {call index, role, "swiftwalk key" or 0 (a class reference), ownership (o owned,
 *                      g guaranteed), verdict, "words"}
 * role: r result, s self, e error, a async context, i indirect result, p0.. parameters. words: one
 * "arm64:x86_64" location per 8-byte word of the value, in value order (x0..x7, x20 self, x8 indirect,
 * v0..; rdi.., r13 self, rax indirect, xmm0..). Stack arguments stay in the interface units (kind K).
 * Kinds: SWIFTCALL_GETTER, _SETTER, _MODIFY, _FUNCTION, _DISPATCH_THUNK, _METHOD_DESCRIPTOR,
 * _ASYNC_FUNCTION_POINTER. Verdicts are swiftwalk_gen.inc's.
 * The generator is not part of this repository. Stamp: include/hao/gen_stamp.h. Do not edit.
 */
#if HAO_GEN_FORMAT != 1
#error "swiftcall_gen.inc is table format 1: include hao/gen_stamp.h first"
#endif
#if defined(SWIFTCALL_FUNCS)
    {"$s11AppStoreKit16ProductTopLockupC19expandedOfferTitlesSSSg5title_AE8subtitletSgvg", "5ebe5feb1cd43369a7eafb5de52e4801", SWIFTCALL_GETTER, SWIFTWALK_CONVERT, 0, 2},
    {"$s11AppStoreKit16ProductTopLockupC19expandedOfferTitlesSSSg5title_AE8subtitletSgvgTj", "5ebe5feb1cd43369a7eafb5de52e4801", SWIFTCALL_DISPATCH_THUNK, SWIFTWALK_CONVERT, 2, 2},
#endif
#if defined(SWIFTCALL_VALUES)
    {0, "r", "SSSg5title_AA8subtitletSg", 'o', SWIFTWALK_CONVERT, "x0:rax x1:rdx x2:rcx x3:r8"},
    {0, "s", 0, 'g', SWIFTWALK_SAME, "x20:r13"},
    {1, "r", "SSSg5title_AA8subtitletSg", 'o', SWIFTWALK_CONVERT, "x0:rax x1:rdx x2:rcx x3:r8"},
    {1, "s", 0, 'g', SWIFTWALK_SAME, "x20:r13"},
#endif
