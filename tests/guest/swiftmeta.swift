// Swift metadata that native code reads or calls into, one section per kind.
// Output must match native arm64. `swiftmeta [SECTION]` runs one section; no argument runs all of them in order.
// tests/guest/swiftmeta.sh compares them and counts the native->guest NX faults.
//
//   classes  Swift classes whose layout is completed at run time: NSObject subclasses with stored properties of
//            resilient types from native frameworks (os.Logger, Foundation's URL/Date/UUID). Their class_ro_t
//            carries a Swift metadata initializer, and swift_updateClassMetadata2 computes the field offsets and
//            realizes the ObjC class (singleton metadata initialization, metadata accessors).
//   witness  resilient witnesses of conformances to stdlib protocols (Codable, CodingKey, Hashable, Equatable,
//            CustomStringConvertible, Actor) called by native Foundation, libswiftCore and libswift_Concurrency:
//            JSONEncoder/JSONDecoder, guest Hashable keys bridged to NSDictionary/NSSet, print, the native dispatch
//            thunks (...Tj) that unspecialized guest generic code calls and that tail-call the guest witness,
//            associated conformance accessors, AsyncStream.Iterator.next(isolation:) asking a guest actor for its
//            unownedExecutor.
//   vwt      value witnesses of guest structs and enums driven by libswiftCore's layout-string interpreter
//            (multiPayloadEnumFN calls the enum's tag function), as the 27.0 userland is built: arrays of guest
//            structs bridged to NSArray/NSDictionary, boxed in Any, copied and destroyed by native code; generic
//            guest types instantiated by the runtime (generic metadata pattern functions).
//   keypath  key paths to computed properties projected by native code (KeyPath._projectReadOnly from Foundation's
//            KeyPathComparator and PartialKeyPath subscripts), next to KVO and Combine on @objc dynamic properties.
//   async    guest async functions resumed by libswift_Concurrency (job/task entry, __swift_as_ret resume points):
//            detached and child tasks, task groups, continuations resumed from a native dispatch queue, async
//            sequences, throwing async functions, actors.
//   mirror   native reflection (Mirror, dump, type names) calling guest metadata accessors of generic types and
//            the enum value witnesses (EnumImpl::subscript).
//
// Built with layout-string value witnesses (swiftmeta.sh), as the 27.0 arm64-only programs are: they import
// swift_cvw_* (kmutil, kernelmanager_helper).
import Foundation
import os
import Combine

let sect = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "all"
let sections = ["classes", "witness", "vwt", "keypath", "async", "mirror"]
if sect != "all" && !sections.contains(sect) { print("unknown section \(sect); sections: \(sections)"); exit(2) }
func on(_ s: String) -> Bool { sect == "all" || sect == s }

class Base: NSObject {
    let log = Logger(subsystem: "com.example.hao", category: "swiftmeta")
    var obj: NSObject = NSObject()
    var arr: [Int] = [1, 2, 3]
    @objc dynamic var name: String = "base"
}

final class Derived: Base {
    var url = URL(fileURLWithPath: "/tmp/hao-swiftmeta")
    var date = Date(timeIntervalSince1970: 1_000_000)
    var id = UUID(uuidString: "12345678-1234-1234-1234-123456789ABC")!
    @objc var count: Int = 7
}

// looked up by name before any Swift code touches its metadata
@objc(HaoLateClass) final class Late: NSObject {
    var when = Date(timeIntervalSince1970: 42)
    @objc var tag: Int = 99
}

// Holder's metadata initializer needs the metadata of KeyPathComparator<Peer>
// (a resilient Foundation type), which realizes Peer through the runtime
// before hao reaches Peer in the class list (sharingd, searchpartyd)
final class Holder: NSObject {
    var cmp = KeyPathComparator(\Peer.v)
    @objc var tag: Int = 3
}

final class Peer: NSObject, NSCopying {
    @objc var v: Int = 5
    @objc func ping() -> Int { v + 1 }
    func copy(with zone: NSZone? = nil) -> Any { let p = Peer(); p.v = v; return p }
}

func ivars(_ c: AnyClass) -> String {
    var n: UInt32 = 0
    guard let l = class_copyIvarList(c, &n) else { return "none" }
    defer { free(l) }
    return (0..<Int(n)).map { "\(String(cString: ivar_getName(l[$0])!))@\(ivar_getOffset(l[$0]))" }.joined(separator: " ")
}

// MARK: classes
if on("classes") {
    if let late = NSClassFromString("HaoLateClass") as? NSObject.Type {
        let o = late.init()
        print("late", NSStringFromClass(late), class_getInstanceSize(late), o.value(forKey: "tag") ?? "nil")
    } else {
        print("late class not found")
    }
    let b = Base()
    b.arr.append(4)
    b.name = "changed"
    print("base", b.arr, type(of: b.obj), b.value(forKey: "name") ?? "nil", class_getInstanceSize(Base.self))
    print("base ivars", ivars(Base.self))
    let d = Derived()
    d.obj = NSString(string: "str")
    d.count += 1
    print("derived", d.arr, d.obj, d.url.path, d.date.timeIntervalSince1970, d.id.uuidString, d.value(forKey: "count") ?? "nil")
    print("derived size", class_getInstanceSize(Derived.self), class_getSuperclass(Derived.self).map(NSStringFromClass) ?? "nil")
    print("derived ivars", ivars(Derived.self))
    print("lookup", NSClassFromString(NSStringFromClass(Derived.self)) == Derived.self, Late().when.timeIntervalSince1970)
    let peers = [Peer(), Peer()]
    peers[1].v = 2
    let h = Holder()
    print("holder", h.tag, peers.sorted(using: h.cmp).map { $0.v }, class_getInstanceSize(Holder.self))
    let pc = peers[0].copy() as! Peer
    print("peer", pc.v, pc.ping(), Peer.conforms(to: NSCopying.self), (peers[0] as NSObject).value(forKey: "v") ?? "nil",
          class_getInstanceSize(Peer.self), ivars(Peer.self))
    d.log.debug("swiftmeta \(d.count)")
    print("classes ok")
}

// MARK: witness
struct WPoint: Codable, Hashable, CustomStringConvertible {
    var x: Int
    var label: String
    var description: String { "P(\(x),\(label))" }
}
enum WShape: Codable, Hashable { case dot(Int), line(Int, String), none }
struct WWrap<T> { var v: T; var n: Int }
extension WWrap: Equatable where T: Equatable {}
extension WWrap: Hashable where T: Hashable {}
extension WWrap: CustomStringConvertible { var description: String { "W(\(v),\(n))" } }
// unspecialized: calls libswiftCore's Hashable/Equatable dispatch thunks, which tail-call the guest witnesses
@_optimize(none) func sameHash<T: Hashable>(_ a: T, _ b: T) -> Bool {
    var h1 = Hasher(), h2 = Hasher()
    a.hash(into: &h1); b.hash(into: &h2)
    return h1.finalize() == h2.finalize() && a == b
}

// Error witness calls originate in native libswiftCore/Foundation, as in r28.
enum WFailure: Int, Error { case missing = 17 }
struct WCustomError: CustomNSError, LocalizedError {
    static var errorDomain: String { "hao.callback.error" }
    var errorCode: Int { 23 }
    var errorUserInfo: [String: Any] { ["reason": "fixture"] }
    var errorDescription: String? { "fixture failure" }
}
actor WFeed {
    var got: [Int] = []
    func drain(_ s: AsyncStream<Int>) async -> [Int] { for await v in s { got.append(v) }; return got }
}
if on("witness") {
    let pts = [WPoint(x: 1, label: "a"), WPoint(x: 2, label: "b")]
    let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]
    let data = try! enc.encode(pts)
    print("witness json", String(decoding: data, as: UTF8.self))
    print("witness decoded", try! JSONDecoder().decode([WPoint].self, from: data))
    let shapes = try! enc.encode([WShape.dot(3), .line(4, "z"), .none])
    print("witness shapes", String(decoding: shapes, as: UTF8.self),
          try! JSONDecoder().decode([WShape].self, from: shapes) == [.dot(3), .line(4, "z"), .none])
    let nd = [pts[0]: 10, pts[1]: 20] as NSDictionary
    print("witness nsdict", nd.count, nd.object(forKey: pts[1]) as? Int ?? -1, nd.object(forKey: WPoint(x: 1, label: "b")) as? Int ?? -1)
    let ns = Set(pts) as NSSet
    print("witness nsset", ns.count, ns.contains(pts[0]), ns.contains(WPoint(x: 9, label: "q")))
    print("witness describe", String(describing: pts[0]), "\(pts[1])", pts)
    print("witness thunk", sameHash(pts[0], WPoint(x: 1, label: "a")), sameHash(pts[0], pts[1]),
          sameHash(WWrap(v: pts[0], n: 1), WWrap(v: pts[0], n: 1)), sameHash(WShape.line(1, "a"), .line(1, "b")))
    let wd = [WWrap(v: 1, n: 2): "a", WWrap(v: 3, n: 4): "b"] as NSDictionary
    print("witness conditional", wd.count, wd[WWrap(v: 3, n: 4)] as? String ?? "nil",
          (wd.allKeys as! [WWrap<Int>]).map(\.description).sorted())
    for error in [WFailure.missing as Error, WCustomError() as Error] {
        let ns = error as NSError
        // The synthesized domain contains the executable module name.
        print("witness error", ns.code, ns.userInfo["reason"] as? String ?? "none",
              ns.domain.hasSuffix("WFailure") || ns.domain == "hao.callback.error")
    }
    let plistEncoder = PropertyListEncoder(); plistEncoder.outputFormat = .binary
    let plist = try! plistEncoder.encode(pts)
    print("witness plist", try! PropertyListDecoder().decode([WPoint].self, from: plist))
    let sem = DispatchSemaphore(value: 0)
    var drained: [Int] = []
    Task.detached {
        let s = AsyncStream<Int> { c in for i in 1...3 { c.yield(i) }; c.finish() }
        drained = await WFeed().drain(s)
        sem.signal()
    }
    sem.wait()
    print("witness actor", drained)
}

// MARK: vwt
final class VRef { let n: Int; init(_ n: Int) { self.n = n } }
enum VPayload { case num(Int), ref(VRef), text(String), empty }
enum VMaybe { case some(VRef), none }
struct VCell { var tag: VPayload; var opt: VMaybe; var name: String }
struct VPair<T> { var a: T; var b: VPayload }
@inline(never) func passAny(_ x: Any) -> Any { x }
func show(_ p: VPayload) -> String {
    switch p {
    case .num(let n): return "num\(n)"
    case .ref(let r): return "ref\(r.n)"
    case .text(let s): return "text:\(s)"
    case .empty: return "empty"
    }
}
if on("vwt") {
    var cells = [VCell(tag: .num(1), opt: .some(VRef(5)), name: "one"), VCell(tag: .ref(VRef(2)), opt: .none, name: "two"),
                 VCell(tag: .text("t"), opt: .some(VRef(6)), name: "three"), VCell(tag: .empty, opt: .none, name: "four")]
    let na = cells as NSArray
    print("vwt nsarray", na.count, (na.object(at: 1) as! VCell).name, show((na.lastObject as! VCell).tag))
    let copy = cells
    cells.removeFirst()
    cells.insert(copy[3], at: 0)
    print("vwt array", cells.map { show($0.tag) }, copy.map { $0.name })
    let anyv = passAny(copy[2]) as! VCell
    print("vwt any", show(anyv.tag), anyv.name)
    let pairs = [VPair(a: VRef(7), b: .text("x")), VPair(a: VRef(8), b: .ref(VRef(9)))]
    print("vwt generic", (passAny(pairs) as! [VPair<VRef>]).reversed().map { "\($0.a.n)/\(show($0.b))" })
    var dict: [String: VCell] = [:]
    for c in copy { dict[c.name] = c }
    let nd = dict as NSDictionary
    print("vwt nsdict", nd.count, (nd["three"] as! VCell).name, show((nd["two"] as! VCell).tag))
}

// MARK: keypath
final class KModel: NSObject {
    @objc dynamic var level: Int = 0
    @objc dynamic var title: String = "t0"
    var doubled: Int { level * 2 }
    var shout: String { title.uppercased() }
}
struct KRow { var n: Int; var s: String; var neg: Int { -n }; var len: Int { s.count } }
if on("keypath") {
    let m = KModel()
    var seen: [String] = []
    let obs = m.observe(\.level, options: [.old, .new]) { o, ch in
        seen.append("kvo \(ch.oldValue ?? -1)->\(ch.newValue ?? -1) doubled=\(o.doubled)")
    }
    var bag = Set<AnyCancellable>()
    m.publisher(for: \.title).sink { seen.append("combine \($0)") }.store(in: &bag)
    m.publisher(for: \.level).map(\.description).sink { seen.append("combine level \($0)") }.store(in: &bag)
    m.level = 4
    m.title = "t1"
    m.level = 5
    obs.invalidate()
    bag.removeAll()
    for s in seen { print("keypath", s) }
    let rows = [KRow(n: 3, s: "ccc"), KRow(n: 1, s: "a"), KRow(n: 2, s: "bbbbb")]
    print("keypath sort neg", rows.sorted(using: KeyPathComparator(\KRow.neg)).map(\.n))
    print("keypath sort len", rows.sorted(using: [KeyPathComparator(\KRow.len, order: .reverse)]).map(\.s))
    let ms = [KModel(), KModel(), KModel()]
    ms[0].level = 2; ms[1].level = 9; ms[2].level = 1
    ms[0].title = "b"; ms[1].title = "c"; ms[2].title = "a"
    print("keypath sort model", ms.sorted(using: KeyPathComparator(\KModel.doubled)).map(\.level),
          ms.sorted(using: KeyPathComparator(\KModel.shout)).map(\.title))
    let kps: [PartialKeyPath<KRow>] = [\KRow.neg, \KRow.len, \KRow.s]
    print("keypath partial", kps.map { "\(rows[2][keyPath: $0])" })
}

// MARK: async
actor ACounter {
    var n = 0
    func add(_ k: Int) -> Int { n += k; return n }
}
@inline(never) func slowSquare(_ x: Int) async -> Int {
    await Task.yield()
    return x * x
}
@inline(never) func fromQueue(_ x: Int) async -> Int {
    await withCheckedContinuation { (c: CheckedContinuation<Int, Never>) in
        DispatchQueue.global().async { c.resume(returning: x + 100) }
    }
}
struct AFail: Error {}
@inline(never) func failing(_ x: Int) async throws -> Int {
    await Task.yield()
    if x < 0 { throw AFail() }
    return x
}
struct ATicks: AsyncSequence {
    typealias Element = Int
    let n: Int
    struct AsyncIterator: AsyncIteratorProtocol {
        var i = 0
        let n: Int
        mutating func next() async -> Int? { await Task.yield(); i += 1; return i <= n ? i : nil }
    }
    func makeAsyncIterator() -> AsyncIterator { AsyncIterator(n: n) }
}
if on("async") {
    let sem = DispatchSemaphore(value: 0)
    var lines: [String] = []
    Task.detached {
        let c = ACounter()
        let a = await slowSquare(7)
        let b = await fromQueue(5)
        let child = await Task { await slowSquare(3) }.value
        var sum = 0
        await withTaskGroup(of: Int.self) { g in
            for i in 1...4 { g.addTask { await slowSquare(i) } }
            for await v in g { sum += v }
        }
        var ticks: [Int] = []
        for await v in ATicks(n: 3) { ticks.append(v) }
        let r1 = try? await failing(4), r2 = try? await failing(-1)
        let k1 = await c.add(2), k2 = await c.add(3)
        lines = ["async square \(a) queue \(b) child \(child) group \(sum)",
                 "async ticks \(ticks) throw \(String(describing: r1)) \(String(describing: r2)) actor \(k1) \(k2)"]
        sem.signal()
    }
    sem.wait()
    for l in lines { print(l) }
}

// MARK: mirror
struct MInner { var a: Int = 1; var b: String = "in" }
struct MBox<T> { var item: T; var count: Int }
enum MTree { case leaf(Int), node(MBox<MInner>, String), empty }
final class MNode { var name = "n"; var kids: [MBox<Int>] = [MBox(item: 4, count: 1)]; var tree: MTree = .leaf(3) }
struct MOuter {
    var inner = MInner()
    var boxed = MBox(item: MInner(), count: 2)
    var tree = MTree.node(MBox(item: MInner(), count: 5), "x")
    var opt: MBox<String>? = MBox(item: "s", count: 0)
    var node = MNode()
    var pair = (MBox(item: 1.5, count: 1), MTree.empty)
}
func walk(_ v: Any, _ depth: Int = 0) -> [String] {
    let m = Mirror(reflecting: v)
    let pad = String(repeating: " ", count: depth)
    var out = ["\(pad)\(m.subjectType) \(m.displayStyle.map { "\($0)" } ?? "-")"]
    if depth < 6 { for c in m.children { out.append("\(pad) \(c.label ?? "_"):"); out += walk(c.value, depth + 2) } }
    return out
}
if on("mirror") {
    for l in walk(MOuter()) { print("mirror", l) }
    var dumped = ""
    dump(MTree.node(MBox(item: MInner(), count: 7), "y"), to: &dumped)
    for l in dumped.split(separator: "\n") { print("mirror dump", l) }
    print("mirror type", _typeName(MBox<MTree>.self), String(reflecting: MBox<MInner>.self))
}
