import Foundation
// hao's Swift value walk (src/swiftvalue/Recipe.c collect_empty_patches_detail, Common.c leaves) over
// native x86_64 values of the shape a published SwiftUI view value has: an enum whose empty cases
// live in a pointer's extra inhabitants (NSObject??, so hao walks the value) beside other Optionals.
// Prints one line per case: "walk", the patch count, or "refused" with the unresolved type.
final class Box { let n: Int; init(_ n: Int) { self.n = n } }
typealias Affected = NSObject??                         // two empty cases in an ObjC reference's extra inhabitants
struct WithAnyObject { var e: Affected; var object: AnyObject? }
struct WithNSObject { var e: Affected; var object: NSObject? }
struct WithNested { var e: Affected; var object: AnyObject?? }
enum Trend { case up, down }                            // no payload: Optional's nil is the byte 2
struct WithTrend { var e: Affected; var trend: Trend? }
struct WithURL { var e: Affected; var url: URL? }        // URL: one field, any _URLProtocol (class-bound, one witness table)

typealias Walk = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UInt32,
                                 UnsafeMutablePointer<UnsafeRawPointer?>) -> Int32
guard let lib = dlopen(CommandLine.arguments[1], RTLD_NOW), let sym = dlsym(lib, "hao_sv_collect_empty_patches_detail") else {
    print("no libhao walker"); exit(2)
}
let walk = unsafeBitCast(sym, to: Walk.self)
let patches = UnsafeMutableRawPointer.allocate(byteCount: 64 * 40, alignment: 8)   // empty_patch: 40 bytes

func run<T>(_ name: String, _ value: T) {
    var v = value
    var unresolved: UnsafeRawPointer? = nil
    let type = unsafeBitCast(T.self as Any.Type, to: UnsafeRawPointer.self)
    let n = withUnsafePointer(to: &v) { walk(type, UnsafeRawPointer($0), patches, 64, &unresolved) }
    if n >= 0 { print(name, "walk", n) }
    else { print(name, "refused", unresolved.map { String(describing: unsafeBitCast($0, to: Any.Type.self)) } ?? "?") }
}
for (label, e) in [("payload", Affected.some(.some(NSObject()))), ("inner nil", .some(nil)), ("outer nil", nil)] {
    run("AnyObject? nil, \(label)", WithAnyObject(e: e, object: nil))
    run("AnyObject? NSObject, \(label)", WithAnyObject(e: e, object: NSObject()))
    run("AnyObject? Swift object, \(label)", WithAnyObject(e: e, object: Box(2)))
    run("AnyObject? tagged NSNumber, \(label)", WithAnyObject(e: e, object: NSNumber(value: 5)))
    run("NSObject? (control), \(label)", WithNSObject(e: e, object: NSObject()))
    run("AnyObject??, \(label)", WithNested(e: e, object: .some(nil)))
    for (t, v) in [("nil", Trend?.none), ("up", .up), ("down", .down)] { run("Trend? \(t), \(label)", WithTrend(e: e, trend: v)) }
    run("URL? nil, \(label)", WithURL(e: e, url: nil))
    run("URL? live, \(label)", WithURL(e: e, url: URL(string: "https://www.apple.com/news")))
}

// A native-made Any whose value is out of line (a box) and holds a native nil, as native code makes it in
// storage hao destroys (swiftvalue.c storage_value): the walk of a value for native code certifies it, a
// box it may not rewrite holding the encoding native code reads. Before, any nil whose arm64 encoding
// differs refused the box, so such storage aborted at its destruction.
typealias Values = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UInt32,
                                   UnsafeMutablePointer<UnsafeRawPointer?>, Int32, UnsafeRawPointer?,
                                   UnsafeMutablePointer<UInt32>) -> Int32
if let vs = dlsym(lib, "hao_sv_collect_value_patches") {
    let values = unsafeBitCast(vs, to: Values.self)
    for (label, inner) in [("outer nil", Optional<Optional<Box>>.none), ("inner nil", .some(nil))] {
        var any: Any = (inner, 1, 2, 3)
        var unresolved: UnsafeRawPointer? = nil
        var passes: UInt32 = 0
        let n = withUnsafePointer(to: &any) {
            values(unsafeBitCast(Any.self, to: UnsafeRawPointer.self), UnsafeRawPointer($0), patches, 64, &unresolved, 1, nil, &passes)
        }
        print("boxed Any native \(label)", n >= 0 ? "walk \(n)" : "refused flags \(passes)")
    }
} else { print("boxed Any: no libhao value walk") }
