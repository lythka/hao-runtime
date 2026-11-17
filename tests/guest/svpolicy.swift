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
//   result: a task group's `next()`, a native generic result, of such a tuple: refused on the way back.
import Foundation

final class C: @unchecked Sendable { let n: Int; init(_ n: Int) { self.n = n } }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }

func shown(_ v: C??) -> String {
    switch v { case .none: return "nil"; case .some(.none): return "some(nil)"; case .some(.some(let c)): return "some(\(c.n))" }
}

// A global: native code destroying the array would read its nils itself (out of scope, docs/ISSUES.md).
let nils: [C??] = [.none, .some(nil), .some(.some(C(3)))]

typealias Wide = (C??, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int)

@inline(never) func group() async -> String {
    await withTaskGroup(of: (InlineArray<2, Int>, C??).self) { g in
        g.addTask { (InlineArray<2, Int>(repeating: 4), .some(nil)) }
        var out: [String] = []
        while let r = await g.next() { out.append("\(r.0[1]) \(shown(r.1))") }
        return out.joined(separator: ",")
    }
}

@main struct Main {
    static func main() async {
        setbuf(stdout, nil)
        let o = NSObject()
        switch CommandLine.arguments.dropFirst().first ?? "pass" {
        case "refuse":
            for v in nils {
                let w = copied((InlineArray<2, Int>(repeating: 7), v))
                print("inline", w.0[1], shown(w.1))
            }
        case "gate":
            let w = copied((InlineArray<2, Int>(repeating: 7), C(5)))
            print("gate", w.0[1], w.1.n)
        case "wide":
            let w: Wide = copied((C??.none, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63))
            print("wide", shown(w.0), w.64)
        case "result":
            print("result", await group())
        default:
            for v in nils {
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
