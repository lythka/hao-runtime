// Nested Optionals whose outer nil is a pointer extra inhabitant, written by guest code and handled by
// the native Optional value witnesses (tests/guest/optwitness.sh). Guest code stores such a nil inline
// with the arm64 encoding (inhabitant 1 is the value 1); generic code that is not specialized copies,
// destroys and tests it through the type's witnesses, which under hao are libswiftCore's x86_64
// Optional witnesses (inhabitant 1 is the value 2). A class's ivar destroyer reaches them for a lazy
// `(any P)?` property that was never read: its storage `(any P)??` holds the outer nil.
// Out of scope (docs/ISSUES.md): native generic code reading such a value itself (an Array element
// read through the native Array subscript, a dynamic cast out of `Any`).
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
        print("done")
    }
}
