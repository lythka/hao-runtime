// The native side of tests/guest/swiftcc.sh's helper cases: Swift functions in each
// swiftcc difference kind between Apple arm64 and x86_64
// that no system library exports today,
// next to controls of the same width that the register bridge places correctly.
// Built with library evolution and a module interface for x86_64 (the native slice
// under hao), arm64 (the native reference) and arm64e, so a generator that reads
// .swiftinterface files can type it as it types an SDK module.
// Every function returns its arguments as text: a wrong register or stack slot
// shows as a wrong number in the position it came from.
import ObjectiveC

@inline(never) func hx<T: BinaryInteger>(_ v: [T]) -> String {
    v.map { String($0, radix: 16) }.joined(separator: " ")
}
@inline(never) func fx(_ v: [Double]) -> String {
    v.map { "\($0)" }.joined(separator: " ")
}

// 16 UInt8: arm64 x0-x7, then bytes 8-15 packed into one stack word (offsets 0..7);
// x86_64 rdi..r9 for bytes 0-5, then one 8-byte slot per byte (UUID.init(uuid:)).
public func hNarrow16(_ a0: UInt8, _ a1: UInt8, _ a2: UInt8, _ a3: UInt8, _ a4: UInt8, _ a5: UInt8, _ a6: UInt8,
                      _ a7: UInt8, _ a8: UInt8, _ a9: UInt8, _ a10: UInt8, _ a11: UInt8, _ a12: UInt8, _ a13: UInt8,
                      _ a14: UInt8, _ a15: UInt8) -> String {
    hx([a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15])
}
// 12 Int16: 4 packed at offsets 0, 2, 4, 6.
public func hNarrow16x12(_ a0: Int16, _ a1: Int16, _ a2: Int16, _ a3: Int16, _ a4: Int16, _ a5: Int16, _ a6: Int16,
                         _ a7: Int16, _ a8: Int16, _ a9: Int16, _ a10: Int16, _ a11: Int16) -> String {
    hx([a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11])
}
// 10 Int32: 2 packed at offsets 0, 4.
public func hNarrow32x10(_ a0: Int32, _ a1: Int32, _ a2: Int32, _ a3: Int32, _ a4: Int32, _ a5: Int32, _ a6: Int32,
                         _ a7: Int32, _ a8: Int32, _ a9: Int32) -> String {
    hx([a0, a1, a2, a3, a4, a5, a6, a7, a8, a9])
}
// 10 Float: v0-v7, then 2 floats packed at offsets 0, 4; x86_64 xmm0-7, then 8-byte slots.
public func hFloat10(_ a0: Float, _ a1: Float, _ a2: Float, _ a3: Float, _ a4: Float, _ a5: Float, _ a6: Float,
                     _ a7: Float, _ a8: Float, _ a9: Float) -> String {
    fx([a0, a1, a2, a3, a4, a5, a6, a7, a8, a9].map(Double.init))
}
// 9 Double and no integer argument: arm64 stack word 0 is the 9th double, x86_64 slot 0 too,
// but the bridge puts x6 into slot 0 (SPRotation3D.init(position:target:up:)).
public func hDouble9(_ a0: Double, _ a1: Double, _ a2: Double, _ a3: Double, _ a4: Double, _ a5: Double,
                     _ a6: Double, _ a7: Double, _ a8: Double) -> String {
    fx([a0, a1, a2, a3, a4, a5, a6, a7, a8])
}
// Control: 8 Int and 9 Double. x6, x7 go to slots 0, 1 and guest word 0 (the 9th double)
// to slot 2, which is where x86_64 reads it.
public func hMix8x9(_ i0: Int, _ i1: Int, _ i2: Int, _ i3: Int, _ i4: Int, _ i5: Int, _ i6: Int, _ i7: Int,
                    _ a0: Double, _ a1: Double, _ a2: Double, _ a3: Double, _ a4: Double, _ a5: Double,
                    _ a6: Double, _ a7: Double, _ a8: Double) -> String {
    hx([i0, i1, i2, i3, i4, i5, i6, i7]) + " | " + fx([a0, a1, a2, a3, a4, a5, a6, a7, a8])
}
// 25 Int: 17 arm64 stack words, one more than the 16 the bridge copies (DateComponents.init).
public func hWords25(_ a0: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int, _ a7: Int,
                     _ a8: Int, _ a9: Int, _ a10: Int, _ a11: Int, _ a12: Int, _ a13: Int, _ a14: Int, _ a15: Int,
                     _ a16: Int, _ a17: Int, _ a18: Int, _ a19: Int, _ a20: Int, _ a21: Int, _ a22: Int,
                     _ a23: Int, _ a24: Int) -> String {
    hx([a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22,
        a23, a24])
}
// Control: 24 Int, exactly 16 arm64 stack words.
public func hWords24(_ a0: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int, _ a7: Int,
                     _ a8: Int, _ a9: Int, _ a10: Int, _ a11: Int, _ a12: Int, _ a13: Int, _ a14: Int, _ a15: Int,
                     _ a16: Int, _ a17: Int, _ a18: Int, _ a19: Int, _ a20: Int, _ a21: Int, _ a22: Int,
                     _ a23: Int) -> String {
    hx([a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22,
        a23])
}
// Control in the shape of _assertionFailure(_:_:file:line:flags:):
// (i64, i64, i8, i64, ptr, i64, i64, i8, i64, i32), the last two in arm64 stack words 0 and 1.
public func hTen(_ a0: Int, _ a1: Int, _ a2: UInt8, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int, _ a7: UInt8,
                 _ a8: Int, _ a9: UInt32) -> String {
    hx([UInt64(a0), UInt64(a1), UInt64(a2), UInt64(a3), UInt64(a4), UInt64(a5), UInt64(a6), UInt64(a7), UInt64(a8),
        UInt64(a9)])
}
// 8-byte vectors: arm64 <2 x float> in d0 (and d1), x86_64 float, float in xmm0, xmm1 (and xmm2, xmm3).
public func hSIMD2(_ a: SIMD2<Float>, _ b: SIMD2<Float>) -> SIMD2<Float> {
    SIMD2(a.x * 1000 + a.y * 100 + b.x * 10 + b.y, a.y - a.x)
}
public func hSIMD2Text(_ a: SIMD2<Float>) -> String { fx([Double(a.x), Double(a.y)]) }
// Control: a 16-byte vector is <4 x float> in q0 on arm64 and xmm0 on x86_64.
public func hSIMD4(_ a: SIMD4<Float>) -> SIMD4<Float> { SIMD4(a.w, a.z, a.y, a.x) }
// Int128 after one Int: x0, x1:x2 on Darwin arm64 (measured: no even-pair alignment) and
// rdi, rsi:rdx on x86_64, so the register bridge places it (a control).
public func hI128(_ a: Int, _ b: Int128) -> String { String(a, radix: 16) + " " + String(b, radix: 16) }
// Int128 after 5 Int: arm64 x5:x6; x86_64 has only r9 left, so the whole i128 goes to the
// stack (slots 0-1) and r9 is unused: the bridge hands x5 to r9 and the high half x6 to slot 0.
public func hI128At5(_ a0: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ b: Int128) -> String {
    hx([a0, a1, a2, a3, a4]) + " " + String(b, radix: 16)
}
// Int128 after 7 Int: arm64 x0-x6 and the i128 on the stack (words 0-1; it may not straddle
// x7 and the stack); x86_64 the 7th Int in slot 0 and the i128 16-byte aligned in slots 2-3,
// where the bridge puts guest words 0-1 (x7 goes to slot 1): placed right (a control).
public func hI128Late(_ a0: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int,
                      _ b: Int128) -> String {
    hx([a0, a1, a2, a3, a4, a5, a6]) + " " + String(b, radix: 16)
}
// Control: Int128 first, x0:x1 = rdi:rsi.
public func hI128First(_ b: Int128, _ a: Int) -> String { String(b, radix: 16) + " " + String(a, radix: 16) }
// ObjC BOOL: bool (i1) on arm64, signed char (i8) on x86_64.
public func hObjCBool(_ b: ObjCBool) -> String { b.boolValue ? "yes" : "no" }
public func hMakeObjCBool(_ v: Int) -> ObjCBool { ObjCBool(v & 1 == 1) }

public final class K {
    public let n: Int
    public init(_ n: Int) { self.n = n }
}
// Multi-payload enums of class references. Up to 16 payload tags take bits 63-60 on both
// targets; with 17, arm64 continues into the low spare bits (0xF000000000000007) and
// x86_64 into bits 59-56 (0xFF00000000000007), so the same value has different bits.
@frozen public enum E4 { case c0(K), c1(K), c2(K), c3(K) }
@frozen public enum E17 {
    case c0(K), c1(K), c2(K), c3(K), c4(K), c5(K), c6(K), c7(K), c8(K), c9(K), c10(K), c11(K), c12(K), c13(K),
         c14(K), c15(K), c16(K)
}
public func hE4(_ e: E4) -> String {
    switch e { case .c0(let k): "c0 \(k.n)"; case .c1(let k): "c1 \(k.n)"; case .c2(let k): "c2 \(k.n)"; case .c3(let k): "c3 \(k.n)" }
}
public func hE17(_ e: E17) -> String {
    switch e {
    case .c0(let k): "c0 \(k.n)"; case .c1(let k): "c1 \(k.n)"; case .c2(let k): "c2 \(k.n)"
    case .c3(let k): "c3 \(k.n)"; case .c4(let k): "c4 \(k.n)"; case .c5(let k): "c5 \(k.n)"
    case .c6(let k): "c6 \(k.n)"; case .c7(let k): "c7 \(k.n)"; case .c8(let k): "c8 \(k.n)"
    case .c9(let k): "c9 \(k.n)"; case .c10(let k): "c10 \(k.n)"; case .c11(let k): "c11 \(k.n)"
    case .c12(let k): "c12 \(k.n)"; case .c13(let k): "c13 \(k.n)"; case .c14(let k): "c14 \(k.n)"
    case .c15(let k): "c15 \(k.n)"; case .c16(let k): "c16 \(k.n)"
    }
}
public func hMakeE17(_ i: Int, _ k: K) -> E17 {
    switch i {
    case 0: .c0(k); case 1: .c1(k); case 2: .c2(k); case 3: .c3(k); case 4: .c4(k); case 5: .c5(k); case 6: .c6(k)
    case 7: .c7(k); case 8: .c8(k); case 9: .c9(k); case 10: .c10(k); case 11: .c11(k); case 12: .c12(k)
    case 13: .c13(k); case 14: .c14(k); case 15: .c15(k); default: .c16(k)
    }
}
