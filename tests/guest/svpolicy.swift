// The Swift value boundary policy (src/swiftvalue.c decline, tests/guest/svpolicy.sh). Generic code the
// optimizer does not specialize copies tuples through libswiftCore's native tuple witnesses; each tuple
// holds a `C??` whose outer nil guest code writes as arm64's 1 (x86_64's is 2) beside another field.
//   pass:   the other field is in a pass class (an ObjC object: owner-opaque; a closure: identical
//           representation; a nil `Int?`: inactive payload): the walk certifies the tuple, the nil is
//           converted, the rest is passed as found.
//   refuse: the other field is live weak ObjC storage, which the walk does not certify: the value is
//           refused (abort). Under HAO_SWIFT_VALUE_PASSTHROUGH=1 it goes on, its nil still converted.
import Foundation

final class C { let n: Int; init(_ n: Int) { self.n = n } }
struct W { weak var o: NSObject? }

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
        if CommandLine.arguments.dropFirst().first == "refuse" {
            for v in nils {
                let w = copied((W(o: o), v))
                print("weak", w.0.o === o, shown(w.1))
            }
            return
        }
        for v in nils {
            let a = copied((o, v))
            print("object", a.0 === o, shown(a.1))
            let f = copied(({ (x: Int) in x + 1 }, v))
            print("closure", f.0(1), shown(f.1))
            let i = copied((Int?.none, v))
            print("int?", i.0 == nil, shown(i.1))
        }
    }
}
