// tests/guest/svarchdiff.sh: Swift.Float16 is declared differently by architecture (libswiftCore's
// x86_64 Float16 has no stored field; src/archdiff_gen.inc) and frozen, so guest code lays it out: a
// guest struct with a Float16 field must be refused by the walker, one with an Int16 there walked.
// Value lines are compared with native arm64: Float16 arithmetic and layout as guest code compiles
// them, and generic sizes, copies, Optionals, collections, tuples and casts, which go through
// libswiftCore's x86_64 Float16 metadata (a stub of size 0, which hao gives a 2-byte POD's witnesses:
// src/swift.c). The "generic: copy" line prints through Float16's conformances, which x86_64
// libswiftCore lacks (docs/ISSUES.md); it is printed, not compared. Walk and metadata lines come only
// from hao.
import Foundation
struct Half { var h: Float16; var c: NSObject?? }
struct Short { var h: Int16; var c: NSObject?? }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func sizeOf<T>(_: T.Type) -> Int { MemoryLayout<T>.size }
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func layout<T>(_: T.Type) -> [Int] { [MemoryLayout<T>.size, MemoryLayout<T>.stride, MemoryLayout<T>.alignment] }

let x: Float16 = 1.5, y: Float16 = 2.25
print("value", Float(x * y), Float(x + y), MemoryLayout<Float16>.size, MemoryLayout<Half>.size, MemoryLayout<Half>.offset(of: \Half.c)!)
print("value generic size", sizeOf(Float16.self), sizeOf(Half.self), layout(Float16.self), layout(Float16?.self),
      layout((Float16, Int8, Float16).self), layout([Float16].self))
print("generic: copy", copied(x), copied([x, y]), copied(Half(h: y, c: nil)).h)
print("value generic copy", Float(copied(x)), copied([x, y]).map { Float($0) }, Float(copied(Half(h: y, c: nil)).h))
let some: Float16? = copied(Optional(-y)), none: Float16? = copied(nil)
print("value generic optional", some.map { Float($0) } as Any, none == nil, copied([some, none, x]).map { $0.map { Float($0) } as Any })
var table: [Int: Float16] = [:]
for i in 0..<20 { table[i] = Float16(i) / 4 }
print("value generic dictionary", (0..<20).map { Float(copied(table)[$0]!) })
let t = copied((x, Int8(7), -y))
let boxed: Any = copied(x)
print("value generic tuple", Float(t.0), t.1, Float(t.2), "cast", Float((boxed as? Float16) ?? 0), boxed is Int16)

if CommandLine.arguments.count > 1 {
    typealias Walk = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UInt32,
                                     UnsafeMutablePointer<UnsafeRawPointer?>) -> Int32
    typealias Listed = @convention(c) (UnsafeRawPointer) -> Int32
    let lib = dlopen(CommandLine.arguments[1], RTLD_NOW)!
    let walk = unsafeBitCast(dlsym(lib, "hao_sv_collect_empty_patches_detail")!, to: Walk.self)
    let listed = dlsym(lib, "hao_sv_archdiff_type").map { unsafeBitCast($0, to: Listed.self) }
    let patches = UnsafeMutableRawPointer.allocate(byteCount: 64 * 40, alignment: 8)
    let f16 = unsafeBitCast(Float16.self, to: UnsafeRawPointer.self)
    let vwt = f16.load(fromByteOffset: -8, as: UnsafeRawPointer.self)
    print("hao Float16 metadata size", vwt.load(fromByteOffset: 64, as: UInt64.self), "listed", listed.map { $0(f16) != 0 ? "yes" : "no" } ?? "unknown")
    func w<T>(_ label: String, _ v: T) {
        var copy = v
        var unresolved: UnsafeRawPointer? = nil
        let n = withUnsafePointer(to: &copy) { walk(unsafeBitCast(T.self, to: UnsafeRawPointer.self), UnsafeRawPointer($0), patches, 64, &unresolved) }
        print("hao walk", label, n < 0 ? "refused \(unresolved.map { _typeName(unsafeBitCast($0, to: Any.Type.self)) } ?? "?")" : "\(n)")
    }
    w("Half", Half(h: x, c: nil))
    w("Short", Short(h: 3, c: nil))
    w("Float16", x)
    w("Optional<Half>", Half?.none)
}
