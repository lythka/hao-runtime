// A native Swift initializer whose arguments past x7 Apple arm64 packs at natural size
// (tests/guest/sitepack.sh). Built with library evolution and no interface unit, as a private
// framework without a generated record is. The shape follows findmylocateagent's crash
// (FindMyLocate Friend.init: two packed Bools at [sp] and [sp+1], then the addresses of an
// indirect Handle? and LocationSharingState? at [sp+8] and [sp+16]; the x86_64 callee reads
// six 8-byte slots, part of them with vector inserts).
import Foundation

public enum Kind { case follower, following, future, pending, offer }
public enum Origin { case app, messages, intents, other }
public enum Pause { case custom(Date), endOfDay, untilResumed }
public enum State { case paused(Pause), on }

public struct Handle {
    public let identifier: String
    public let server: String?
    public let pushes: [String]?
    public let siblings: [String]
    public init(identifier: String, server: String?, pushes: [String]?, siblings: [String]) {
        self.identifier = identifier; self.server = server; self.pushes = pushes; self.siblings = siblings
    }
}

public struct Friend {
    public let handle: Handle
    public let kind: Kind
    public let secure: Bool, shallow: Bool, legacyAllowed: Bool, legacy: Bool
    public let expiry: Date?
    public let state: State?
    public let origin: Origin
    public let created: Date?
    public let owner: Handle?
    public let optedOut: Bool?

    // x0 handle*, x1 kind*, x2 expiry*, x3 created*, x4 origin*, w5-w7 secure shallow legacyAllowed,
    // [sp] legacy, [sp+1] optedOut, [sp+8] owner*, [sp+16] state*
    @inline(never)
    public init(handle: Handle, kind: Kind, expiry: Date?, created: Date?, origin: Origin,
                secure: Bool, shallow: Bool, legacyAllowed: Bool, legacy: Bool,
                optedOut: Bool?, owner: Handle?, state: State?) {
        self.handle = handle; self.kind = kind; self.expiry = expiry; self.created = created
        self.origin = origin; self.secure = secure; self.shallow = shallow
        self.legacyAllowed = legacyAllowed; self.legacy = legacy; self.optedOut = optedOut
        self.owner = owner; self.state = state
    }
}

@inline(never)
public func describe(_ f: Friend) -> String {
    func h(_ x: Handle) -> String { "\(x.identifier)/\(x.server ?? "-")/\(x.pushes?.joined(separator: ",") ?? "-")/\(x.siblings.joined(separator: ","))" }
    return "\(h(f.handle)) kind=\(f.kind) b=\(f.secure),\(f.shallow),\(f.legacyAllowed),\(f.legacy) " +
        "opted=\(f.optedOut.map { "\($0)" } ?? "nil") owner=\(f.owner.map(h) ?? "nil") " +
        "state=\(f.state.map { "\($0)" } ?? "nil") origin=\(f.origin) " +
        "expiry=\(f.expiry.map { "\($0.timeIntervalSince1970)" } ?? "nil") created=\(f.created.map { "\($0.timeIntervalSince1970)" } ?? "nil")"
}

// Shapes whose adjacent constant arguments LLVM merges into one wider store at the call site: the
// store looks like one argument and the layout would have one too few. hao must keep the word copy
// for them, never shift the later arguments.
// [sp] a, [sp+1] b, [sp+2] c, [sp+8] n: a and b constants, one strh
@inline(never)
public func pairAt0(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                    _ a: Bool, _ b: Bool, _ c: Bool, _ n: Int) -> String {
    "x=\(x0),\(x1),\(x2),\(x3),\(x4),\(x5),\(x6),\(x7) a=\(a) b=\(b) c=\(c) n=\(n)"
}

// [sp] a, [sp+1] b, [sp+8] p, [sp+16] c, [sp+17] d, [sp+24] n: c and d constants, one strh at [sp+16]
@inline(never)
public func pairAt16(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                     _ a: Bool, _ b: Bool, _ p: Int, _ c: Bool, _ d: Bool, _ n: Int) -> String {
    "x=\(x0),\(x1),\(x2),\(x3),\(x4),\(x5),\(x6),\(x7) a=\(a) b=\(b) p=\(p) c=\(c) d=\(d) n=\(n)"
}

// [sp]..[sp+3] four constant Bools (one 4-byte store), [sp+4] e, [sp+8] n
@inline(never)
public func quadAt0(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                    _ a: Bool, _ b: Bool, _ c: Bool, _ d: Bool, _ e: Bool, _ n: Int) -> String {
    "x=\(x0),\(x1),\(x2),\(x3),\(x4),\(x5),\(x6),\(x7) a=\(a) b=\(b) c=\(c) d=\(d) e=\(e) n=\(n)"
}

// [sp] a, [sp+1] b, [sp+8] o; the caller keeps a SIMD value across the call and spills it above the
// arguments inside the call sequence
public final class Tag {
    public let name: String
    public init(_ name: String) { self.name = name }
}
@inline(never)
public func tagged(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                   _ a: Bool, _ b: Bool, _ o: Tag?) -> String {
    "x=\(x0),\(x1),\(x2),\(x3),\(x4),\(x5),\(x6),\(x7) a=\(a) b=\(b) o=\(o?.name ?? "nil")"
}

// A Codable struct whose initializer takes four Int32 and the address of an indirect Offer? past x7:
// [sp] [sp+4] [sp+8] [sp+12] the Int32, [sp+16] the Offer? (AppSubscriptions
// UserSubscriptionContextData.init's shape; the x86_64 callee reads 4 bytes of slots 2-5). A call
// site with constant zero Int32 stores them with one `stp xzr, xzr`, often the one that also
// allocates the outgoing area (`[sp, #-32]!`); the native JSON encoding reads the Offer? back.
public enum Status: String, Codable { case none, active, expired, trial }
public enum Offer: String, Codable { case unknown, winback, promotional, code, introductory }

public struct Context: Codable {
    public var status: Status
    public var product: String?
    public var renews: Bool?
    public var purchaser: Bool
    public var age: Int32, currentAge: Int32, trialAge: Int32
    public var trialMonth: Int32, trialYear: Int32, startMonth: Int32, startYear: Int32
    public var offer: Offer?
    public var offerID: String?
    public var grouped: Bool

    // x0 status*, x1 x2 product, w3 renews, w4 purchaser, w5-w7 the ages, then the stack as above
    @inline(never)
    public init(status: Status, product: String?, renews: Bool?, purchaser: Bool, age: Int32, currentAge: Int32,
                trialAge: Int32, trialMonth: Int32, trialYear: Int32, startMonth: Int32, startYear: Int32,
                offer: Offer?) {
        self.status = status; self.product = product; self.renews = renews; self.purchaser = purchaser
        self.age = age; self.currentAge = currentAge; self.trialAge = trialAge
        self.trialMonth = trialMonth; self.trialYear = trialYear; self.startMonth = startMonth; self.startYear = startYear
        self.offer = offer; self.offerID = nil; self.grouped = false
    }
}

// a class method called through its dispatch thunk,
// constant Bool? arguments merged into one store on the stack
open class Offers {
    public init() {}
    open func changing(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                       _ a: Bool?, _ b: Bool?, _ c: Bool?, _ d: Bool?, _ t: String?, _ e: Bool?, _ f: Bool?, _ n: Int) -> String {
        let s = { (v: Bool?) in v.map { "\($0)" } ?? "nil" }
        return "\(x0 + x7) a=\(s(a)) b=\(s(b)) c=\(s(c)) d=\(s(d)) t=\(t ?? "nil") e=\(s(e)) f=\(s(f)) n=\(n)"
    }
    open func nils(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                   _ a: Bool?, _ p: String?, _ q: String?, _ b: Bool?, _ r: [String]?, _ c: Bool?, _ u: String?,
                   _ d: Bool?, _ e: Bool?, _ n: Int) -> String {
        let s = { (v: Bool?) in v.map { "\($0)" } ?? "nil" }
        return "\(x0 + x7) a=\(s(a)) p=\(p ?? "nil") q=\(q ?? "nil") b=\(s(b)) r=\(r?.joined() ?? "nil") c=\(s(c)) u=\(u ?? "nil") d=\(s(d)) e=\(s(e)) n=\(n)"
    }
}

// An initializer with more than 32 stack arguments past x7, packed Bools in the middle and argument
// offsets past 255: Strings, Optionals of resilient values passed indirectly (URL?, Date?), Double?,
// Int64?, Decimal? and Float? (an Int32 and its tag merged into one 8-byte word), eleven Bools. The
// caller's call sequence is longer than 64 instructions and the x86_64 callee reads 44 stack slots.
public struct Record {
    public let logID: String, groups: [String]?, id: String, sortTitle: String, storeID: String?, title: String
    public let url: URL?
    public let author: String
    public let ratio: Double?
    public let coverURL: String?
    public let finished: Date?
    public let summary: String?, path: String?
    public let kind: Kind?
    public let genres: [String]?
    public let b0: Bool, b1: Bool, b2: Bool, b3: Bool, b4: Bool, b5: Bool, b6: Bool, b7: Bool, b8: Bool, b9: Bool, b10: Bool
    public let language: String?
    public let engaged: Date?
    public let rating: Int64?
    public let progress: Double
    public let released: Date?
    public let label: String?
    public let number: Decimal?
    public let sortKey: Int?
    public let version: Float?

    @inline(never)
    public init(logID: String, groups: [String]?, id: String, sortTitle: String, storeID: String?, title: String,
                url: URL?, author: String, ratio: Double?, coverURL: String?, finished: Date?, summary: String?,
                path: String?, kind: Kind?, genres: [String]?, b0: Bool, b1: Bool, b2: Bool, b3: Bool, b4: Bool,
                b5: Bool, b6: Bool, b7: Bool, b8: Bool, b9: Bool, b10: Bool, language: String?, engaged: Date?,
                rating: Int64?, progress: Double, released: Date?, label: String?, number: Decimal?, sortKey: Int?,
                version: Float?) {
        self.logID = logID; self.groups = groups; self.id = id; self.sortTitle = sortTitle; self.storeID = storeID
        self.title = title; self.url = url; self.author = author; self.ratio = ratio; self.coverURL = coverURL
        self.finished = finished; self.summary = summary; self.path = path; self.kind = kind; self.genres = genres
        self.b0 = b0; self.b1 = b1; self.b2 = b2; self.b3 = b3; self.b4 = b4; self.b5 = b5; self.b6 = b6
        self.b7 = b7; self.b8 = b8; self.b9 = b9; self.b10 = b10; self.language = language; self.engaged = engaged
        self.rating = rating; self.progress = progress; self.released = released; self.label = label
        self.number = number; self.sortKey = sortKey; self.version = version
    }
}

@inline(never)
public func describe(_ r: Record) -> String {
    func o<T>(_ v: T?) -> String { v.map { "\($0)" } ?? "nil" }
    let d = { (v: Date?) in v.map { "\($0.timeIntervalSince1970)" } ?? "nil" }
    let bs = [r.b0, r.b1, r.b2, r.b3, r.b4, r.b5, r.b6, r.b7, r.b8, r.b9, r.b10].map { $0 ? "1" : "0" }.joined()
    return "\(r.logID) \(o(r.groups)) \(r.id) \(r.sortTitle) \(o(r.storeID)) \(r.title) \(o(r.url)) \(r.author) " +
        "\(o(r.ratio)) \(o(r.coverURL)) \(d(r.finished)) \(o(r.summary)) \(o(r.path)) \(o(r.kind)) \(o(r.genres)) " +
        "b=\(bs) \(o(r.language)) \(d(r.engaged)) \(o(r.rating)) \(r.progress) \(d(r.released)) \(o(r.label)) " +
        "\(o(r.number)) \(o(r.sortKey)) \(o(r.version))"
}
