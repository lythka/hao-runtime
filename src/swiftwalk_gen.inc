/* Generated from macOS 27.2.1 (26B131):
 * arm64 cache 26B131 arm64e images (LC_UUIDs checked), x86_64 cache 954AA5DE-1C5D-3D47-9921-AF7AF681D333.
 * Per Swift type (key: mangled name, sorted), the verdict of the value walk (SAME, CONVERT, DYNAMIC,
 * REFUSE). Include once per
 * section, with exactly one of these defined:
 *   SWIFTWALK_TYPES     {"mangled name", verdict, size, stride, align, x86_64 VW flags, arm64 XI count,
 *                        x86_64 XI count, XI class, XI word offset, payload cases, empty cases,
 *                        first child, child count, first image, image count}
 *                       XI class, for an enum around this type that the table does not list:
 *                       NONE no XIs; SAME identical XIs; POINTER the XIs differ only in the 8-byte word
 *                       at the offset, where x86_64 holds twice the arm64 value (pointer XI k: k-1 on
 *                       arm64, 2(k-1) on x86_64); OTHER anything else
 *                       (spare bits: see mpenum_gen.inc).
 *   SWIFTWALK_CHILDREN  {type index, offset (-1 for enum cases), child type index (-1 none), indirect}
 *   SWIFTWALK_IMAGES    {type index, case, "arm64 image", "arm64 mask", "x86_64 image", "x86_64 mask"}:
 *                        every case of each CONVERT row. The image is what destructiveInjectEnumTag
 *                        writes over zeros; the mask has a 1 for each bit the injection defines (the
 *                        same over a zeros and a ones background). Bits outside the mask are payload.
 *                        A payload case whose mask is all zero (single-payload enums) has no image of
 *                        its own: a value is in it when it matches no other case under that case's mask.
 * A type that is not listed is refused.
 * The generator is not part of this repository. Stamp: include/hao/gen_stamp.h. Do not edit.
 */
#if HAO_GEN_FORMAT != 1
#error "swiftwalk_gen.inc is table format 1: include hao/gen_stamp.h first"
#endif
#if defined(SWIFTWALK_TYPES)
    {"12CoreGraphics7CGFloatV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 0, 1, 0, 0}, /* CoreGraphics.CGFloat */
    {"14AttributeGraph0A0Vy7SwiftUI01_B6InputsV5PhaseVG", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 1, 1, 0, 0}, /* AttributeGraph.Attribute<SwiftUI._GraphInputs.Phase> */
    {"14AttributeGraph0A0Vy7SwiftUI11TransactionVG", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 2, 1, 0, 0}, /* AttributeGraph.Attribute<SwiftUI.Transaction> */
    {"14AttributeGraph0A0Vy7SwiftUI4TimeVG", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 3, 1, 0, 0}, /* AttributeGraph.Attribute<SwiftUI.Time> */
    {"7SwiftUI$yXZ19ViewFactoryProtocol_pXp", SWIFTWALK_SAME, 16, 16, 8, 0x7, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 4, 0, 0, 0}, /* SwiftUI.(unknown context).ViewFactoryProtocol.Type */
    {"7SwiftUI10EdgeInsetsV", SWIFTWALK_SAME, 32, 32, 8, 0x20007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 4, 4, 0, 0}, /* SwiftUI.EdgeInsets */
    {"7SwiftUI10EdgeInsetsVSg", SWIFTWALK_SAME, 33, 40, 8, 0x220007, 0, 0, SWIFTWALK_XI_NONE, -1, 1, 1, 8, 2, 0, 0}, /* Swift.Optional<SwiftUI.EdgeInsets> */
    {"7SwiftUI10MutableBoxCyAA17CachedEnvironmentVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 10, 0, 0, 0}, /* SwiftUI.MutableBox<SwiftUI.CachedEnvironment> */
    {"7SwiftUI10VisibilityO", SWIFTWALK_SAME, 1, 1, 1, 0x200000, 253, 253, SWIFTWALK_XI_SAME, -1, 0, 3, 10, 3, 0, 0}, /* SwiftUI.Visibility */
    {"7SwiftUI10_ViewDebugO10PropertiesV", SWIFTWALK_SAME, 4, 4, 4, 0x2000003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 13, 1, 0, 0}, /* SwiftUI._ViewDebug.Properties */
    {"7SwiftUI12AlignmentKeyV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 14, 1, 0, 0}, /* SwiftUI.AlignmentKey */
    {"7SwiftUI12DropDelegate_p", SWIFTWALK_DYNAMIC, 40, 40, 8, 0x30007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 15, 0, 0, 0}, /* SwiftUI.DropDelegate; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
    {"7SwiftUI12PropertyListV", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 0, 0, 15, 1, 0, 0}, /* SwiftUI.PropertyList */
    {"7SwiftUI12PropertyListV7ElementC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 16, 0, 0, 0}, /* SwiftUI.PropertyList.Element */
    {"7SwiftUI12PropertyListV7ElementCSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 16, 2, 0, 0}, /* Swift.Optional<SwiftUI.PropertyList.Element> */
    {"7SwiftUI12_GraphInputsV", SWIFTWALK_SAME, 48, 48, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 16, 0, 0, 18, 8, 0, 0}, /* SwiftUI._GraphInputs */
    {"7SwiftUI12_GraphInputsV7OptionsV", SWIFTWALK_SAME, 4, 4, 4, 0x2000003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 26, 1, 0, 0}, /* SwiftUI._GraphInputs.Options */
    {"7SwiftUI13_VariadicViewO4TreeVy_AA13_ZStackLayoutVAA4TextVSgG", SWIFTWALK_SAME, 48, 48, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 27, 2, 0, 0}, /* SwiftUI._VariadicView.Tree<SwiftUI._ZStackLayout, Swift.Optional<SwiftUI.Text>> */
    {"7SwiftUI13_ZStackLayoutV", SWIFTWALK_SAME, 16, 16, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 29, 1, 0, 0}, /* SwiftUI._ZStackLayout */
    {"7SwiftUI14AnyTextStorageC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 30, 0, 0, 0}, /* SwiftUI.AnyTextStorage */
    {"7SwiftUI14_PaddingLayoutV", SWIFTWALK_SAME, 41, 48, 8, 0x20007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 30, 2, 0, 0}, /* SwiftUI._PaddingLayout */
    {"7SwiftUI15BoundInputsViewV", SWIFTWALK_SAME, 88, 88, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 32, 5, 0, 0}, /* SwiftUI.BoundInputsView */
    {"7SwiftUI15BoundInputsViewVSg", SWIFTWALK_SAME, 88, 88, 8, 0x2230007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 37, 2, 0, 0}, /* Swift.Optional<SwiftUI.BoundInputsView> */
    {"7SwiftUI15ModifiedContentVyAA6ZStackVyAA4TextVSgGAA14_PaddingLayoutVG", SWIFTWALK_SAME, 89, 96, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 39, 2, 0, 0}, /* SwiftUI.ModifiedContent<SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>>, SwiftUI._PaddingLayout> */
    {"7SwiftUI15ModifiedContentVyACyAA6ZStackVyAA4TextVSgGAA14_PaddingLayoutVGAA25_AllowsHitTestingModifierVG", SWIFTWALK_SAME, 90, 96, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 41, 2, 0, 0}, /* SwiftUI.ModifiedContent<SwiftUI.ModifiedContent<SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>>, SwiftUI._PaddingLayout>, SwiftUI._AllowsHitTestingModifier> */
    {"7SwiftUI15ModifiedContentVyACyAA6ZStackVyAA4TextVSgGAA14_PaddingLayoutVGAA25_AllowsHitTestingModifierVGSg", SWIFTWALK_CONVERT, 90, 96, 8, 0x2230007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 40, 1, 1, 43, 2, 0, 2}, /* Swift.Optional<SwiftUI.ModifiedContent<SwiftUI.ModifiedContent<SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>>, SwiftUI._PaddingLayout>, SwiftUI._AllowsHitTestingModifier>>; case encodings differ: cases [1] */
    {"7SwiftUI16ToolbarItemGroupVyAA4TextVG", SWIFTWALK_DYNAMIC, 81, 88, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 72, 0, 0, 45, 3, 2, 0}, /* SwiftUI.ToolbarItemGroup<SwiftUI.Text>; field placement: DYNAMIC */
    {"7SwiftUI16ToolbarItemGroupVyAA4TextVGSg", SWIFTWALK_DYNAMIC, 81, 88, 8, 0x2230007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 72, 1, 1, 48, 2, 2, 0}, /* Swift.Optional<SwiftUI.ToolbarItemGroup<SwiftUI.Text>>; case some: DYNAMIC */
    {"7SwiftUI16ToolbarItemGroupVyAA9EmptyViewVG", SWIFTWALK_DYNAMIC, 42, 48, 8, 0x30007, 254, 254, SWIFTWALK_XI_SAME, -1, 0, 0, 50, 3, 2, 0}, /* SwiftUI.ToolbarItemGroup<SwiftUI.EmptyView>; field placement: DYNAMIC */
    {"7SwiftUI16ToolbarItemGroupVyAA9EmptyViewVGSg", SWIFTWALK_DYNAMIC, 42, 48, 8, 0x230007, 253, 253, SWIFTWALK_XI_SAME, -1, 1, 1, 53, 2, 2, 0}, /* Swift.Optional<SwiftUI.ToolbarItemGroup<SwiftUI.EmptyView>>; case some: DYNAMIC */
    {"7SwiftUI17UncheckedSendableVyAA12PropertyListVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 0, 0, 55, 1, 2, 0}, /* SwiftUI.UncheckedSendable<SwiftUI.PropertyList> */
    {"7SwiftUI17VerticalAlignmentV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 56, 1, 2, 0}, /* SwiftUI.VerticalAlignment */
    {"7SwiftUI19HorizontalAlignmentV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 57, 1, 2, 0}, /* SwiftUI.HorizontalAlignment */
    {"7SwiftUI20ToolbarItemPlacementV", SWIFTWALK_DYNAMIC, 41, 48, 8, 0x2030007, 253, 253, SWIFTWALK_XI_SAME, -1, 0, 0, 58, 1, 2, 0}, /* SwiftUI.ToolbarItemPlacement; field role: DYNAMIC */
    {"7SwiftUI20ToolbarItemPlacementV4RoleO", SWIFTWALK_DYNAMIC, 41, 48, 8, 0x230007, 253, 253, SWIFTWALK_XI_SAME, -1, 2, 34, 59, 36, 2, 0}, /* SwiftUI.ToolbarItemPlacement.Role; case accessoryBar: DYNAMIC */
    {"7SwiftUI23MaterialProviderBoxBaseC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 95, 0, 2, 0}, /* SwiftUI.MaterialProviderBoxBase */
    {"7SwiftUI24MaterialActiveAppearanceV", SWIFTWALK_SAME, 1, 1, 1, 0x2000000, 252, 252, SWIFTWALK_XI_SAME, -1, 0, 0, 95, 1, 2, 0}, /* SwiftUI.MaterialActiveAppearance */
    {"7SwiftUI24MaterialActiveAppearanceV$yXZ7StorageO", SWIFTWALK_SAME, 1, 1, 1, 0x200000, 252, 252, SWIFTWALK_XI_SAME, -1, 0, 4, 96, 4, 2, 0}, /* SwiftUI.MaterialActiveAppearance.(unknown context).Storage */
    {"7SwiftUI25_AllowsHitTestingModifierV", SWIFTWALK_SAME, 1, 1, 1, 0x0, 254, 254, SWIFTWALK_XI_SAME, -1, 0, 0, 100, 1, 2, 0}, /* SwiftUI._AllowsHitTestingModifier */
    {"7SwiftUI4EdgeO3SetV", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 101, 1, 2, 0}, /* SwiftUI.Edge.Set */
    {"7SwiftUI4TextV", SWIFTWALK_SAME, 32, 32, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 102, 2, 2, 0}, /* SwiftUI.Text */
    {"7SwiftUI4TextV7StorageO", SWIFTWALK_SAME, 17, 24, 8, 0x2210007, 254, 254, SWIFTWALK_XI_SAME, -1, 2, 0, 104, 2, 2, 0}, /* SwiftUI.Text.Storage */
    {"7SwiftUI4TextVSg", SWIFTWALK_SAME, 32, 32, 8, 0x2230007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 24, 1, 1, 106, 2, 2, 0}, /* Swift.Optional<SwiftUI.Text> */
    {"7SwiftUI6ZStackVyAA4TextVSgG", SWIFTWALK_SAME, 48, 48, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 108, 1, 2, 0}, /* SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>> */
    {"7SwiftUI6_GlassV14VariantOptionsV", SWIFTWALK_SAME, 8, 8, 8, 0x2000007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 109, 1, 2, 0}, /* SwiftUI._Glass.VariantOptions */
    {"7SwiftUI6_GlassV7VariantV", SWIFTWALK_SAME, 32, 32, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 0, 0, 110, 4, 2, 0}, /* SwiftUI._Glass.Variant */
    {"7SwiftUI6_GlassV7VariantV3MixV", SWIFTWALK_SAME, 72, 72, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 0, 0, 114, 3, 2, 0}, /* SwiftUI._Glass.Variant.Mix */
    {"7SwiftUI6_GlassV7VariantV4RoleO", SWIFTWALK_SAME, 8, 8, 8, 0x2210007, 125, 2045, SWIFTWALK_XI_OTHER, -1, 2, 24, 117, 26, 2, 0}, /* SwiftUI._Glass.Variant.Role */
    {"7SwiftUI8MaterialV", SWIFTWALK_SAME, 17, 24, 8, 0x2010007, 252, 252, SWIFTWALK_XI_SAME, -1, 0, 0, 143, 3, 2, 0}, /* SwiftUI.Material */
    {"7SwiftUI8MaterialV08ResolvedC0V5FlagsV", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 146, 1, 2, 0}, /* SwiftUI.Material.ResolvedMaterial.Flags */
    {"7SwiftUI8MaterialV2IDO", SWIFTWALK_SAME, 9, 16, 8, 0x2210007, 252, 252, SWIFTWALK_XI_SAME, -1, 3, 12, 147, 15, 2, 0}, /* SwiftUI.Material.ID */
    {"7SwiftUI8MaterialVAAE6LayersV", SWIFTWALK_SAME, 8, 8, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 162, 1, 2, 0}, /* (extension in SwiftUI):SwiftUI.Material.Layers */
    {"7SwiftUI8UniqueIDV", SWIFTWALK_SAME, 8, 8, 8, 0x2000007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 163, 1, 2, 0}, /* SwiftUI.UniqueID */
    {"7SwiftUI9AlignmentV", SWIFTWALK_SAME, 16, 16, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 164, 2, 2, 0}, /* SwiftUI.Alignment */
    {"7SwiftUI9EmptyViewV", SWIFTWALK_SAME, 0, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 166, 0, 2, 0}, /* SwiftUI.EmptyView */
    {"Bb", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 166, 0, 2, 0}, /* Builtin.BridgeObject */
    {"Bf64_", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 166, 0, 2, 0}, /* Builtin.FPIEEE64 */
    {"Bi1_", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 166, 0, 2, 0}, /* Builtin.Int1 */
    {"Bi32_", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 166, 0, 2, 0}, /* Builtin.Int32 */
    {"Bi64_", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 166, 0, 2, 0}, /* Builtin.Int64 */
    {"Bi8_", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 166, 0, 2, 0}, /* Builtin.Int8 */
    {"SS", SWIFTWALK_SAME, 16, 16, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 166, 1, 2, 0}, /* Swift.String */
    {"SS5light_SS4darkSo8NSBundleCSg6bundlet", SWIFTWALK_SAME, 40, 40, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 167, 3, 2, 0}, /* (light: Swift.String, dark: Swift.String, bundle: Swift.Optional<__C.NSBundle>) */
    {"Say7SwiftUI4TextV8ModifierOG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 170, 1, 2, 0}, /* Swift.Array<SwiftUI.Text.Modifier> */
    {"Say7SwiftUI8MaterialVAAE5LayerVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 171, 1, 2, 0}, /* Swift.Array<(extension in SwiftUI):SwiftUI.Material.Layer> */
    {"Sb", SWIFTWALK_SAME, 1, 1, 1, 0x0, 254, 254, SWIFTWALK_XI_SAME, -1, 0, 0, 172, 1, 2, 0}, /* Swift.Bool */
    {"Sd", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 173, 1, 2, 0}, /* Swift.Double */
    {"Sh8_VariantVySo11AGAttributea_G", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 174, 1, 2, 0}, /* Swift.Set<__C.AGAttribute>._Variant */
    {"ShySo11AGAttributeaG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 175, 1, 2, 0}, /* Swift.Set<__C.AGAttribute> */
    {"Si", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 176, 1, 2, 0}, /* Swift.Int */
    {"So11AGAttributea", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 177, 1, 2, 0}, /* __C.AGAttribute */
    {"So13AGSubgraphRefa", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 178, 0, 2, 0}, /* __C.AGSubgraphRef */
    {"So8NSBundleC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 178, 0, 2, 0}, /* NSBundle */
    {"So8NSBundleCSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 178, 2, 2, 0}, /* Swift.Optional<__C.NSBundle> */
    {"Su", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 180, 1, 2, 0}, /* Swift.UInt */
    {"s11AnyHashableV", SWIFTWALK_DYNAMIC, 40, 40, 8, 0x30007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 181, 1, 2, 0}, /* Swift.AnyHashable; field _box: DYNAMIC */
    {"s11_StringGutsV", SWIFTWALK_SAME, 16, 16, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 182, 1, 2, 0}, /* Swift._StringGuts */
    {"s12_ArrayBufferVy7SwiftUI4TextV8ModifierOG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 183, 1, 2, 0}, /* Swift._ArrayBuffer<SwiftUI.Text.Modifier> */
    {"s12_ArrayBufferVy7SwiftUI8MaterialVACE5LayerVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 184, 1, 2, 0}, /* Swift._ArrayBuffer<(extension in SwiftUI):SwiftUI.Material.Layer> */
    {"s13_StringObjectV", SWIFTWALK_SAME, 16, 16, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 185, 2, 2, 0}, /* Swift._StringObject */
    {"s14_BridgeStorageVys017__ContiguousArrayB4BaseCG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 187, 1, 2, 0}, /* Swift._BridgeStorage<Swift.__ContiguousArrayStorageBase> */
    {"s14_BridgeStorageVys08__RawSetB0CG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 188, 1, 2, 0}, /* Swift._BridgeStorage<Swift.__RawSetStorage> */
    {"s15_AnyHashableBox_p", SWIFTWALK_DYNAMIC, 40, 40, 8, 0x30007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 189, 0, 2, 0}, /* Swift._AnyHashableBox; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
    {"s4Int8V", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 189, 1, 2, 0}, /* Swift.Int8 */
    {"s6UInt32V", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 190, 1, 2, 0}, /* Swift.UInt32 */
    {"s6UInt64V", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 191, 1, 2, 0}, /* Swift.UInt64 */
    {"yXl", SWIFTWALK_DYNAMIC, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 192, 0, 2, 0}, /* Swift.AnyObject; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
#endif
#if defined(SWIFTWALK_CHILDREN)
    {0, 0, 66, 0}, /* native */
    {1, 0, 70, 0}, /* identifier */
    {2, 0, 70, 0}, /* identifier */
    {3, 0, 70, 0}, /* identifier */
    {5, 0, 0, 0}, /* top */
    {5, 8, 0, 0}, /* leading */
    {5, 16, 0, 0}, /* bottom */
    {5, 24, 0, 0}, /* trailing */
    {6, -1, 5, 0}, /* some */
    {6, -1, -1, 0}, /* none */
    {8, -1, -1, 0}, /* automatic */
    {8, -1, -1, 0}, /* visible */
    {8, -1, -1, 0}, /* hidden */
    {9, 0, 84, 0}, /* rawValue */
    {10, 0, 74, 0}, /* bits */
    {12, 0, 14, 0}, /* elements */
    {14, -1, 13, 0}, /* some */
    {14, -1, -1, 0}, /* none */
    {15, 0, 12, 0}, /* customInputs */
    {15, 8, 3, 0}, /* time */
    {15, 16, 7, 0}, /* cachedEnvironment */
    {15, 24, 1, 0}, /* phase */
    {15, 28, 2, 0}, /* transaction */
    {15, 32, 9, 0}, /* changedDebugProperties */
    {15, 36, 16, 0}, /* options */
    {15, 40, 68, 0}, /* mergedInputs */
    {16, 0, 84, 0}, /* rawValue */
    {17, 0, 18, 0}, /* root */
    {17, 16, 42, 0}, /* content */
    {18, 0, 53, 0}, /* alignment */
    {20, 0, 39, 0}, /* edges */
    {20, 8, 6, 0}, /* insets */
    {21, 0, 4, 0}, /* factory */
    {21, 16, 70, 0}, /* content */
    {21, 24, 52, 0}, /* childID */
    {21, 32, 71, 0}, /* contentSubgraph */
    {21, 40, 15, 0}, /* inputs */
    {22, -1, 21, 0}, /* some */
    {22, -1, -1, 0}, /* none */
    {23, 0, 43, 0}, /* content */
    {23, 48, 20, 0}, /* modifier */
    {24, 0, 23, 0}, /* content */
    {24, 89, 38, 0}, /* modifier */
    {25, -1, 24, 0}, /* some */
    {25, -1, -1, 0}, /* none */
    {26, 0, 33, 0}, /* placement */
    {26, 48, 40, 0}, /* content */
    {26, 80, 65, 0}, /* isEmpty */
    {27, -1, 26, 0}, /* some */
    {27, -1, -1, 0}, /* none */
    {28, 0, 33, 0}, /* placement */
    {28, 41, 54, 0}, /* content */
    {28, 41, 65, 0}, /* isEmpty */
    {29, -1, 28, 0}, /* some */
    {29, -1, -1, 0}, /* none */
    {30, 0, 12, 0}, /* value */
    {31, 0, 10, 0}, /* key */
    {32, 0, 10, 0}, /* key */
    {33, 0, 34, 0}, /* role */
    {34, -1, 8, 0}, /* primaryAction */
    {34, -1, 75, 0}, /* accessoryBar */
    {34, -1, -1, 0}, /* automatic */
    {34, -1, -1, 0}, /* principal */
    {34, -1, -1, 0}, /* navigation */
    {34, -1, -1, 0}, /* secondaryAction */
    {34, -1, -1, 0}, /* status */
    {34, -1, -1, 0}, /* confirmationAction */
    {34, -1, -1, 0}, /* cancellationAction */
    {34, -1, -1, 0}, /* destructiveAction */
    {34, -1, -1, 0}, /* keyboard */
    {34, -1, -1, 0}, /* navigationBarLeading */
    {34, -1, -1, 0}, /* navigationBarTrailing */
    {34, -1, -1, 0}, /* title */
    {34, -1, -1, 0}, /* largeTitle */
    {34, -1, -1, 0}, /* bottomBar */
    {34, -1, -1, 0}, /* largeNavigationTitleAccessory */
    {34, -1, -1, 0}, /* subtitle */
    {34, -1, -1, 0}, /* largeSubtitle */
    {34, -1, -1, 0}, /* tabBarLeading */
    {34, -1, -1, 0}, /* tabBarTrailing */
    {34, -1, -1, 0}, /* bottomOrnament */
    {34, -1, -1, 0}, /* topBarPinnedTrailing */
    {34, -1, -1, 0}, /* navigationBarTitle */
    {34, -1, -1, 0}, /* defaultNavigationBarLeading */
    {34, -1, -1, 0}, /* _implicitNavigation */
    {34, -1, -1, 0}, /* _implicitSidebarLeading */
    {34, -1, -1, 0}, /* _implicitSidebarTrailing */
    {34, -1, -1, 0}, /* _navigationBarTrailing */
    {34, -1, -1, 0}, /* _navigationBarLeading */
    {34, -1, -1, 0}, /* _navigationSplitViewLeading */
    {34, -1, -1, 0}, /* _navigationSplitViewTrailing */
    {34, -1, -1, 0}, /* search */
    {34, -1, -1, 0}, /* _tabViewSidebarOverflowMenu */
    {34, -1, -1, 0}, /* _tabViewSidebarBottomBar */
    {34, -1, -1, 0}, /* marker */
    {36, 0, 37, 0}, /* storage */
    {37, -1, -1, 0}, /* automatic */
    {37, -1, -1, 0}, /* active */
    {37, -1, -1, 0}, /* inactive */
    {37, -1, -1, 0}, /* matchWindow */
    {38, 0, 65, 0}, /* allowsHitTesting */
    {39, 0, 83, 0}, /* rawValue */
    {40, 0, 41, 0}, /* storage */
    {40, 24, 63, 0}, /* modifiers */
    {41, -1, 61, 0}, /* verbatim */
    {41, -1, 19, 0}, /* anyTextStorage */
    {42, -1, 40, 0}, /* some */
    {42, -1, -1, 0}, /* none */
    {43, 0, 17, 0}, /* _tree */
    {44, 0, 69, 0}, /* rawValue */
    {45, 0, 47, 0}, /* role */
    {45, 8, 30, 0}, /* plist */
    {45, 16, 44, 0}, /* addedOptions */
    {45, 24, 44, 0}, /* removedOptions */
    {46, 0, 45, 0}, /* from */
    {46, 32, 45, 0}, /* to */
    {46, 64, 66, 0}, /* fraction */
    {47, -1, 46, 1}, /* mix */
    {47, -1, 48, 1}, /* explicit */
    {47, -1, -1, 0}, /* regular */
    {47, -1, -1, 0}, /* identity */
    {47, -1, -1, 0}, /* clear */
    {47, -1, -1, 0}, /* dock */
    {47, -1, -1, 0}, /* appIcons */
    {47, -1, -1, 0}, /* widgets */
    {47, -1, -1, 0}, /* text */
    {47, -1, -1, 0}, /* avplayer */
    {47, -1, -1, 0}, /* facetime */
    {47, -1, -1, 0}, /* controlCenter */
    {47, -1, -1, 0}, /* notificationCenter */
    {47, -1, -1, 0}, /* monogram */
    {47, -1, -1, 0}, /* focusBorder */
    {47, -1, -1, 0}, /* keyboard */
    {47, -1, -1, 0}, /* sidebar */
    {47, -1, -1, 0}, /* control */
    {47, -1, -1, 0}, /* loupe */
    {47, -1, -1, 0}, /* slider */
    {47, -1, -1, 0}, /* camera */
    {47, -1, -1, 0}, /* cartouchePopover */
    {47, -1, -1, 0}, /* menu */
    {47, -1, -1, 0}, /* siri */
    {47, -1, -1, 0}, /* siriSnippet */
    {47, -1, -1, 0}, /* vibrantFill */
    {48, 0, 50, 0}, /* id */
    {48, 12, 49, 0}, /* flags */
    {48, 16, 36, 0}, /* activeAppearance */
    {49, 0, 84, 0}, /* rawValue */
    {50, -1, 62, 1}, /* coreMaterial */
    {50, -1, 35, 0}, /* provider */
    {50, -1, 51, 0}, /* layers */
    {50, -1, -1, 0}, /* ultraThin */
    {50, -1, -1, 0}, /* thin */
    {50, -1, -1, 0}, /* regular */
    {50, -1, -1, 0}, /* thick */
    {50, -1, -1, 0}, /* ultraThick */
    {50, -1, -1, 0}, /* systemBars */
    {50, -1, -1, 0}, /* contentFill */
    {50, -1, -1, 0}, /* windowFill */
    {50, -1, -1, 0}, /* menu */
    {50, -1, -1, 0}, /* secondaryBackground */
    {50, -1, -1, 0}, /* tertiaryBackground */
    {50, -1, -1, 0}, /* quaternaryBackground */
    {51, 0, 64, 0}, /* layers */
    {52, 0, 74, 0}, /* value */
    {53, 0, 32, 0}, /* horizontal */
    {53, 8, 31, 0}, /* vertical */
    {61, 0, 76, 0}, /* _guts */
    {62, 0, 61, 0}, /* # */
    {62, 16, 61, 0}, /* # */
    {62, 32, 73, 0}, /* # */
    {63, 0, 77, 0}, /* _buffer */
    {64, 0, 78, 0}, /* _buffer */
    {65, 0, 57, 0}, /* _value */
    {66, 0, 56, 0}, /* _value */
    {67, 0, 81, 0}, /* object */
    {68, 0, 67, 0}, /* _variant */
    {69, 0, 59, 0}, /* _value */
    {70, 0, 84, 0}, /* rawValue */
    {73, -1, 72, 0}, /* some */
    {73, -1, -1, 0}, /* none */
    {74, 0, 59, 0}, /* _value */
    {75, 0, 82, 0}, /* _box */
    {76, 0, 79, 0}, /* _object */
    {77, 0, 80, 0}, /* _storage */
    {78, 0, 80, 0}, /* _storage */
    {79, 0, 85, 0}, /* _countAndFlagsBits */
    {79, 8, 55, 0}, /* _object */
    {80, 0, 55, 0}, /* rawValue */
    {81, 0, 55, 0}, /* rawValue */
    {83, 0, 60, 0}, /* _value */
    {84, 0, 58, 0}, /* _value */
    {85, 0, 59, 0}, /* _value */
#endif
#if defined(SWIFTWALK_IMAGES)
    {25, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {25, 1, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
#endif
