// A release build's closures have no symbols (strip -x). SwiftUI calls them
// later, from code with no exported symbol: the
// alert(_:isPresented:actions:message:) message closure, passed in a stack
// slot, and GeometryReader contents stored in values guest code built: two
// lent to a modifier as self (in a body, in main), one a body returns bare, one in a modifier's
// argument, one a guest View's @ViewBuilder property returns, one in a
// guest ViewModifier's body. Each returns its view in memory through rax (x8 on
// arm64); hao must hand the caller's buffer over, or SwiftUI reads an
// uninitialized one (swift_retain of a small address). ImageRenderer
// evaluates the bodies with no window. tests/guest/stripview.sh; output
// must equal native arm64.
import SwiftUI

final class Model: ObservableObject { @Published var show = false; @Published var items = [1, 2, 3] }
struct Alerting: View {
    @ObservedObject var m: Model
    var body: some View {
        VStack { Text("x") }
            .padding()
            .alert(Text("title"), isPresented: $m.show, actions: { Button("OK") {} }, message: { Text("msg") })
    }
}
struct Card<C: View>: View {
    let title: String
    @ViewBuilder let content: () -> C
    var body: some View { VStack { Text(title); content() } }
}
struct Measured: View {
    @ObservedObject var m: Model
    var body: some View {
        GeometryReader { g in
            Card(title: "c\(m.items.count)") { ForEach(m.items, id: \.self) { Text("\($0)") } }
                .overlay(alignment: .topLeading) { Text("w \(Int(g.size.width))") }
        }
        .sheet(isPresented: .constant(false)) { Text("s") }
    }
}
struct Bare: View {
    @ObservedObject var m: Model
    var body: some View { GeometryReader { g in Text("b \(Int(g.size.width)) \(m.items.count)") } }
}
struct Holder<C: View>: View {
    @ViewBuilder let content: () -> C
    var body: some View { content() }
}
struct Wrapped: View {
    @ObservedObject var m: Model
    var body: some View {
        Holder { GeometryReader { g in Text("h \(Int(g.size.height))") } }
            .background(GeometryReader { g in Color.clear.frame(width: g.size.width / 2) })
    }
}
struct Measure: ViewModifier {
    let n: Int
    func body(content: Content) -> some View { content.overlay(GeometryReader { g in Text("o \(Int(g.size.width)) \(n)") }) }
}
struct Modified: View {
    @ObservedObject var m: Model
    var body: some View { Text("m").modifier(Measure(n: m.items.count)) }
}
@main struct Main {
    @MainActor static func main() {
        setbuf(stdout, nil)
        let m = Model()
        for k in 0..<3 {
            if k == 1 { m.show = true; m.items.append(4) }
            if k == 2 { m.items = [] }
            let a = ImageRenderer(content: Alerting(m: m).frame(width: 50, height: 50)).cgImage
            let b = ImageRenderer(content: Measured(m: m).frame(width: 80, height: 120)).cgImage
            print("render", k, a?.width ?? -1, b?.height ?? -1)
            let c = ImageRenderer(content: Bare(m: m).frame(width: 60, height: 30)).cgImage
            let d = ImageRenderer(content: Wrapped(m: m).frame(width: 70, height: 40)).cgImage
            let e = ImageRenderer(content: Modified(m: m).frame(width: 90, height: 20)).cgImage
            let f = ImageRenderer(content: GeometryReader { g in Text("l \(Int(g.size.width))") }
                .sheet(isPresented: .constant(false)) { Text("s") }).cgImage
            print("more", k, c?.width ?? -1, d?.height ?? -1, e?.width ?? -1, f?.width ?? -1)
        }
        print("done")
    }
}
