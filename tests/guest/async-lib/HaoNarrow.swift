// Native side of async-15-narrow-returns: built as a universal (x86_64 +
// arm64e) dylib by run-async.sh. Swift returns Bool, Int8 and Int16 in
// al/ax on x86_64 (w0 on arm64) without extending them: these functions
// leave the argument's other bits in rax (`movq %rdi, %rax`), and callers
// in both ABIs mask or extend the result themselves.

@inline(never) public func narrowI8(_ a: Int) -> Int8 { Int8(truncatingIfNeeded: a) }
@inline(never) public func narrowU8(_ a: Int) -> UInt8 { UInt8(truncatingIfNeeded: a) }
@inline(never) public func narrowI16(_ a: Int) -> Int16 { Int16(truncatingIfNeeded: a) }
@inline(never) public func narrowU16(_ a: Int) -> UInt16 { UInt16(truncatingIfNeeded: a) }
@inline(never) public func narrowI32(_ a: Int) -> Int32 { Int32(truncatingIfNeeded: a) }
@inline(never) public func narrowBool(_ a: Int) -> Bool { a & 1 == 1 }
@inline(never) public func narrowOptBool(_ a: Int) -> Bool? { a % 3 == 0 ? nil : a % 3 == 1 }
@inline(never) public func narrowPair(_ a: Int) -> (Bool, Int8) { (a & 2 == 2, Int8(truncatingIfNeeded: a)) }

@inline(never) public func narrowI8Async(_ a: Int) async -> Int8 {
    await Task.yield()
    return Int8(truncatingIfNeeded: a)
}
@inline(never) public func narrowBoolAsync(_ a: Int) async -> Bool {
    await Task.yield()
    return a & 1 == 1
}

// Native callers of guest closures returning narrow values.
@inline(never) public func callI8(_ f: (Int) -> Int8, _ a: Int) -> Int { Int(f(a)) }
@inline(never) public func callU16(_ f: (Int) -> UInt16, _ a: Int) -> Int { Int(f(a)) }
@inline(never) public func callBool(_ f: (Int) -> Bool, _ a: Int) -> Int { f(a) ? 1 : 0 }
@inline(never) public func callI8Async(_ f: @Sendable (Int) async -> Int8, _ a: Int) async -> Int {
    await Task.yield()
    return Int(await f(a))
}
@inline(never) public func callBoolAsync(_ f: @Sendable (Int) async -> Bool, _ a: Int) async -> Int {
    await Task.yield()
    return await f(a) ? 1 : 0
}
