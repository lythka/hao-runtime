// Guest Swift code calling the native Swift runtime's value witnesses and helpers that
// have C++ or C names (tests/guest/swiftvw.sh): the swift::metadataimpl templates behind
// the builtin value witness tables ($sBoWV, $sBOWV, $sBbWV, $sBi64_WV, ...), the
// existential tables (OpaqueExistentialBox<N>, ClassExistentialBox<N>), the tuple
// witnesses swift_getTupleTypeMetadata installs (tuple_destroy<IsPOD, IsInline>, ...),
// the POD witnesses swift_initStructMetadata installs (_swift_pod_copy, ...), the POD and
// compact witnesses native libraries' value witness tables hold (__swift_memcpyN_M,
// swift_cvw_*, __swift_*_strong), the multi-payload enum tag functions
// (swift_getMultiPayloadEnumTagSinglePayload), and the weak and unowned reference entry
// points. Built -Onone, so generic code is not specialized: every copy, take, assign,
// destroy, buffer copy and enum tag of a T goes through T's value witness table, and for
// these types the table is native code.
//
//   swiftvw list            the sections
//   swiftvw SECTION         run one section
//   swiftvw vw SECTION      the symbol (dladdr) of every value witness the section's types
//                           have: what the section reaches (not compared: the slices fold
//                           identical functions differently)
//   swiftvw syms SECTION    the imports the section calls by name (checked with nm -u)
import Foundation

var deinits = 0
var log: [String] = []

final class Box: CustomStringConvertible {
    let v: Int
    init(_ v: Int) { self.v = v }
    deinit { deinits += 1; log.append("Box(\(v))") }
    var description: String { "Box(\(v))" }
}

class Tracked: NSObject {
    let v: Int
    init(_ v: Int) { self.v = v }
    deinit { deinits += 1; log.append("Tracked(\(v))") }
    override var description: String { "Tracked(\(v))" }
}

protocol CP: AnyObject { var tag: String { get } }
final class CPBox: CP {
    let v: Int
    init(_ v: Int) { self.v = v }
    deinit { deinits += 1; log.append("CPBox(\(v))") }
    var tag: String { "CPBox(\(v))" }
}
final class CPObjC: NSObject, CP {
    let v: Int
    init(_ v: Int) { self.v = v }
    deinit { deinits += 1; log.append("CPObjC(\(v))") }
    var tag: String { "CPObjC(\(v))" }
}

struct Pair<A, B> { var a: A; var b: B }
enum MP<T> { case a(T), b(T, Int), c, d }
struct Big { var x0 = 0, x1 = 1, x2 = 2, x3 = 3, x4 = 4, x5 = 5, x6 = 6, x7 = 7 }

func out(_ s: String, _ label: String, _ v: Any) { print("\(s) \(label): \(v)") }

// ---------------------------------------------------------------- generic exercisers

@inline(never) func gcopy<T>(_ x: T) -> T { return x }                        // initializeWithCopy
@inline(never) func gassign<T>(_ d: inout T, _ s: T) { d = s }                // assignWithCopy
@inline(never) func gassignTake<T>(_ d: inout T, _ make: () -> T) { d = make() } // assignWithTake
@inline(never) func gmove<T>(_ p: UnsafeMutablePointer<T>) -> T { p.move() }   // initializeWithTake
@inline(never) func gsome<T>(_ x: T) -> T? { return x }                       // storeEnumTagSinglePayload(0)
@inline(never) func gnone<T>(_: T.Type) -> T? { return nil }                  // storeEnumTagSinglePayload(1)
@inline(never) func gisNil<T>(_ x: T?) -> Bool { if case .none = x { return true }; return false }  // getEnumTagSinglePayload
@inline(never) func gunwrap<T>(_ x: T?, _ d: T) -> T { x ?? d }
@inline(never) func gany<T>(_ x: T) -> Any { return x }                       // into an existential buffer
@inline(never) func ganycopy(_ a: Any) -> Any { let b = a; return b }         // initializeBufferWithCopyOfBuffer
@inline(never) func garray<T>(_ x: T, _ n: Int) -> [T] { var a: [T] = []; for _ in 0..<n { a.append(x) }; return a }
@inline(never) func gpair<A, B>(_ a: A, _ b: B) -> Pair<A, B> { Pair(a: a, b: b) }
@inline(never) func gswap<T>(_ a: inout T, _ b: inout T) { let t = a; a = b; b = t }

// Every value-witness operation on a T, through generic code, with T's own description.
@inline(never) func exercise<T>(_ s: String, _ label: String, _ make: () -> T, _ other: () -> T, _ show: (T) -> String) {
    let x = make()
    let c = gcopy(x)
    out(s, "\(label) copy", show(c))
    var d = other()
    gassign(&d, x)
    out(s, "\(label) assign", show(d))
    gassignTake(&d, other)
    out(s, "\(label) assignTake", show(d))
    let p = UnsafeMutablePointer<T>.allocate(capacity: 1)
    p.initialize(to: x)
    let m = gmove(p)
    p.deallocate()
    out(s, "\(label) take", show(m))
    let o = gsome(x)
    out(s, "\(label) some", "\(gisNil(o)) \(show(gunwrap(o, other())))")
    let n = gnone(T.self)
    out(s, "\(label) none", "\(gisNil(n)) \(show(gunwrap(n, other())))")
    let a = gany(x)
    let a2 = ganycopy(a)
    out(s, "\(label) any", show(a2 as! T))
    let arr = garray(x, 3)
    out(s, "\(label) array", arr.map(show).joined(separator: ","))
    var e1 = make(), e2 = other()
    gswap(&e1, &e2)
    out(s, "\(label) swap", "\(show(e1)) \(show(e2))")
}

func scope(_ s: String, _ label: String, _ body: () -> Void) {
    let before = deinits
    log.removeAll()
    body()
    out(s, "\(label) deinits", "\(deinits - before) [\(log.sorted().joined(separator: " "))]")
}

// ---------------------------------------------------------------- sections

func sec_objc() {
    let s = "objc"
    var k = 0
    scope(s, "Tracked") {
        exercise(s, "Tracked", { k += 1; return Tracked(k) }, { k += 1; return Tracked(100 + k) }, { $0.description })
    }
    scope(s, "NSMutableString") {
        exercise(s, "NSMutableString", { NSMutableString(string: "ms") }, { NSMutableString(string: "other") }, { $0 as String })
    }
    scope(s, "AnyObject") {
        exercise(s, "AnyObject", { k += 1; return Tracked(200 + k) as AnyObject }, { k += 1; return Box(300 + k) as AnyObject },
                 { "\($0)" })
    }
}

func sec_swiftobj() {
    let s = "swiftobj"
    var k = 0
    scope(s, "Box") {
        exercise(s, "Box", { k += 1; return Box(k) }, { k += 1; return Box(100 + k) }, { $0.description })
    }
}

func sec_bridge() {
    let s = "bridge"
    var k = 0
    scope(s, "[Tracked]") {
        exercise(s, "[Tracked]", { () -> [Tracked] in k += 1; return [Tracked(k), Tracked(k + 50)] }, { [] }, { (x: [Tracked]) in "\(x.map { $0.v })" })
    }
    scope(s, "[String:Box]") {
        exercise(s, "[String:Box]", { () -> [String: Box] in k += 1; return ["k\(k)": Box(k)] }, { [:] }, { (x: [String: Box]) in "\(x.mapValues { $0.v })" })
    }
    scope(s, "Set<Int>") {
        exercise(s, "Set<Int>", { [1, 2, 3] }, { [] }, { "\($0.sorted())" })
    }
}

func sec_native() {
    let s = "native"
    exercise(s, "Int", { -42 }, { 7 }, { "\($0)" })
    exercise(s, "UInt64", { UInt64.max - 1 }, { 3 }, { "\($0)" })
    exercise(s, "Double", { 2.5 }, { -0.0 }, { "\($0)" })
    exercise(s, "Int8", { Int8(-5) }, { 9 }, { "\($0)" })
    exercise(s, "Int16", { Int16(-300) }, { 9 }, { "\($0)" })
    exercise(s, "Unicode.Scalar", { Unicode.Scalar(0x1F600)! }, { "a" }, { "\($0.value)" })
    exercise(s, "Void", { () }, { () }, { "\($0)" })
    exercise(s, "RawPointer", { UnsafeRawPointer(bitPattern: 0x1234)! }, { UnsafeRawPointer(bitPattern: 8)! }, { "\($0)" })
}

func sec_existential() {
    let s = "existential"
    var k = 0
    scope(s, "Any") {
        exercise(s, "Any(Box)", { k += 1; return Box(k) as Any }, { k += 1; return "str\(k)" as Any }, { "\($0)" })
        exercise(s, "Any(Big)", { Big() as Any }, { 5 as Any }, { "\($0)" })
    }
    scope(s, "CustomStringConvertible") {
        exercise(s, "CSC", { k += 1; return Box(k) as any CustomStringConvertible }, { 3.5 as any CustomStringConvertible },
                 { $0.description })
    }
    scope(s, "CP") {
        exercise(s, "CP(swift)", { k += 1; return CPBox(k) as any CP }, { k += 1; return CPObjC(k) as any CP }, { $0.tag })
    }
    scope(s, "Error") {
        exercise(s, "Error", { CocoaError(.fileNoSuchFile) as any Error }, { URLError(.timedOut) as any Error },
                 { "\(($0 as NSError).domain) \(($0 as NSError).code)" })
    }
}

func sec_tuple() {
    let s = "tuple"
    var k = 0
    scope(s, "(String,String,Box)") {
        exercise(s, "(String,String,Box)", { k += 1; return ("a\(k)", "b\(k)", Box(k)) }, { k += 1; return ("", "", Box(100 + k)) },
                 { "\($0.0) \($0.1) \($0.2)" })
    }
    scope(s, "(Box,Int)") {
        exercise(s, "(Box,Int)", { k += 1; return (Box(k), k) }, { k += 1; return (Box(100 + k), -k) }, { "\($0.0) \($0.1)" })
    }
    exercise(s, "(Int,Int8)", { (77, Int8(-1)) }, { (0, 0) }, { "\($0.0) \($0.1)" })
    exercise(s, "(Int,Int,Int,Int)", { (1, 2, 3, 4) }, { (9, 9, 9, 9) }, { "\($0)" })
    scope(s, "(Tracked,Tracked,Tracked,Tracked)") {
        exercise(s, "(Tracked x4)", { k += 1; return (Tracked(k), Tracked(k + 1), Tracked(k + 2), Tracked(k + 3)) },
                 { (Tracked(0), Tracked(0), Tracked(0), Tracked(0)) }, { "\($0.0.v) \($0.1.v) \($0.2.v) \($0.3.v)" })
    }
}

func sec_pod() {
    let s = "pod"
    var k = 0
    exercise(s, "Pair<Int,Int8>", { gpair(123456789, Int8(-7)) }, { gpair(0, Int8(0)) }, { "\($0.a) \($0.b)" })
    let d0 = Date(timeIntervalSince1970: 5000)
    exercise(s, "Range<Date>", { d0..<d0.addingTimeInterval(60) }, { d0..<d0 },
             { "\($0.lowerBound.timeIntervalSince1970) \($0.upperBound.timeIntervalSince1970)" })
    exercise(s, "Pair<Big,Int8>", { gpair(Big(), Int8(3)) }, { gpair(Big(x0: 9), Int8(0)) }, { "\($0.a.x0) \($0.a.x7) \($0.b)" })
    scope(s, "Pair<Box,Int>") {
        exercise(s, "Pair<Box,Int>", { k += 1; return gpair(Box(k), k) }, { k += 1; return gpair(Box(100 + k), 0) },
                 { "\($0.a) \($0.b)" })
    }
}

func sec_memcpy() {
    let s = "memcpy"
    // (not UUID(uuid:): its 16 UInt8 arguments are swiftcc's narrow stack packing, tests/guest/swiftcc.sh)
    let u = UUID(uuidString: "01020304-0506-0708-090A-0B0C0D0E0F10")!
    exercise(s, "UUID", { u }, { UUID(uuidString: "00000000-0000-0000-0000-000000000000")! }, { $0.uuidString })
    let d0 = Date(timeIntervalSince1970: 1_000_000)
    exercise(s, "Date", { d0 }, { Date(timeIntervalSince1970: 0) }, { "\($0.timeIntervalSince1970)" })
    exercise(s, "DateInterval", { DateInterval(start: d0, duration: 60) }, { DateInterval(start: d0, duration: 1) },
             { "\($0.start.timeIntervalSince1970) \($0.duration)" })
    exercise(s, "Bool", { true }, { false }, { "\($0)" })
    exercise(s, "Calendar.Component", { Calendar.Component.weekOfYear }, { Calendar.Component.era }, { "\($0)" })
    exercise(s, "Decimal", { Decimal(string: "3.14159")! }, { Decimal(0) }, { "\($0)" })
    exercise(s, "Duration", { Duration.seconds(90) + .milliseconds(5) }, { Duration.zero }, { "\($0)" })
    exercise(s, "CGPoint", { CGPoint(x: 1.5, y: -2) }, { CGPoint(x: 0, y: 0) }, { "\($0.x) \($0.y)" })
    // 72 bytes: past the inline buffer, so initializeBufferWithCopyOfBuffer boxes it
    // (the hash is fixed by SWIFT_DETERMINISTIC_HASHING=1)
    exercise(s, "Hasher", { var h = Hasher(); h.combine(42); h.combine("x"); return h }, { Hasher() },
             { (h: Hasher) in var c = h; return "\(c.finalize())" })
    exercise(s, "ObjectIdentifier", { ObjectIdentifier(NSObject.self) }, { ObjectIdentifier(NSString.self) },
             { $0 == ObjectIdentifier(NSObject.self) ? "NSObject" : "NSString" })
}

func sec_cvw() {
    let s = "cvw"
    exercise(s, "String", { "hello, a long string that is not small" }, { "x" }, { $0 })
    exercise(s, "Substring", { "hello, world"[...].dropFirst(7) }, { "" }, { String($0) })
    exercise(s, "Character", { "é" }, { "a" }, { String($0) })
    exercise(s, "Data", { Data([1, 2, 3, 250]) }, { Data() }, { $0.map { "\($0)" }.joined(separator: ".") })
    exercise(s, "URL", { URL(string: "https://example.com/a?b=c")! }, { URL(fileURLWithPath: "/tmp") }, { $0.absoluteString })
    exercise(s, "TimeZone", { TimeZone(identifier: "Asia/Tokyo")! }, { TimeZone(identifier: "UTC")! }, { $0.identifier })
    exercise(s, "Locale", { Locale(identifier: "fr_FR") }, { Locale(identifier: "en_US") }, { $0.identifier })
    exercise(s, "PersonNameComponents", { var p = PersonNameComponents(); p.givenName = "Ada"; return p },
             { PersonNameComponents() }, { $0.givenName ?? "-" })
    exercise(s, "Measurement", { Measurement(value: 3, unit: UnitLength.meters) }, { Measurement(value: 0, unit: UnitLength.feet) },
             { "\($0.value) \($0.unit.symbol)" })
}

@inline(never) func mpdesc<T>(_ e: MP<T>, _ show: (T) -> String) -> String {
    switch e {
    case .a(let t): return "a(\(show(t)))"
    case .b(let t, let i): return "b(\(show(t)),\(i))"
    case .c: return "c"
    case .d: return "d"
    }
}
@inline(never) func mpmake<T>(_ which: Int, _ t: T) -> MP<T> {
    switch which { case 0: return .a(t); case 1: return .b(t, which * 10); case 2: return .c; default: return .d }
}

func sec_enum() {
    let s = "enum"
    var k = 0
    scope(s, "MP<Tracked>") {
        for w in 0..<4 {
            exercise(s, "MP<Tracked>.\(w)", { k += 1; return mpmake(w, Tracked(k)) }, { .d }, { mpdesc($0) { $0.description } })
        }
    }
    scope(s, "MP<Box>") {
        for w in 0..<4 {
            exercise(s, "MP<Box>.\(w)", { k += 1; return mpmake(w, Box(k)) }, { .c }, { mpdesc($0) { $0.description } })
        }
    }
    exercise(s, "MP<Int>.1", { mpmake(1, 5) }, { .c }, { mpdesc($0) { "\($0)" } })
    // Optional of Optional: the payload's extra inhabitants are used up one level at a time
    scope(s, "Box??") {
        exercise(s, "Box??", { k += 1; return Optional(Optional(Box(k))) }, { Optional(Optional<Box>.none) },
                 { $0.map { $0.map { $0.description } ?? "inner nil" } ?? "outer nil" })
    }
}

// weak and unowned references, Swift-native and ObjC (unknownObject) flavours
final class WeakHolder { weak var w: Box?; init(_ b: Box?) { w = b } }
final class WeakObjC { weak var w: Tracked?; init(_ b: Tracked?) { w = b } }
struct WeakCP { weak var w: (any CP)? }
struct WeakAny { weak var w: AnyObject? }
struct UnownedBox { unowned let u: Box }
struct UnownedObjC { unowned let u: Tracked }
struct UnownedCP { unowned let u: any CP }

func sec_weak() {
    let s = "weak"
    var b: Box? = Box(1)
    let h = WeakHolder(b)                             // swift_weakInit
    out(s, "swift load", h.w?.description ?? "nil")  // swift_weakLoadStrong
    var h2 = WeakCP(w: CPBox(2))                      // released at once
    out(s, "cp temp", h2.w?.tag ?? "nil")
    let keep = CPBox(3)
    h2.w = keep                                       // weakAssign
    let h3 = h2                                       // weakCopyInit
    out(s, "cp copy", h3.w?.tag ?? "nil")
    b = nil
    out(s, "swift after release", h.w?.description ?? "nil")
    h.w = Box(4)
    out(s, "swift assign temp", h.w?.description ?? "nil")
    exercise(s, "WeakCP", { h3 }, { WeakCP(w: nil) }, { $0.w?.tag ?? "nil" })   // weakCopyInit/TakeInit/CopyAssign/TakeAssign
    var t: Tracked? = Tracked(5)
    let o = WeakObjC(t)                               // swift_unknownObjectWeakInit
    out(s, "objc load", o.w?.description ?? "nil")
    var wa = WeakAny(w: t)
    exercise(s, "WeakAny", { wa }, { WeakAny(w: nil) }, { $0.w.map { "\($0)" } ?? "nil" })
    t = nil
    out(s, "objc after release", o.w?.description ?? "nil")
    out(s, "any after release", wa.w.map { "\($0)" } ?? "nil")
    let t2 = Tracked(6)
    o.w = t2                                          // swift_unknownObjectWeakAssign
    wa.w = t2
    out(s, "objc assign", "\(o.w?.description ?? "nil") \(wa.w.map { "\($0)" } ?? "nil")")
    withExtendedLifetime(keep) {}
    withExtendedLifetime(t2) {}
    out(s, "deinits", deinits)
}

func sec_unowned() {
    let s = "unowned"
    let b = Box(1)
    let ub = UnownedBox(u: b)
    exercise(s, "UnownedBox", { ub }, { UnownedBox(u: b) }, { $0.u.description })
    let t = Tracked(2)
    let uo = UnownedObjC(u: t)
    exercise(s, "UnownedObjC", { uo }, { UnownedObjC(u: t) }, { $0.u.description })
    let c = CPObjC(3)
    let uc = UnownedCP(u: c)
    exercise(s, "UnownedCP", { uc }, { UnownedCP(u: c) }, { $0.u.tag })
    withExtendedLifetime((b, t, c)) {}
    out(s, "deinits", deinits)
}

// ---------------------------------------------------------------- driver

let sections: [(String, () -> Void)] = [
    ("objc", sec_objc), ("swiftobj", sec_swiftobj), ("bridge", sec_bridge), ("native", sec_native),
    ("existential", sec_existential), ("tuple", sec_tuple), ("pod", sec_pod), ("memcpy", sec_memcpy),
    ("cvw", sec_cvw), ("enum", sec_enum), ("weak", sec_weak), ("unowned", sec_unowned),
]

// the types whose value witness tables each section reaches
let vwtypes: [String: [(String, Any.Type)]] = [
    "objc": [("Tracked", Tracked.self), ("NSMutableString", NSMutableString.self), ("AnyObject", AnyObject.self)],
    "swiftobj": [("Box", Box.self)],
    "bridge": [("[Tracked]", [Tracked].self), ("[String:Box]", [String: Box].self), ("Set<Int>", Set<Int>.self)],
    "native": [("Int", Int.self), ("UInt64", UInt64.self), ("Double", Double.self), ("Int8", Int8.self), ("Int16", Int16.self),
               ("Unicode.Scalar", Unicode.Scalar.self), ("Void", Void.self), ("RawPointer", UnsafeRawPointer.self)],
    "existential": [("Any", Any.self), ("CSC", (any CustomStringConvertible).self), ("CP", (any CP).self), ("Error", (any Error).self)],
    "tuple": [("(String,String,Box)", (String, String, Box).self), ("(Box,Int)", (Box, Int).self), ("(Int,Int8)", (Int, Int8).self),
              ("(Int,Int,Int,Int)", (Int, Int, Int, Int).self), ("(Tracked x4)", (Tracked, Tracked, Tracked, Tracked).self)],
    "pod": [("Pair<Int,Int8>", Pair<Int, Int8>.self), ("Range<Date>", Range<Date>.self), ("Pair<Big,Int8>", Pair<Big, Int8>.self), ("Pair<Box,Int>", Pair<Box, Int>.self)],
    "memcpy": [("UUID", UUID.self), ("Date", Date.self), ("DateInterval", DateInterval.self), ("Bool", Bool.self),
               ("Calendar.Component", Calendar.Component.self), ("Decimal", Decimal.self), ("Duration", Duration.self),
               ("CGPoint", CGPoint.self), ("Hasher", Hasher.self), ("ObjectIdentifier", ObjectIdentifier.self)],
    "cvw": [("String", String.self), ("Substring", Substring.self), ("Character", Character.self), ("Data", Data.self),
            ("URL", URL.self), ("TimeZone", TimeZone.self), ("Locale", Locale.self),
            ("PersonNameComponents", PersonNameComponents.self), ("Measurement", Measurement<UnitLength>.self)],
    "enum": [("MP<Tracked>", MP<Tracked>.self), ("MP<Box>", MP<Box>.self), ("MP<Int>", MP<Int>.self), ("Box??", Box??.self),
             ("Optional<Box>", Optional<Box>.self)],
    "weak": [("WeakCP", WeakCP.self), ("WeakAny", WeakAny.self)],
    "unowned": [("UnownedBox", UnownedBox.self), ("UnownedObjC", UnownedObjC.self), ("UnownedCP", UnownedCP.self)],
]

// the imports each section calls by name
let imports: [String: [String]] = [
    // the generic enum's own witnesses; swift_initEnumMetadataMultiPayload puts
    // swift_getMultiPayloadEnumTagSinglePayload/swift_storeMultiPayloadEnumTagSinglePayload in its table
    "enum": ["swift_initEnumMetadataMultiPayload", "swift_getEnumCaseMultiPayload", "swift_storeEnumTagMultiPayload"],
    "weak": ["swift_weakInit", "swift_weakLoadStrong", "swift_weakAssign", "swift_weakDestroy",
             "swift_unknownObjectWeakInit", "swift_unknownObjectWeakLoadStrong", "swift_unknownObjectWeakAssign",
             "swift_unknownObjectWeakDestroy", "swift_unknownObjectWeakCopyInit", "swift_unknownObjectWeakTakeInit",
             "swift_unknownObjectWeakCopyAssign", "swift_unknownObjectWeakTakeAssign"],
    "unowned": ["swift_unknownObjectUnownedInit", "swift_unknownObjectUnownedLoadStrong", "swift_unknownObjectUnownedDestroy",
                "swift_unknownObjectUnownedCopyInit", "swift_unknownObjectUnownedTakeInit", "swift_unknownObjectUnownedCopyAssign",
                "swift_unknownObjectUnownedTakeAssign", "swift_unownedRetainStrong", "swift_unownedRetain", "swift_unownedRelease"],
]

func strip(_ p: UnsafeRawPointer) -> UnsafeRawPointer? { UnsafeRawPointer(bitPattern: UInt(bitPattern: p) & 0x7fff_ffff_ffff) }

func vwdump(_ sec: String) {
    let slots = ["initializeBufferWithCopyOfBuffer", "destroy", "initializeWithCopy", "assignWithCopy",
                 "initializeWithTake", "assignWithTake", "getEnumTagSinglePayload", "storeEnumTagSinglePayload"]
    for (label, t) in vwtypes[sec] ?? [] {
        let md = unsafeBitCast(t, to: UnsafeRawPointer.self)
        guard let vwt = strip(md.load(fromByteOffset: -8, as: UnsafeRawPointer.self)) else { continue }
        for (i, slot) in slots.enumerated() {
            guard let f = strip(vwt.load(fromByteOffset: i * 8, as: UnsafeRawPointer.self)) else { continue }
            var di = Dl_info()
            var name = "?"
            if dladdr(f, &di) != 0, let sn = di.dli_sname {
                name = String(cString: sn)
                if di.dli_saddr != f { name += "+\(UInt(bitPattern: f) - UInt(bitPattern: di.dli_saddr))" }
            }
            let img = di.dli_fname.map { String(cString: $0) } ?? "?"
            print("\(sec)\t\(label)\t\(slot)\t\(img)\t_\(name)")
        }
    }
}

let args = CommandLine.arguments
if args.count == 2 && args[1] == "list" {
    print(sections.map { $0.0 }.joined(separator: " "))
} else if args.count == 3 && args[1] == "vw" {
    vwdump(args[2])
} else if args.count == 3 && args[1] == "syms" {
    for s in imports[args[2]] ?? [] { print(s) }
} else if args.count == 2, let f = sections.first(where: { $0.0 == args[1] }) {
    f.1()
    print("\(args[1]) end: deinits \(deinits)")
} else {
    print("usage: swiftvw list | SECTION | vw SECTION | syms SECTION")
    exit(2)
}
