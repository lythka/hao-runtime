@frozen public struct Wide {
    public let a, b, c, d, e: Int
    public init(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int) {
        self.a = a; self.b = b; self.c = c; self.d = d; self.e = e
    }
}

public protocol PackedRequirement {
    func identity(_ x: Int) -> Int
    func packed(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                _ g: Int, _ h: Int, _ i: Int8, _ j: Int16) -> Int
    func floating(_ a: Double, _ b: Double, _ c: Double, _ d: Double, _ e: Double,
                  _ f: Double, _ g: Double, _ h: Double, _ i: Double, _ j: Int8) -> Double
    func wide(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
              _ g: Int, _ h: Int, _ i: Int8, _ j: Int16) -> Wide
    func generic<T>(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                    _ g: Int, _ h: Int, _ i: Int8, _ j: Int16, _ value: T) -> (T, Int)
}

@inline(never) public func invokePacked(_ value: any PackedRequirement) -> Int {
    value.packed(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
}

@inline(never) public func invoke(_ value: any PackedRequirement) {
    print("exact witness", value.identity(42))
    print("packed witness", value.packed(1, 2, 3, 4, 5, 6, 7, 8, 9, 10))
    print("negative witness", value.packed(1, 2, 3, 4, 5, 6, 7, 8, -9, -10))
    print("FP spill", value.floating(1.25, 2.5, 3.75, 4, 5, 6, 7, 8, 9.5, -12))
    let w = value.wide(1, 2, 3, 4, 5, 6, 7, 8, -9, 10)
    print("indirect result", w.a, w.b, w.c, w.d, w.e)
    let (s, n) = value.generic(1, 2, 3, 4, 5, 6, 7, 8, 9, -10, "payload")
    print("generic result", s, n)
}
