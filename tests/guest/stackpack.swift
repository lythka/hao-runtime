// Guest calls of native Swift functions with packed stack arguments (tests/guest/stackpack.sh,
// helper tests/guest/stackpackhelper.swift). One case per process: opts, mgr, mixed, box.
import Foundation
import StackPackHelper

final class Pred: NSObject { override var description: String { "Pred(42)" } }

// the words just past the packed arguments hold a stale stack address, as Photos' did
@inline(never) func dirty() -> Int {
    var junk = [Int](repeating: 0x1_10cc_3702, count: 64)
    return withUnsafeMutableBytes(of: &junk) { $0.count }
}

@inline(never) func opts(_ p: NSObject?) -> Opts {
    Opts(rel: .assets(3), a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, curation: 1,
         prepare: false, transient: true, featured: false, pred: p)
}

let args = CommandLine.arguments
switch args.count > 1 ? args[1] : "" {
case "list":
    print("opts mgr mixed box")
case "opts":
    for p in [nil, Pred()] as [NSObject?] {
        _ = dirty()
        let o = opts(p)
        print("opts guest: transient=\(o.transient) featured=\(o.featured) prepare=\(o.prepare) cur=\(o.curation) pred=\(o.pred?.description ?? "nil")")
        print("opts native: \(describeOpts(o))")
    }
case "mgr":
    for p in [nil, Pred()] as [NSObject?] {
        _ = dirty()
        let m = Mgr(tag: 7, opts: opts(p))
        print("mgr: \(m.describe())")
        let s = Mgr(tag: "s", opts: opts(p))
        print("mgr string: \(s.describe())")
    }
case "mixed":
    print("mixed: \(hMixed(1, 2, 3, 4, 5, 6, 7, 8, -3, true, -70000, 200, 1 << 40, true))")
    print("mixed2: \(hMixed(-1, -2, -3, -4, -5, -6, -7, -8, 32767, false, Int32.min, 255, -1, false))")
case "box":
    let b = Box()
    print("box: \(b.pack(1, 2, 3, 4, 5, 6, true, false, true, true, "hello, packed world"))")
    print("box2: \(b.pack(9, 8, 7, 6, 5, 4, false, true, false, true, "x"))")
default:
    print("usage: stackpack opts|mgr|mixed|box")
}
