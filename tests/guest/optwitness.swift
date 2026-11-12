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
        // A frozen native struct (CollectionOfOne) carries the nil: guest code reads it inline.
        print("CollectionOfOne generic nil", describe(noneOf(CollectionOfOne<AnyObject>?.self)) { "\($0.first === nil)" })
        print("done")
    }
}
