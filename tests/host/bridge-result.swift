// The unsymbolized arm64 entry forwards x8 to a callee returning five words.
private struct Wide {
    let a, b, c, d, e: UInt64
}

@_cdecl("bridge_result")
public func bridgeResult(_ entry: UnsafeRawPointer) -> Bool {
    let callback = unsafeBitCast(entry, to: (@convention(thin) () -> Wide).self)
    let result = callback()
    return (result.a, result.b, result.c, result.d, result.e) == (17, 17, 17, 17, 17)
}
