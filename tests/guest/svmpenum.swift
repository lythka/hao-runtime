// A runtime-laid-out guest multi-payload enum with several empty cases inside a guest struct, handed by guest
// generic code to native Optional and Array value witnesses (tests/guest/svmpenum.sh). One payload is a
// resilient native struct, so the runtime lays the enum out (an extra tag byte, xi 253) and its witnesses are
// native; the struct's first field is a String, whose inhabitants are the Optional's nil.
//   state: every case of View, with and without the Optional, and an Array of them.
import Foundation

enum View { case failed(PersonNameComponents), loaded(places: [String]), idle, loading, empty }
enum Focus { case none, field, list }
struct State { var title: String; var view: View; var focus: Focus; var count: Int?; var note: String? }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }

func person(_ s: String) -> PersonNameComponents { var p = PersonNameComponents(); p.givenName = s; return p }
func shown(_ v: View) -> String {
    switch v { case .failed(let p): return "failed(\(p.givenName!))"; case .loaded(let l): return "loaded(\(l))"
    case .idle: return "idle"; case .loading: return "loading"; case .empty: return "empty" }
}
func shown(_ s: State) -> String { "\(s.title) \(shown(s.view)) \(s.focus) \(s.count.map(String.init) ?? "nil") \(s.note ?? "nil")" }

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let views: [View] = [.failed(person("a")), .loaded(places: ["x", "y"]), .idle, .loading, .empty]
        var all: [State] = []
        for (i, v) in views.enumerated() {
            let s = State(title: "t\(i)", view: v, focus: i % 2 == 0 ? .field : .none, count: i % 2 == 0 ? nil : i,
                          note: i == 1 ? "n" : nil)
            all.append(s)
            print(shown(copied(s)), copied(Optional(s)).map(shown) ?? "nil", copied(State?.none) == nil)
        }
        print(copied(all).map(shown))
    }
}
