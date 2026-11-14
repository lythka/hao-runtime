// Guest code specialized for a SwiftUI view type builds it inline and calls a
// native View modifier, which copies self with native value witnesses, and the
// guest later destroys it through that type's native witness. With an
// `if` around an `AnyView?` in a ViewBuilder the view holds `AnyView??`, whose
// outer nil is a pointer word's second extra inhabitant: 1 on arm64, 2 on
// x86_64 (the Bluetooth Settings pane's sheet(isPresented:)). A guest View
// struct holding the same field keeps its own encoding: its witnesses are
// guest code. tests/guest/viewselfnil.sh; output must equal native arm64.
import SwiftUI

struct G: View {
    var a: AnyView??
    var body: some View { HStack { a; Spacer() } }
}
@inline(never) func row(_ show: Bool, _ acc: AnyView?) -> some View {
    HStack { if show { acc }; Spacer() }.sheet(isPresented: .constant(false)) { Text("sheet") }
}
@inline(never) func guestSelf(_ a: AnyView??) -> some View {
    G(a: a).padding().sheet(isPresented: .constant(false)) { Text("s") }
}
@inline(never) func nested(_ c: Bool, _ show: Bool, _ acc: AnyView?) -> some View {
    VStack { if c { HStack { if show { acc }; Spacer() } } else { Text("t") } }
        .padding()
        .sheet(isPresented: .constant(false)) { Text("s") }
}
@inline(never) func size<V: View>(_ v: V) -> Int { MemoryLayout<V>.size }

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let av = AnyView(Text("a"))
        for (show, acc) in [(false, nil), (true, nil), (true, av)] as [(Bool, AnyView?)] {
            print("row", show, acc == nil ? "nil" : "some", size(row(show, acc)))
        }
        for a in [AnyView??.none, .some(nil), .some(av)] {
            print("guest", a == nil ? "nil" : a! == nil ? "some(nil)" : "some", size(guestSelf(a)))
        }
        for (c, show, acc) in [(true, false, nil), (true, true, nil), (true, true, av), (false, false, nil)] as [(Bool, Bool, AnyView?)] {
            print("nested", c, show, acc == nil ? "nil" : "some", size(nested(c, show, acc)))
        }
    }
}
