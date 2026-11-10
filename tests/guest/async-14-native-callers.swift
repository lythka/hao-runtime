// async ladder 14: native async code calling translated async functions
// with x86_64 stack arguments. hao-lib: async-lib/HaoNativeAsync.swift
// (run-async.sh builds it as a universal dylib; under hao its x86_64 slice
// runs natively). The guest functions here are tail-called by native
// swifttailcc code that grew the argument area, so on return they must pop
// their own argument area for the native executor (8, 24, 40, 56 bytes).
// hao takes that area from the guest function's own code (src/swiftcc.c).
import HaoNativeAsync
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

struct Odd: Error, CustomStringConvertible {
    let v: Int
    var description: String { "Odd(\(v))" }
}

@Sendable func two(_ a: Int, _ b: Int) async -> Int {
    await Task.yield()
    return a * b
}
@Sendable func seven(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int) async -> Int {
    await Task.yield()
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g
}
@Sendable func eight(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int) async -> Int {
    await Task.yield()
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h
}
@Sendable func nine(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int) async -> Int {
    await Task.yield()
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i
}
@Sendable func ten(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int {
    await Task.yield()
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j
}
// no suspension: returns straight to the native resume function's continuation
@Sendable func tenNoYield(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int {
    return a - b + c - d + e - f + g - h + i - j
}
@Sendable func twelve(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                      _ g: Int, _ h: Int, _ i: Int, _ j: Int, _ k: Int, _ l: Int) async -> Int {
    await Task.yield()
    let s = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f
    try? await Task.sleep(nanoseconds: 100_000)
    return s + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k + 12 * l
}
@Sendable func tenThrows(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async throws -> Int {
    await Task.yield()
    if a % 2 == 1 { throw Odd(v: a + j) }
    return a + b + c + d + e + f + g + h + i + j
}
@Sendable func tenMixed(_ a: Int, _ x: Double, _ b: Int, _ y: Double, _ c: Int, _ z: Double,
                        _ d: Int, _ w: Double, _ e: Int, _ f: Int, _ g: Int, _ h: Int) async -> Double {
    await Task.yield()
    return Double(a + b + c + d + e + f + g + h) + x + y + z + w
}
@Sendable func tenWide(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Wide {
    await Task.yield()
    return Wide(a + b, c + d, e + f, g + h, i + j, a * j)
}

struct Adder: TenSink {
    let k: Int
    func put(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int,
             _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int {
        await Task.yield()
        return k * (a + b + c + d + e) + f + g + h + i + j
    }
}
final class Weigher: TenSink {
    let w: Int
    init(_ w: Int) { self.w = w }
    func put(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int,
             _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int {
        return w &* (a - b + c - d + e - f + g - h + i - j)
    }
}

print("two", await nativeCallTwo(two))
print("seven", await nativeCallSeven(seven))
print("eight", await nativeCallEight(eight))
print("nine", await nativeCallNine(nine))
print("ten", await nativeCallTen(ten))
print("ten-noyield", await nativeCallTen(tenNoYield))
print("twelve", await nativeCallTwelve(twelve))
print("ten-now", await nativeCallTenNow(ten))
let k = 5
print("ten-closure", await nativeCallTen { a, b, c, d, e, f, g, h, i, j in
    await Task.yield(); return k * (a + b + c + d + e + f + g + h + i + j) })
print("ten-twice", await nativeCallTenTwice(ten))
print("ten-calls-two", await nativeTenCallsTwo(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, two))
print("ten-calls-twelve", await nativeTenCallsTwelveLater(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, twelve))
print("throws-even", await nativeCallTenThrows(tenThrows, 2))
print("throws-odd", await nativeCallTenThrows(tenThrows, 3))
print("mixed", await nativeCallTenMixed(tenMixed))
print("wide", await nativeCallTenWide(tenWide))
print("guest-to-native-ten", await nativeTen(1, 2, 3, 4, 5, 6, 7, 8, 9, 10))
print("group", await nativeGroupTen(ten, 40))
print("sink-struct", await nativeSink(Adder(k: 3)))
print("sink-class", await nativeSink(Weigher(7)))
print("sink-any", await nativeSinkAny(Adder(k: 2)))
var tot = 0
for r in 0..<50 { tot &+= await nativeCallTen(ten) &* r &+ (await nativeCallEight(eight)) }
print("loop", tot)
print("done")
