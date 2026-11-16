// tests/guest/dynfunc.sh's guest: Swift `dynamic` (dynamically replaceable) functions. Each one's
// prologue calls swift_getFunctionReplacement(&link, itself) in the native runtime and tail-calls the
// result when it is not null; the runtime returns null when *link is the function's own address.
// Covered: free, method, Bool and stored-property accessors (get, set, modify), static, generic,
// throwing, subscript and async functions; an @objc dynamic Bool read by Foundation's KVC
// (_NSGetBoolValueWithMethod) and observed by KVO; a dynamic function passed to native code
// as a closure. Every line must equal native arm64.
import Foundation

dynamic func freeAnswer() -> Int { 41 + 1 }

dynamic func freeGeneric<T: CustomStringConvertible>(_ v: T) -> String { "<\(v.description)>" }

enum Failure: Error { case negative(Int) }
dynamic func checked(_ v: Int) throws -> Int {
    if v < 0 { throw Failure.negative(v) }
    return v * 2
}

dynamic func less(_ a: Int, _ b: Int) -> Bool { a < b }

dynamic func slowDouble(_ v: Int) async -> Int {
    await Task.yield()
    return v * 2
}

final class Plain {
    dynamic var stored = 5
    dynamic var flag: Bool { stored > 3 }
    dynamic func scaled(by k: Int) -> Int { stored * k }
    dynamic static func make() -> Plain { Plain() }
    dynamic subscript(i: Int) -> Int { stored + i }
}

struct Value {
    var base: Double
    dynamic func half() -> Double { base / 2 }
}

final class Model: NSObject {
    @objc dynamic var visible = false
    @objc dynamic var computedVisible: Bool { visible && count > 0 }
    @objc dynamic var count = 0
    @objc dynamic func describe() -> String { "count=\(count) visible=\(visible)" }
}

final class Watcher: NSObject {
    var seen: [String] = []
    override func observeValue(forKeyPath keyPath: String?, of object: Any?, change: [NSKeyValueChangeKey: Any]?,
                               context: UnsafeMutableRawPointer?) {
        seen.append("\(keyPath ?? "?")=\(change?[.newKey] ?? "nil")")
    }
}

@main struct Main {
    static func main() async {
        print("free", freeAnswer())
        print("generic", freeGeneric(7), freeGeneric("s"))
        do { print("throws", try checked(4)); _ = try checked(-1) } catch { print("caught", error) }
        print("sorted", [5, 3, 9, 1].sorted(by: less))
        print("async", await slowDouble(21))

        let p = Plain.make()
        print("plain", p.stored, p.flag, p.scaled(by: 3), p[10])
        p.stored = 1
        print("set", p.stored, p.flag)
        p.stored += 6   // the modify coroutine
        print("modify", p.stored, p.flag)
        for _ in 0..<1000 { p.stored = p.scaled(by: 1) + (p.flag ? 0 : 1) }
        print("loop", p.stored)
        print("struct", Value(base: 9).half())

        let m = Model()
        let w = Watcher()
        m.addObserver(w, forKeyPath: "visible", options: [.new], context: nil)
        print("kvc", m.value(forKey: "visible") as Any, m.value(forKey: "computedVisible") as Any)
        m.visible = true
        m.count = 2
        m.setValue(false, forKey: "visible")
        m.setValue(true, forKey: "visible")
        print("kvc", m.value(forKey: "visible") as Any, m.value(forKey: "computedVisible") as Any,
              m.value(forKeyPath: "count") as Any)
        print("msg", m.perform(#selector(Model.describe))?.takeUnretainedValue() as Any)
        m.removeObserver(w, forKeyPath: "visible")
        print("kvo", w.seen)
    }
}
