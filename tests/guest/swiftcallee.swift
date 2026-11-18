// tests/guest/swiftcallee.sh: guest closures passed to native generic Swift functions return T
// through the native caller's indirect result buffer.
import SwiftCalleeHelper

struct Rec { var items: [Int]; var name: String; var n: Int }

@inline(never) func make(_ k: Int) -> Rec { Rec(items: Array(0..<k), name: "rec\(k)", n: k * 7) }

@main struct Main {
    static func main() {
        let k = Int(CommandLine.arguments.count) + 3
        let r = run { make(k) }
        print(r.items, r.name, r.n)
        let a = run { [k, k + 1, k + 2] }
        print(a)
        let s = run { "s\(k)" + String(repeating: "x", count: k) }
        print(s)
        let t = run { run { make(k + 1) } }
        print(t.name, t.items.count)
    }
}
