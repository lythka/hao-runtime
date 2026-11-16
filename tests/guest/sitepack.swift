// Guest calls of a native Swift initializer with packed stack arguments and no interface unit
// (tests/guest/sitepack.sh, helper tests/guest/sitepackhelper.swift): the values it builds are
// printed by native code and copied into an array through the native value witnesses
// (swift_arrayInitWithCopy), as findmylocateagent's friend list was.
import Foundation
import SitePackHelper

// the stack below the packed arguments holds stale words, as a busy guest thread's does
@inline(never) func dirty() -> Int {
    var junk = [Int](repeating: 0x1_10cc_3702, count: 96)
    return withUnsafeMutableBytes(of: &junk) { $0.count }
}

@inline(never) func make(_ n: Int, owner: Handle?, opted: Bool?, state: State?) -> Friend {
    let h = Handle(identifier: "friend-\(n)@example.com", server: n % 2 == 0 ? "srv-\(n)" : nil,
                   pushes: n % 3 == 0 ? nil : ["p\(n)", "q\(n)"], siblings: ["s\(n)"])
    return Friend(handle: h, kind: n % 2 == 0 ? .following : .offer,
                  expiry: n % 2 == 0 ? Date(timeIntervalSince1970: 1_700_000_000 + Double(n)) : nil,
                  created: Date(timeIntervalSince1970: 1_600_000_000), origin: n % 2 == 0 ? .app : .messages,
                  secure: n % 2 == 0, shallow: true, legacyAllowed: n % 3 == 0, legacy: n % 2 != 0,
                  optedOut: opted, owner: owner, state: state)
}

// runtime Bools the optimizer cannot fold
@inline(never) func flag(_ n: Int) -> Bool { CommandLine.arguments.count + n > 2 }
@inline(never) func num(_ n: Int) -> Int { CommandLine.arguments.count * 1000 + n }

// a SIMD value live across the call: spilled with `str q` above the outgoing arguments
@inline(never) func spill(_ v: SIMD4<Float>, _ t: Tag?, _ a: Bool, _ b: Bool) -> String {
    let s = tagged(1, 2, 3, 4, 5, 6, 7, 8, a, b, t)
    return s + " v=\(v * v)"
}

// a guest Codable value around the native ones: nested keyed and unkeyed containers, an enum with
// payloads, optionals; the native JSONEncoder calls its guest encode(to:) and the native Context's
enum Shape: Codable { case note(String), sticker(Int, Offer?), group([Shape]) }
struct Board: Codable {
    var title: String
    var contexts: [Context]
    var byName: [String: Context?]
    var shapes: [Shape]
    var pinned: Context?
}

@inline(never) func context(_ n: Int) -> Context {
    _ = dirty()
    return Context(status: n % 2 == 0 ? .active : .trial, product: n % 3 == 0 ? nil : "com.example.plan\(n)",
                   renews: n % 2 == 0 ? nil : flag(n), purchaser: flag(n + 1), age: 0, currentAge: 0, trialAge: 0,
                   trialMonth: 0, trialYear: 0, startMonth: 0, startYear: 0, offer: n == 0 ? nil : .promotional)
}

@inline(never) func encode() {
    for n in 0..<3 {
        let c = context(n)
        print("context \(n): offer=\(c.offer.map { $0.rawValue } ?? "nil") years=\(c.trialYear),\(c.startMonth),\(c.startYear)")
        let b = Board(title: "board \(n)", contexts: [c, context(n + 1)], byName: ["a": c, "none": nil],
                      shapes: [.note("hello"), .sticker(n, c.offer), .group([.note("in"), .sticker(-1, nil)])],
                      pinned: n == 1 ? nil : c)
        let e = JSONEncoder()
        e.outputFormatting = [.sortedKeys]
        do {
            let d = try e.encode(b)
            print("board \(n): \(String(decoding: d, as: UTF8.self))")
            let back = try JSONDecoder().decode(Board.self, from: d)
            print("decoded \(n): \(back.contexts.map { $0.offer?.rawValue ?? "nil" }) \(back.pinned?.status.rawValue ?? "nil")")
        } catch { print("board \(n): \(error)") }
    }
}

@main struct Main {
    static func main() {
        switch CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "" {
        case "list": print("friend pair0 pair16 quad spill encode")
        case "friend": friends()
        case "pair0":
            for n in 0..<2 {
                _ = dirty()
                print("pair0: \(pairAt0(1, 2, 3, 4, 5, 6, 7, 8, true, true, flag(n), num(n)))")
            }
        case "pair16":
            for n in 0..<2 {
                _ = dirty()
                print("pair16: \(pairAt16(1, 2, 3, 4, 5, 6, 7, 8, flag(n), flag(n + 1), num(n), false, true, num(n + 7)))")
            }
        case "quad":
            for n in 0..<2 {
                _ = dirty()
                print("quad: \(quadAt0(1, 2, 3, 4, 5, 6, 7, 8, true, false, true, true, flag(n), num(n)))")
            }
        case "spill":
            for n in 0..<2 {
                _ = dirty()
                print("spill: \(spill(SIMD4<Float>(Float(n), 2, 3, 4), n == 0 ? nil : Tag("tag-\(n)"), flag(n), flag(n + 1)))")
            }
        case "encode": encode()
        default: print("usage: sitepack list|friend|pair0|pair16|quad|spill|encode")
        }
    }

    static func friends() {
        let owner = Handle(identifier: "owner@example.com", server: "srv-owner", pushes: ["po"], siblings: [])
        let cases: [(Handle?, Bool?, State?)] = [
            (nil, nil, nil), (owner, true, .on), (nil, false, .paused(.endOfDay)),
            (owner, nil, .paused(.custom(Date(timeIntervalSince1970: 1_800_000_000)))), (owner, false, nil),
        ]
        var all: [Friend] = []
        for (n, c) in cases.enumerated() {
            _ = dirty()
            let f = make(n, owner: c.0, opted: c.1, state: c.2)
            print("friend \(n): \(describe(f))")
            print("friend \(n) guest: opted=\(f.optedOut.map { "\($0)" } ?? "nil") owner=\(f.owner?.identifier ?? "nil") legacy=\(f.legacy)")
            all.append(contentsOf: [f])
        }
        var copy: [Friend] = []
        copy.append(contentsOf: all)
        print("copied \(copy.count): \(copy.map { $0.owner?.identifier ?? "nil" }.joined(separator: " "))")
    }
}
