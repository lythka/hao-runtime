import ProtocolNative

struct Guest: PackedRequirement {
    let bias: Int
    @inline(never) func identity(_ x: Int) -> Int { x + bias }
    @inline(never) func packed(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                              _ g: Int, _ h: Int, _ i: Int8, _ j: Int16) -> Int {
        a + b + c + d + e + f + g + h + Int(i) * 100 + Int(j) * 10000 + bias
    }
    @inline(never) func floating(_ a: Double, _ b: Double, _ c: Double, _ d: Double, _ e: Double,
                                _ f: Double, _ g: Double, _ h: Double, _ i: Double, _ j: Int8) -> Double {
        a + b + c + d + e + f + g + h + i + Double(j) + Double(bias)
    }
    @inline(never) func wide(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                            _ g: Int, _ h: Int, _ i: Int8, _ j: Int16) -> Wide {
        Wide(a + b + bias, c + d, e + f, g + h, Int(i) * 100 + Int(j) * 10000)
    }
    @inline(never) func generic<T>(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                                  _ g: Int, _ h: Int, _ i: Int8, _ j: Int16, _ value: T) -> (T, Int) {
        (value, packed(a, b, c, d, e, f, g, h, i, j))
    }
}
if CommandLine.arguments.contains("--packed-only") {
    print("packed witness", invokePacked(Guest(bias: 0)))
} else {
    invoke(Guest(bias: 0))
    invoke(Guest(bias: 123))
}
