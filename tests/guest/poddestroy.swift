// Swift storage of trivially destroyed values that native code destroys last (tests/guest/poddestroy.sh):
// arrays of `UnsafePointer<Int8>?`, of a tuple holding one and of a struct of optional pointers, held in
// a native NSMutableArray past the guest's last reference, and a generic box of `UnsafePointer<Int8>?`
// that native dispatch blocks capture. Before the fix the walk at their final destruction refused the
// pointer's nil (abort); destroying them reads nothing. Output must equal native arm64.
import Foundation

struct Pair { var a: UnsafeRawPointer?; var b: UnsafeMutablePointer<Int8>? }
let held = NSMutableArray()

@inline(never) func hold(_ v: Any) { held.add(v) }
@inline(never) func drop(_ what: String) { print(what, "held", held.count); held.removeAllObjects() }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func boxed<T>(_ v: T, _ show: @escaping (T) -> String) {
    var p = v
    let g = DispatchGroup()
    DispatchQueue.global().async(group: g) { print("box", show(p)); p = v }
    g.wait()
    DispatchQueue.global().async(group: g) { print("box again", show(p)) }
    g.wait()
}

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let s = strdup("abc")!
        var names: [UnsafePointer<Int8>?] = [UnsafePointer(s), nil, UnsafePointer(s), nil]
        names.append(nil)
        print("names", names.count, names.filter { $0 == nil }.count)
        hold(names); names = []; drop("names")
        var tuples: [(UnsafePointer<Int8>?, Int)] = [(nil, 1), (UnsafePointer(s), 2)]
        print("tuples", tuples.count, tuples.filter { $0.0 == nil }.count)
        hold(tuples); tuples = []; drop("tuples")
        var pairs: [Pair?] = [Pair(a: UnsafeRawPointer(s), b: nil), nil, Pair(a: nil, b: s)]
        pairs.append(Pair(a: nil, b: nil))
        print("pairs", pairs.count, pairs.filter { $0?.a == nil }.count)
        hold(pairs); pairs = []; drop("pairs")
        boxed(Optional(UnsafePointer(s))) { $0 == nil ? "nil" : "ptr" }
        boxed(Optional<UnsafePointer<Int8>>.none) { $0 == nil ? "nil" : "ptr" }
        print("done")
        free(s)
    }
}
