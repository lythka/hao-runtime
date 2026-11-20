// The Swift value walk's per-type facts (src/swiftvalue/Facts.h), from the fixture tests/guest/svfacts.facts
// (test switch HAO_SWIFT_WALK_FACTS), and the survey image's audit (tests/guest/svfacts.sh). Generic code
// the optimizer does not specialize copies tuples through libswiftCore's native tuple witnesses; each holds
// a `C??` whose outer nil guest code writes as arm64's 1 (x86_64's is 2) beside one other field.
//   same:    an InlineArray<2, Int>, a kind the rules have no rule for; its row says SAME: passed as found.
//   refuse:  an Int? the rules certify; its row says REFUSE: refused.
//   convert: an ObjC object; the `C??`'s own row (CONVERT) gives both encodings of its outer nil.
import Foundation

final class C { let n: Int; init(_ n: Int) { self.n = n } }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }

func shown(_ v: C??) -> String {
    switch v { case .none: return "nil"; case .some(.none): return "some(nil)"; case .some(.some(let c)): return "some(\(c.n))" }
}

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let o = NSObject()
        let nils: [C??] = [.none, .some(nil), .some(.some(C(3)))]
        let part = CommandLine.arguments.dropFirst().first ?? ""
        for v in nils {
            if part == "report" && v == nil { continue }
            switch part {
            case "same", "report": let w = copied((InlineArray<2, Int>(repeating: 4), v)); print("inline", w.0[1], shown(w.1))
            case "refuse": let f = copied((Int?.some(5), v)); print("int?", f.0 ?? 0, shown(f.1))
            default: let a = copied((o, v)); print("object", a.0 === o, shown(a.1))
            }
        }
    }
}
