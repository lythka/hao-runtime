@frozen public struct Wide {
    public let a, b, c, d, e: Int
    public init(_ n: Int) { a = n; b = n + 1; c = n + 2; d = n + 3; e = n + 4 }
}
public protocol Associated {
    associatedtype First
    associatedtype Second
    var first: First { get }
    var second: Second { get }
    func packed(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                _ g: Int, _ h: Int, _ i: Int8, _ j: Int16) -> Int
}
@inline(never) public func expected<T>(_ t: T.Type) {
    print("expected", UInt(bitPattern: unsafeBitCast(t, to: UnsafeRawPointer.self)), MemoryLayout<T>.size)
}
@inline(never) public func invoke<T: Associated>(_ x: T) {
    print("packed", x.packed(1, 2, 3, 4, 5, 6, 7, 8, 9, 10))
    let a = x.first
    let b = x.second
    // Resolve independently after the callbacks. The bridge must also work
    // when the witness's metadata slot was still a tagged mangled type.
    expected(T.First.self)
    expected(T.Second.self)
    print("values", withUnsafeBytes(of: a) { $0.load(as: Int.self) },
          withUnsafeBytes(of: b) { $0.load(as: Int.self) })
    withExtendedLifetime((a, b)) {}
}
