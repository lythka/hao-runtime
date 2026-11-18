// A release build's closures have no symbols (strip -x). SwiftUI calls them
// from code with no exported symbol and through no entry hao made: the
// alert(_:isPresented:actions:message:) message closure, passed in a stack
// slot, and a GeometryReader's content, stored in a value guest code built.
// Each returns its view in memory through rax (x8 on arm64); hao must hand
// the caller's buffer over, or SwiftUI reads an uninitialized one
// (swift_retain of a small address). ImageRenderer evaluates the bodies
// with no window. tests/guest/stripview.sh; output must equal native arm64.
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
        }
        print("done")
    }
}
