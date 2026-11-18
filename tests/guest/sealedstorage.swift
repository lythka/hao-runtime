// Array storage of Any holding native Optionals whose nil differs between arm64 and x86_64 (Data?, Obj??),
// its _ContiguousArrayStorage<Any> metadata in the guest image's sealed __DATA_CONST (sealedstorage.s, as
// compiler-prespecialized metadata would be), made by guest code and destroyed by native code.
import Foundation

final class Obj { let n: Int; init(_ n: Int) { self.n = n }; deinit { print("deinit obj\(n)") } }

@_silgen_name("sealed_storage_metadata") func sealedMetadata() -> UnsafeRawPointer
@_silgen_name("sealed_storage_alloc") func allocObject(_ m: UnsafeRawPointer, _ size: Int, _ align: Int) -> UnsafeMutableRawPointer

func describe(_ v: Any) -> String {
    if type(of: v) == Data?.self, let d = v as? Data? { return d.map { "data\($0.count)" } ?? "data-nil" }
    if type(of: v) == Obj??.self, let o = v as? Obj?? { return o.map { $0.map { "obj\($0.n)" } ?? "inner-nil" } ?? "nil" }
    return "\(v)"
}
@inline(never) func make(_ items: [Any]) -> [Any] {
    let p = allocObject(sealedMetadata(), 32 + items.count * MemoryLayout<Any>.stride, 7)
    p.storeBytes(of: items.count, toByteOffset: 16, as: Int.self)
    p.storeBytes(of: items.count << 1, toByteOffset: 24, as: Int.self)
    (p + 32).bindMemory(to: Any.self, capacity: items.count).initialize(from: items, count: items.count)
    return unsafeBitCast(Unmanaged<AnyObject>.fromOpaque(p).takeRetainedValue(), to: [Any].self)
}
let held = NSMutableArray()

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        for round in 1...3 {
            var a = make([Data?.none as Any, Data([9]) as Data? as Any, Obj??.none as Any, Obj??.some(nil) as Any,
                          Obj??.some(Obj(round)) as Any, round])
            print(round, a.map(describe).joined(separator: ", "))
            if round == 2 { held.add(a); a = []; print("held", held.count); held.removeAllObjects() }
            else { a = [] }
            print(round, "dropped", a.count)
        }
        print("done")
    }
}
