// Narrow results of Swift runtime entry points that Swift's IRGen calls with
// the C convention and a zeroext result (include/swift/Runtime/RuntimeFunctions.def):
// compiled callers use all 32 bits (`eor w1, w0, #1`, `cmp w0, #0`), while the
// x86_64 runtime writes only %al. tests/guest/narrowret.sh runs this natively
// (arm64) and under hao (arm64e) and compares the output.
import Foundation
import IOKit

setvbuf(stdout, nil, _IOLBF, 0)

final class Box { var v = 0 }

// Unspecialized, as in kernelmanagerd's deserialize<T>: swift_dynamicCast's
// bool becomes the whichCase of the Optional's storeEnumTagSinglePayload.
@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func cast<T>(_ o: AnyObject?, _ t: T.Type) -> T? { o as? T }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func castAny<T>(_ o: Any, _ t: T.Type) -> T? { o as? T }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func isClass<T>(_ t: T.Type) -> Bool { T.self is AnyClass }

// isKnownUniquelyReferenced: swift_isUniquelyReferenced_nonNull_native (Swift
// class), swift_isUniquelyReferenced_nonNull (ObjC class)
// Through globals: Swift 6.4.2 -O drops the store of a value that only an
// isKnownUniquelyReferenced(&local or &stackPromotedObject.field) reads, natively too.
var gBox = Box()
var gObject = NSObject()
@inline(never) func uniqueBox() -> Bool { isKnownUniquelyReferenced(&gBox) }
@inline(never) func uniqueObject() -> Bool { isKnownUniquelyReferenced(&gObject) }

// heap objects that escape (a non-escaping one may be promoted to the stack)
var sink: [AnyObject] = []
@inline(never) func makeBox() -> Box { let b = Box(); sink.append(b); sink.removeLast(); return b }
@inline(never) func makeObject() -> NSObject { let o = NSObject(); sink.append(o); sink.removeLast(); return o }
@inline(never) func keep(_ o: AnyObject) { sink.append(o) }

@inline(never) func escapeCheck(_ f: () -> Int) -> Int {
    withoutActuallyEscaping(f) { g in g() }
}

let xml = "<array><array><string>a</string><integer size=\"32\">5</integer></array><array><string>b</string></array></array>"
var err: Unmanaged<CFString>?
let plist: AnyObject? = IOCFUnserialize(xml, kCFAllocatorDefault, 0, &err) as AnyObject?

@inline(never) func run(_ round: Int) {
    if let r = cast(plist, [[Any]].self) {
        print(round, "[[Any]]", r.count, r.map { $0.count }, r[0][0] as? String ?? "?")
    } else { print(round, "[[Any]] nil") }
    print(round, "[[String]]", cast(plist, [[String]].self).map { "\($0)" } ?? "nil")
    print(round, "NSArray", cast(plist, NSArray.self)?.count ?? -1)
    print(round, "String", cast(plist, String.self) ?? "nil")
    print(round, "Int from Any", castAny(42 as Any, Int.self) ?? -1, castAny("x" as Any, Int.self) ?? -1)
    print(round, "isClass", isClass(Box.self), isClass(Int.self), isClass([Int].self), isClass(NSObject.self))

    gBox = makeBox()
    let u1 = uniqueBox()
    keep(gBox)
    let u2 = uniqueBox()
    sink.removeAll()
    let u3 = uniqueBox()
    print(round, "unique", u1, u2, u3, gBox.v)
    gObject = makeObject()
    let n1 = uniqueObject()
    keep(gObject)
    let n2 = uniqueObject()
    sink.removeAll()
    print(round, "uniqueObjC", n1, n2)

    // copy on write: a shared buffer must be copied, never mutated in place
    var a = [1, 2, 3]
    let shared = a
    a[0] = 100 + round
    var d = ["k": [1]]
    let dshared = d
    d["k"]!.append(2)
    print(round, "cow", a, shared, d["k"]!, dshared["k"]!)

    print(round, "escape", escapeCheck { 7 * (round + 1) })
}

for round in 0..<3 { run(round) }
