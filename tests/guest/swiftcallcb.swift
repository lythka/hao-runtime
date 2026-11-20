import SCBLib
// tests/guest/swiftcallcb.sh: native code calls guest witnesses and overrides whose values differ in their nil's
// encoding.
final class GuestSource: Source {
    func labels(_ k: Int) -> Pair? {
        switch k {
        case 0: return nil
        case 1: return (nil, nil)
        case 2: return ("g", nil)
        case 3: return (nil, "h")
        case 4: return ("guest-title-long-enough-to-be-heap-allocated", "g2")
        default: return ("c", "d")
        }
    }
    func show(_ v: Pair?) -> String {
        guard let v = v else { return "none" }
        return (v.title ?? "nil") + ":" + (v.subtitle ?? "nil")
    }
}
final class GuestBase: Base {
    override func labels(_ k: Int) -> Pair? { GuestSource().labels(k) }
    override func show(_ v: Pair?) -> String { "o" + GuestSource().show(v) }
}
@main struct Main {
    static func main() {
        let s = GuestSource()
        for k in 0..<6 { print(k, drive(s, k)) }
        let b = GuestBase()
        for k in 0..<6 { print("override", k, driveBase(b, k)) }
    }
}
