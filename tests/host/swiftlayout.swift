import Foundation
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
// Lines: L layout, X extra inhabitant k written into 0xa5 storage, C case injected into zeroed (z) or
// 0xa5 (f) storage with the tag read back, V a live value's words (P = the Box object's address),
// W a walk: "walk N" with each patch offset:source>receiver, or "refused TYPE".
// Payload words are never dereferenced, retained or destroyed by the witness calls here.

final class Box { let n: Int; init(_ n: Int) { self.n = n } }
struct Entry { var date: Date; var value: Double }
enum Trend { case up, down }
struct Holder<E> { var items: [E]; var count: Int }   // its extra inhabitants: the Array buffer word
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
struct TwoRefused { var a: Seventeen; var b: TwoEmpty; var c: String }

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
let patches = UnsafeMutableRawPointer.allocate(byteCount: 64 * 32, alignment: 8)
func name(_ m: UnsafeRawPointer) -> String { String(describing: unsafeBitCast(m, to: Any.Type.self)) }
func walkRaw(_ label: String, _ type: UnsafeRawPointer, _ p: UnsafeRawPointer) {
    guard let walk = walker else { return }
    var unresolved: UnsafeRawPointer? = nil
    let n = walk(type, p, patches, 64, &unresolved)
    if n < 0 { print("W \(label) refused \(unresolved.map(name) ?? "?")"); return }
    var line = "W \(label) walk \(n)"
    for i in 0..<Int(n) {
        let e = patches + 32 * i   // empty_patch {offset, bits, receiver_bits, consumed}
        line += " \(e.load(as: UInt64.self)):\(hex(e.load(fromByteOffset: 8, as: UInt64.self)))>\(hex(e.load(fromByteOffset: 16, as: UInt64.self)))"
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

if CommandLine.arguments.count > 1 {
    guard let lib = dlopen(CommandLine.arguments[1], RTLD_NOW),
          let sym = dlsym(lib, "hao_sv_collect_empty_patches_detail") else { print("no libhao walker"); exit(2) }
    walker = unsafeBitCast(sym, to: Walk.self)
    if let s = dlsym(lib, "hao_sv_collect_survey_patches") { surveyor = unsafeBitCast(s, to: Walk.self) }
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
    walkRaw("Optional<SwiftUI.Content> nil (stays refused)", meta(o), p)
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
walk("TwoEmpty.one (stays refused)", TwoEmpty.one("x"))
walk("Optional<SecondaryText> nil (stays refused)", SecondaryText?.none)
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
    var v = TwoRefused(a: .c1(Box(1)), b: .one("x"), c: "y")
    withUnsafeBytes(of: &v) { p in
        guard let survey = surveyor else { return }
        var unresolved: UnsafeRawPointer? = nil
        let n = survey(meta(TwoRefused.self), p.baseAddress!, patches, 64, &unresolved)
        print("W TwoRefused survey \(n < 0 ? "refused \(unresolved.map(name) ?? "?")" : "walk \(n)")")
        fflush(stdout)
    }
}
