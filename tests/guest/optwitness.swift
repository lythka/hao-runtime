// Nested Optionals whose outer nil is a pointer extra inhabitant, written by guest code and handled by
// the native Optional value witnesses (tests/guest/optwitness.sh). Guest code stores such a nil inline
// with the arm64 encoding (inhabitant 1 is the value 1); generic code that is not specialized copies,
// destroys and tests it through the type's witnesses, which under hao are libswiftCore's x86_64
// Optional witnesses (inhabitant 1 is the value 2). A class's ivar destroyer reaches them for a lazy
// `(any P)?` property that was never read: its storage `(any P)??` holds the outer nil.
// A nil made that way inside a resilient native struct (OSSignposter? in a guest struct's first field)
// keeps the native encoding: no guest code can read it inline, and native witnesses read it back.
// Out of scope (docs/ISSUES.md): native generic code reading such a value itself (an Array element
// read through the native Array subscript, a dynamic cast out of `Any`).
import os

protocol P { func f() -> Int }
protocol Q: AnyObject { func g() -> Int }
final class C: Q { let n: Int; init(_ n: Int) { self.n = n }; func g() -> Int { n } }
struct S: P { var a = [1, 2, 3]; func f() -> Int { a.count } }

// Generic code the optimizer must not specialize: every operation goes through T's witnesses.
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func dropped<T>(_ v: __owned T) -> Int { _ = consume v; return 1 }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func isSome<T>(_ v: T?) -> Bool { if case .some = v { return true }; return false }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func noneOf<T>(_: T.Type) -> T? { nil }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func someOf<T>(_ v: T) -> T? { v }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func assigned<T>(_ a: T, _ b: T) -> T { var x = a; x = b; return x }

func describe<T>(_ v: T??, _ inner: (T) -> String) -> String {
    switch v { case .none: return "nil"; case .some(.none): return "some(nil)"; case .some(.some(let x)): return "some(some(\(inner(x))))" }
}

func run<T>(_ name: String, _ values: [T??], _ inner: @escaping (T) -> String) {
    for (k, v) in values.enumerated() {
        let d = { (x: T??) in describe(x, inner) }
        print(name, k, "copy", d(copied(v)))
        print(name, k, "drop", dropped(v))
        print(name, k, "outer some", isSome(v))
        print(name, k, "inner some", { if case .some(let i) = v { return isSome(i) }; return false }())
        print(name, k, "assign", d(assigned(values[(k + 1) % values.count], v)))
    }
    print(name, "generic nil", describe(noneOf(T?.self), inner))
    print(name, "generic some(nil)", describe(someOf(T?.none), inner))
}

// A guest struct whose extra inhabitants come from a resilient native struct's Optional (its first field).
struct Interval { var signposter: OSSignposter?; var name: StaticString?; var state: OSSignpostIntervalState? }

func intervals() {
    let sp = OSSignposter(subsystem: "hao.test", category: "optwitness")
    let st = sp.beginInterval("iv")
    let shown = { (v: Interval?) -> String in
        guard let i = v else { return "nil" }
        return "some(\(i.signposter != nil) \(i.name.map { "\($0)" } ?? "nil") \(i.state != nil))"
    }
    let values: [Interval?] = [nil, Interval(signposter: sp, name: "iv", state: st), Interval(signposter: nil, name: nil, state: nil)]
    for (k, v) in values.enumerated() {
        print("Interval", k, "copy", shown(copied(v)), "some", isSome(v), "drop", dropped(v))
        print("Interval", k, "assign", shown(assigned(values[(k + 1) % values.count], v)))
    }
    print("Interval generic nil", shown(copied(noneOf(Interval.self))), isSome(noneOf(Interval.self)))
    sp.endInterval("iv", st)
}

// A guest enum with a trailing tag byte (String / (String, String) / one empty case: the byte is the
// case index) beside a nested Optional in a tuple. The tuple's native witnesses take the whole tuple,
// so its walk has to get through the enum, by its layout (its witnesses are arm64 code), to give the
// Optional's outer nil the native encoding.
enum Text { case one(String), two(String, String), none }
func texts() {
    let shown = { (t: Text) -> String in
        switch t { case .one(let a): return "one(\(a))"; case .two(let a, let b): return "two(\(a), \(b))"; case .none: return "none" }
    }
    let d = { (x: (Text, C??)) in "\(shown(x.0)) \(describe(x.1) { "\($0.n)" })" }
    let values: [(Text, C??)] = [(.one("Cupertino"), nil), (.two("12:00", String(repeating: "x", count: 40)), .some(nil)),
                                 (.none, nil), (.one("x"), .some(.some(C(9)))), (.two("a", "b"), nil)]
    for (k, v) in values.enumerated() {
        print("Text", k, "copy", d(copied(v)), "drop", dropped(v))
        print("Text", k, "assign", d(assigned(values[(k + 1) % values.count], v)))
    }
}

// The same with several empty cases: they share the tag after the cases and keep their index in the
// payload's first bytes (String payloads with two, Int payloads with five).
enum Text2 { case one(String), two(String, String), none, other }
enum Count5 { case one(Int), two(Int, Int), e0, e1, e2, e3, e4 }
func emptyCases() {
    let s2 = { (t: Text2) -> String in
        switch t { case .one(let a): return "one(\(a))"; case .two(let a, let b): return "two(\(a), \(b))"
                   case .none: return "none"; case .other: return "other" }
    }
    let s5 = { (t: Count5) -> String in
        switch t { case .one(let a): return "one(\(a))"; case .two(let a, let b): return "two(\(a), \(b))"
                   case .e0: return "e0"; case .e1: return "e1"; case .e2: return "e2"; case .e3: return "e3"; case .e4: return "e4" }
    }
    let d2 = { (x: (Text2, C??)) in "\(s2(x.0)) \(describe(x.1) { "\($0.n)" })" }
    let d5 = { (x: (Count5, C??)) in "\(s5(x.0)) \(describe(x.1) { "\($0.n)" })" }
    let v2: [(Text2, C??)] = [(.one("Cupertino"), nil), (.none, .some(nil)), (.other, nil),
                              (.two("a", String(repeating: "y", count: 40)), .some(.some(C(3)))), (.other, .some(nil))]
    for (k, v) in v2.enumerated() {
        print("Text2", k, "copy", d2(copied(v)), "drop", dropped(v))
        print("Text2", k, "assign", d2(assigned(v2[(k + 1) % v2.count], v)))
    }
    let v5: [(Count5, C??)] = [(.e0, nil), (.one(7), .some(nil)), (.e4, nil), (.two(1, 2), nil), (.e2, .some(.some(C(8)))),
                               (.e3, .some(nil)), (.e1, nil)]
    for (k, v) in v5.enumerated() {
        print("Count5", k, "copy", d5(copied(v)), "drop", dropped(v))
        print("Count5", k, "assign", d5(assigned(v5[(k + 1) % v5.count], v)))
    }
}

// Guest enums whose tag lies in their payloads' spare bits (arm64 layout, arm64 witnesses), beside a
// C?? in a tuple the native tuple witnesses take whole: the walk decodes the case from the image's
// layout record. Pointer tags with an empty case, tags in a Bool's spare bits with empty cases, and a
// 17-case enum (its tag needs bit 2). Inlined, so that the arrays are destroyed by code specialized
// for their element (native code destroying them would read the guest's nils: out of scope).
struct IB { var i: Int; var b: Bool }
enum GPtr { case a(C), b(C), none }
enum GLow { case a(IB), b(IB), c(IB), e0, e1 }
enum G17 { case c0(C), c1(C), c2(C), c3(C), c4(C), c5(C), c6(C), c7(C), c8(C)
           case c9(C), c10(C), c11(C), c12(C), c13(C), c14(C), c15(C), c16(C) }
@inline(__always) func run2<E>(_ name: String, _ values: [(E, C??)], _ shown: @escaping (E) -> String) {
    let d = { (x: (E, C??)) in "\(shown(x.0)) \(describe(x.1) { "\($0.n)" })" }
    for (k, v) in values.enumerated() {
        print(name, k, "copy", d(copied(v)), "drop", dropped(v))
        print(name, k, "assign", d(assigned(values[(k + 1) % values.count], v)))
    }
}
func spareCases() {
    run2("GPtr", [(GPtr.a(C(1)), nil), (.b(C(2)), .some(nil)), (.none, nil), (.none, .some(.some(C(3))))]) {
        switch $0 { case .a(let c): return "a(\(c.n))"; case .b(let c): return "b(\(c.n))"; case .none: return "none" }
    }
    run2("GLow", [(GLow.a(IB(i: 1, b: true)), nil), (.c(IB(i: 3, b: false)), .some(nil)), (.e0, nil), (.e1, .some(nil)),
                  (.b(IB(i: 2, b: true)), .some(.some(C(4))))]) {
        switch $0 { case .a(let x): return "a(\(x.i) \(x.b))"; case .b(let x): return "b(\(x.i) \(x.b))"
                    case .c(let x): return "c(\(x.i) \(x.b))"; case .e0: return "e0"; case .e1: return "e1" }
    }
    let n17 = { (g: G17) -> String in
        switch g { case .c0(let c), .c1(let c), .c2(let c), .c3(let c), .c4(let c), .c5(let c), .c6(let c), .c7(let c),
                        .c8(let c), .c9(let c), .c10(let c), .c11(let c), .c12(let c), .c13(let c), .c14(let c),
                        .c15(let c), .c16(let c): return "\(String(describing: g).split(separator: "(")[0]) \(c.n)" }
    }
    run2("G17", [(G17.c0(C(5)), nil), (.c16(C(6)), .some(nil)), (.c9(C(7)), nil), (.c4(C(8)), .some(.some(C(9))))], n17)
}

final class Holder {
    lazy var store: (any P)? = nil
    lazy var other: (any P)? = S()
    var tag = 0
    deinit { print("Holder deinit", tag) }
}

@main struct Main {
    static func main() {
        run("P", [(any P)??.none, .some(nil), .some(.some(S()))]) { "\($0.f())" }
        run("Q", [(any Q)??.none, .some(nil), .some(.some(C(4)))]) { "\($0.g())" }
        run("C", [C??.none, .some(nil), .some(.some(C(5)))]) { "\($0.n)" }
        run("AnyObject", [AnyObject??.none, .some(nil), .some(.some(C(6)))]) { "\(($0 as! C).n)" }
        run("[Int]", [[Int]??.none, .some(nil), .some(.some([7, 8]))]) { "\($0)" }
        // A lazy existential property that was never read, destroyed with its object.
        for k in 0..<3 {
            let h = Holder()
            h.tag = k
            if k == 1 { print("read", h.store == nil) }
            if k == 2 { print("read", h.other?.f() ?? -1) }
            print("holder kept", copied(h) === h)
        }
        intervals()
        texts()
        emptyCases()
        spareCases()
        // A frozen native struct (CollectionOfOne) carries the nil: guest code reads it inline.
        print("CollectionOfOne generic nil", describe(noneOf(CollectionOfOne<AnyObject>?.self)) { "\($0.first === nil)" })
        print("done")
    }
}
