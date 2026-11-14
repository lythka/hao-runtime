// A guest closure that native SwiftUI calls (View.overlay's content: () -> V) returns its V in place, the
// indirect result. With V = P?, P? itself an Optional over native frozen structs whose extra inhabitants come
// from an inner guest Optional (ControlCenter's CompactModuleSliderView on hovering Sound), the closure makes
// V's nil, or copies a P? nil into V, through libswiftCore's Optional witnesses. hao runs those for the guest
// and leaves their nils in the guest's encoding (arm64 extra inhabitant 1, where x86_64's is 2); returned to
// native code as they are, overlay's copy of V took the 1 for a live payload and retained it (swift_retain at
// 0x8). Generic code is kept unspecialized so the witnesses are called, as in ControlCenter.
// tests/guest/overlaynil.sh; output must equal native arm64.
import SwiftUI

final class Box { let n: Int; init(_ n: Int) { self.n = n } }
struct Leaf: View { let box: Box; var body: some View { EmptyView() } }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func tile<P: View>(_ p: P, _ hovered: Bool) -> some View {
    Color.clear.overlay(alignment: .center) { if hovered { p } }
}

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func render<P: View>(_ p: P, _ hovered: Bool) -> String {
    let v = AnyView(tile(p, hovered))
    return "\(MemoryLayout.size(ofValue: v)) \(String(describing: type(of: tile(p, hovered))).count)"
}

@inline(never) func placeholder(_ inner: Bool) -> some View {
    ZStack { if inner { Leaf(box: Box(7)) } }.padding(2).allowsHitTesting(false)
}

@main struct Main {
    static func main() {
        for hovered in [false, true] {
            for inner in [false, true] {
                // V = P?: P a SwiftUI struct, its nil an inner guest Optional's inhabitant (the control)
                print("plain", hovered, inner, render(placeholder(inner), hovered))
                // V = P??: P? an Optional (ControlCenter's shape), present or nil
                for present in [false, true] {
                    let p = present ? Optional(placeholder(inner)) : nil
                    print("optional", hovered, inner, present, render(p, hovered))
                }
            }
        }
    }
}
