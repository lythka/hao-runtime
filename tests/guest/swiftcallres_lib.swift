// A native class whose getter returns a 32-byte Optional of a tuple of two String? directly in four
// registers (x0-x3 / rax rdx rcx r8), and a function taking one in four argument registers
// (x0-x3 / rdi rsi rdx rcx). The outer nil's word 1 is 1 on arm64 and 2 on x86_64.
public typealias Pair = (title: String?, subtitle: String?)
open class Card {
    var k: Int
    var stored: Pair?
    public init(_ k: Int) { self.k = k }
    open var labels: Pair? {
        get {
            if k < 0 { return stored }
            switch k {
            case 0: return nil
            case 1: return (nil, nil)
            case 2: return ("t", nil)
            case 3: return (nil, "s")
            case 4: return ("title-long-enough-to-be-heap-allocated", "s2")
            default: return ("a", "b")
            }
        }
        set { stored = newValue; k = -1 }
    }
}
@inline(never) public func show(_ v: Pair?) -> String {
    guard let v = v else { return "none" }
    return (v.title ?? "nil") + "/" + (v.subtitle ?? "nil")
}
