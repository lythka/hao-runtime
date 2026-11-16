// Guest structs built with layout-string value witnesses (as system binaries are) whose layout depends on
// a resilient field (UUID, Date) are laid out at run time by swift_cvw_initStructMetadataWithLayoutString,
// which gets a frozen Data field's metadata too. Their compiled tag witnesses pick the field by the extra
// inhabitant count (Data's on arm64: 12). Native generic code (DispatchQueue.sync, Combine,
// Array, Optional) then makes, tests, copies and destroys Optionals of them. Built
// -enable-layout-string-value-witnesses(-instantiation) by layoutxi.sh; every line must equal native arm64.
import Combine
import Foundation

final class Box { let n: Int; init(_ n: Int) { self.n = n } }

// The shape of a record the system's Find My agents keep: Data, UUID, UUID, UInt64, Date.
struct Record: Equatable {
    var meta: Data
    var id: UUID
    var beacon: UUID
    var index: UInt64
    var date: Date
}
// Data inside a tuple field, and Data? (the struct's inhabitants come from them).
struct TupleRecord { var pair: (Int, Data); var id: UUID }
struct OptRecord { var meta: Data?; var date: Date }
// Control: a Bool has more inhabitants than Data on both architectures.
struct BoolRecord { var meta: Data; var flag: Bool; var id: UUID }
// A class reference: pointer-word inhabitants, the same count on both.
struct BoxRecord { var meta: Data; var box: Box; var date: Date }
// A guest enum around the record, and a generic struct holding Data.
enum Wrapped { case one(Record); case none; case other }
struct Generic<T> { var meta: Data; var t: T }

let fixed = UUID(uuidString: "11111111-2222-3333-4444-555555555555")!
let other = UUID(uuidString: "66666666-7777-8888-9999-000000000000")!
let when = Date(timeIntervalSince1970: 1_000_000)
let big = Data((0..<2483).map { UInt8(truncatingIfNeeded: $0) })
let datas: [(String, Data)] = [("empty", Data()), ("inline", Data([1, 2, 3])), ("slice", big),
                               ("sub", big[100..<110])]

func say(_ s: String) { print(s); fflush(stdout) }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func roundTrip<T>(_ v: T) -> (T?, T?, Bool, Bool) {
    var some: T? = v
    var none: T? = nil
    let copy = some
    some = none
    none = copy
    return (none, some, none == nil, some == nil)
}

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func layout<T>(_: T.Type) -> String {
    "\(MemoryLayout<T>.size) \(MemoryLayout<T?>.size) \(MemoryLayout<T??>.size)"
}

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func nils<T>(_ v: T, _ n: Int) -> String {
    var a: [T?] = Array(repeating: nil, count: n)
    for i in stride(from: 0, to: n, by: 3) { a[i] = v }
    let b = a
    a.removeAll()
    return b.map { $0 == nil ? "-" : "+" }.joined()
}

final class Store {
    let q = DispatchQueue(label: "layoutxi")
    var records: [Record] = []
    @inline(never) func get(_ i: Int) -> Record { q.sync { records[i % records.count] } }
}

func describe(_ r: Record?) -> String {
    guard let r = r else { return "nil" }
    return "\(r.meta.count):\(r.meta.first ?? 0):\(r.meta.last ?? 0) \(r.id == fixed) \(r.index) \(r.date == when)"
}

@main struct Main {
    static func main() {
        say("layout Record \(layout(Record.self))")
        say("layout TupleRecord \(layout(TupleRecord.self))")
        say("layout OptRecord \(layout(OptRecord.self))")
        say("layout BoolRecord \(layout(BoolRecord.self))")
        say("layout BoxRecord \(layout(BoxRecord.self))")
        say("layout Wrapped \(layout(Wrapped.self))")
        say("layout Generic<UUID> \(layout(Generic<UUID>.self))")

        let store = Store()
        for (i, (name, d)) in datas.enumerated() {
            let r = Record(meta: d, id: fixed, beacon: other, index: UInt64(i), date: when)
            store.records.append(r)
            let (n, s, nn, sn) = roundTrip(r)
            say("roundTrip \(name) \(describe(n)) \(describe(s)) \(nn) \(sn)")
            say("nils \(name) \(nils(r, 10))")
            let t = TupleRecord(pair: (i, d), id: fixed)
            let (tn, ts, _, _) = roundTrip(t)
            say("tuple \(name) \(tn.map { "\($0.pair.0) \($0.pair.1.count)" } ?? "nil") \(ts == nil) \(nils(t, 7))")
            let o = OptRecord(meta: i % 2 == 0 ? d : nil, date: when)
            let (on, os, _, _) = roundTrip(o)
            say("opt \(name) \(on.map { "\($0.meta?.count ?? -1)" } ?? "nil") \(os == nil) \(nils(o, 7))")
            let b = BoolRecord(meta: d, flag: true, id: fixed)
            say("bool \(name) \(roundTrip(b).0.map { "\($0.meta.count) \($0.flag)" } ?? "nil") \(nils(b, 5))")
            let x = BoxRecord(meta: d, box: Box(i), date: when)
            say("box \(name) \(roundTrip(x).0.map { "\($0.meta.count) \($0.box.n)" } ?? "nil") \(nils(x, 5))")
            let w: [Wrapped] = [.one(r), .none, .other]
            say("wrapped \(name) " + w.map { v -> String in
                switch roundTrip(v).0 {
                case .one(let r)?: return "one(\(describe(r)))"
                case .none?: return "none"
                case .other?: return "other"
                case nil: return "nil"
                }
            }.joined(separator: " ") + " \(nils(Wrapped.one(r), 4))")
            let g = Generic(meta: d, t: fixed)
            say("generic \(name) \(roundTrip(g).0.map { "\($0.meta.count) \($0.t == fixed)" } ?? "nil") \(nils(g, 5))")
        }

        // Native generic code returning and holding the record: DispatchQueue.sync's T?, then a Combine
        // pipeline (compactMap, removeDuplicates) that keeps the last value as an Optional.
        let subject = PassthroughSubject<Record?, Never>()
        var seen = 0, bytes = 0
        let sink = subject.compactMap { $0 }.removeDuplicates().sink { r in
            seen += 1
            bytes += r.meta.count
        }
        for i in 0..<400 {
            let r = store.get(i / 3)
            subject.send(i % 5 == 4 ? nil : r)
        }
        sink.cancel()
        say("combine seen=\(seen) bytes=\(bytes)")
        var total = 0
        for i in 0..<400 { total += store.get(i).meta.count }
        say("sync total=\(total) \(describe(store.get(2)))")
    }
}
