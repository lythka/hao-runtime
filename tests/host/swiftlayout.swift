import Foundation
import Network   // NWListener.State: a native spare-bit enum with empty cases, listed in mpenum_gen.inc
import SwiftUI   // loaded only to resolve SwiftUI.ContainerBackgroundValue.Content; no view is made
// Layout contracts of hao's Swift value walk (src/swiftvalue/Recipe.c selected/collect, Enum.c
// discover_fixed_enum_case), measured in one program built natively for arm64 and for x86_64
// (tests/host/swiftlayout.sh compares the two), and the walk's verdicts over the x86_64 values.
// A: an Array buffer word supplies a struct's extra inhabitants for every Element kind (XI0 0, XI k
//    k on arm64, 2k on x86_64); a live Array's elements stay opaque to the walk.
// B: a multi-payload enum with native witnesses whose payload-case tags are only in bits 60-63 of
//    its words and that has no empty case (a 17-case enum and Optional of such an enum stay refused).
// C: a multi-payload enum with a trailing tag byte (size = largest payload + 1, xi = 256 - cases,
//    at most one empty case); two empty cases, a mismatching size or xi, a bad tag stay refused.
// F: a function value other than a block (Swift closure, thin, C) has the same nils on both
//    architectures under any number of Optionals (XI k = code word k); a block's differ (k against 2k).
// G: a Clang-imported enum is its integer, native metadata or guest, and holds no nil.
// S: survey mode walks a refused value to the end and prints every refusing type (x86_64 stderr).
// D: a value of fewer than 8 bytes holds no pointer word, so nothing in it is patched, whatever its
//    enum layout (spare bits of a payload, extra inhabitants of a payload enum, an Optional of one).
// E: C with several empty cases (they share a tag and keep their index in the payload area); spare-bit
//    tags decided by a generated table row (mpenum_gen.inc): rows given explicitly to authored enums
//    whose encodings are measured equal here (empty cases, tags in a Bool's spare bits) or not (17
//    cases), and the table's own rows for native types of this host; native types the tables list as
//    declared differently on arm64 (archdiff_gen.inc): refused where frozen, walked where resilient.
// N: nils of standard pointer-word types: Set/Dictionary storage, Any and any P, AnyHashable, AnyObject,
//    Swift class references, metatypes and URL have XI0 0 and XI k k on arm64, 2k on x86_64 (as Array);
//    the raw pointers, OpaquePointer, Selector and ObjectIdentifier have one XI, 0; IndexPath's XIs are
//    tag bytes ff, fe, ... on both; Data's differ (spare bits of its representation) and stay refused.
// Lines: L layout, X extra inhabitant k written into 0xa5 storage, C case injected into zeroed (z) or
// 0xa5 (f) storage with the tag read back, V a live value's words (P = the Box object's address),
// W a walk: "walk N" with each patch offset:source>receiver, or "refused TYPE"; x86_64 only: R the
// fixed-enum case found with an explicit table row, T a table lookup.
// Payload words are never dereferenced, retained or destroyed by the witness calls here.

final class Box { let n: Int; init(_ n: Int) { self.n = n } }
struct Entry { var date: Date; var value: Double }
enum Trend { case up, down }
struct Holder<E> { var items: [E]; var count: Int }   // its extra inhabitants: the Array buffer word
struct Inline<T> { var n: Int; var t: T }
// One field shape per field order of a measured value: the Array first, then a TimeZone with as many
// extra inhabitants (the first maximum field supplies them).
struct Sparkline { var entries: [Entry]; var dates: ClosedRange<Date>; var tz: TimeZone
                   var values: ClosedRange<Double>; var baseline: Double? }

struct AV { let storage: Box }
struct StyleStorage { let box: Box }
struct Style { let storage: StyleStorage }
enum Content { case view(AV); case styled(AV, Style) }
enum Five { case a(Box), b(Box), c(Box), d(Box), e(Box) }
enum Seventeen { case c0(Box), c1(Box), c2(Box), c3(Box), c4(Box), c5(Box), c6(Box), c7(Box), c8(Box)
                 case c9(Box), c10(Box), c11(Box), c12(Box), c13(Box), c14(Box), c15(Box), c16(Box) }
enum SpareEmpty { case a(Box), b(Box), none }          // spare-bit tags and an empty case
enum SpareArray { case a(Box), b([Box]?, Box) }        // a payload nil to patch, in a word without tag bits
enum SpareNested { case a(Box), b(Box, NSObject??) }

enum SecondaryText { case one(String); case two(String, String); case none }
enum TwoEmpty { case one(String); case two(String, String); case none, other }
enum TextNil { case one(String); case two(NSObject??, String); case none }   // a nested nil in a payload

enum FiveEmpty { case one(Int), two(Int, Int), e0, e1, e2, e3, e4 }   // tag byte, five empty cases share tag 2
struct IntBool { var i: Int; var b: Bool }
enum Low2 { case a(IntBool), b(IntBool) }                         // spare-bit tag in the Bool's byte
enum Low3E2 { case a(IntBool), b(IntBool), c(IntBool), e0, e1 }   // the same with empty cases

enum Kind { case clear, cloudy(Trend), snow }                 // single payload, the payload's inhabitants
enum Flags { case a(Bool), b(Bool), c(Bool, Bool), none }    // multi-payload, tag in Bool's spare bits
struct Small { var kind: Kind; var flags: Flags; var count: Int16 }
typealias Action = (Bool) -> ()
typealias CAction = @convention(c) (Bool) -> ()
typealias BAction = @convention(block) (Bool) -> ()
struct Row { var title: String; var action: Action?; var tap: CAction? }
struct Button { var label: String; var action: @MainActor () -> () }          // a closure, not optional
struct Handlers { var edit: ((inout [Int]) -> ())?; var items: (() -> [NSObject])?; var open: ((URL) -> Bool?)?
                  var send: (@Sendable (NSObject) async throws -> ())? }
struct Oriented { var orientation: ComparisonResult?; var style: NSLineBreakMode; var count: Int }
struct TwoRefused { var a: Seventeen; var b: SpareEmpty; var c: String }

typealias StoreTag = @convention(c) (UnsafeMutableRawPointer, UInt32, UInt32, UnsafeRawPointer) -> Void
typealias GetTag = @convention(c) (UnsafeRawPointer, UnsafeRawPointer) -> UInt32
typealias Inject = @convention(c) (UnsafeMutableRawPointer, UInt32, UnsafeRawPointer) -> Void
func meta(_ t: Any.Type) -> UnsafeRawPointer { unsafeBitCast(t, to: UnsafeRawPointer.self) }
func vwt(_ m: UnsafeRawPointer) -> UnsafeRawPointer { m.load(fromByteOffset: -8, as: UnsafeRawPointer.self) }
func fn<F>(_ m: UnsafeRawPointer, _ slot: Int, _: F.Type) -> F {
    unsafeBitCast(vwt(m).load(fromByteOffset: 8 * slot, as: UnsafeRawPointer.self), to: F.self)
}
func size(_ m: UnsafeRawPointer) -> Int { Int(vwt(m).load(fromByteOffset: 64, as: UInt64.self)) }
func stride(_ m: UnsafeRawPointer) -> Int { Int(vwt(m).load(fromByteOffset: 72, as: UInt64.self)) }
func flags(_ m: UnsafeRawPointer) -> UInt32 { vwt(m).load(fromByteOffset: 80, as: UInt32.self) }
func xi(_ m: UnsafeRawPointer) -> UInt32 { vwt(m).load(fromByteOffset: 84, as: UInt32.self) }
let hex = { (v: UInt64) in String(v, radix: 16) }

setvbuf(stdout, nil, _IOLBF, 0)   // every line printed so far survives a crash
var boxAddress: UInt64 = 0
func words(_ p: UnsafeRawPointer, _ n: Int) -> String {
    var out: [String] = []
    var at = 0
    while at < n {
        if n - at < 8 {
            out.append((at..<n).map { String(format: "%02x", p.load(fromByteOffset: $0, as: UInt8.self)) }.joined())
            break
        }
        let w = p.loadUnaligned(fromByteOffset: at, as: UInt64.self)
        if boxAddress != 0 && w != 0 && (w ^ boxAddress) & 0x00ff_ffff_ffff_ffff == 0 {
            out.append(w == boxAddress ? "P" : hex(w ^ boxAddress) + "|P")
        } else {
            out.append(String(format: "%016llx", w))
        }
        at += 8
    }
    return out.joined(separator: " ")
}
func storage(_ m: UnsafeRawPointer, _ fill: UInt8) -> UnsafeMutableRawPointer {
    let p = UnsafeMutableRawPointer.allocate(byteCount: stride(m) + 16, alignment: 16)
    p.initializeMemory(as: UInt8.self, repeating: fill, count: stride(m) + 16)
    return p
}
func layout(_ label: String, _ t: Any.Type) {
    let m = meta(t)
    print("L \(label) size=\(size(m)) stride=\(stride(m)) flags=0x\(hex(UInt64(flags(m)))) xi=0x\(hex(UInt64(xi(m))))")
}
func inhabitants(_ label: String, _ t: Any.Type, _ count: UInt32 = 3) {
    let m = meta(t)
    for k in 1...min(count, xi(m)) {
        let p = storage(m, 0xa5)
        fn(m, 7, StoreTag.self)(p, k, xi(m), m)   // storeEnumTagSinglePayload(value, k, xi, type)
        print("X \(label) \(k - 1) \(words(p, size(m)))")
        p.deallocate()
    }
}
func cases(_ label: String, _ t: Any.Type, _ n: UInt32, _ fill: UInt8) {
    let m = meta(t)
    for c in 0..<n {
        let p = storage(m, fill)
        fn(m, 13, Inject.self)(p, c, m)
        let back = fn(m, 11, GetTag.self)(p, m)
        print("C \(label)#\(c) \(fill == 0 ? "z" : "f") \(words(p, size(m))) back=\(back)")
        p.deallocate()
    }
}
func value<T>(_ label: String, _ v: T) {
    var copy = v
    withUnsafeBytes(of: &copy) { print("V \(label) \(words($0.baseAddress!, $0.count))") }
}

// --- the walk (x86_64 only: libhao is an x86_64 dylib)
typealias Walk = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UInt32,
                                 UnsafeMutablePointer<UnsafeRawPointer?>) -> Int32
var walker: Walk? = nil
var surveyor: Walk? = nil
let patches = UnsafeMutableRawPointer.allocate(byteCount: 64 * 40, alignment: 8)
func name(_ m: UnsafeRawPointer) -> String { String(describing: unsafeBitCast(m, to: Any.Type.self)) }
func walkRaw(_ label: String, _ type: UnsafeRawPointer, _ p: UnsafeRawPointer) {
    guard let walk = walker else { return }
    var unresolved: UnsafeRawPointer? = nil
    let n = walk(type, p, patches, 64, &unresolved)
    if n < 0 { print("W \(label) refused \(unresolved.map(name) ?? "?")"); return }
    var line = "W \(label) walk \(n)"
    for i in 0..<Int(n) {
        let e = patches + 40 * i   // empty_patch {offset, bits, receiver_bits, consumed, arm_bits}
        line += " \(e.load(as: UInt64.self)):\(hex(e.load(fromByteOffset: 8, as: UInt64.self)))>\(hex(e.load(fromByteOffset: 16, as: UInt64.self)))"
        // The arm64 encoding, where it is not a pointer word's (consumed - 1): a translated spare-bit XI.
        let consumed = UInt64(e.load(fromByteOffset: 24, as: UInt32.self)), arm = e.load(fromByteOffset: 32, as: UInt64.self)
        if arm != consumed - 1 { line += "~\(hex(arm))" }
    }
    print(line)
}
func walk<T>(_ label: String, _ v: T) {
    var copy = v
    withUnsafePointer(to: &copy) { walkRaw(label, meta(T.self), UnsafeRawPointer($0)) }
}
// A copy of an enum's metadata with its value witness table's size or xi changed: the C rule must
// refuse a layout that does not match.
func forged(_ t: Any.Type, size newSize: Int? = nil, xi newXI: UInt32? = nil) -> UnsafeRawPointer {
    let m = meta(t)
    let table = UnsafeMutableRawPointer.allocate(byteCount: 112, alignment: 16)
    table.copyMemory(from: vwt(m), byteCount: 112)
    if let s = newSize { table.storeBytes(of: UInt64(s), toByteOffset: 64, as: UInt64.self) }
    if let x = newXI { table.storeBytes(of: x, toByteOffset: 84, as: UInt32.self) }
    let fake = UnsafeMutableRawPointer.allocate(byteCount: 32, alignment: 16)
    fake.storeBytes(of: UnsafeRawPointer(table), as: UnsafeRawPointer.self)
    fake.storeBytes(of: m.load(as: UInt64.self), toByteOffset: 8, as: UInt64.self)
    fake.storeBytes(of: m.load(fromByteOffset: 8, as: UInt64.self), toByteOffset: 16, as: UInt64.self)
    fake.storeBytes(of: 0, toByteOffset: 24, as: UInt64.self)
    return UnsafeRawPointer(fake + 8)
}

// The fixed-enum case with an explicit table row {payload cases, empty cases, xi, flags} (nil: none).
typealias FixedRow = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UnsafeRawPointer?) -> Int32
typealias ArchdiffType = @convention(c) (UnsafeRawPointer) -> Int32
typealias MpenumType = @convention(c) (UnsafeRawPointer, UnsafeMutableRawPointer) -> Int32
typealias ArchdiffLookup = @convention(c) (UnsafeRawPointer, UInt64) -> Int32
typealias MpenumLookup = @convention(c) (UnsafeRawPointer, UInt64, UnsafeMutableRawPointer) -> Int32
var fixedRow: FixedRow? = nil, archdiffType: ArchdiffType? = nil, mpenumType: MpenumType? = nil
var archdiffLookup: ArchdiffLookup? = nil, mpenumLookup: MpenumLookup? = nil
let fixedOut = UnsafeMutableRawPointer.allocate(byteCount: 512, alignment: 16)
func rowRaw(_ label: String, _ type: UnsafeRawPointer, _ p: UnsafeRawPointer, _ row: [UInt32]?) {
    guard let f = fixedRow else { return }
    // mpenum_row: 9 UInt32 fields, then two mask pointers (zero here: not used by this rule).
    let full = (row ?? [0]) + [UInt32](repeating: 0, count: 14 - (row ?? [0]).count)
    let ok = full.withUnsafeBytes { f(type, p, fixedOut, row == nil ? nil : $0.baseAddress!) }
    let shown = row.map { "{\($0[0]),\($0[1]),0x\(hex(UInt64($0[2]))),\($0[3])}" } ?? "none"
    if ok == 0 { print("R \(label) row \(shown) refused"); return }
    let payload = fixedOut.load(as: UInt.self), tag = fixedOut.load(fromByteOffset: 8, as: UInt32.self)
    print("R \(label) row \(shown) case \(tag) \(payload == 0 ? "empty" : name(UnsafeRawPointer(bitPattern: payload)!))")
}
func row<T>(_ label: String, _ v: T, _ r: [UInt32]?) {
    var copy = v
    withUnsafePointer(to: &copy) { rowRaw(label, meta(T.self), UnsafeRawPointer($0), r) }
}
func uuidBytes(_ text: String) -> [UInt8] {
    var out: [UInt8] = []
    var i = text.startIndex
    while i < text.endIndex { let j = text.index(i, offsetBy: 2); out.append(UInt8(text[i..<j], radix: 16)!); i = j }
    return out
}
// A loaded image's mach header by path suffix, and its LC_UUID.
func image(_ suffix: String) -> UnsafeRawPointer? {
    for i in 0..<_dyld_image_count() where String(cString: _dyld_get_image_name(i)).hasSuffix(suffix) {
        return UnsafeRawPointer(_dyld_get_image_header(i))
    }
    return nil
}
func uuidOf(_ header: UnsafeRawPointer) -> String {
    var at = 32
    for _ in 0..<header.load(fromByteOffset: 16, as: UInt32.self) {
        let cmd = header.load(fromByteOffset: at, as: UInt32.self), size = Int(header.load(fromByteOffset: at + 4, as: UInt32.self))
        if cmd == 0x1b { return (0..<16).map { String(format: "%02x", header.load(fromByteOffset: at + 8 + $0, as: UInt8.self)) }.joined() }
        at += size
    }
    return "-"
}
// The metadata of a nominal struct or enum from its descriptor (its access function, request 0): a
// non-generic one, or one with a single unconstrained generic parameter, given Int.
typealias Access = @convention(c) (Int) -> UnsafeRawPointer
typealias Access1 = @convention(c) (Int, UnsafeRawPointer) -> UnsafeRawPointer
func typeAt(_ header: UnsafeRawPointer, _ offset: Int) -> UnsafeRawPointer? {
    let desc = header + offset
    let flags = desc.load(as: UInt32.self)
    let fn = desc + 12 + Int(desc.load(fromByteOffset: 12, as: Int32.self))
    if flags & 0x80 == 0 { return unsafeBitCast(fn, to: Access.self)(0) }
    // The generic context header follows the 28-byte struct or enum descriptor.
    guard flags & 0x1f == 0x11 || flags & 0x1f == 0x12, desc.load(fromByteOffset: 36, as: UInt16.self) == 1,
          desc.load(fromByteOffset: 40, as: UInt16.self) == 1 else { return nil }
    return unsafeBitCast(fn, to: Access1.self)(0, meta(Int.self))
}

if CommandLine.arguments.count > 1 {
    guard let lib = dlopen(CommandLine.arguments[1], RTLD_NOW),
          let sym = dlsym(lib, "hao_sv_collect_empty_patches_detail") else { print("no libhao walker"); exit(2) }
    walker = unsafeBitCast(sym, to: Walk.self)
    if let s = dlsym(lib, "hao_sv_collect_survey_patches") { surveyor = unsafeBitCast(s, to: Walk.self) }
    // The table entry points (a libhao without them prints no R or T line, and the script fails).
    if let fr = dlsym(lib, "hao_sv_discover_fixed_enum_case_row"), let at = dlsym(lib, "hao_sv_archdiff_type"),
       let mt = dlsym(lib, "hao_sv_mpenum_type"), let al = dlsym(lib, "hao_sv_archdiff_lookup"),
       let ml = dlsym(lib, "hao_sv_mpenum_lookup") {
        fixedRow = unsafeBitCast(fr, to: FixedRow.self)
        archdiffType = unsafeBitCast(at, to: ArchdiffType.self); mpenumType = unsafeBitCast(mt, to: MpenumType.self)
        archdiffLookup = unsafeBitCast(al, to: ArchdiffLookup.self); mpenumLookup = unsafeBitCast(ml, to: MpenumLookup.self)
    }
}

// A. The Array buffer word, every Element kind; a struct's XIs from it; Optional<S>.none writes only it.
for (label, t) in [("Entry", [Entry].self), ("Trend", [Trend].self), ("Box", [Box].self),
                   ("AnyObject", [AnyObject].self), ("NSObject", [NSObject].self), ("Any", [Any].self)] as [(String, Any.Type)] {
    layout("Array<\(label)>", t)
    inhabitants("Array<\(label)>", t)
}
layout("Sparkline", Sparkline.self)
layout("Optional<Sparkline>", Sparkline?.self)
inhabitants("Sparkline", Sparkline.self)
cases("Optional<Sparkline>", Sparkline?.self, 2, 0xa5)
layout("Optional<Holder<Box>>", Holder<Box>?.self)
cases("Optional<Holder<Box>>", Holder<Box>?.self, 2, 0xa5)
cases("Optional<Optional<Holder<Box>>>", Holder<Box>??.self, 2, 0xa5)

let now = Date(timeIntervalSince1970: 1_800_000_000)
let spark = Sparkline(entries: [Entry(date: now, value: 1)], dates: now...now, tz: TimeZone(identifier: "UTC")!,
                      values: 0...1, baseline: nil)
walk("Optional<Sparkline> nil", Sparkline?.none)
walk("Optional<Sparkline> some", Sparkline?.some(spark))
walk("Optional<Holder<Entry>> nil", Holder<Entry>?.none)
walk("Optional<Holder<Trend>> nil", Holder<Trend>?.none)
walk("Optional<Holder<Box>> nil", Holder<Box>?.none)
walk("Optional<Holder<Box>> some", Holder<Box>?.some(Holder(items: [Box(1)], count: 1)))
walk("Optional<Holder<AnyObject>> nil", Holder<AnyObject>?.none)
walk("Optional<Holder<NSObject>> nil", Holder<NSObject>?.none)
walk("Optional<Holder<NSObject>> some", Holder<NSObject>?.some(Holder(items: [NSObject()], count: 1)))
walk("Optional<Holder<Any>> nil", Holder<Any>?.none)
walk("Optional<Optional<Holder<Box>>> outer nil", Holder<Box>??.none)
walk("Optional<Optional<Holder<Box>>> inner nil", Holder<Box>??.some(nil))
// The elements of a live Array are not walked: outer nils inside them are no patch of this value.
walk("Optional<Holder<NSObject??>> some, elements nil", Holder<NSObject??>?.some(Holder(items: [nil, .some(nil)], count: 2)))

// B. Spare-bit tags.
let box = Box(7)
boxAddress = UInt64(UInt(bitPattern: Unmanaged.passUnretained(box).toOpaque()))
for (label, t, n) in [("Content", Content.self, 2), ("Five", Five.self, 5), ("Seventeen", Seventeen.self, 17),
                      ("SpareEmpty", SpareEmpty.self, 3), ("SpareArray", SpareArray.self, 2),
                      ("SpareNested", SpareNested.self, 2)] as [(String, Any.Type, UInt32)] {
    layout(label, t)
    cases(label, t, n, 0)
}
layout("Optional<Content>", Content?.self)
inhabitants("Content", Content.self, 1)
let view = Content.view(AV(storage: box)), styled = Content.styled(AV(storage: box), Style(storage: StyleStorage(box: box)))
value("Content.view", view)
value("Content.styled", styled)
value("Five.e", Five.e(box))
value("SpareArray.b(nil)", SpareArray.b(nil, box))
value("SpareNested.b(outerNil)", SpareNested.b(box, nil))
walk("Content.view", view)
walk("Content.styled", styled)
for (k, v) in [Five.a(box), .b(box), .c(box), .d(box), .e(box)].enumerated() { walk("Five#\(k)", v) }
walk("SpareArray.b(nil)", SpareArray.b(nil, box))
walk("SpareArray.b(some)", SpareArray.b([box], box))
walk("SpareArray.a", SpareArray.a(box))
walk("Seventeen.c1 (stays refused)", Seventeen.c1(box))
walk("Optional<Content> nil (stays refused)", Content?.none)
walk("SpareEmpty.b (stays refused)", SpareEmpty.b(box))
walk("SpareNested.b(outer nil)", SpareNested.b(box, nil))
walk("SpareNested.b(inner nil)", SpareNested.b(box, .some(nil)))
// The SwiftUI type of this shape (one AnyView / AnyView and AnyShapeStyle: class-reference words). Its
// values are words made here around the Box address, never retained, released or dereferenced.
_ = Color.red   // SwiftUI loaded, so that the name resolves
if let t = _typeByName("7SwiftUI24ContainerBackgroundValueV7ContentO"),
   _typeName(t, qualified: true) == "SwiftUI.ContainerBackgroundValue.Content" {
    layout("SwiftUI.Content", t)
    cases("SwiftUI.Content", t, 2, 0)
    inhabitants("SwiftUI.Content", t, 1)
    func optional<T>(_: T.Type) -> Any.Type { T?.self }
    let o = _openExistential(t, do: optional)
    layout("Optional<SwiftUI.Content>", o)
    for (label, w) in [("SwiftUI.Content view", [boxAddress, 0]), ("SwiftUI.Content styled", [boxAddress, 0x8000_0000_0000_0000 | boxAddress])] {
        w.withUnsafeBytes { walkRaw(label, meta(t), $0.baseAddress!) }
    }
    let p = storage(meta(o), 0xa5)
    fn(meta(o), 13, Inject.self)(p, 1, meta(o))   // Optional.none
    walkRaw("Optional<SwiftUI.Content> nil (translated: a table row)", meta(o), p)
    p.deallocate()
} else {
    print("SwiftUI.ContainerBackgroundValue.Content unresolved")
}

// C. Trailing tag byte.
for (label, t, n) in [("SecondaryText", SecondaryText.self, 3), ("TwoEmpty", TwoEmpty.self, 4),
                      ("TextNil", TextNil.self, 3)] as [(String, Any.Type, UInt32)] {
    layout(label, t)
    cases(label, t, n, 0xa5)
    inhabitants(label, t, 2)
}
layout("Optional<SecondaryText>", SecondaryText?.self)
let large = String(repeating: "x", count: 40)
value("SecondaryText.one(small)", SecondaryText.one("Cupertino"))
value("SecondaryText.two(small)", SecondaryText.two("12:00", "Sunny"))
value("SecondaryText.none", SecondaryText.none)
value("TextNil.two(outerNil)", TextNil.two(nil, "x"))
value("TextNil.two(innerNil)", TextNil.two(.some(nil), "x"))
walk("SecondaryText.one(small)", SecondaryText.one("Cupertino"))
walk("SecondaryText.one(large)", SecondaryText.one(large))
walk("SecondaryText.two(small)", SecondaryText.two("12:00", "Sunny"))
walk("SecondaryText.two(large)", SecondaryText.two(large, large))
walk("SecondaryText.none", SecondaryText.none)
walk("TextNil.one", TextNil.one("x"))
walk("TextNil.two(outer nil)", TextNil.two(nil, "x"))
walk("TextNil.two(inner nil)", TextNil.two(.some(nil), "x"))
walk("TextNil.two(object)", TextNil.two(NSObject(), "x"))
walk("TextNil.none", TextNil.none)
walk("TwoEmpty.one", TwoEmpty.one("x"))
walk("Optional<SecondaryText> nil", SecondaryText?.none)
do {
    var v = SecondaryText.one("Cupertino")
    withUnsafeMutableBytes(of: &v) { p in
        let m = meta(SecondaryText.self)
        walkRaw("SecondaryText size+1 (stays refused)", forged(SecondaryText.self, size: size(m) + 1), p.baseAddress!)
        walkRaw("SecondaryText xi-1 (stays refused)", forged(SecondaryText.self, xi: xi(m) - 1), p.baseAddress!)
        p[32] = 3
        walkRaw("SecondaryText tag 3 (stays refused)", m, p.baseAddress!)
        p[32] = 0
    }
}

// D. Fewer than 8 bytes.
for (label, t) in [("Kind", Kind.self), ("Flags", Flags.self), ("Small", Small.self),
                   ("Optional<Kind>", Kind?.self)] as [(String, Any.Type)] { layout(label, t) }
cases("Kind", Kind.self, 3, 0)
cases("Flags", Flags.self, 4, 0xa5)
for (label, v) in [("Kind.clear", Kind.clear), ("Kind.cloudy", .cloudy(.down)), ("Kind.snow", .snow)] { walk(label, v) }
for (label, v) in [("Flags.a", Flags.a(true)), ("Flags.c", .c(false, true)), ("Flags.none", .none)] { walk(label, v) }
walk("Small", Small(kind: .cloudy(.up), flags: .b(true), count: 7))
walk("Optional<Kind> nil", Kind?.none)
walk("Optional<Bool> nil", Bool?.none)
walk("Optional<Bool> some", Bool?.some(true))

// F. Functions.
for (label, t) in [("Action", Action.self), ("Optional<Action>", Action?.self), ("Optional<Optional<Action>>", Action??.self),
                   ("CAction", CAction.self), ("BAction", BAction.self), ("Row", Row.self)] as [(String, Any.Type)] { layout(label, t) }
inhabitants("Action", Action.self)
inhabitants("CAction", CAction.self)
inhabitants("BAction", BAction.self)
var taps = 0
let action: Action = { taps += $0 ? 1 : 0 }
let tap: CAction = { _ in }
walk("Optional<Action> nil", Action?.none)
walk("Optional<Action> some", Action?.some(action))
walk("Optional<Optional<Action>> outer nil", Action??.none)
walk("Optional<Optional<Action>> inner nil", Action??.some(nil))
walk("Optional<CAction> nil", CAction?.none)
walk("Optional<Optional<CAction>> outer nil", CAction??.none)
walk("Row nils", Row(title: "Cupertino", action: nil, tap: nil))
walk("Row some", Row(title: "Cupertino", action: action, tap: tap))
walk("Button", Button(label: "Add", action: {}))
walk("Handlers nils", Handlers(edit: nil, items: nil, open: nil, send: nil))
walk("Handlers some", Handlers(edit: { $0.append(1) }, items: { [] }, open: { _ in nil }, send: { _ in }))
walk("Optional<Optional<Handlers>> outer nil", Handlers??.none)
walk("Optional<Optional<Handlers>> inner nil", Handlers??.some(nil))
walk("Optional<Optional<Handlers>> some", Handlers??.some(Handlers(edit: nil, items: { [] }, open: nil, send: nil)))

// G. Imported C enums.
layout("ComparisonResult", ComparisonResult.self)
layout("Optional<ComparisonResult>", ComparisonResult?.self)
walk("ComparisonResult", ComparisonResult.orderedDescending)
walk("Optional<ComparisonResult> nil", ComparisonResult?.none)
walk("Optional<ComparisonResult> some", ComparisonResult?.some(.orderedSame))
walk("Oriented", Oriented(orientation: .orderedAscending, style: .byTruncatingTail, count: 3))
walk("Oriented nil", Oriented(orientation: nil, style: .byClipping, count: 0))

// S. Survey mode: two refusing fields of one value.
do {
    var v = TwoRefused(a: .c1(Box(1)), b: .b(Box(2)), c: "y")
    withUnsafeBytes(of: &v) { p in
        guard let survey = surveyor else { return }
        var unresolved: UnsafeRawPointer? = nil
        let n = survey(meta(TwoRefused.self), p.baseAddress!, patches, 64, &unresolved)
        print("W TwoRefused survey \(n < 0 ? "refused \(unresolved.map(name) ?? "?")" : "walk \(n)")")
        fflush(stdout)
    }
}

// E1. Several empty cases behind one extra tag byte: tag 2, the index in the payload's first bytes.
layout("FiveEmpty", FiveEmpty.self)
cases("FiveEmpty", FiveEmpty.self, 7, 0xa5)
inhabitants("FiveEmpty", FiveEmpty.self, 2)
walk("TwoEmpty.two", TwoEmpty.two("12:00", large))
walk("TwoEmpty.none", TwoEmpty.none)
walk("TwoEmpty.other", TwoEmpty.other)
for (k, v) in [FiveEmpty.one(1), .two(2, 3), .e0, .e1, .e2, .e3, .e4].enumerated() { walk("FiveEmpty#\(k)", v) }
do {
    var v = FiveEmpty.e4
    withUnsafeMutableBytes(of: &v) { p in
        let m = meta(FiveEmpty.self)
        walkRaw("FiveEmpty xi-1 (stays refused)", forged(FiveEmpty.self, xi: xi(m) - 1), p.baseAddress!)
        p[0] = 5
        walkRaw("FiveEmpty empty index 5 (stays refused)", m, p.baseAddress!)
        p[0] = 4; p[16] = 3
        walkRaw("FiveEmpty tag 3 (stays refused)", m, p.baseAddress!)
        p[16] = 2
    }
}

// E2. Spare-bit tags decided by a table row, given explicitly ({payload cases, empty cases, xi, flags}).
// Each measured enum's payload-case and empty-case lines are equal on both architectures (flags 1|2 is
// their true row) except Seventeen's (flags 0). A row applies only with the live XI count.
for (label, t, n) in [("Low2", Low2.self, 2), ("Low3E2", Low3E2.self, 5)] as [(String, Any.Type, UInt32)] {
    layout(label, t)
    cases(label, t, n, 0)
}
let ib = IntBool(i: 5, b: true)
let cx = xi(meta(Content.self)), ex = xi(meta(SpareEmpty.self)), lx = xi(meta(Low2.self)), l3x = xi(meta(Low3E2.self))
row("Content.styled", styled, nil)
row("Content.styled", styled, [2, 0, cx, 0])        // payload cases differ by architecture: refused
row("Content.styled", styled, [2, 0, cx, 4])
row("Content.styled", styled, [2, 0, cx, 1])        // ... and so do (or may) the empty ones
row("Content.styled", styled, [2, 0, cx, 3])
row("Content.styled", styled, [2, 0, cx + 1, 0])    // another layout's row: ignored, the bit rule decides
row("Content.styled", styled, [3, 0, cx, 3])        // case counts that are not the type's
row("SpareEmpty.b", SpareEmpty.b(box), nil)
row("SpareEmpty.b", SpareEmpty.b(box), [2, 1, ex, 3])
row("SpareEmpty.none", SpareEmpty.none, [2, 1, ex, 3])
row("SpareEmpty.none", SpareEmpty.none, [2, 1, ex, 1])
row("SpareEmpty.none", SpareEmpty.none, [2, 1, ex + 1, 3])
row("Low2.b", Low2.b(ib), nil)
row("Low2.b", Low2.b(ib), [2, 0, lx, 3])
row("Low3E2.c", Low3E2.c(ib), [3, 2, l3x, 3])
row("Low3E2.e1", Low3E2.e1, [3, 2, l3x, 3])
row("Seventeen.c1", Seventeen.c1(box), nil)
row("Seventeen.c1", Seventeen.c1(box), [17, 0, xi(meta(Seventeen.self)), 0])

// E3. The tables themselves: lookups by (x86_64 image UUID, descriptor offset), then the rows of native
// types of this host (T ... none where this host's image is another build than the tables').
if let al = archdiffLookup, let ml = mpenumLookup {
    for (label, u, off) in [("ComputationalGraph.Output.Errors", "df649c0e20ba3737acf6823cd32bdeae", 0x11ab60),
                            ("ComputationalGraph.Feature", "df649c0e20ba3737acf6823cd32bdeae", 0x117bd4),
                            ("Charts._PrimitivePlottableKind.Storage", "cc8b2ed6e36c31f386c8ae255af6ef8b", 0x32b164),
                            ("Swift.Float16", "54f671eeb22d3f02930c2cc027116e14", 0x507a10),
                            ("unlisted", "df649c0e20ba3737acf6823cd32bdeae", 0x11ab64)] {
        let r = uuidBytes(u).withUnsafeBytes { al($0.baseAddress!, UInt64(off)) }
        print("T lookup archdiff \(label) \(r == 0 ? "-" : String(UnicodeScalar(UInt8(r))))")
    }
    for (label, u, off) in [("FontParser.(private).GlyphMap", "2adf65c06f8c3c99be6bc248ed304cf8", 0x278c58),
                            ("MediaPlaybackCore.Event", "fab3826800e431c58a0eaea02056b11a", 0x483e34),
                            ("Network.NWListener.State", "04e480edd6803b4d9121165ab0e7048e", 0x16b6b50),
                            ("first", "00301d5b02e23a8fa67b06efa0bdcbfa", 0x56eac0),
                            ("last", "ff2948bd9cec39f1ad66386cf451bd6a", 0x143c6c),
                            ("unlisted", "ff2948bd9cec39f1ad66386cf451bd6a", 0x143c70)] {
        var r = [UInt32](repeating: 0, count: 4)
        let ok = uuidBytes(u).withUnsafeBytes { ub in r.withUnsafeMutableBytes { ml(ub.baseAddress!, UInt64(off), $0.baseAddress!) } }
        print("T lookup mpenum \(label) \(ok == 0 ? "-" : "{\(r[0]),\(r[1]),0x\(hex(UInt64(r[2]))),\(r[3])}")")
    }
}
func tableRow(_ label: String, _ t: Any.Type) {
    guard let f = mpenumType else { return }
    var r = [UInt32](repeating: 0, count: 4)
    let ok = r.withUnsafeMutableBytes { f(meta(t), $0.baseAddress!) }
    print(ok == 0 ? "T \(label) none" : "T \(label) row {\(r[0]),\(r[1]),0x\(hex(UInt64(r[2]))),\(r[3])} live xi=0x\(hex(UInt64(xi(meta(t)))))")
}
// Native spare-bit enums of this host's images whose table rows say their encodings are the same on
// arm64: with empty cases and tags outside bits 60-63 (NWListener.State, 5 bytes: rule D walks it
// anyway), with an empty case (BundleDescription, tags in bits 62-63, XIs differ), and with tags in
// the spare bits of an Optional<Double>'s tag byte (AppIntents ResizeBehavior and ZoomBehavior, 9
// bytes). Their injected cases must be equal on both architectures; the AppIntents values are those
// injected cases (payloads zero: Optional<Double>.some(0) and 0).
typealias Bundle = LocalizedStringResource.BundleDescription
layout("NWListener.State", NWListener.State.self)
cases("NWListener.State", NWListener.State.self, 5, 0)
layout("BundleDescription", Bundle.self)
cases("BundleDescription", Bundle.self, 3, 0)
tableRow("NWListener.State", NWListener.State.self)
tableRow("BundleDescription", Bundle.self)
_ = dlopen("/System/Library/Frameworks/AppIntents.framework/AppIntents", RTLD_NOW)
for (label, mangled, n) in [("ResizeBehavior", "10AppIntents14ResizeBehaviorO", 2), ("ZoomBehavior", "10AppIntents12ZoomBehaviorO", 3)]
    as [(String, String, UInt32)] {
    guard let t = _typeByName(mangled) else { print("\(label) unresolved"); continue }
    layout(label, t)
    cases(label, t, n, 0)
    tableRow(label, t)
    for c in 0..<n {
        let p = storage(meta(t), 0)
        fn(meta(t), 13, Inject.self)(p, c, meta(t))
        walkRaw("\(label)#\(c)", meta(t), p)
        p.deallocate()
    }
}
walk("NWListener.State.waiting", NWListener.State.waiting(.posix(.ENETDOWN)))
walk("NWListener.State.failed", NWListener.State.failed(.dns(-65540)))
walk("NWListener.State.setup", NWListener.State.setup)
walk("NWListener.State.ready", NWListener.State.ready)
walk("NWListener.State.cancelled", NWListener.State.cancelled)
walk("BundleDescription.main", Bundle.main)
walk("BundleDescription.forClass", Bundle.forClass(NSObject.self))
walk("BundleDescription.atURL", Bundle.atURL(URL(fileURLWithPath: "/System")))

// E4. Native types declared differently on arm64 (archdiff_gen.inc), from this host's images when they
// are the build the table describes. A resilient one (Output.Errors, Feature.DataType) is held by guest
// code only through native witnesses: its bytes are x86_64 throughout and it is walked as before (rule
// C, rule D). A frozen one (Swift.Float16, 0 bytes on x86_64) is compiled into guest layouts: refused
// whatever rule would otherwise admit it, alone, in a tuple and as an Optional nil; inside a native
// struct its module did not freeze it is native-laid-out and walked.
// FILL makes the value valid where zero bytes are not (a String payload).
func listedType(_ label: String, _ path: String, _ uuid: String, _ offset: Int,
                _ fill: (UnsafeMutableRawPointer) -> Void = { _ in }) {
    guard walker != nil else { return }
    _ = dlopen(path, RTLD_NOW)
    guard let h = image("/" + path.split(separator: "/").last!), uuidOf(h) == uuid else { print("T \(label) none"); return }
    guard let m = typeAt(h, offset) else { print("T \(label) unresolved"); return }
    if let at = archdiffType {
        let r = at(m)
        print("T \(label) archdiff \(r == 0 ? "-" : String(UnicodeScalar(UInt8(r)))) size=\(size(m))")
    }
    let p = storage(m, 0)
    fill(p)
    walkRaw(label, m, p)
    func tuple<T>(_: T.Type) -> Any.Type { (Int, T).self }
    func optional<T>(_: T.Type) -> Any.Type { T?.self }
    let t = _openExistential(unsafeBitCast(m, to: Any.Type.self), do: tuple)
    let q = storage(meta(t), 0)
    fill(q + 8)
    walkRaw("\(label) in a tuple", meta(t), q)
    let o = _openExistential(unsafeBitCast(m, to: Any.Type.self), do: optional)
    let n = storage(meta(o), 0)
    fn(meta(o), 13, Inject.self)(n, 1, meta(o))   // Optional.none
    walkRaw("\(label) Optional nil", meta(o), n)
    func unfrozen<T>(_: T.Type) -> Any.Type { Inline<T>.self }   // this program's own struct: not frozen
    let u = _openExistential(unsafeBitCast(m, to: Any.Type.self), do: unfrozen)
    let z = storage(meta(u), 0)
    fill(z + 8)
    walkRaw("\(label) in a native unfrozen struct", meta(u), z)
    z.deallocate()
    p.deallocate(); q.deallocate(); n.deallocate()
}
let graph = "/System/Library/PrivateFrameworks/ComputationalGraph.framework/ComputationalGraph"
// Output<Int>.Errors.emptyFeature(name: "abc"): a String payload at offset 0, tag byte 16 = case 0.
let abc = "abc"
listedType("ComputationalGraph.Output.Errors", graph, "df649c0e20ba3737acf6823cd32bdeae", 0x11ab60) { p in
    withUnsafeBytes(of: abc) { p.copyMemory(from: $0.baseAddress!, byteCount: 16) }
    p.storeBytes(of: 0, toByteOffset: 16, as: UInt8.self)
}
listedType("ComputationalGraph.Feature.DataType", graph, "df649c0e20ba3737acf6823cd32bdeae", 0x117bf0)
listedType("Swift.Float16", "/usr/lib/swift/libswiftCore.dylib", "54f671eeb22d3f02930c2cc027116e14", 0x507a10)

// N. Nils of standard types (x86_64 walks: the plain walker, as for a published value).
protocol NP {}
struct NPS: NP { var n = 1 }
final class Obs: NSObject {}
let pointerWord: [(String, Any.Type)] = [
    ("Set<String>", Set<String>.self), ("Dictionary<String,Int>", [String: Int].self), ("Any", Any.self),
    ("anyNP", (any NP).self), ("AnyHashable", AnyHashable.self), ("AnyObject", AnyObject.self), ("Box", Box.self),
    ("NSKeyValueObservation", NSKeyValueObservation.self), ("Box.Type", Box.Type.self),
    ("AnyObject.Type", AnyObject.Type.self), ("Any.Type", Any.Type.self), ("URL", URL.self),
    ("DateComponents", DateComponents.self), ("UnsafeRawPointer", UnsafeRawPointer.self),
    ("UnsafeMutableRawPointer", UnsafeMutableRawPointer.self), ("OpaquePointer", OpaquePointer.self),
    ("Selector", Selector.self), ("ObjectIdentifier", ObjectIdentifier.self), ("IndexPath", IndexPath.self),
    ("Data", Data.self)]
for (label, t) in pointerWord {
    func optional<T>(_: T.Type) -> Any.Type { T?.self }
    layout(label, t)
    inhabitants(label, t, 3)
    let o = _openExistential(t, do: optional)
    layout("Optional<\(label)>", o)
    cases("Optional<\(label)>", o, 2, 0xa5)
}
// Each Optional's nil and its Optional's inner and outer nils, made by native generic code.
@inline(never) func nils<T>(_ label: String, _ some: T) {
    walk("N Optional<\(label)> nil", T?.none)
    walk("N Optional<\(label)> some", T?.some(some))
    walk("N Optional<Optional<\(label)>> outer nil", T??.none)
    walk("N Optional<Optional<\(label)>> inner nil", T??.some(nil))
}
let kvo = NSObject().observe(\.description) { _, _ in }
let raw = UnsafeMutableRawPointer.allocate(byteCount: 8, alignment: 8)
var dc = DateComponents(); dc.year = 2026
nils("Set<String>", Set(["a"]))
nils("Dictionary<String,Int>", ["a": 1])
nils("Any", 7 as Any)
nils("anyNP", NPS() as any NP)
nils("AnyHashable", AnyHashable("a"))
nils("AnyObject", Box(1) as AnyObject)
nils("Box", Box(2))
nils("NSKeyValueObservation", kvo)
nils("Box.Type", Box.self)
nils("AnyObject.Type", Box.self as AnyObject.Type)
nils("Any.Type", Int.self as Any.Type)
nils("URL", URL(fileURLWithPath: "/System"))
nils("DateComponents", dc)
nils("UnsafeRawPointer", UnsafeRawPointer(raw))
nils("UnsafeMutableRawPointer", raw)
nils("OpaquePointer", OpaquePointer(raw))
nils("Selector", #selector(NSObject.description))
nils("ObjectIdentifier", ObjectIdentifier(Box.self))
nils("IndexPath", IndexPath(indexes: [1, 2, 3]))
nils("Data", Data([1, 2, 3]))   // its nil differs (word 1 f000... against ff00...): translated (N2)

// N2. Data's nils translated from the mpenum table's masks of Data._Representation (x86_64 spare byte
// 15 ff, arm64 f0): XI k on both, a nil in either encoding patched, the arm64 one beside it (~).
struct DataBox { var d: Data?; var n: Int }
layout("Optional<Optional<Data>>", Data??.self)
cases("Optional<Optional<Data>>", Data??.self, 2, 0xa5)
layout("DataBox", DataBox.self)
inhabitants("DataBox", DataBox.self, 2)
walk("N2 DataBox nil", DataBox(d: nil, n: 1))
walk("N2 DataBox some", DataBox(d: Data([4]), n: 1))
walk("N2 Optional<DataBox> nil", DataBox?.none)
do {   // guest-made (arm64) nils: XI 0 and 1 of the arm64 layout
    for (label, t, word) in [("N2 Optional<Data> arm64 nil", Data?.self as Any.Type, UInt64(0xf000_0000_0000_0000)),
                             ("N2 Optional<Optional<Data>> arm64 outer nil", Data??.self, 0xb000_0000_0000_0000),
                             ("N2 Optional<Optional<Data>> arm64 inner nil", Data??.self, 0xf000_0000_0000_0000)] {
        let p = storage(meta(t), 0)
        p.storeBytes(of: word, toByteOffset: 8, as: UInt64.self)
        walkRaw(label, meta(t), p)
        p.deallocate()
    }
    // Thirteen Optionals: the outermost nil is XI 12, past arm64's 12 (refused: no arm64 encoding).
    func optional<T>(_: T.Type) -> Any.Type { T?.self }
    var t: Any.Type = Data.self
    for _ in 0..<13 { t = _openExistential(t, do: optional) }
    let p = storage(meta(t), 0)
    fn(meta(t), 13, Inject.self)(p, 1, meta(t))
    walkRaw("N2 Data, 13 Optionals: outer nil (no arm64 encoding)", meta(t), p)
    p.deallocate()
}
