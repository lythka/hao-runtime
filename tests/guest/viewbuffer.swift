// A guest View's body copies a guest-defined value through an existential
// (Any) while SwiftUI evaluates it: the guest calls the value's own
// initializeBufferWithCopyOfBuffer witness, which with layout-string value
// witnesses is native code. A value past the inline buffer is boxed, and the
// copy shares the box (no value bytes move); one that fits inline is copied
// like initializeWithCopy. Each value holds a Data? and a String?, whose
// nils the walk must account for, and both are part of the body's result
// type. ImageRenderer evaluates the bodies with no window.
// tests/guest/viewbuffer.sh; output must equal native arm64.
import SwiftUI

enum Module {   // past the inline buffer: boxed in an existential
    case tile(String, Data?, Int, Int, Int, Int)
    case wide(String?, [Int], Double, Double, Double, Double, Double)
    case none
}
enum Small {   // fits the inline buffer
    case tag(Data?)
    case name(String?)
    case none
}
func shown(_ m: Module) -> String {
    switch m {
    case .tile(let s, let d, let a, _, _, let e): return "tile \(s) \(d?.count ?? -1) \(a) \(e)"
    case .wide(let s, let x, let a, _, _, _, let e): return "wide \(s ?? "nil") \(x.count) \(a) \(e)"
    case .none: return "none"
    }
}
func shown(_ m: Small) -> String {
    switch m {
    case .tag(let d): return "tag \(d?.count ?? -1)"
    case .name(let s): return "name \(s ?? "nil")"
    case .none: return "none"
    }
}
@inline(never) func copied(_ a: Any) -> Any { let b = a; return b }   // initializeBufferWithCopyOfBuffer

struct Tile: View {   // holds the values, so they are part of Row's body type
    let m: Module
    let s: Small
    var body: some View { Text(shown(m) + " / " + shown(s)) }
}
struct Row: View {
    let m: Module
    let s: Small
    var body: some View {
        let a = copied(m), b = copied(s)
        let t = Tile(m: a as! Module, s: b as! Small)
        print("body", shown(t.m), "/", shown(t.s))
        return HStack { t; Spacer() }.padding()
    }
}

@main struct Main {
    @MainActor static func main() {
        setbuf(stdout, nil)
        let cases: [(Module, Small)] = [
            (.tile("t", nil, 1, 2, 3, 4), .tag(nil)),
            (.tile("u", Data([1, 2]), 5, 6, 7, 8), .tag(Data([9]))),
            (.wide(nil, [1, 2, 3], 1.5, 0, 0, 0, 2.5), .name(nil)),
            (.wide("w", [], 0, 0, 0, 0, 3), .name("n")),
            (.none, .none),
        ]
        for (k, c) in cases.enumerated() {
            let r = ImageRenderer(content: Row(m: c.0, s: c.1).frame(width: 120, height: 30)).cgImage
            print("render", k, r?.width ?? -1)
        }
        print("done")
    }
}
