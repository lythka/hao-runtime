// The Swift value boundary policy (src/swiftvalue.c, tests/guest/svpolicy.sh). Generic code the optimizer
// does not specialize copies tuples through libswiftCore's native tuple witnesses; most hold a `C??` whose
// outer nil guest code writes as arm64's 1 (x86_64's is 2).
//   pass:   beside a field of each pass class (an ObjC object: owner-opaque; a closure: identical
//           representation; a nil `Int?`: inactive payload) the tuple is certified and its nil converted;
//           an `Any` holding a `C??` nil is walked through its dynamic type and converted too.
//   refuse: beside an InlineArray (a metadata kind the walk has no rule for) the value is refused; under
//           HAO_SWIFT_VALUE_PASSTHROUGH=1 it goes on with the sibling nil still converted.
//   gate:   an InlineArray beside a class reference alone, no Optional anywhere: still examined, refused.
//   wide:   a 65-element tuple holding a `C??` nil, past the walk's bounds: refused, never passed unexamined.
//   result: an AsyncStream iterator's `next()`, a native generic result, of such a tuple: refused on the
//           way back to guest code.
//   frozen: a native frozen enum (no generic argument, reference payloads) the guest built, beside a
//           class reference: examined, never taken as native-made.
//   boxed:  an `Any` whose (out-of-line, boxed) value holds a `C??` nil, destroyed in a tuple: refused.
//   task:   a child task whose result is a `C??`: refused when the task is created (the runtime copies
//           the result natively before any boundary hao sees).
//   array:  an array of `C??` the guest fills with a nil and releases last: refused when its storage is
//           made (the runtime destroys its elements natively, with no boundary hao sees).
//   box:    a `C??` variable an escaping closure captures (a box): refused when the box is made.
//   anyarray, anycopy, anyassign, anybox: an [Any] holding a `C??` nil written by guest code, dropped,
//           copied (the array value) then dropped, or assigned over; a captured generic `Any` holding one
//           (a box): the runtime destroys the storage natively; hao converts the nils at that final
//           destruction, so each equals native arm64.
//   unreflected: a struct compiled without reflection metadata, holding an `Any.Type??`, beside a `C??`:
//           its fields are not known, so it is refused (zero reported fields are not an empty struct).
import Foundation
import SvpolicyLib

final class C: @unchecked Sendable { let n: Int; init(_ n: Int) { self.n = n } }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func dropped<T>(_ v: __owned T) -> Int { _ = consume v; return 1 }
@inline(never) func array(_ n: Int) -> Int {
    var a: [C??] = []
    for i in 0..<n { a.append(i % 2 == 0 ? .none : .some(nil)) }
    return a.count
}
// A generic captured variable: its box comes from swift_allocBox and the runtime destroys it.
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func captured<T>(_ v: T) -> () -> T { var x = v; let f = { x }; x = v; return f }
@inline(never) func box() -> String { shown(captured(C??.none)()) }
@inline(never) func anyArray(_ extra: Int) -> [Any] {
    var a: [Any] = []
    a.append(C??.none as Any)
    a.append(C??.some(nil) as Any)
    for i in 0..<extra { a.append(i) }
    return a
}
@inline(never) func anyShown(_ a: [Any]) -> String {
    a.map { v in (v as? C??).map { shown($0) } ?? "\(v)" }.joined(separator: ",")
}
@inline(never) func boxed() -> Any { (C??.none, 1, 2, 3) as (C??, Int, Int, Int) }

func shown(_ v: C??) -> String {
    switch v { case .none: return "nil"; case .some(.none): return "some(nil)"; case .some(.some(let c)): return "some(\(c.n))" }
}

// No array of C??: its storage is refused (the "array" part).
@inline(never) func nilAt(_ i: Int) -> C?? { i == 0 ? .none : i == 1 ? .some(nil) : .some(.some(C(3))) }

typealias Wide = (C??, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int)

@inline(never) func stream() async -> String {
    let (s, k) = AsyncStream.makeStream(of: (InlineArray<2, C>, C??).self)
    k.yield((InlineArray<2, C>(repeating: C(4)), .some(nil)))
    k.finish()
    var out: [String] = []
    for await r in s { out.append("\(r.0[1].n) \(shown(r.1))") }
    return out.joined(separator: ",")
}

@inline(never) func task() async -> String {
    await withTaskGroup(of: C??.self) { g in
        g.addTask { .none }
        var out: [String] = []
        while let r = await g.next() { out.append(shown(r)) }
        return out.joined(separator: ",")
    }
}

@main struct Main {
    static func main() async {
        setbuf(stdout, nil)
        let o = NSObject()
        switch CommandLine.arguments.dropFirst().first ?? "pass" {
        case "refuse":
            for i in 0..<3 {
                let v = nilAt(i)
                let w = copied((InlineArray<2, C>(repeating: C(7)), v))
                print("inline", w.0[1].n, shown(w.1))
            }
        case "gate":
            let w = copied((InlineArray<2, C>(repeating: C(7)), C(5)))
            print("gate", w.0[1].n, w.1.n)
        case "wide":
            let w: Wide = copied((C??.none, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63))
            print("wide", shown(w.0), w.64)
        case "result":
            print("result", await stream())
        case "frozen":
            let w = copied((_PlaygroundQuickLook.text("quick"), C(6)))
            if case .text(let t) = w.0 { print("frozen", t, w.1.n) }
        case "boxed":
            print("boxed", dropped((boxed(), C(7))))
        case "task":
            print("task", await task())
        case "array":
            print("array", array(3))
        case "box":
            print("box", box())
        case "anyarray":
            print("anyarray", anyShown(anyArray(2)))
        case "anycopy":
            let a = anyArray(1)
            let b = copied(a)
            print("anycopy", anyShown(b), a.count)
        case "anyassign":
            var a = anyArray(3)
            print("anyassign", anyShown(a))
            a = anyArray(0)
            print("anyassign", anyShown(a))
        case "anybox":
            print("anybox", shown(captured(C??.none as Any)() as! C??))
        case "unreflected":
            let w = copied((Unreflected(t: .none), C??.none))
            print("unreflected", w.0.t == nil, shown(w.1))
        default:
            for i in 0..<3 {
                let v = nilAt(i)
                let a = copied((o, v))
                print("object", a.0 === o, shown(a.1))
                let f = copied(({ (x: Int) in x + 1 }, v))
                print("closure", f.0(1), shown(f.1))
                let i = copied((Int?.none, v))
                print("int?", i.0 == nil, shown(i.1))
                let any: Any = v
                let e = copied((any, C(4)))
                print("any", shown(e.0 as! C??), e.1.n)
            }
        }
    }
}
