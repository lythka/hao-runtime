// Extra inhabitants the Swift value walk certifies by ABI rule (src/swiftvalue/Recipe.c), seen through
// native code (tests/guest/svcarriers.sh). Generic code the optimizer does not specialize copies tuples
// through libswiftCore's native tuple witnesses; each tuple holds a `C??` whose outer nil guest code
// writes as arm64's 1 (x86_64's is 2), so hao walks the whole tuple, beside one of:
//   meta:    `M??`, M a struct whose most-inhabited field is an existential metatype with a witness table
//            (`any P.Type`): its nils are the metadata word's, k on arm64 and 2k on x86_64, converted too;
//   flag:    `F??`, F a struct whose inhabitants are its trailing Bool's byte values, the same on both;
//   mode:    `E??`, E a struct whose inhabitants are a payload-free enum's byte values;
//   toolbar: `ToolbarItemGroup<V>??`, SwiftUI's struct whose inhabitants are its isEmpty Bool's.
// Before these rules each tuple was refused (abort). Output must equal native arm64.
import SwiftUI

final class C { let n: Int; init(_ n: Int) { self.n = n } }
protocol P { static var tag: String { get } }
struct PA: P { static let tag = "PA" }
struct M { var t: any P.Type; var n: Int }
struct F { var n: Int; var on: Bool }
enum Mode { case a, b, c }
struct E { var n: Int; var m: Mode }
struct V: View { var body: some View { EmptyView() } }
typealias T = ToolbarItemGroup<V>

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<X>(_ v: X) -> X { let w = v; return w }

func shown<X>(_ v: X??, _ some: (X) -> String) -> String {
    switch v { case .none: return "nil"; case .some(.none): return "some(nil)"; case .some(.some(let x)): return "some(\(some(x)))" }
}
func shownC(_ v: C??) -> String { shown(v) { "\($0.n)" } }

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let cs: [C??] = [.none, .some(nil), .some(.some(C(3)))]
        let ms: [M??] = [.none, .some(nil), .some(M(t: PA.self, n: 4))]
        let fs: [F??] = [.none, .some(nil), .some(F(n: 5, on: true))]
        let es: [E??] = [.none, .some(nil), .some(E(n: 6, m: .c))]
        let ts: [T??] = [.none, .some(nil), .some(ToolbarItemGroup { V() })]
        for c in cs {
            for m in ms {
                let a = copied((c, m))
                print("meta", shownC(a.0), shown(a.1) { "\($0.t.tag) \($0.n)" })
            }
            for f in fs {
                let a = copied((c, f))
                print("flag", shownC(a.0), shown(a.1) { "\($0.n) \($0.on)" })
            }
            for e in es {
                let a = copied((c, e))
                print("mode", shownC(a.0), shown(a.1) { "\($0.n) \($0.m)" })
            }
            for t in ts {
                let a = copied((c, t))
                print("toolbar", shownC(a.0), shown(a.1) { _ in "group" })
            }
        }
    }
}
