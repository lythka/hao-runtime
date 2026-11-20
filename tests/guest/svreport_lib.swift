// Read the argument inside the native call, before a return conversion can undo a patch.
@inline(never) public func observe<T, U>(_ value: T, _ expected: U) -> U {
    let original = expected as! UnsafeRawPointer
    withUnsafeBytes(of: value) { bytes in
        let reference = UnsafeRawBufferPointer(start: original, count: bytes.count)
        if let i = bytes.indices.first(where: { bytes[$0] != reference[$0] }) {
            print("byte", i, "expected", reference[i], "received", bytes[i])
        } else {
            print("unchanged", bytes.count)
        }
    }
    return expected
}

public typealias ReportValue = (InlineArray<2, UInt>, Any.Type??, UnsafePointer<Int>, Int)

// Set the nil in the native encoding and keep its original bytes for comparison after return.
@inline(never) public func nativeValue<T, U>(_ storage: U, _: T.Type) -> T {
    let original = storage as! UnsafeMutablePointer<ReportValue>
    original.pointee.1 = .none
    return original.pointee as! T
}
