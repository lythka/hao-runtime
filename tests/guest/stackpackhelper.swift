// Native Swift functions whose arguments past x7 Apple arm64 packs at natural size
// (tests/guest/stackpack.sh). Built without library evolution and without an interface, as a
// private framework is: tools/gen_swift.py cannot declare these, tools/gen_stackpack.py types them
// from both slices' code. The shapes follow Photos' crash (PhotosUICore
// PhotoKitItemListManagerOptions.init: three packed Bools, then an object reference that the
// options' copy retains).
import Foundation

public enum Rel { case assets(Int), people, none }

public struct Opts {
    public var rel: Rel
    public var a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int
    public var curation: Int
    public var prepare: Bool, transient: Bool, featured: Bool
    public var pred: NSObject?

    // x0 rel (indirect), x1-x7, then [sp] curation (8), [sp+8] [sp+9] [sp+10] the Bools, [sp+16] pred
    @inline(never)
    public init(rel: Rel, a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int, curation: Int,
                prepare: Bool, transient: Bool, featured: Bool, pred: NSObject?) {
        self.rel = rel
        self.a = a; self.b = b; self.c = c; self.d = d; self.e = e; self.f = f; self.g = g
        self.curation = curation
        self.prepare = prepare; self.transient = transient; self.featured = featured
        self.pred = pred
    }
}

// a generic class holding a copy of the options: its init copies them through the value witness
// (the Photos crash site: swift_cvw_initWithCopy -> objc_retain of the stale word)
public final class Mgr<T> {
    public let opts: Opts
    public let tag: T
    @inline(never)
    public init(tag: T, opts: Opts) { self.tag = tag; self.opts = opts }
    public func describe() -> String { describeOpts(opts) + " tag=\(tag)" }
}

@inline(never)
public func describeOpts(_ o: Opts) -> String {
    "rel=\(o.rel) a-g=\(o.a),\(o.b),\(o.c),\(o.d),\(o.e),\(o.f),\(o.g) cur=\(o.curation) " +
    "prepare=\(o.prepare) transient=\(o.transient) featured=\(o.featured) pred=\(o.pred.map { "\($0.description)" } ?? "nil")"
}

// mixed widths: Int16 at [sp+0], Bool [sp+2], Int32 [sp+4], UInt8 [sp+8], Int64 [sp+16], Bool [sp+24]
@inline(never)
public func hMixed(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ x6: Int, _ x7: Int,
                   _ s: Int16, _ b: Bool, _ i: Int32, _ u: UInt8, _ l: Int64, _ z: Bool) -> String {
    "x=\(x0),\(x1),\(x2),\(x3),\(x4),\(x5),\(x6),\(x7) s=\(s) b=\(b) i=\(i) u=\(u) l=\(l) z=\(z)"
}

// a class method (vtable on the guest side, a symbol natively): Bools then a String
open class Box {
    public init() {}
    @inline(never)
    open func pack(_ x0: Int, _ x1: Int, _ x2: Int, _ x3: Int, _ x4: Int, _ x5: Int, _ b1: Bool, _ b2: Bool,
                   _ b3: Bool, _ b4: Bool, _ name: String) -> String {
        // self in x20; x0-x5, b1 b2 in x6 x7, b3 at [sp], b4 at [sp+1], name at [sp+8] (two words)
        "x=\(x0),\(x1),\(x2),\(x3),\(x4),\(x5) b=\(b1),\(b2),\(b3),\(b4) name=\(name)"
    }
}
