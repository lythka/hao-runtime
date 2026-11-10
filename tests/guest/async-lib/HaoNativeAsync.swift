// Native side of async-14-native-callers: built as a universal (x86_64 +
// arm64e) dylib by run-async.sh. Natively the test runs its arm64e slice;
// under hao the guest's calls bind to the x86_64 slice, so this code calls
// translated (guest) async functions from native x86_64 swifttailcc code.
//
// After a suspension, the native resume function is called by the executor
// (`callq *56(%r13); subq $8, %rsp`) and tail-calls the guest closure; with
// more than 6 integer arguments it first grows the x86_64 argument area and
// moves the return address down, so the guest function must pop its own
// argument area (7 args: 8, 8 args: 24, 9: 24, 10: 40, 12: 56).

public typealias Seven = @Sendable (Int, Int, Int, Int, Int, Int, Int) async -> Int
public typealias Eight = @Sendable (Int, Int, Int, Int, Int, Int, Int, Int) async -> Int
public typealias Nine = @Sendable (Int, Int, Int, Int, Int, Int, Int, Int, Int) async -> Int
public typealias Ten = @Sendable (Int, Int, Int, Int, Int, Int, Int, Int, Int, Int) async -> Int
public typealias Twelve = @Sendable (Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int) async -> Int
public typealias Two = @Sendable (Int, Int) async -> Int
public typealias TenThrows = @Sendable (Int, Int, Int, Int, Int, Int, Int, Int, Int, Int) async throws -> Int
public typealias TenMixed = @Sendable (Int, Double, Int, Double, Int, Double, Int, Double, Int, Int, Int, Int) async -> Double

public struct Wide: Sendable, Equatable {
    public var a, b, c, d, e, f: Int
    public init(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int) {
        self.a = a; self.b = b; self.c = c; self.d = d; self.e = e; self.f = f
    }
}
public typealias TenWide = @Sendable (Int, Int, Int, Int, Int, Int, Int, Int, Int, Int) async -> Wide

@inline(never) public func nativeCallTwo(_ f: Two) async -> Int {
    await Task.yield()
    return await f(3, 4)
}

@inline(never) public func nativeCallSeven(_ f: Seven) async -> Int {
    await Task.yield()
    return await f(1, 2, 3, 4, 5, 6, 7)
}

@inline(never) public func nativeCallEight(_ f: Eight) async -> Int {
    await Task.yield()
    return await f(1, 2, 3, 4, 5, 6, 7, 8)
}

@inline(never) public func nativeCallNine(_ f: Nine) async -> Int {
    await Task.yield()
    return await f(1, 2, 3, 4, 5, 6, 7, 8, 9)
}

@inline(never) public func nativeCallTen(_ f: Ten) async -> Int {
    await Task.yield()
    return await f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
}

@inline(never) public func nativeCallTwelve(_ f: Twelve) async -> Int {
    await Task.yield()
    return await f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)
}

// No suspension first: the native function (called through the bridge)
// tail-calls the guest closure directly.
@inline(never) public func nativeCallTenNow(_ f: Ten) async -> Int {
    return await f(10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
}

// Native code with its own stack arguments that tail-calls a guest closure
// with fewer (the argument area shrinks) and with more.
@inline(never) public func nativeTenCallsTwo(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int,
                                             _ g: Int, _ h: Int, _ i: Int, _ j: Int, _ k: Int, _ f: Two) async -> Int {
    return await f(a + b + c + d + e, g + h + i + j + k)
}

@inline(never) public func nativeTenCallsTwelveLater(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int,
                                                     _ g: Int, _ h: Int, _ i: Int, _ j: Int, _ k: Int, _ f: Twelve) async -> Int {
    await Task.yield()
    return await f(a, b, c, d, e, g, h, i, j, k, a * k, b * j)
}

// Native code that awaits the guest closure and works with the result.
@inline(never) public func nativeCallTenTwice(_ f: Ten) async -> Int {
    await Task.yield()
    let a = await f(1, 1, 1, 1, 1, 1, 1, 1, 1, 1)
    await Task.yield()
    let b = await f(2, 2, 2, 2, 2, 2, 2, 2, 2, 2)
    return a * 1000 + b
}

@inline(never) public func nativeCallTenThrows(_ f: TenThrows, _ x: Int) async -> String {
    await Task.yield()
    do {
        let r = try await f(x, 1, 2, 3, 4, 5, 6, 7, 8, 9)
        return "value \(r)"
    } catch {
        return "caught \(error)"
    }
}

@inline(never) public func nativeCallTenMixed(_ f: TenMixed) async -> Double {
    await Task.yield()
    return await f(1, 0.5, 2, 0.25, 3, 0.125, 4, 2.0, 5, 6, 7, 8)
}

@inline(never) public func nativeCallTenWide(_ f: TenWide) async -> Wide {
    await Task.yield()
    return await f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
}

// Child tasks on the cooperative pool: native executors on several threads.
@inline(never) public func nativeGroupTen(_ f: @escaping Ten, _ n: Int) async -> Int {
    await withTaskGroup(of: Int.self) { g in
        for k in 0..<n {
            g.addTask { await f(k, k, k, k, k, k, k, k, k, k) }
        }
        var s = 0
        for await v in g { s += v }
        return s
    }
}

// The guest -> native direction with stack arguments: a native async
// function with 10 arguments that suspends.
@inline(never) public func nativeTen(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int,
                                     _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int {
    await Task.yield()
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j
}

// Generic native code calling a guest conformance's async witness after a
// suspension: 10 arguments plus the Self metadata and witness table (12
// integer-class arguments, 56 bytes of x86_64 argument area), reached
// through the guest's protocol witness thunk.
public protocol TenSink: Sendable {
    func put(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int,
             _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int
}

@inline(never) public func nativeSink<S: TenSink>(_ s: S) async -> Int {
    await Task.yield()
    return await s.put(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
}

@inline(never) public func nativeSinkAny(_ s: any TenSink) async -> Int {
    await Task.yield()
    return await s.put(10, 20, 30, 40, 50, 60, 70, 80, 90, 100)
}
