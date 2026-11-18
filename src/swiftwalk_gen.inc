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
    {"10Foundation12_URLProtocol_p", SWIFTWALK_DYNAMIC, 16, 16, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 0, 0, 0, 0}, /* Foundation._URLProtocol; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
    {"10Foundation13__DataStorageC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 0, 0, 0, 0}, /* Foundation.__DataStorage */
    {"10Foundation3URLV", SWIFTWALK_DYNAMIC, 16, 16, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 0, 1, 0, 0}, /* Foundation.URL; field _url: DYNAMIC */
    {"10Foundation4DataV", SWIFTWALK_SAME, 16, 16, 8, 0x2010007, 12, 252, SWIFTWALK_XI_OTHER, -1, 0, 0, 1, 1, 0, 0}, /* Foundation.Data */
    {"10Foundation4DataV06InlineB0V", SWIFTWALK_SAME, 15, 15, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 2, 2, 0, 0}, /* Foundation.Data.InlineData */
    {"10Foundation4DataV10LargeSliceV", SWIFTWALK_SAME, 16, 16, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 4, 2, 0, 0}, /* Foundation.Data.LargeSlice */
    {"10Foundation4DataV11InlineSliceV", SWIFTWALK_SAME, 16, 16, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 6, 2, 0, 0}, /* Foundation.Data.InlineSlice */
    {"10Foundation4DataV14RangeReferenceC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 8, 0, 0, 0}, /* Foundation.Data.RangeReference */
    {"10Foundation4DataV15_RepresentationO", SWIFTWALK_SAME, 16, 16, 8, 0x2210007, 12, 252, SWIFTWALK_XI_OTHER, -1, 3, 1, 8, 4, 0, 0}, /* Foundation.Data._Representation */
    {"10Foundation4DataVSg", SWIFTWALK_CONVERT, 16, 16, 8, 0x2210007, 11, 251, SWIFTWALK_XI_OTHER, -1, 1, 1, 12, 2, 0, 2}, /* Swift.Optional<Foundation.Data>; case encodings differ: cases [1] */
    {"12CoreGraphics7CGFloatV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 14, 1, 2, 0}, /* CoreGraphics.CGFloat */
    {"14AttributeGraph0A0Vy7SwiftUI01_B6InputsV5PhaseVG", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 15, 1, 2, 0}, /* AttributeGraph.Attribute<SwiftUI._GraphInputs.Phase> */
    {"14AttributeGraph0A0Vy7SwiftUI11TransactionVG", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 16, 1, 2, 0}, /* AttributeGraph.Attribute<SwiftUI.Transaction> */
    {"14AttributeGraph0A0Vy7SwiftUI4TimeVG", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 17, 1, 2, 0}, /* AttributeGraph.Attribute<SwiftUI.Time> */
    {"15Synchronization12_MutexHandleV", SWIFTWALK_SAME, 4, 4, 4, 0x3800003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 18, 1, 2, 0}, /* Synchronization._MutexHandle */
    {"15Synchronization5MutexVyScS12ContinuationVyyyYbc_GG", SWIFTWALK_SAME, 16, 16, 8, 0x3810007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 19, 2, 2, 0}, /* Synchronization.Mutex<Swift.AsyncStream<@Sendable () -> ()>.Continuation> */
    {"15Synchronization5_CellVyScS12ContinuationVyyyYbc_GG", SWIFTWALK_SAME, 8, 8, 8, 0x1830007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 21, 1, 2, 0}, /* Synchronization._Cell<Swift.AsyncStream<@Sendable () -> ()>.Continuation> */
    {"15Synchronization5_CellVySo16os_unfair_lock_sVG", SWIFTWALK_SAME, 4, 4, 4, 0x1830003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 22, 1, 2, 0}, /* Synchronization._Cell<__C.os_unfair_lock_s> */
    {"7SwiftUI$yXZ19ViewFactoryProtocol_pXp", SWIFTWALK_SAME, 16, 16, 8, 0x7, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 23, 0, 2, 0}, /* SwiftUI.(unknown context).ViewFactoryProtocol.Type */
    {"7SwiftUI10EdgeInsetsV", SWIFTWALK_SAME, 32, 32, 8, 0x20007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 23, 4, 2, 0}, /* SwiftUI.EdgeInsets */
    {"7SwiftUI10EdgeInsetsVSg", SWIFTWALK_SAME, 33, 40, 8, 0x220007, 0, 0, SWIFTWALK_XI_NONE, -1, 1, 1, 27, 2, 2, 0}, /* Swift.Optional<SwiftUI.EdgeInsets> */
    {"7SwiftUI10MutableBoxCyAA17CachedEnvironmentVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 29, 0, 2, 0}, /* SwiftUI.MutableBox<SwiftUI.CachedEnvironment> */
    {"7SwiftUI10VisibilityO", SWIFTWALK_SAME, 1, 1, 1, 0x200000, 253, 253, SWIFTWALK_XI_SAME, -1, 0, 3, 29, 3, 2, 0}, /* SwiftUI.Visibility */
    {"7SwiftUI10_ViewDebugO10PropertiesV", SWIFTWALK_SAME, 4, 4, 4, 0x2000003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 32, 1, 2, 0}, /* SwiftUI._ViewDebug.Properties */
    {"7SwiftUI12AlignmentKeyV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 33, 1, 2, 0}, /* SwiftUI.AlignmentKey */
    {"7SwiftUI12DropDelegate_p", SWIFTWALK_DYNAMIC, 40, 40, 8, 0x30007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 34, 0, 2, 0}, /* SwiftUI.DropDelegate; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
    {"7SwiftUI12PropertyListV", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 0, 0, 34, 1, 2, 0}, /* SwiftUI.PropertyList */
    {"7SwiftUI12PropertyListV7ElementC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 35, 0, 2, 0}, /* SwiftUI.PropertyList.Element */
    {"7SwiftUI12PropertyListV7ElementCSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 35, 2, 2, 0}, /* Swift.Optional<SwiftUI.PropertyList.Element> */
    {"7SwiftUI12_GraphInputsV", SWIFTWALK_SAME, 48, 48, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 16, 0, 0, 37, 8, 2, 0}, /* SwiftUI._GraphInputs */
    {"7SwiftUI12_GraphInputsV7OptionsV", SWIFTWALK_SAME, 4, 4, 4, 0x2000003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 45, 1, 2, 0}, /* SwiftUI._GraphInputs.Options */
    {"7SwiftUI13_VariadicViewO4TreeVy_AA13_ZStackLayoutVAA4TextVSgG", SWIFTWALK_SAME, 48, 48, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 46, 2, 2, 0}, /* SwiftUI._VariadicView.Tree<SwiftUI._ZStackLayout, Swift.Optional<SwiftUI.Text>> */
    {"7SwiftUI13_ZStackLayoutV", SWIFTWALK_SAME, 16, 16, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 48, 1, 2, 0}, /* SwiftUI._ZStackLayout */
    {"7SwiftUI14AnyTextStorageC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 49, 0, 2, 0}, /* SwiftUI.AnyTextStorage */
    {"7SwiftUI14_PaddingLayoutV", SWIFTWALK_SAME, 41, 48, 8, 0x20007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 49, 2, 2, 0}, /* SwiftUI._PaddingLayout */
    {"7SwiftUI15BoundInputsViewV", SWIFTWALK_SAME, 88, 88, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 51, 5, 2, 0}, /* SwiftUI.BoundInputsView */
    {"7SwiftUI15BoundInputsViewVSg", SWIFTWALK_SAME, 88, 88, 8, 0x2230007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 56, 2, 2, 0}, /* Swift.Optional<SwiftUI.BoundInputsView> */
    {"7SwiftUI15ModifiedContentVyAA6ZStackVyAA4TextVSgGAA14_PaddingLayoutVG", SWIFTWALK_SAME, 89, 96, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 58, 2, 2, 0}, /* SwiftUI.ModifiedContent<SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>>, SwiftUI._PaddingLayout> */
    {"7SwiftUI15ModifiedContentVyACyAA6ZStackVyAA4TextVSgGAA14_PaddingLayoutVGAA25_AllowsHitTestingModifierVG", SWIFTWALK_SAME, 90, 96, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 60, 2, 2, 0}, /* SwiftUI.ModifiedContent<SwiftUI.ModifiedContent<SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>>, SwiftUI._PaddingLayout>, SwiftUI._AllowsHitTestingModifier> */
    {"7SwiftUI15ModifiedContentVyACyAA6ZStackVyAA4TextVSgGAA14_PaddingLayoutVGAA25_AllowsHitTestingModifierVGSg", SWIFTWALK_CONVERT, 90, 96, 8, 0x2230007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 40, 1, 1, 62, 2, 2, 2}, /* Swift.Optional<SwiftUI.ModifiedContent<SwiftUI.ModifiedContent<SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>>, SwiftUI._PaddingLayout>, SwiftUI._AllowsHitTestingModifier>>; case encodings differ: cases [1] */
    {"7SwiftUI16ToolbarItemGroupVyAA4TextVG", SWIFTWALK_DYNAMIC, 81, 88, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 72, 0, 0, 64, 3, 4, 0}, /* SwiftUI.ToolbarItemGroup<SwiftUI.Text>; field placement: DYNAMIC */
    {"7SwiftUI16ToolbarItemGroupVyAA4TextVGSg", SWIFTWALK_DYNAMIC, 81, 88, 8, 0x2230007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 72, 1, 1, 67, 2, 4, 0}, /* Swift.Optional<SwiftUI.ToolbarItemGroup<SwiftUI.Text>>; case some: DYNAMIC */
    {"7SwiftUI16ToolbarItemGroupVyAA9EmptyViewVG", SWIFTWALK_DYNAMIC, 42, 48, 8, 0x30007, 254, 254, SWIFTWALK_XI_SAME, -1, 0, 0, 69, 3, 4, 0}, /* SwiftUI.ToolbarItemGroup<SwiftUI.EmptyView>; field placement: DYNAMIC */
    {"7SwiftUI16ToolbarItemGroupVyAA9EmptyViewVGSg", SWIFTWALK_DYNAMIC, 42, 48, 8, 0x230007, 253, 253, SWIFTWALK_XI_SAME, -1, 1, 1, 72, 2, 4, 0}, /* Swift.Optional<SwiftUI.ToolbarItemGroup<SwiftUI.EmptyView>>; case some: DYNAMIC */
    {"7SwiftUI17UncheckedSendableVyAA12PropertyListVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 0, 0, 74, 1, 4, 0}, /* SwiftUI.UncheckedSendable<SwiftUI.PropertyList> */
    {"7SwiftUI17VerticalAlignmentV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 75, 1, 4, 0}, /* SwiftUI.VerticalAlignment */
    {"7SwiftUI19HorizontalAlignmentV", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 76, 1, 4, 0}, /* SwiftUI.HorizontalAlignment */
    {"7SwiftUI20ToolbarItemPlacementV", SWIFTWALK_DYNAMIC, 41, 48, 8, 0x2030007, 253, 253, SWIFTWALK_XI_SAME, -1, 0, 0, 77, 1, 4, 0}, /* SwiftUI.ToolbarItemPlacement; field role: DYNAMIC */
    {"7SwiftUI20ToolbarItemPlacementV4RoleO", SWIFTWALK_DYNAMIC, 41, 48, 8, 0x230007, 253, 253, SWIFTWALK_XI_SAME, -1, 2, 34, 78, 36, 4, 0}, /* SwiftUI.ToolbarItemPlacement.Role; case accessoryBar: DYNAMIC */
    {"7SwiftUI23MaterialProviderBoxBaseC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 114, 0, 4, 0}, /* SwiftUI.MaterialProviderBoxBase */
    {"7SwiftUI24MaterialActiveAppearanceV", SWIFTWALK_SAME, 1, 1, 1, 0x2000000, 252, 252, SWIFTWALK_XI_SAME, -1, 0, 0, 114, 1, 4, 0}, /* SwiftUI.MaterialActiveAppearance */
    {"7SwiftUI24MaterialActiveAppearanceV$yXZ7StorageO", SWIFTWALK_SAME, 1, 1, 1, 0x200000, 252, 252, SWIFTWALK_XI_SAME, -1, 0, 4, 115, 4, 4, 0}, /* SwiftUI.MaterialActiveAppearance.(unknown context).Storage */
    {"7SwiftUI25_AllowsHitTestingModifierV", SWIFTWALK_SAME, 1, 1, 1, 0x0, 254, 254, SWIFTWALK_XI_SAME, -1, 0, 0, 119, 1, 4, 0}, /* SwiftUI._AllowsHitTestingModifier */
    {"7SwiftUI4EdgeO3SetV", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 120, 1, 4, 0}, /* SwiftUI.Edge.Set */
    {"7SwiftUI4TextV", SWIFTWALK_SAME, 32, 32, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 121, 2, 4, 0}, /* SwiftUI.Text */
    {"7SwiftUI4TextV7StorageO", SWIFTWALK_SAME, 17, 24, 8, 0x2210007, 254, 254, SWIFTWALK_XI_SAME, -1, 2, 0, 123, 2, 4, 0}, /* SwiftUI.Text.Storage */
    {"7SwiftUI4TextVSg", SWIFTWALK_SAME, 32, 32, 8, 0x2230007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 24, 1, 1, 125, 2, 4, 0}, /* Swift.Optional<SwiftUI.Text> */
    {"7SwiftUI6ZStackVyAA4TextVSgG", SWIFTWALK_SAME, 48, 48, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 40, 0, 0, 127, 1, 4, 0}, /* SwiftUI.ZStack<Swift.Optional<SwiftUI.Text>> */
    {"7SwiftUI6_GlassV14VariantOptionsV", SWIFTWALK_SAME, 8, 8, 8, 0x2000007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 128, 1, 4, 0}, /* SwiftUI._Glass.VariantOptions */
    {"7SwiftUI6_GlassV7VariantV", SWIFTWALK_SAME, 32, 32, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 0, 0, 129, 4, 4, 0}, /* SwiftUI._Glass.Variant */
    {"7SwiftUI6_GlassV7VariantV3MixV", SWIFTWALK_SAME, 72, 72, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 0, 0, 133, 3, 4, 0}, /* SwiftUI._Glass.Variant.Mix */
    {"7SwiftUI6_GlassV7VariantV4RoleO", SWIFTWALK_SAME, 8, 8, 8, 0x2210007, 125, 2045, SWIFTWALK_XI_OTHER, -1, 2, 24, 136, 26, 4, 0}, /* SwiftUI._Glass.Variant.Role */
    {"7SwiftUI8MaterialV", SWIFTWALK_SAME, 17, 24, 8, 0x2010007, 252, 252, SWIFTWALK_XI_SAME, -1, 0, 0, 162, 3, 4, 0}, /* SwiftUI.Material */
    {"7SwiftUI8MaterialV08ResolvedC0V5FlagsV", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 165, 1, 4, 0}, /* SwiftUI.Material.ResolvedMaterial.Flags */
    {"7SwiftUI8MaterialV2IDO", SWIFTWALK_SAME, 9, 16, 8, 0x2210007, 252, 252, SWIFTWALK_XI_SAME, -1, 3, 12, 166, 15, 4, 0}, /* SwiftUI.Material.ID */
    {"7SwiftUI8MaterialVAAE6LayersV", SWIFTWALK_SAME, 8, 8, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 181, 1, 4, 0}, /* (extension in SwiftUI):SwiftUI.Material.Layers */
    {"7SwiftUI8UniqueIDV", SWIFTWALK_SAME, 8, 8, 8, 0x2000007, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 182, 1, 4, 0}, /* SwiftUI.UniqueID */
    {"7SwiftUI9AlignmentV", SWIFTWALK_SAME, 16, 16, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 183, 2, 4, 0}, /* SwiftUI.Alignment */
    {"7SwiftUI9EmptyViewV", SWIFTWALK_SAME, 0, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 185, 0, 4, 0}, /* SwiftUI.EmptyView */
    {"9SwiftData18ModelConfigurationV", SWIFTWALK_DYNAMIC, 240, 240, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 185, 13, 4, 0}, /* SwiftData.ModelConfiguration; field url: DYNAMIC */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V", SWIFTWALK_CONVERT, 98, 104, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 0, 0, 198, 9, 4, 0}, /* SwiftData.ModelConfiguration.DefaultStoreConfiguration; field historyTracking: CONVERT */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V015HistoryTrackingD0V", SWIFTWALK_SAME, 18, 24, 8, 0x2010007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 0, 0, 207, 4, 4, 0}, /* SwiftData.ModelConfiguration.DefaultStoreConfiguration.HistoryTrackingConfiguration */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V015HistoryTrackingD0VSg", SWIFTWALK_CONVERT, 18, 24, 8, 0x2210007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 0, 1, 1, 211, 2, 4, 2}, /* Swift.Optional<SwiftData.ModelConfiguration.DefaultStoreConfiguration.HistoryTrackingConfiguration>; case encodings differ: cases [1] */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V08XPCStoreD0V", SWIFTWALK_SAME, 34, 40, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 0, 0, 213, 4, 6, 0}, /* SwiftData.ModelConfiguration.DefaultStoreConfiguration.XPCStoreConfiguration */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V08XPCStoreD0VSg", SWIFTWALK_CONVERT, 34, 40, 8, 0x2230007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 8, 1, 1, 217, 2, 6, 2}, /* Swift.Optional<SwiftData.ModelConfiguration.DefaultStoreConfiguration.XPCStoreConfiguration>; case encodings differ: cases [1] */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V16EncryptionKeyBoxV", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 219, 1, 8, 0}, /* SwiftData.ModelConfiguration.DefaultStoreConfiguration.EncryptionKeyBox */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0V16EncryptionKeyBoxVSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 220, 2, 8, 0}, /* Swift.Optional<SwiftData.ModelConfiguration.DefaultStoreConfiguration.EncryptionKeyBox> */
    {"9SwiftData18ModelConfigurationV012DefaultStoreD0VSg", SWIFTWALK_CONVERT, 98, 104, 8, 0x2230007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 0, 1, 1, 222, 2, 8, 2}, /* Swift.Optional<SwiftData.ModelConfiguration.DefaultStoreConfiguration>; case encodings differ: cases [1] */
    {"9SwiftData18ModelConfigurationV14GroupContainerV", SWIFTWALK_SAME, 24, 24, 8, 0x2010007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 16, 0, 0, 224, 3, 10, 0}, /* SwiftData.ModelConfiguration.GroupContainer */
    {"9SwiftData18ModelConfigurationV16CloudKitDatabaseV", SWIFTWALK_SAME, 24, 24, 8, 0x2010007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 16, 0, 0, 227, 3, 10, 0}, /* SwiftData.ModelConfiguration.CloudKitDatabase */
    {"9SwiftData6SchemaC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 230, 0, 10, 0}, /* SwiftData.Schema */
    {"9SwiftData6SchemaC9AttributeC6OptionV6CodersO", SWIFTWALK_CONVERT, 16, 16, 8, 0x200007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 0, 1, 2, 230, 3, 10, 3}, /* SwiftData.Schema.Attribute.Option.Coders; case encodings differ: cases [2] */
    {"9SwiftData6SchemaC9AttributeC6OptionV6CodersO11CustomCoder_pXp", SWIFTWALK_SAME, 16, 16, 8, 0x7, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 233, 0, 13, 0}, /* SwiftData.Schema.Attribute.Option.Coders.CustomCoder.Type */
    {"9SwiftData6SchemaC9AttributeC6OptionV6CodersOSg", SWIFTWALK_CONVERT, 16, 16, 8, 0x200007, 2147483644, 2147483644, SWIFTWALK_XI_POINTER, 0, 1, 1, 233, 2, 13, 2}, /* Swift.Optional<SwiftData.Schema.Attribute.Option.Coders>; case encodings differ: cases [1] */
    {"9SwiftData6SchemaCSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 235, 2, 15, 0}, /* Swift.Optional<SwiftData.Schema> */
    {"Bb", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 237, 0, 15, 0}, /* Builtin.BridgeObject */
    {"Bf64_", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 237, 0, 15, 0}, /* Builtin.FPIEEE64 */
    {"Bi1_", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 237, 0, 15, 0}, /* Builtin.Int1 */
    {"Bi32_", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 237, 0, 15, 0}, /* Builtin.Int32 */
    {"Bi64_", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 237, 0, 15, 0}, /* Builtin.Int64 */
    {"Bi8_", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 237, 0, 15, 0}, /* Builtin.Int8 */
    {"SD8_VariantVyS2S_G", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 237, 1, 15, 0}, /* Swift.Dictionary<Swift.String, Swift.String>._Variant */
    {"SDyS2SG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 238, 1, 15, 0}, /* Swift.Dictionary<Swift.String, Swift.String> */
    {"SDyS2SGSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 239, 2, 15, 0}, /* Swift.Optional<Swift.Dictionary<Swift.String, Swift.String>> */
    {"SS", SWIFTWALK_SAME, 16, 16, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 241, 1, 15, 0}, /* Swift.String */
    {"SS5light_SS4darkSo8NSBundleCSg6bundlet", SWIFTWALK_SAME, 40, 40, 8, 0x2030007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 242, 3, 15, 0}, /* (light: Swift.String, dark: Swift.String, bundle: Swift.Optional<__C.NSBundle>) */
    {"SSSg", SWIFTWALK_SAME, 16, 16, 8, 0x2210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 1, 1, 245, 2, 15, 0}, /* Swift.Optional<Swift.String> */
    {"SSSg5title_AA8subtitlet", SWIFTWALK_SAME, 32, 32, 8, 0x2030007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 8, 0, 0, 247, 2, 15, 0}, /* (title: Swift.Optional<Swift.String>, subtitle: Swift.Optional<Swift.String>) */
    {"SSSg5title_AA8subtitletSg", SWIFTWALK_CONVERT, 32, 32, 8, 0x2230007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 8, 1, 1, 249, 2, 15, 2}, /* Swift.Optional<(title: Swift.Optional<Swift.String>, subtitle: Swift.Optional<Swift.String>)>; case encodings differ: cases [1] */
    {"Say7SwiftUI4TextV8ModifierOG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 251, 1, 17, 0}, /* Swift.Array<SwiftUI.Text.Modifier> */
    {"Say7SwiftUI8MaterialVAAE5LayerVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 252, 1, 17, 0}, /* Swift.Array<(extension in SwiftUI):SwiftUI.Material.Layer> */
    {"Sb", SWIFTWALK_SAME, 1, 1, 1, 0x0, 254, 254, SWIFTWALK_XI_SAME, -1, 0, 0, 253, 1, 17, 0}, /* Swift.Bool */
    {"ScS12ContinuationVyyyYbc_G", SWIFTWALK_SAME, 8, 8, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 254, 1, 17, 0}, /* Swift.AsyncStream<@Sendable () -> ()>.Continuation */
    {"ScS8_StorageCyyyYbc_G", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 255, 0, 17, 0}, /* Swift.AsyncStream<@Sendable () -> ()>._Storage */
    {"Sd", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 255, 1, 17, 0}, /* Swift.Double */
    {"Sh8_VariantVy9SwiftData6SchemaC6EntityC_G", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 256, 1, 17, 0}, /* Swift.Set<SwiftData.Schema.Entity>._Variant */
    {"Sh8_VariantVySo11AGAttributea_G", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 257, 1, 17, 0}, /* Swift.Set<__C.AGAttribute>._Variant */
    {"Shy9SwiftData6SchemaC6EntityCG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 258, 1, 17, 0}, /* Swift.Set<SwiftData.Schema.Entity> */
    {"Shy9SwiftData6SchemaC6EntityCGSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 259, 2, 17, 0}, /* Swift.Optional<Swift.Set<SwiftData.Schema.Entity>> */
    {"ShySo11AGAttributeaG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 261, 1, 17, 0}, /* Swift.Set<__C.AGAttribute> */
    {"Si", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 262, 1, 17, 0}, /* Swift.Int */
    {"Snys5Int32VG", SWIFTWALK_SAME, 8, 8, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 263, 2, 17, 0}, /* Swift.Range<Swift.Int32> */
    {"So10CGImageRefa", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 265, 0, 17, 0}, /* __C.CGImageRef */
    {"So10CGImageRefaSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 265, 2, 17, 0}, /* Swift.Optional<__C.CGImageRef> */
    {"So10CGImageRefaSgSg", SWIFTWALK_CONVERT, 8, 8, 8, 0x210007, 2147483645, 2147483645, SWIFTWALK_XI_POINTER, 0, 1, 1, 267, 2, 17, 2}, /* Swift.Optional<Swift.Optional<__C.CGImageRef>>; case encodings differ: cases [1] */
    {"So11AGAttributea", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 269, 1, 19, 0}, /* __C.AGAttribute */
    {"So13AGSubgraphRefa", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 270, 0, 19, 0}, /* __C.AGSubgraphRef */
    {"So16os_unfair_lock_sV", SWIFTWALK_SAME, 4, 4, 4, 0x2000003, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 270, 1, 19, 0}, /* __C.os_unfair_lock_s */
    {"So20NSFileProtectionTypea", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 271, 1, 19, 0}, /* __C.NSFileProtectionType */
    {"So20NSFileProtectionTypeaSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 272, 2, 19, 0}, /* Swift.Optional<__C.NSFileProtectionType> */
    {"So8NSBundleC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 274, 0, 19, 0}, /* NSBundle */
    {"So8NSBundleCSg", SWIFTWALK_SAME, 8, 8, 8, 0x210007, 2147483646, 2147483646, SWIFTWALK_XI_POINTER, 0, 1, 1, 274, 2, 19, 0}, /* Swift.Optional<__C.NSBundle> */
    {"So8NSObjectC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 276, 0, 19, 0}, /* NSObject */
    {"So8NSStringC", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 276, 0, 19, 0}, /* NSString */
    {"Su", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 276, 1, 19, 0}, /* Swift.UInt */
    {"s11AnyHashableV", SWIFTWALK_DYNAMIC, 40, 40, 8, 0x30007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 277, 1, 19, 0}, /* Swift.AnyHashable; field _box: DYNAMIC */
    {"s11_StringGutsV", SWIFTWALK_SAME, 16, 16, 8, 0x2010007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 278, 1, 19, 0}, /* Swift._StringGuts */
    {"s12_ArrayBufferVy7SwiftUI4TextV8ModifierOG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 279, 1, 19, 0}, /* Swift._ArrayBuffer<SwiftUI.Text.Modifier> */
    {"s12_ArrayBufferVy7SwiftUI8MaterialVACE5LayerVG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 280, 1, 19, 0}, /* Swift._ArrayBuffer<(extension in SwiftUI):SwiftUI.Material.Layer> */
    {"s13_StringObjectV", SWIFTWALK_SAME, 16, 16, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 8, 0, 0, 281, 2, 19, 0}, /* Swift._StringObject */
    {"s14_BridgeStorageVys015__RawDictionaryB0CG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 283, 1, 19, 0}, /* Swift._BridgeStorage<Swift.__RawDictionaryStorage> */
    {"s14_BridgeStorageVys017__ContiguousArrayB4BaseCG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 284, 1, 19, 0}, /* Swift._BridgeStorage<Swift.__ContiguousArrayStorageBase> */
    {"s14_BridgeStorageVys08__RawSetB0CG", SWIFTWALK_SAME, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 285, 1, 19, 0}, /* Swift._BridgeStorage<Swift.__RawSetStorage> */
    {"s15_AnyHashableBox_p", SWIFTWALK_DYNAMIC, 40, 40, 8, 0x30007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 24, 0, 0, 286, 0, 19, 0}, /* Swift._AnyHashableBox; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
    {"s4Int8V", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 286, 1, 19, 0}, /* Swift.Int8 */
    {"s5Int32V", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 287, 1, 19, 0}, /* Swift.Int32 */
    {"s5UInt8V", SWIFTWALK_SAME, 1, 1, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 288, 1, 19, 0}, /* Swift.UInt8 */
    {"s5UInt8V_A13Bt", SWIFTWALK_SAME, 14, 14, 1, 0x0, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 289, 14, 19, 0}, /* (Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8, Swift.UInt8) */
    {"s6UInt32V", SWIFTWALK_SAME, 4, 4, 4, 0x3, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 303, 1, 19, 0}, /* Swift.UInt32 */
    {"s6UInt64V", SWIFTWALK_SAME, 8, 8, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, -1, 0, 0, 304, 1, 19, 0}, /* Swift.UInt64 */
    {"yXl", SWIFTWALK_DYNAMIC, 8, 8, 8, 0x10007, 2147483647, 2147483647, SWIFTWALK_XI_POINTER, 0, 0, 0, 305, 0, 19, 0}, /* Swift.AnyObject; existential: certify the contained value by its dynamic type (inline when size <= 24, align <= 8 and bitwise-takable, else boxed) */
#endif
#if defined(SWIFTWALK_CHILDREN)
    {2, 0, 0, 0}, /* _url */
    {3, 0, 8, 0}, /* _representation */
    {4, 0, 137, 0}, /* bytes */
    {4, 14, 136, 0}, /* length */
    {5, 0, 7, 0}, /* slice */
    {5, 8, 1, 0}, /* storage */
    {6, 0, 111, 0}, /* slice */
    {6, 8, 1, 0}, /* storage */
    {8, -1, 4, 0}, /* inline */
    {8, -1, 6, 0}, /* slice */
    {8, -1, 5, 0}, /* large */
    {8, -1, -1, 0}, /* empty */
    {9, -1, 3, 0}, /* some */
    {9, -1, -1, 0}, /* none */
    {10, 0, 104, 0}, /* native */
    {11, 0, 115, 0}, /* identifier */
    {12, 0, 115, 0}, /* identifier */
    {13, 0, 115, 0}, /* identifier */
    {14, 0, 17, 0}, /* value */
    {15, 0, 14, 0}, /* handle */
    {15, 8, 16, 0}, /* value */
    {16, 0, 102, 0}, /* _rawLayout */
    {17, 0, 117, 0}, /* _rawLayout */
    {19, 0, 10, 0}, /* top */
    {19, 8, 10, 0}, /* leading */
    {19, 16, 10, 0}, /* bottom */
    {19, 24, 10, 0}, /* trailing */
    {20, -1, 19, 0}, /* some */
    {20, -1, -1, 0}, /* none */
    {22, -1, -1, 0}, /* automatic */
    {22, -1, -1, 0}, /* visible */
    {22, -1, -1, 0}, /* hidden */
    {23, 0, 138, 0}, /* rawValue */
    {24, 0, 124, 0}, /* bits */
    {26, 0, 28, 0}, /* elements */
    {28, -1, 27, 0}, /* some */
    {28, -1, -1, 0}, /* none */
    {29, 0, 26, 0}, /* customInputs */
    {29, 8, 13, 0}, /* time */
    {29, 16, 21, 0}, /* cachedEnvironment */
    {29, 24, 11, 0}, /* phase */
    {29, 28, 12, 0}, /* transaction */
    {29, 32, 23, 0}, /* changedDebugProperties */
    {29, 36, 30, 0}, /* options */
    {29, 40, 109, 0}, /* mergedInputs */
    {30, 0, 138, 0}, /* rawValue */
    {31, 0, 32, 0}, /* root */
    {31, 16, 56, 0}, /* content */
    {32, 0, 67, 0}, /* alignment */
    {34, 0, 53, 0}, /* edges */
    {34, 8, 20, 0}, /* insets */
    {35, 0, 18, 0}, /* factory */
    {35, 16, 115, 0}, /* content */
    {35, 24, 66, 0}, /* childID */
    {35, 32, 116, 0}, /* contentSubgraph */
    {35, 40, 29, 0}, /* inputs */
    {36, -1, 35, 0}, /* some */
    {36, -1, -1, 0}, /* none */
    {37, 0, 57, 0}, /* content */
    {37, 48, 34, 0}, /* modifier */
    {38, 0, 37, 0}, /* content */
    {38, 89, 52, 0}, /* modifier */
    {39, -1, 38, 0}, /* some */
    {39, -1, -1, 0}, /* none */
    {40, 0, 47, 0}, /* placement */
    {40, 48, 54, 0}, /* content */
    {40, 80, 101, 0}, /* isEmpty */
    {41, -1, 40, 0}, /* some */
    {41, -1, -1, 0}, /* none */
    {42, 0, 47, 0}, /* placement */
    {42, 41, 68, 0}, /* content */
    {42, 41, 101, 0}, /* isEmpty */
    {43, -1, 42, 0}, /* some */
    {43, -1, -1, 0}, /* none */
    {44, 0, 26, 0}, /* value */
    {45, 0, 24, 0}, /* key */
    {46, 0, 24, 0}, /* key */
    {47, 0, 48, 0}, /* role */
    {48, -1, 22, 0}, /* primaryAction */
    {48, -1, 125, 0}, /* accessoryBar */
    {48, -1, -1, 0}, /* automatic */
    {48, -1, -1, 0}, /* principal */
    {48, -1, -1, 0}, /* navigation */
    {48, -1, -1, 0}, /* secondaryAction */
    {48, -1, -1, 0}, /* status */
    {48, -1, -1, 0}, /* confirmationAction */
    {48, -1, -1, 0}, /* cancellationAction */
    {48, -1, -1, 0}, /* destructiveAction */
    {48, -1, -1, 0}, /* keyboard */
    {48, -1, -1, 0}, /* navigationBarLeading */
    {48, -1, -1, 0}, /* navigationBarTrailing */
    {48, -1, -1, 0}, /* title */
    {48, -1, -1, 0}, /* largeTitle */
    {48, -1, -1, 0}, /* bottomBar */
    {48, -1, -1, 0}, /* largeNavigationTitleAccessory */
    {48, -1, -1, 0}, /* subtitle */
    {48, -1, -1, 0}, /* largeSubtitle */
    {48, -1, -1, 0}, /* tabBarLeading */
    {48, -1, -1, 0}, /* tabBarTrailing */
    {48, -1, -1, 0}, /* bottomOrnament */
    {48, -1, -1, 0}, /* topBarPinnedTrailing */
    {48, -1, -1, 0}, /* navigationBarTitle */
    {48, -1, -1, 0}, /* defaultNavigationBarLeading */
    {48, -1, -1, 0}, /* _implicitNavigation */
    {48, -1, -1, 0}, /* _implicitSidebarLeading */
    {48, -1, -1, 0}, /* _implicitSidebarTrailing */
    {48, -1, -1, 0}, /* _navigationBarTrailing */
    {48, -1, -1, 0}, /* _navigationBarLeading */
    {48, -1, -1, 0}, /* _navigationSplitViewLeading */
    {48, -1, -1, 0}, /* _navigationSplitViewTrailing */
    {48, -1, -1, 0}, /* search */
    {48, -1, -1, 0}, /* _tabViewSidebarOverflowMenu */
    {48, -1, -1, 0}, /* _tabViewSidebarBottomBar */
    {48, -1, -1, 0}, /* marker */
    {50, 0, 51, 0}, /* storage */
    {51, -1, -1, 0}, /* automatic */
    {51, -1, -1, 0}, /* active */
    {51, -1, -1, 0}, /* inactive */
    {51, -1, -1, 0}, /* matchWindow */
    {52, 0, 101, 0}, /* allowsHitTesting */
    {53, 0, 134, 0}, /* rawValue */
    {54, 0, 55, 0}, /* storage */
    {54, 24, 99, 0}, /* modifiers */
    {55, -1, 94, 0}, /* verbatim */
    {55, -1, 33, 0}, /* anyTextStorage */
    {56, -1, 54, 0}, /* some */
    {56, -1, -1, 0}, /* none */
    {57, 0, 31, 0}, /* _tree */
    {58, 0, 110, 0}, /* rawValue */
    {59, 0, 61, 0}, /* role */
    {59, 8, 44, 0}, /* plist */
    {59, 16, 58, 0}, /* addedOptions */
    {59, 24, 58, 0}, /* removedOptions */
    {60, 0, 59, 0}, /* from */
    {60, 32, 59, 0}, /* to */
    {60, 64, 104, 0}, /* fraction */
    {61, -1, 60, 1}, /* mix */
    {61, -1, 62, 1}, /* explicit */
    {61, -1, -1, 0}, /* regular */
    {61, -1, -1, 0}, /* identity */
    {61, -1, -1, 0}, /* clear */
    {61, -1, -1, 0}, /* dock */
    {61, -1, -1, 0}, /* appIcons */
    {61, -1, -1, 0}, /* widgets */
    {61, -1, -1, 0}, /* text */
    {61, -1, -1, 0}, /* avplayer */
    {61, -1, -1, 0}, /* facetime */
    {61, -1, -1, 0}, /* controlCenter */
    {61, -1, -1, 0}, /* notificationCenter */
    {61, -1, -1, 0}, /* monogram */
    {61, -1, -1, 0}, /* focusBorder */
    {61, -1, -1, 0}, /* keyboard */
    {61, -1, -1, 0}, /* sidebar */
    {61, -1, -1, 0}, /* control */
    {61, -1, -1, 0}, /* loupe */
    {61, -1, -1, 0}, /* slider */
    {61, -1, -1, 0}, /* camera */
    {61, -1, -1, 0}, /* cartouchePopover */
    {61, -1, -1, 0}, /* menu */
    {61, -1, -1, 0}, /* siri */
    {61, -1, -1, 0}, /* siriSnippet */
    {61, -1, -1, 0}, /* vibrantFill */
    {62, 0, 64, 0}, /* id */
    {62, 12, 63, 0}, /* flags */
    {62, 16, 50, 0}, /* activeAppearance */
    {63, 0, 138, 0}, /* rawValue */
    {64, -1, 95, 1}, /* coreMaterial */
    {64, -1, 49, 0}, /* provider */
    {64, -1, 65, 0}, /* layers */
    {64, -1, -1, 0}, /* ultraThin */
    {64, -1, -1, 0}, /* thin */
    {64, -1, -1, 0}, /* regular */
    {64, -1, -1, 0}, /* thick */
    {64, -1, -1, 0}, /* ultraThick */
    {64, -1, -1, 0}, /* systemBars */
    {64, -1, -1, 0}, /* contentFill */
    {64, -1, -1, 0}, /* windowFill */
    {64, -1, -1, 0}, /* menu */
    {64, -1, -1, 0}, /* secondaryBackground */
    {64, -1, -1, 0}, /* tertiaryBackground */
    {64, -1, -1, 0}, /* quaternaryBackground */
    {65, 0, 100, 0}, /* layers */
    {66, 0, 124, 0}, /* value */
    {67, 0, 46, 0}, /* horizontal */
    {67, 8, 45, 0}, /* vertical */
    {69, 0, 2, 0}, /* url */
    {69, 16, 94, 0}, /* name */
    {69, 32, 96, 0}, /* groupAppContainerIdentifier */
    {69, 48, 96, 0}, /* cloudKitContainerIdentifier */
    {69, 64, 78, 0}, /* groupContainer */
    {69, 88, 79, 0}, /* cloudKitDatabase */
    {69, 112, 84, 0}, /* schema */
    {69, 120, 101, 0}, /* allowsSave */
    {69, 121, 101, 0}, /* isStoredInMemoryOnly */
    {69, 122, 101, 0}, /* _isDocumentSource */
    {69, 128, 77, 0}, /* overrideOptions */
    {69, 226, 101, 0}, /* defaultURL */
    {69, 232, 84, 0}, /* containerSchema */
    {70, 0, 93, 0}, /* sqlitePragmas */
    {70, 8, 101, 0}, /* shouldMigrateStoreAutomatically */
    {70, 9, 101, 0}, /* shouldInferMappingModelAutomatically */
    {70, 10, 101, 0}, /* enableRemoteChangeNotifications */
    {70, 16, 76, 0}, /* _encryptionKey */
    {70, 24, 101, 0}, /* serviceConfiguration */
    {70, 32, 119, 0}, /* fileProtection */
    {70, 40, 72, 0}, /* historyTracking */
    {70, 64, 74, 0}, /* xpcStoreSettings */
    {71, 0, 108, 0}, /* includedEntities */
    {71, 8, 108, 0}, /* excludedEntities */
    {71, 16, 101, 0}, /* contextObjectForDelete */
    {71, 17, 101, 0}, /* allowPartialHistoryMigration */
    {72, -1, 71, 0}, /* some */
    {72, -1, -1, 0}, /* none */
    {73, 0, 96, 0}, /* serviceName */
    {73, 16, 96, 0}, /* machServiceName */
    {73, 32, 101, 0}, /* skipModelCheck */
    {73, 33, 101, 0}, /* skipRetryWait */
    {74, -1, 73, 0}, /* some */
    {74, -1, -1, 0}, /* none */
    {75, 0, 122, 0}, /* keychainItem */
    {76, -1, 75, 0}, /* some */
    {76, -1, -1, 0}, /* none */
    {77, -1, 70, 0}, /* some */
    {77, -1, -1, 0}, /* none */
    {78, 0, 101, 0}, /* _automatic */
    {78, 1, 101, 0}, /* _none */
    {78, 8, 96, 0}, /* _identifier */
    {79, 0, 101, 0}, /* _automatic */
    {79, 1, 101, 0}, /* _none */
    {79, 8, 96, 0}, /* _privateDBName */
    {81, -1, 82, 0}, /* custom */
    {81, -1, -1, 0}, /* json */
    {81, -1, -1, 0}, /* plist */
    {83, -1, 81, 0}, /* some */
    {83, -1, -1, 0}, /* none */
    {84, -1, 80, 0}, /* some */
    {84, -1, -1, 0}, /* none */
    {91, 0, 130, 0}, /* object */
    {92, 0, 91, 0}, /* _variant */
    {93, -1, 92, 0}, /* some */
    {93, -1, -1, 0}, /* none */
    {94, 0, 126, 0}, /* _guts */
    {95, 0, 94, 0}, /* # */
    {95, 16, 94, 0}, /* # */
    {95, 32, 121, 0}, /* # */
    {96, -1, 94, 0}, /* some */
    {96, -1, -1, 0}, /* none */
    {97, 0, 96, 0}, /* # */
    {97, 16, 96, 0}, /* # */
    {98, -1, 97, 0}, /* some */
    {98, -1, -1, 0}, /* none */
    {99, 0, 127, 0}, /* _buffer */
    {100, 0, 128, 0}, /* _buffer */
    {101, 0, 87, 0}, /* _value */
    {102, 0, 103, 0}, /* storage */
    {104, 0, 86, 0}, /* _value */
    {105, 0, 132, 0}, /* object */
    {106, 0, 132, 0}, /* object */
    {107, 0, 105, 0}, /* _variant */
    {108, -1, 107, 0}, /* some */
    {108, -1, -1, 0}, /* none */
    {109, 0, 106, 0}, /* _variant */
    {110, 0, 89, 0}, /* _value */
    {111, 0, 135, 0}, /* lowerBound */
    {111, 4, 135, 0}, /* upperBound */
    {113, -1, 112, 0}, /* some */
    {113, -1, -1, 0}, /* none */
    {114, -1, 113, 0}, /* some */
    {114, -1, -1, 0}, /* none */
    {115, 0, 138, 0}, /* rawValue */
    {117, 0, 138, 0}, /* _os_unfair_lock_opaque */
    {118, 0, 123, 0}, /* _rawValue */
    {119, -1, 118, 0}, /* some */
    {119, -1, -1, 0}, /* none */
    {121, -1, 120, 0}, /* some */
    {121, -1, -1, 0}, /* none */
    {124, 0, 89, 0}, /* _value */
    {125, 0, 133, 0}, /* _box */
    {126, 0, 129, 0}, /* _object */
    {127, 0, 131, 0}, /* _storage */
    {128, 0, 131, 0}, /* _storage */
    {129, 0, 139, 0}, /* _countAndFlagsBits */
    {129, 8, 85, 0}, /* _object */
    {130, 0, 85, 0}, /* rawValue */
    {131, 0, 85, 0}, /* rawValue */
    {132, 0, 85, 0}, /* rawValue */
    {134, 0, 90, 0}, /* _value */
    {135, 0, 88, 0}, /* _value */
    {136, 0, 90, 0}, /* _value */
    {137, 0, 136, 0}, /* # */
    {137, 1, 136, 0}, /* # */
    {137, 2, 136, 0}, /* # */
    {137, 3, 136, 0}, /* # */
    {137, 4, 136, 0}, /* # */
    {137, 5, 136, 0}, /* # */
    {137, 6, 136, 0}, /* # */
    {137, 7, 136, 0}, /* # */
    {137, 8, 136, 0}, /* # */
    {137, 9, 136, 0}, /* # */
    {137, 10, 136, 0}, /* # */
    {137, 11, 136, 0}, /* # */
    {137, 12, 136, 0}, /* # */
    {137, 13, 136, 0}, /* # */
    {138, 0, 88, 0}, /* _value */
    {139, 0, 89, 0}, /* _value */
#endif
#if defined(SWIFTWALK_IMAGES)
    {9, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {9, 1, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf0", "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff", "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"},
    {39, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {39, 1, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {72, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {72, 1, "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {74, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {74, 1, "\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {77, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {77, 1, "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {81, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {81, 1, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00"},
    {81, 2, "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00", "\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00"},
    {83, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {83, 1, "\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00", "\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00"},
    {98, 0, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {98, 1, "\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {114, 0, "\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00", "\x00\x00\x00\x00\x00\x00\x00\x00"},
    {114, 1, "\x01\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff", "\x02\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff"},
#endif
