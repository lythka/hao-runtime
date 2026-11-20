import ReportLib

// The script omits field reflection for this guest struct. The walk classifies the
// tuple containing it and a sibling nil as a value native code would misread.
struct Unreflected { var tag: Any.Type?? }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func check<T>(_ value: T) {
    let snapshot = withUnsafeBytes(of: value) { Array($0) }
    snapshot.withUnsafeBytes { expected in
        _ = observe(value, expected.baseAddress!)
    }
}

let elements = [17, 29, 41]
elements.withUnsafeBufferPointer { buffer in
    // Whole-word fields avoid padding. Neither metatype nil carries reference ownership.
    let inline: InlineArray<2, UInt> = [11, 13]
    check((inline, Any.Type??.none, buffer.baseAddress!, buffer.count))
    // Fill the three inline payload words in Any. Only the dynamic payload lacks field metadata.
    let opaque: Any = (Unreflected(tag: .none), Any.Type??.none, UInt(19))
    check((opaque, Any.Type??.none, buffer.baseAddress!, buffer.count))
    var original: ReportValue = (inline, .some(nil), buffer.baseAddress!, buffer.count)
    withUnsafeMutablePointer(to: &original) { pointer in
        let result = nativeValue(pointer, ReportValue.self)
        withUnsafeBytes(of: result) { bytes in
            let source = UnsafeRawBufferPointer(start: pointer, count: bytes.count)
            print("returned unchanged", bytes.elementsEqual(source))
        }
    }
}
