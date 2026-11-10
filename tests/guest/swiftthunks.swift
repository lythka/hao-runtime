// Native code calling guest functions that Swift metadata names by relative pointer (src/swiftmeta.c, W5a):
// every section below makes the native Swift runtime or stdlib call a guest function through a resilient
// witness, a metadata accessor, a generic witness-table instantiator, a metadata pattern or completion function,
// a class destroyer, a vtable entry, a layout string's enum-tag function, or an async witness. Under hao each
// such call must reach the guest function through an island thunk, not the NX-fault path
// (tests/guest/swiftthunks.sh checks the output against native arm64 and the fault count).
import Foundation

// resilient witnesses of stdlib protocols, called by native stdlib code through existentials
struct Point: Hashable, CustomStringConvertible, Comparable {
    var x: Int, y: Int
    var description: String { "(\(x),\(y))" }
    static func < (a: Point, b: Point) -> Bool { a.x != b.x ? a.x < b.x : a.y < b.y }
}

enum Color: String, CaseIterable, CustomDebugStringConvertible {
    case red, green, blue
    var debugDescription: String { "Color.\(rawValue)" }
}

// conditional conformance: a generic witness table, instantiated by the runtime
struct Box<T> { var value: T }
extension Box: CustomStringConvertible where T: CustomStringConvertible {
    var description: String { "Box[\(value.description)]" }
}
extension Box: Equatable where T: Equatable {}
extension Box: Hashable where T: Hashable {}

// generic class: metadata pattern instantiation, destroyer called by native swift_release
final class Node<T> {
    var value: T
    var next: Node<T>?
    init(_ v: T, _ n: Node<T>? = nil) { value = v; next = n }
    deinit { Node<Int>.freed += 1 }
}
extension Node where T == Int { nonisolated(unsafe) static var freed = 0 }

// singleton metadata initialization (a resilient field type)
final class Stamp: CustomStringConvertible {
    var date: Date
    var tag: URL
    init(_ t: Double) { date = Date(timeIntervalSince1970: t); tag = URL(fileURLWithPath: "/tmp/p\(Int(t))") }
    var description: String { "\(date.timeIntervalSince1970)@\(tag.lastPathComponent)" }
}

// multi-payload enum (a layout string with a tag function), destroyed and copied by native value witnesses
enum Shape {
    case circle(String)
    case rect(Int, String)
    case poly([Int], String)
    case none
}
struct Drawing { var shapes: [Shape]; var name: String; var extra: Shape }

// an open class subclassed in the guest, called through its vtable by native code (NSObject dynamic dispatch via ObjC
// and Swift vtable for a generic base)
class Animal { func sound() -> String { "..." }; func legs() -> Int { 4 } }
final class Bird: Animal { override func sound() -> String { "tweet" }; override func legs() -> Int { 2 } }

// async witnesses: native AsyncSequence dispatch thunks call the guest's next()
struct Countdown: AsyncSequence, AsyncIteratorProtocol {
    typealias Element = Int
    var n: Int
    mutating func next() async -> Int? { if n == 0 { return nil }; n -= 1; return n }
    func makeAsyncIterator() -> Countdown { self }
}

func drain<S: AsyncSequence>(_ s: S) async throws -> [S.Element] {
    var out: [S.Element] = []
    for try await v in s { out.append(v) }
    return out
}

// a generic struct's value witnesses, copied by the runtime (swift_initStructMetadata) while ObjC registration runs
// Keeper's metadata initializer (a resilient field), before any guest code runs
struct GBox<T> { var t: T; var s: String; var a: [T] }
final class Keeper { var d = Date(timeIntervalSince1970: 0); var b = GBox(t: 1, s: "k", a: [1, 2]) }

// key paths: computed properties and subscripts, applied by native KeyPath code (swift_getKeyPath patterns)
struct Rec { var a: Int; var name: String; var upper: String { name.uppercased() } }
struct Twice { var v = 0; var twice: Int { get { v * 2 } set { v = newValue / 2 } } }
struct Grid { var cells = [10, 20, 30]; subscript(i: Int) -> Int { cells[i] } }

@inline(never) func describe(_ x: Any) -> String { String(describing: x) }
@inline(never) func hashOf(_ x: AnyHashable) -> Int { x.hashValue == x.hashValue ? 1 : 0 }

@main struct Main {
    static func main() async throws {
        // 1. witnesses through existentials and AnyHashable
        let pts = [Point(x: 3, y: 1), Point(x: 1, y: 2), Point(x: 1, y: 1)]
        print("1", describe(pts[0]), pts.sorted().map { describe($0) }.joined(separator: " "))
        var set = Set<AnyHashable>()
        for p in pts { set.insert(AnyHashable(p)) }
        set.insert(AnyHashable(Point(x: 3, y: 1)))
        print("1", set.count, hashOf(AnyHashable(pts[1])), AnyHashable(pts[0]) == AnyHashable(Point(x: 3, y: 1)))
        print("1", Color.allCases.map { String(reflecting: $0) }.joined(separator: ","), Color(rawValue: "green").map { describe($0) } ?? "nil")
        // 2. a conditional conformance's witness table
        let boxes: [Any] = [Box(value: 7), Box(value: "s"), Box(value: Point(x: 0, y: 9))]
        print("2", boxes.map(describe).joined(separator: " "))
        print("2", Set([Box(value: 1), Box(value: 1), Box(value: 2)]).count, AnyHashable(Box(value: "a")) == AnyHashable(Box(value: "a")))
        // 3. generic class: metadata instantiation, deinit from native releases
        do {
            var list: Node<Int>? = nil
            for i in 0..<50 { list = Node(i, list) }
            var sum = 0
            var p = list
            while let q = p { sum += q.value; p = q.next }
            let strs = Node("a", Node("b"))
            print("3", sum, strs.value + (strs.next?.value ?? ""), describe(type(of: strs)))
            list = nil
        }
        print("3", "freed", Node<Int>.freed)
        // 4. singleton metadata initialization
        let st = (0..<3).map { Stamp(Double($0 * 1000)) }
        print("4", st.map(describe).joined(separator: " "))
        // 5. multi-payload enum values copied and destroyed by native code
        var drawings: [Any] = []
        for i in 0..<20 {
            let d = Drawing(shapes: [.circle("c\(i)"), .rect(i, "r"), .poly([i, i + 1], "p"), .none], name: "d\(i)",
                            extra: i % 2 == 0 ? .rect(i, "e") : .circle("x"))
            drawings.append(d)
        }
        let copy = drawings
        drawings.removeAll()
        var n = 0
        for case let d as Drawing in copy { n += d.shapes.count; if case .rect(let k, _) = d.extra { n += k } }
        print("5", n, describe((copy[3] as! Drawing).extra), Mirror(reflecting: copy[1]).children.count)
        // 6. vtable dispatch from a native caller (Mirror, String(describing:) through type metadata)
        let animals: [Animal] = [Animal(), Bird()]
        print("6", animals.map { $0.sound() + String($0.legs()) }.joined(separator: " "), describe(type(of: animals[1])))
        // 7. reflection: field types resolved from mangled names, accessors called by the runtime
        let m = Mirror(reflecting: Drawing(shapes: [], name: "m", extra: .none))
        print("7", m.children.map { "\($0.label ?? "?"):\(type(of: $0.value))" }.joined(separator: " "))
        // 8. async witnesses through native dispatch thunks
        let v = try await drain(Countdown(n: 5))
        print("8", v)
        // 9. value witnesses of an instantiated generic struct, called by native existential and array code
        var boxes2: [Any] = (0..<10).map { GBox(t: $0, s: "s\($0)", a: [$0, $0]) }
        let k = Keeper()
        print("9", boxes2.count, describe(boxes2[3]), k.b.s, k.b.a)
        boxes2.removeAll()
        print("9", boxes2.isEmpty)
        // 10. key path accessors called by the native stdlib
        let recs = [Rec(a: 2, name: "bo"), Rec(a: 1, name: "al"), Rec(a: 3, name: "cy")]
        let up: KeyPath<Rec, String> = \Rec.upper
        var tw = Twice()
        tw[keyPath: \Twice.twice] = 14
        let cell: KeyPath<Grid, Int> = \Grid.[2]
        print("10", recs.map { $0[keyPath: up] }, recs.sorted(using: KeyPathComparator(\Rec.upper, order: .reverse)).map(\.a),
              tw.v, tw[keyPath: \Twice.twice], Grid()[keyPath: cell], cell == \Grid.[2])
    }
}
