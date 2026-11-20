import GPLib
import CoreGraphics
// tests/guest/genpair.sh: guest code specialized for the concrete types reads native-made nils inline.
final class C { let n: Int; init(_ n: Int) { self.n = n } }
enum GuestGeneric<T> { case loading(Int), loaded(T), failed(any Error), none }
@inline(never) func show(_ v: C??) -> String {
    switch v { case .none: return "none"; case .some(.none): return "some(nil)"; case .some(.some(let c)): return "c\(c.n)" }
}
@inline(never) func show(_ v: CGImage??) -> String {
    switch v { case .none: return "none"; case .some(.none): return "some(nil)"; case .some(.some(let i)): return "image \(i.width)" }
}
@inline(never) func show<T>(_ g: GuestGeneric<T>) -> String {
    switch g { case .loading(let k): return "loading \(k)"; case .loaded: return "loaded"; case .failed: return "failed"; case .none: return "none" }
}
@main struct Main {
    static func main() {
        let a = emptyPair(GuestGeneric<C>.loaded(C(7)), C?.self)
        print("empty", show(a.0), show(a.1))
        let b = fullPair(C?.none, GuestGeneric<C>.none)
        print("full-nil", show(b.0), show(b.1))
        let d = fullPair(C?.some(C(3)), GuestGeneric<C>.loading(4))
        print("full", show(d.0), show(d.1))
        print("one", show(emptyOne(C?.self)))
        let ctx = CGContext(data: nil, width: 5, height: 3, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        let image = ctx.makeImage()!
        let e = emptyPair(GuestGeneric<CGImage>.loaded(image), CGImage?.self)
        print("image-empty", show(e.0), show(e.1))
        let f = fullPair(CGImage?.some(image), GuestGeneric<CGImage>.loading(9))
        print("image-full", show(f.0), show(f.1))
    }
}
