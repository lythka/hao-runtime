// Native helper for tests/guest/xinhab.sh: reads enum cases that live in the extra inhabitants of a
// pointer payload, with this slice's encoding (x86_64: inhabitant i is the value i << 1, bit 0 being
// the ObjC tagged-pointer bit; arm64: the value i). Only the tag is inspected, nothing is retained.
public enum Three { case obj([Int]), a, b, c }

@inline(never) public func caseOfDouble(_ p: UnsafeRawPointer) -> Int {
    switch p.assumingMemoryBound(to: [Int]??.self).pointee {
    case .none: return 0
    case .some(.none): return 1
    case .some(.some): return 2
    }
}

@inline(never) public func caseOfThree(_ p: UnsafeRawPointer) -> Int {
    switch p.assumingMemoryBound(to: Three.self).pointee {
    case .obj: return 0
    case .a: return 1
    case .b: return 2
    case .c: return 3
    }
}

@inline(never) public func writeDouble(_ p: UnsafeMutableRawPointer, _ k: Int) {
    let q = p.assumingMemoryBound(to: [Int]??.self)
    switch k {
    case 0: q.initialize(to: .none)
    case 1: q.initialize(to: .some(.none))
    default: q.initialize(to: .some(.some([7])))
    }
}

@inline(never) public func writeThree(_ p: UnsafeMutableRawPointer, _ k: Int) {
    let q = p.assumingMemoryBound(to: Three.self)
    q.initialize(to: [Three.obj([7]), .a, .b, .c][k])
}
