// Swift values whose copies and destroys native libswiftCore runs from layout strings (swift_cvw_* value
// witnesses) and whose enum tags come from guest tag functions the layout strings name by relative pointer:
// SinglePayloadEnumFN and MultiPayloadEnumFN in the layout strings of non-generic types (static metadata),
// in the one layout string of a generic type whose layout does not depend on its arguments (handed to
// swift_cvw_instantiateLayoutString by the type's metadata instantiation function), and copied as the
// *Resolved forms into the strings the runtime builds for generic types that hold such fields
// (swift_cvw_initStructMetadataWithLayoutString / swift_cvw_initEnumMetadata*WithLayoutString); and a
// Dictionary keyed by a guest Hashable type (_NativeDictionary calls the guest's hash and == witnesses).
// Built by tests/guest/swiftls.sh with layout-string value witnesses; output must equal native arm64.
//   swiftls [static|template|generic|dict|all]
import Foundation

final class Obj {
    let v: Int
    init(_ v: Int) { self.v = v }
    deinit { print("deinit \(v)") }
}

struct Inner { var o: Obj; var s: String; var n: Int8 }
enum Multi { case a(Obj), b(String), c(Int, Obj), d, e(Inner) }
enum Single { case some(Inner), none, other, more }
struct S1 { var m: Multi; var s: Single; var opt: Inner?; var tail: Int16 }

// a generic type whose layout does not depend on T: one layout string for all instantiations
enum Tagged<T> { case x(Obj), y(String), z(Inner), w }
struct Fixed<T> { var t: Tagged<T>; var m: Multi; var k: Int }

// generic types holding the enums next to T: strings built at instantiation from the fields' strings
struct Holder<T> { var t: T; var m: Multi; var s: Single }
enum Either<T> { case left(T), right(Multi), neither(Single) }

struct Key: Hashable {
    var name: String
    var tag: Single
    static func == (a: Key, b: Key) -> Bool { a.name == b.name }
    func hash(into h: inout Hasher) { h.combine(name) }
}

@inline(never) func copies<T>(_ x: T, _ n: Int) -> [T] { Array(repeating: x, count: n) }

func describe(_ m: Multi) -> String {
    switch m {
    case .a(let o): return "a\(o.v)"
    case .b(let s): return "b\(s)"
    case .c(let i, let o): return "c\(i),\(o.v)"
    case .d: return "d"
    case .e(let i): return "e\(i.o.v)\(i.s)\(i.n)"
    }
}
func describe(_ s: Single) -> String {
    switch s {
    case .some(let i): return "some\(i.o.v)\(i.s)"
    case .none: return "none"
    case .other: return "other"
    case .more: return "more"
    }
}
func describe<T>(_ t: Tagged<T>) -> String {
    switch t {
    case .x(let o): return "x\(o.v)"
    case .y(let s): return "y\(s)"
    case .z(let i): return "z\(i.o.v)"
    case .w: return "w"
    }
}

func runStatic() {
    var all: [S1] = []
    for k in 0..<5 {
        let inner = Inner(o: Obj(100 + k), s: "s\(k)-" + String(repeating: "x", count: 20), n: Int8(k))
        let ms: [Multi] = [.a(Obj(k)), .b("str\(k)" + String(repeating: "y", count: 20)), .c(k, Obj(10 + k)), .d, .e(inner)]
        let ss: [Single] = [.some(inner), .none, .other, .more]
        let v = S1(m: ms[k % ms.count], s: ss[k % ss.count], opt: k % 2 == 0 ? inner : nil, tail: Int16(k))
        all += copies(v, 3)
    }
    for v in all.prefix(15) where v.tail % 2 == 0 { print("static", describe(v.m), describe(v.s), v.opt?.o.v ?? -1, v.tail) }
    all.removeAll()
    print("static done")
}

@inline(never) func makeFixed<T>(_: T.Type, _ k: Int) -> [Fixed<T>] {
    let inner = Inner(o: Obj(300 + k), s: "fixed\(k)" + String(repeating: "z", count: 20), n: 7)
    let ts: [Tagged<T>] = [.x(Obj(200 + k)), .y("ty\(k)" + String(repeating: "q", count: 20)), .z(inner), .w]
    return copies(Fixed<T>(t: ts[k % 4], m: k % 2 == 0 ? .c(k, Obj(400 + k)) : .b("fb\(k)" + String(repeating: "r", count: 20)), k: k), 2)
}

func runTemplate() {
    var a = makeFixed(Int.self, 0) + makeFixed(Int.self, 1)
    var b = makeFixed(String.self, 2) + makeFixed(String.self, 3)
    var c = makeFixed(Double.self, 4)
    for f in a { print("template Int", describe(f.t), describe(f.m), f.k) }
    for f in b { print("template String", describe(f.t), describe(f.m), f.k) }
    for f in c { print("template Double", describe(f.t), describe(f.m), f.k) }
    a.removeAll(); b.removeAll(); c.removeAll()
    print("template done")
}

@inline(never) func holders<T>(_ t: T, _ k: Int) -> [Holder<T>] {
    let inner = Inner(o: Obj(500 + k), s: "h\(k)" + String(repeating: "h", count: 20), n: 1)
    let m: Multi = k % 3 == 0 ? .a(Obj(600 + k)) : k % 3 == 1 ? .e(inner) : .b("hb\(k)" + String(repeating: "b", count: 20))
    let s: Single = k % 2 == 0 ? .some(inner) : .other
    return copies(Holder(t: t, m: m, s: s), 2)
}
@inline(never) func eithers<T>(_ t: T, _ k: Int) -> [Either<T>] {
    let inner = Inner(o: Obj(700 + k), s: "e\(k)" + String(repeating: "e", count: 20), n: 2)
    let all: [Either<T>] = [.left(t), .right(.c(k, Obj(800 + k))), .neither(.some(inner)), .neither(.none)]
    return copies(all[k % 4], 2)
}

func runGeneric() {
    var h1 = holders(Obj(900), 0) + holders(Obj(901), 1)
    var h2 = holders("gen" + String(repeating: "g", count: 20), 2)
    var h3 = holders(42, 3)
    for h in h1 { print("generic Obj", h.t.v, describe(h.m), describe(h.s)) }
    for h in h2 { print("generic String", h.t.count, describe(h.m), describe(h.s)) }
    for h in h3 { print("generic Int", h.t, describe(h.m), describe(h.s)) }
    var e = eithers(Obj(950), 0) + eithers(Obj(951), 1) + eithers(Obj(952), 2) + eithers(Obj(953), 3)
    for x in e {
        switch x {
        case .left(let o): print("either left", o.v)
        case .right(let m): print("either right", describe(m))
        case .neither(let s): print("either neither", describe(s))
        }
    }
    h1.removeAll(); h2.removeAll(); h3.removeAll(); e.removeAll()
    print("generic done")
}

func runDict() {
    var d: [Key: Multi] = [:]
    for k in 0..<200 {
        let inner = Inner(o: Obj(1000 + k), s: "d", n: 3)
        d[Key(name: "key\(k)", tag: k % 2 == 0 ? .some(inner) : .none)] = k % 2 == 0 ? .c(k, Obj(2000 + k)) : .d
    }
    var sum = 0
    for k in stride(from: 0, to: 200, by: 7) { if case .c(let i, _)? = d[Key(name: "key\(k)", tag: .other)] { sum += i } }
    print("dict count", d.count, "sum", sum)
    d.removeAll()
    print("dict done")
}

@_silgen_name("swiftls_filled") func swiftlsFilled() -> Int32   // tests/guest/swiftlsfill.c (links it)
_ = swiftlsFilled()

let which = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "all"
if which == "all" || which == "static" { runStatic() }
if which == "all" || which == "template" { runTemplate() }
if which == "all" || which == "generic" { runGeneric() }
if which == "all" || which == "dict" { runDict() }
