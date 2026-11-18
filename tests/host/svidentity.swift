// A SAME row of the Swift value walk's facts applies only to the native type it names (Facts.c
// native_identity; tests/host/svidentity.sh). With a fixture SAME row for each type below (loaded through
// hao_sv_walk_facts_load, a test build only), the runtime's own metadata finds its row, and a copy of
// that metadata on the heap, the same name and layout, does not.
import CoreGraphics
import Foundation
import Synchronization

typealias Load = @convention(c) (UnsafePointer<CChar>) -> Int32
typealias FactOf = @convention(c) (UnsafeRawPointer, UnsafeMutableRawPointer) -> Int32

guard CommandLine.arguments.count > 1, let lib = dlopen(CommandLine.arguments[1], RTLD_NOW),
      let l = dlsym(lib, "hao_sv_walk_facts_load"), let f = dlsym(lib, "hao_sv_walk_fact_of") else { print("no libhao"); exit(2) }
let types: [(String, Any.Type)] = [
    ("Int", Int.self), ("String?", String?.self), ("[String: String]", [String: String].self),
    ("Set<Int32>", Set<Int32>.self), ("Result<Int, CocoaError>", Result<Int, CocoaError>.self),
    ("Dictionary<Int, String>", [Int: String].self), ("(title: String?, subtitle: String?)", (title: String?, subtitle: String?).self),
    ("AsyncStream.Continuation", AsyncStream<@Sendable () -> ()>.Continuation.self), ("NSObject", NSObject.self),
    ("CGImage?", CGImage?.self), ("Data", Data.self), ("os_unfair_lock_s", os_unfair_lock_s.self),
    ("(any Sendable).Type", (any Sendable).Type.self), ("Int.Type", Int.Type.self), ("@Sendable () -> ()", (@Sendable () -> ()).self),
    ("ManagedBuffer<Int, Int>", ManagedBuffer<Int, Int>.self), ("JSONEncoder", JSONEncoder.self),
]
let noncopyable: [(String, any ~Copyable.Type)] = [
    ("Mutex<Int>", Mutex<Int>.self), ("Mutex<ManagedBuffer<Int, Int>>", Mutex<ManagedBuffer<Int, Int>>.self),
]
typealias Mangled = @convention(thin) (UnsafeRawPointer) -> (UnsafePointer<UInt8>, Int)
let mangledName = unsafeBitCast(dlsym(UnsafeMutableRawPointer(bitPattern: -2), "swift_getMangledTypeName")!, to: Mangled.self)
func key(_ m: UnsafeRawPointer) -> String { let (p, n) = mangledName(m); return String(decoding: UnsafeBufferPointer(start: p, count: n), as: UTF8.self) }
let all = types.map { ($0.0, unsafeBitCast($0.1, to: UnsafeRawPointer.self)) } + noncopyable.map { ($0.0, unsafeBitCast($0.1, to: UnsafeRawPointer.self)) }
let path = NSTemporaryDirectory() + "svidentity-\(getpid()).facts"
try! all.map { "type \(key($0.1)) SAME\n" }.joined().write(toFile: path, atomically: true, encoding: .utf8)
let loaded = unsafeBitCast(l, to: Load.self)(path)
unlink(path)
guard loaded == 1 else { print("fixture not loaded (not a test build)"); exit(2) }
let of = unsafeBitCast(f, to: FactOf.self)
var out = [UInt8](repeating: 0, count: 64)
func row(_ m: UnsafeRawPointer) -> Int32 { out.withUnsafeMutableBytes { of(m, $0.baseAddress!) } }
for (name, m) in all {
    print("native", name, row(m))
    if name.hasPrefix("@") { continue }   // a function value is admitted whatever its signature
    let copy = UnsafeMutableRawPointer.allocate(byteCount: 8 * 17, alignment: 16)
    copy.copyMemory(from: m - 8, byteCount: 8 * 17)
    print("forged", name, row(UnsafeRawPointer(copy + 8)))
}
