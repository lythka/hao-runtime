// Guest-defined types' rows of the Swift value walk (src/swiftvalue/Facts.h walk_fact_of), from a fixture
// tests/guest/svguest.sh writes (test switch HAO_SWIFT_GUEST_FACTS) with this binary's own LC_UUID and
// descriptor offsets. Each type holds a resilient native struct, so the runtime lays it out and its value
// witnesses are native; generic code the optimizer does not specialize copies it through native Optional and
// Array witnesses, which the walk's rules refuse for these enums.
//   status: a multi-payload enum, two empty cases; its row says SAME.
//   fault:  a multi-payload enum with a Data? payload (an arm64 nil); its row says CONVERT, with case images.
//   record: a struct of a Status?, a Data?, a String? and a [Fault]; its row says CONVERT, by its fields' rows.
//   nil:    only nils of Status? and Fault?, which their rows' XI class (SAME) reads.
//   images: the case images of these types and of Data? on this architecture (the test's fixture).
import Foundation

enum Status { case curated(PersonNameComponents), one(PersonNameComponents), none, diff }
enum Fault: Error { case name(PersonNameComponents), blob(Data?), gone, lost }
struct Record { var status: Status?; var data: Data?; var text: String?; var faults: [Fault] = [] }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }

func person(_ s: String) -> PersonNameComponents { var p = PersonNameComponents(); p.givenName = s; return p }
func shown(_ s: Status) -> String {
    switch s { case .curated(let p): return "curated(\(p.givenName!))"; case .one(let p): return "one(\(p.givenName!))"
    case .none: return "none"; case .diff: return "diff" }
}
func shown(_ f: Fault) -> String {
    switch f { case .name(let p): return "name(\(p.givenName!))"; case .blob(let d): return "blob(\(d.map { "\($0.count)" } ?? "nil"))"
    case .gone: return "gone"; case .lost: return "lost" }
}
func shown(_ r: Record) -> String {
    "\(r.status.map(shown) ?? "nil") \(r.data.map { "\($0.count)" } ?? "nil") \(r.text ?? "nil") \(r.faults.map(shown))"
}

/* Each case's bytes as the type's destructiveInjectEnumTag writes them over zeros, and its mask (the bits the
 * same over zeros and ones). */
typealias Inject = @convention(c) (UnsafeMutableRawPointer, UInt32, UnsafeRawPointer) -> Void
func images(_ name: String, _ t: Any.Type, _ cases: Int) {
    let meta = unsafeBitCast(t, to: UnsafeRawPointer.self)
    let vwt = meta.load(fromByteOffset: -8, as: UnsafeRawPointer.self)
    let size = vwt.load(fromByteOffset: 64, as: Int.self)
    let inject = unsafeBitCast(vwt.load(fromByteOffset: 13 * 8, as: UnsafeRawPointer.self), to: Inject.self)
    func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined() }
    print("layout", name, size, vwt.load(fromByteOffset: 72, as: Int.self), String(vwt.load(fromByteOffset: 80, as: UInt32.self), radix: 16),
          vwt.load(fromByteOffset: 84, as: UInt32.self))
    for c in 0..<cases {
        var z = [UInt8](repeating: 0, count: size), o = [UInt8](repeating: 0xff, count: size)
        z.withUnsafeMutableBytes { inject($0.baseAddress!, UInt32(c), meta) }
        o.withUnsafeMutableBytes { inject($0.baseAddress!, UInt32(c), meta) }
        print(name, c, hex(z), hex(zip(z, o).map { ~($0 ^ $1) }))
    }
}

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        switch CommandLine.arguments.dropFirst().first ?? "" {
        case "status":
            let all: [Status] = [.curated(person("a")), .one(person("b")), .none, .diff]
            for v in all { print(shown(copied(v)), copied(Optional(v)).map(shown) ?? "nil", copied(Status?.none) == nil) }
            print(copied(all).map(shown))
        case "nil":
            print(copied(Status?.none).map(shown) ?? "nil", copied(Fault?.none).map(shown) ?? "nil")
        case "fault":
            let all: [Fault] = [.name(person("a")), .blob(Data([1, 2])), .blob(nil), .gone, .lost]
            for v in all { print(shown(copied(v)), copied(Optional(v)).map(shown) ?? "nil") }
            print(copied(all).map(shown))
        case "record":
            let all = [Record(status: nil, data: nil, text: nil), Record(status: Status.none, data: Data([3]), text: "t"),
                       Record(status: .one(person("c")), data: nil, text: nil, faults: [.blob(nil), .gone])]
            for v in all { print(shown(copied(v)), copied(Optional(v)).map(shown) ?? "nil") }
            print(copied(all).map(shown))
        case "images":
            images("status", Status.self, 4); images("fault", Fault.self, 4); images("data", Data?.self, 2)
            images("record", Record.self, 0)
        default: print("status, nil, fault, record or images")
        }
    }
}
