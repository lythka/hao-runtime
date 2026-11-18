// A guest type spelled like a native one (tests/guest/svnoncopy.sh): this program's module is named
// Synchronization and defines its own noncopyable Mutex, so a box of its Mutex<Int> has the native type's
// name and table key. A SAME row of the native type must not admit it.
import Foundation

struct Mutex<Value: ~Copyable>: ~Copyable, @unchecked Sendable {
    let value: Value
    let tag: AnyObject?
    init(_ value: consuming Value) { self.value = value; self.tag = nil }
}

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func holder<T: Sendable>(_ v: T) -> () -> T {
    let m = Mutex(v)
    return { m.value }
}

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let f = holder(41)
        print("fake", f(), f())
    }
}
