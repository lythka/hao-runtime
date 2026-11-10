// Swift functions typed from their mangled names. One case per swiftcc difference kind
// between Apple arm64 and x86_64, through real system libraries where one exports such
// a function and through tests/guest/swiftcchelper.swift where none does, and EXACT
// controls (the shapes that carry 98.3% of the boot's UNKNOWN Swift calls).
//   swiftcc list        the case names
//   swiftcc syms CASE   the imports the case is about (mangled, no leading _)
//   swiftcc CASE        that case's lines ("CASE label: value")
// Every value is deterministic natively on arm64 (SWIFT_DETERMINISTIC_HASHING=1 for the
// hash case); no pointer is printed. Each case runs in its own process
// (tests/guest/swiftcc.sh), so a crash costs only its own lines.
// Built -O so the stdlib's inlinable wrappers are inlined and the guest calls the entry
// points system code calls (_stringCompareWithSmolCheck, not ==).
import Foundation
import Spatial
import Dispatch
import ObjectiveC
import SwiftCCHelper

nonisolated(unsafe) var cur = ""
func P(_ label: String, _ value: String) { print("\(cur) \(label): \(value)") }

// Values the optimizer cannot see through.
@_optimize(none) @inline(never) func o<T>(_ v: T) -> T { v }

// ------------------------------------------------------------------ TYPED by the rules

// UUID.init(uuid:): 16 UInt8, bytes 8-15 packed in one arm64 stack word.
func uuid() {
    let b = o((UInt8(1), UInt8(2), UInt8(3), UInt8(4), UInt8(5), UInt8(6), UInt8(7), UInt8(8), UInt8(9), UInt8(10),
               UInt8(11), UInt8(12), UInt8(13), UInt8(14), UInt8(15), UInt8(16)))
    P("UUID(uuid:)", UUID(uuid: b).uuidString)
    let c = o((UInt8(0xf0), UInt8(0xe1), UInt8(0xd2), UInt8(0xc3), UInt8(0xb4), UInt8(0xa5), UInt8(0x96), UInt8(0x87),
               UInt8(0x78), UInt8(0x69), UInt8(0x5a), UInt8(0x4b), UInt8(0x3c), UInt8(0x2d), UInt8(0x1e), UInt8(0x0f)))
    P("UUID(uuid:) 2", UUID(uuid: c).uuidString)
}

// DateComponents.init(calendar:timeZone:era:...:yearForWeekOfYear:): 14 Int? = (i64, i8)
// pairs, 22 arm64 stack words; the bridge copies 16.
func datecomp() {
    let dc = DateComponents(calendar: nil, timeZone: nil, era: o(1), year: o(2026), month: o(11), day: o(4), hour: o(5),
                            minute: o(6), second: o(7), nanosecond: o(8), weekday: o(3), weekdayOrdinal: o(1),
                            quarter: o(4), weekOfMonth: o(1), weekOfYear: o(45), yearForWeekOfYear: o(2027))
    func s(_ v: Int?) -> String { v.map(String.init) ?? "nil" }
    P("DateComponents era..second", [dc.era, dc.year, dc.month, dc.day, dc.hour, dc.minute, dc.second].map(s).joined(separator: " "))
    P("DateComponents nanosecond..", [dc.nanosecond, dc.weekday, dc.weekdayOrdinal, dc.quarter, dc.weekOfMonth,
                                      dc.weekOfYear, dc.yearForWeekOfYear].map(s).joined(separator: " "))
    let dn = DateComponents(calendar: nil, timeZone: nil, era: nil, year: o(1999), month: nil, day: nil, hour: nil,
                            minute: nil, second: nil, nanosecond: nil, weekday: nil, weekdayOrdinal: nil, quarter: nil,
                            weekOfMonth: nil, weekOfYear: nil, yearForWeekOfYear: o(-7))
    P("DateComponents nils", [dn.era, dn.year, dn.month, dn.weekOfYear, dn.yearForWeekOfYear].map(s).joined(separator: " "))
}

// SPRotation3D.init(position:target:up:): 9 Double and no integer argument; the 9th is
// arm64 stack word 0, which the bridge puts in x86 slot 2 (slot 0 gets x6).
func rot3d() {
    func q(_ r: Rotation3D) -> String {
        String(format: "%.6f %.6f %.6f %.6f", r.quaternion.vector.x, r.quaternion.vector.y, r.quaternion.vector.z,
               r.quaternion.vector.w)
    }
    P("Rotation3D up.z=0.3", q(Rotation3D(position: Point3D(x: o(1), y: 2, z: 3), target: Point3D(x: 4, y: 5, z: 7),
                                          up: Vector3D(x: 0.1, y: 1, z: o(0.3)))))
    P("Rotation3D up.z=2", q(Rotation3D(position: Point3D(x: o(0), y: 0, z: 0), target: Point3D(x: 1, y: 0, z: 0),
                                        up: Vector3D(x: 0, y: 0.5, z: o(2)))))
}

// ObjCBool: bool (i1) on arm64, signed char (i8) on x86_64.
func objcbool() {
    P("ObjCBool(true).description", ObjCBool(o(true)).description)
    P("ObjCBool(false).description", ObjCBool(o(false)).description)
    P("hObjCBool(true)", hObjCBool(ObjCBool(o(true))))
    P("hObjCBool(false)", hObjCBool(ObjCBool(o(false))))
    P("hMakeObjCBool(3)", "\(hMakeObjCBool(o(3)).boolValue)")
    P("hMakeObjCBool(2)", "\(hMakeObjCBool(o(2)).boolValue)")
}

// ------------------------------------------------------------------ helper: every kind
func narrow() {
    P("hNarrow16", hNarrow16(o(0x10), 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
                             0x1e, o(0x1f)))
    P("hNarrow16x12", hNarrow16x12(o(0x100), 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10a,
                                   o(0x10b)))
    P("hNarrow32x10", hNarrow32x10(o(0x1000), 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008,
                                   o(0x1009)))
    P("hFloat10", hFloat10(o(0.5), 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, o(9.5)))
}
func fpstack() {
    P("hDouble9", hDouble9(o(1.25), 2.25, 3.25, 4.25, 5.25, 6.25, 7.25, 8.25, o(9.25)))
}
func words() {
    P("hWords25", hWords25(o(0x100), 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10a, 0x10b,
                           0x10c, 0x10d, 0x10e, 0x10f, 0x110, 0x111, 0x112, 0x113, 0x114, 0x115, 0x116, 0x117,
                           o(0x118)))
}
func simd2() {
    let r = hSIMD2(SIMD2(o(1), 2), SIMD2(3, o(4)))
    P("hSIMD2", "\(r.x) \(r.y)")
    P("hSIMD2Text", hSIMD2Text(SIMD2(o(7.5), o(-1.25))))
}
// Int128 across the end of x86_64's integer registers (hI128At5); hI128Late is a control.
func i128() {
    P("hI128At5", hI128At5(o(1), 2, 3, 4, o(5), o(Int128(0x1111_2222_3333_4444) << 64 | Int128(0x5555_6666_7777_8888))))
    P("hI128Late", hI128Late(o(1), 2, 3, 4, 5, 6, o(7), o(Int128(0x1234_5678_9abc_def0) << 64 | Int128(0x0fed_cba9_8765_4321))))
}
final class GK {}
// A 17-tag multi-payload enum by value: the same i64 on both targets, different bits.
// Built by the guest and read by native code, and the other way round.
func enum17in() {
    P("hE17(.c5) built here", hE17(.c5(K(o(55)))))
    P("hE17(.c16) built here", hE17(.c16(K(o(16)))))
}
func enum17out() {
    P("hE17(hMakeE17(9)) native both ways", hE17(hMakeE17(o(9), K(9))))
    let e = hMakeE17(o(12), K(12))
    let s: String
    switch e {
    case .c12(let k): s = "c12 \(k.n)"
    case .c0, .c1, .c2, .c3, .c4, .c5, .c6, .c7, .c8, .c9, .c10, .c11, .c13, .c14, .c15, .c16: s = "another case"
    }
    P("hMakeE17(12) read here", s)
}

// ------------------------------------------------------------------ EXACT controls
func controls() {
    P("hMix8x9", hMix8x9(o(1), 2, 3, 4, 5, 6, 7, o(8), o(0.5), 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, o(8.5)))
    P("hWords24", hWords24(o(0x200), 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,
                           0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x211, 0x212, 0x213, 0x214, 0x215, 0x216, o(0x217)))
    P("hTen", hTen(o(0x31), 0x32, o(0x33), 0x34, 0x35, 0x36, 0x37, o(0x38), 0x39, o(0x3a)))
    let v = hSIMD4(SIMD4(o(1), 2, 3, o(4)))
    P("hSIMD4", "\(v.x) \(v.y) \(v.z) \(v.w)")
    P("hI128First", hI128First(o(Int128(-3) << 70), o(0x66)))
    P("hE4(.c3)", hE4(.c3(K(o(33)))))
    // Measured: Darwin arm64 passes an i128 in the next two GPRs with no even-pair
    // alignment (x1:x2 here), so the register bridge places it.
    P("hI128", hI128(o(0x55), o(Int128(0x1234_5678_9abc_def0) << 64 | Int128(0x0fed_cba9_8765_4321))))
}

// _assertionFailure(_:_:file:line:flags:): 10 GPR words, 2 on the arm64 stack (placed
// right today). fatalError's message goes to stderr; the case puts it on stdout.
func assertfail() {
    fflush(stdout)
    dup2(1, 2)
    fatalError("swiftcc assert \(o(41) + 1)")
}

// String: _stringCompareWithSmolCheck, hasPrefix, the Character subscript getter,
// index(after:), append, count.
func strings() {
    let a = o("abcdefghijklmnopqrstuvwxyz-long-enough-to-be-native"), b = o("abcdefghijklmnopqrstuvwxyz-long-enough-to-be-nativf")
    P("== < >", "\(a == b) \(a < b) \(b < a) \(o("héllo") == o("he\u{301}llo"))")
    P("hasPrefix", "\(a.hasPrefix(o("abcdef"))) \(a.hasPrefix(o("abd"))) \(o("ünïcödé").hasPrefix(o("ünï")))")
    var n = 0, last: Character = " "
    for c in o("héllo wörld 👍🏽 ok") { n += 1; last = c }
    P("Character count", "\(n) last=\(last)")
    let s = o("x🇫🇷yz")
    var i = s.startIndex, parts: [String] = []
    while i != s.endIndex { parts.append(String(s[i])); i = s.index(after: i) }
    P("index(after:)", parts.joined(separator: "|"))
    var t = o("one")
    t.append(o(" two"))
    t.append(contentsOf: o(" three"))
    P("append", "\(t) \(t.count) \(t.utf8.count)")
    P("sorted", o(["pear", "Äpfel", "apple", "zebra", "émile"]).sorted().joined(separator: ","))
}

// Hasher.init(_seed:), _finalize, _combine, String.hash(into:), Data.hash(into:), Set.
func hashing() {
    var h = Hasher()
    h.combine(o(UInt8(7)))
    h.combine(o("seven"))
    h.combine(o(77 as Int))
    P("Hasher", String(h.finalize(), radix: 16))
    P("String.hashValue", String(o("hash me").hashValue, radix: 16))
    P("Data.hashValue", String(Data(o([1, 2, 3, 4, 5] as [UInt8])).hashValue, radix: 16))
    let set = Set(o(["b", "a", "c", "a", "d"]))
    P("Set", "\(set.count) \(set.contains(o("c"))) \(set.contains(o("z")))")
    var d: [String: Int] = [:]
    for (k, w) in o(["x", "y", "x", "z", "x"]).enumerated() { d[w, default: 0] += k }
    P("Dictionary", d.keys.sorted().map { "\($0)=\(d[$0]!)" }.joined(separator: " "))
}

// Data._Representation subscript setter and getter.
func data() {
    var d = Data(count: o(64))
    for i in 0..<64 { d[i] = UInt8(truncatingIfNeeded: i &* 37) }
    var sum = 0
    for i in 0..<64 { sum = sum &* 31 &+ Int(d[i]) }
    P("Data subscript", "\(d.count) \(sum) \(d[o(63)])")
    var small = Data(o([9, 8, 7] as [UInt8]))
    small[1] = o(0xee)
    P("Data small", small.map { String($0, radix: 16) }.joined(separator: " "))
}

// Type metadata accessors (...Ma): (i64 request [, metadata...]) -> {ptr, i64}.
func gname<T>(_: T.Type) -> String { "\(T.self)" }
func metadata() {
    P("names", [gname(URL.self), gname(UUID.self), gname(Date.self), gname(DateComponents.self),
                gname([URL].self), gname([String: UUID].self), gname(Rotation3D.self), gname(K.self)].joined(separator: " "))
    let u = o(URL(fileURLWithPath: "/tmp/swiftcc/x.txt"))
    P("URL", "\(u.lastPathComponent) \(u.pathExtension) \(u.deletingLastPathComponent().path)")
}

// JSONDecoder.decode<T>(_:from:) dispatch thunk: class, generic, throws, sret.
struct Pt: Codable { var x: Int; var name: String; var tags: [String] }
func json() {
    let ok = o(#"{"x": 42, "name": "pt", "tags": ["a", "b"]}"#).data(using: .utf8)!
    do {
        let p = try JSONDecoder().decode(Pt.self, from: ok)
        P("decode", "\(p.x) \(p.name) \(p.tags.joined(separator: ","))")
    } catch { P("decode", "threw") }
    do {
        _ = try JSONDecoder().decode(Pt.self, from: o(#"{"x": "no"}"#).data(using: .utf8)!)
        P("decode bad", "no error")
    } catch DecodingError.typeMismatch(let t, let c) {
        P("decode bad", "typeMismatch \(t) at \(c.codingPath.map(\.stringValue))")
    } catch { P("decode bad", "other error") }
    let enc = try! JSONEncoder().encode(Pt(x: o(7), name: "q", tags: []))
    P("encode", "\(enc.count)")
}

// DispatchQueue.async(group:qos:flags:execute:): a closure argument (a masked slot).
func dispatch() {
    let g = DispatchGroup(), q = DispatchQueue(label: "swiftcc")
    nonisolated(unsafe) var out: [Int] = []
    for i in 0..<o(5) { q.async(group: g, qos: .utility, flags: []) { out.append(i * i) } }
    g.wait()
    P("async(group:)", out.map(String.init).joined(separator: " "))
}

// Task.sleep(nanoseconds:) and a task group (swifttailcc: EXACT-ASYNC).
func async() async {
    try? await Task.sleep(nanoseconds: o(1000))
    let total = await withTaskGroup(of: Int.self) { g in
        for i in 1...o(4) { g.addTask { i * 10 } }
        var t = 0
        for await v in g { t += v }
        return t
    }
    P("Task.sleep + group", "\(total)")
}

let cases: [(String, [String], () async -> Void)] = [
    ("uuid", ["$s10Foundation4UUIDV4uuidACs5UInt8V_A15Ft_tcfC", "$s10Foundation4UUIDVMa"], { uuid() }),
    ("datecomp", ["$s10Foundation14DateComponentsV8calendar8timeZone3era4year5month3day4hour6minute6second10nanosecond7weekday0O7Ordinal7quarter11weekOfMonth0rS4Year0h7ForWeeksU0AcA8CalendarVSg_AA04TimeF0VSgSiSgA13ZtcfC"], { datecomp() }),
    ("rot3d", ["$sSo12SPRotation3Da7SpatialE8position6target2upABSo9SPPoint3Da_AHSo10SPVector3DatcfC"], { rot3d() }),
    ("objcbool", ["$s10ObjectiveC8ObjCBoolV11descriptionSSvg", "$s13SwiftCCHelper9hObjCBoolySS10ObjectiveC0cD0VF",
                  "$s13SwiftCCHelper13hMakeObjCBooly10ObjectiveC0dE0VSiF"], { objcbool() }),
    ("narrow", ["$s13SwiftCCHelper9hNarrow16ySSs5UInt8V_A15DtF", "$s13SwiftCCHelper12hNarrow16x12ySSs5Int16V_A11DtF",
                "$s13SwiftCCHelper12hNarrow32x10ySSs5Int32V_A9DtF", "$s13SwiftCCHelper8hFloat10ySSSf_S9ftF"], { narrow() }),
    ("fpstack", ["$s13SwiftCCHelper8hDouble9ySSSd_S8dtF"], { fpstack() }),
    ("words", ["$s13SwiftCCHelper8hWords25ySSSi_S24itF"], { words() }),
    ("simd2", ["$s13SwiftCCHelper6hSIMD2ys0C0VySfGAE_AEtF", "$s13SwiftCCHelper10hSIMD2TextySSs0C0VySfGF"], { simd2() }),
    ("i128", ["$s13SwiftCCHelper8hI128At5ySSSi_S4is6Int128VtF", "$s13SwiftCCHelper9hI128LateySSSi_S6is6Int128VtF"], { i128() }),
    ("enum17in", ["$s13SwiftCCHelper4hE17ySSAA0C0OF"], { enum17in() }),
    ("enum17out", ["$s13SwiftCCHelper8hMakeE17yAA0D0OSi_AA1KCtF"], { enum17out() }),
    ("controls", ["$s13SwiftCCHelper7hMix8x9ySSSi_S7iS9dtF", "$s13SwiftCCHelper8hWords24ySSSi_S23itF",
                  "$s13SwiftCCHelper4hTenySSSi_Sis5UInt8VS4iADSis6UInt32VtF", "$s13SwiftCCHelper6hSIMD4ys0C0VySfGAEF",
                  "$s13SwiftCCHelper10hI128FirstySSs6Int128V_SitF", "$s13SwiftCCHelper3hE4ySSAA0C0OF",
                  "$s13SwiftCCHelper5hI128ySSSi_s6Int128VtF"], { controls() }),
    ("assertfail", ["$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_SSAHSus6UInt32VtF"], { assertfail() }),
    ("strings", ["$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_ADs01_G16ComparisonResultOtF",
                 "$sSS9hasPrefixySbSSF", "$sSSySJSS5IndexVcig", "$sSS5index5afterSS5IndexVAD_tF"], { strings() }),
    ("hashing", ["$ss6HasherV5_seedABSi_tcfC", "$ss6HasherV9_finalizeSiyF", "$sSS4hash4intoys6HasherVz_tF"], { hashing() }),
    ("data", ["$s10Foundation4DataV15_RepresentationOys5UInt8VSicis"], { data() }),
    ("metadata", ["$s10Foundation3URLVMa", "$sScMMa"], { metadata() }),
    ("json", ["$s10Foundation11JSONDecoderC6decode_4fromxxm_AA4DataVtKSeRzlFTj", "$s10Foundation11JSONDecoderCMa",
              "$ss13DecodingErrorOMa"], { json() }),
    ("dispatch", ["$sSo17OS_dispatch_queueC8DispatchE5async5group3qos5flags7executeySo0a1_b1_F0CSg_AC0D3QoSVAC0D13WorkItemFlagsVyyXBtF"], { dispatch() }),
    ("async", ["$sScTss5NeverORszABRs_rlE5sleep11nanosecondsys6UInt64V_tYaKFZ"], { await async() }),
]

let args = CommandLine.arguments
if args.count >= 2 && args[1] == "list" {
    print(cases.map(\.0).joined(separator: " "))
} else if args.count >= 3 && args[1] == "syms" {
    for c in cases where c.0 == args[2] { for s in c.1 { print(s) } }
} else if args.count >= 2, let c = cases.first(where: { $0.0 == args[1] }) {
    cur = c.0
    await c.2()
} else {
    print("usage: swiftcc list | syms CASE | CASE")
    exit(2)
}
