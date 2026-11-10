// Enum cases in a pointer's extra inhabitants across the guest/native boundary (tests/guest/xinhab.sh):
// guest code writes `[Int]??` and a four-case enum over `[Int]` with its own (arm64) static encoding and a
// native helper reads the case, then the helper writes and the guest reads. Natively both sides are arm64.
import xinhab_lib

func show(_ s: String, _ v: Int) { print(s, v) }

var buf = UnsafeMutableRawPointer.allocate(byteCount: 64, alignment: 16)
let d: [[Int]??] = [.none, .some(.none), .some(.some([1, 2]))]
for (k, v) in d.enumerated() {
    buf.assumingMemoryBound(to: [Int]??.self).initialize(to: v)
    show("guest wrote [Int]?? case \(k), native read", caseOfDouble(buf))
    buf.assumingMemoryBound(to: [Int]??.self).deinitialize(count: 1)
}
let t: [Three] = [.obj([1]), .a, .b, .c]
for (k, v) in t.enumerated() {
    buf.assumingMemoryBound(to: Three.self).initialize(to: v)
    show("guest wrote Three case \(k), native read", caseOfThree(buf))
    buf.assumingMemoryBound(to: Three.self).deinitialize(count: 1)
}
for k in 0..<3 {
    writeDouble(buf, k)
    let r: Int
    switch buf.assumingMemoryBound(to: [Int]??.self).pointee { case .none: r = 0; case .some(.none): r = 1; case .some(.some): r = 2 }
    show("native wrote [Int]?? case \(k), guest read", r)
}
for k in 0..<4 {
    writeThree(buf, k)
    let r: Int
    switch buf.assumingMemoryBound(to: Three.self).pointee { case .obj: r = 0; case .a: r = 1; case .b: r = 2; case .c: r = 3 }
    show("native wrote Three case \(k), guest read", r)
}
