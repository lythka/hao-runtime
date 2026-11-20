// A native protocol whose requirements a guest type implements: native code calls the guest's witnesses
// with a 32-byte Optional of a tuple of two String? as a result (x0-x3 / rax rdx rcx r8) and as an argument
// (x0-x3 / rdi rsi rdx rcx). The outer nil's word 1 is 1 on arm64 and 2 on x86_64.
public typealias Pair = (title: String?, subtitle: String?)
public protocol Source: AnyObject {
    func labels(_ k: Int) -> Pair?
    func show(_ v: Pair?) -> String
}
@inline(never) func fmt(_ v: Pair?) -> String {
    guard let v = v else { return "none" }
    return (v.title ?? "nil") + "/" + (v.subtitle ?? "nil")
}
@inline(never) func make(_ k: Int) -> Pair? {
    switch k {
    case 0: return nil
    case 1: return (nil, nil)
    case 2: return ("t", nil)
    case 3: return (nil, "s")
    case 4: return ("title-long-enough-to-be-heap-allocated", "s2")
    default: return ("a", "b")
    }
}
// Native code reads the guest's result and hands the guest a value it made.
@inline(never) public func drive(_ s: any Source, _ k: Int) -> String {
    "labels=" + fmt(s.labels(k)) + " show=" + s.show(make(k))
}
