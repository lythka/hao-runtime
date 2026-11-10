// async ladder 15: narrow Swift results across the bridge.
// hao-lib: async-lib/HaoNarrow.swift
// Native x86_64 Swift functions return Bool/Int8/Int16 in al/ax with the
// other bits of rax undefined (here: the argument's bits), and translated
// callers read w0 the same way arm64 Swift code does (masking or
// extending it); guest closures returning narrow values to native callers
// are the reverse direction. Every value printed must match native arm64.
import HaoNarrow
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)

let base = CommandLine.arguments.count   // 1: not a compile-time constant
for v in [base + 0x1_7f, base + 0x2_80fe, base - 2, base + 0x7fff_0000_ffff] {
    print("i8", narrowI8(v), "u8", narrowU8(v), "i16", narrowI16(v), "u16", narrowU16(v), "i32", narrowI32(v))
    let p = narrowPair(v)
    print("bool", narrowBool(v), "opt", narrowOptBool(v) as Any, "pair", p.0, p.1)
    if narrowBool(v) { print("  odd") } else { print("  even") }
    print("async", await narrowI8Async(v), await narrowBoolAsync(v))
    print("cb", callI8({ Int8(truncatingIfNeeded: $0 &* 3) }, v), callU16({ UInt16(truncatingIfNeeded: $0 &+ 0x1_0001) }, v),
          callBool({ $0 & 4 == 4 }, v))
    print("cb-async", await callI8Async({ a in await Task.yield(); return Int8(truncatingIfNeeded: a &- 5) }, v),
          await callBoolAsync({ a in await Task.yield(); return a & 8 == 8 }, v))
}
var mask = 0
for k in 0..<64 where narrowBool(base &+ k) { mask |= 1 << k }
print("mask", String(mask, radix: 16))
print("done")
