// The swifttailcc callee pop of guest Swift partials resumed by the native executor on the main queue
// (tests/guest/swiftpop.sh; WallpaperAgent's "autorelease pool page corrupted").
//
// Built with -Osize, the machine outliner gives the stepN partials that resume after `await pause()` (on the global
// executor, then a hop back to the main actor: a main-queue job whose ResumeTask is the partial) an outlined
// prologue, `mov x0, x30; bl OUTLINED_FUNCTION_k; mov x30, x0` where OUTLINED_FUNCTION_k does `sub sp, sp, #N` and
// the callee-saved stores. The partial then reads its own frame (`ldr x0, [sp, #0x10]`). A scan that takes a call
// as preserving sp sees those reads as incoming stack arguments (WallpaperAgent: 5 slots, a 40-byte pop), and a pop
// of 40 where swift::runJobInEstablishedExecutorContext's frame needs 8 puts the return address of its next call
// on its autorelease pool token: objc_autoreleasePoolPop then aborts with "page corrupted, magic 0".
// Each step's synchronous work keeps 10 values and two 16-word structs live, so the frames are large.
import Foundation
@MainActor final class Box { var n = 0 }
struct Big { var a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p: Int }
@inline(never) func pause() async {
    await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in DispatchQueue.global().async { k.resume() } }
}
@inline(never) func mix(_ a: Int, _ b: Int) -> Int { a &* 31 &+ b }
@inline(never) func fold(_ x: Big) -> Int { x.a &+ x.b &* 2 &+ x.c &* 3 &+ x.d &+ x.e &+ x.f &+ x.g &+ x.h &+ x.i &+ x.j &+ x.k &+ x.l &+ x.m &+ x.n &+ x.o &+ x.p }
@inline(never) func make(_ s: Int) -> Big { Big(a: s, b: s &+ 1, c: s &+ 2, d: s &+ 3, e: s &+ 4, f: s &+ 5, g: s &+ 6, h: s &+ 7, i: s &+ 8, j: s &+ 9, k: s &+ 10, l: s &+ 11, m: s &+ 12, n: s &+ 13, o: s &+ 14, p: s &+ 15) }
@MainActor @inline(never) func step1(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "1 %d", i)
    await pause()
    var v0 = mix(i, 1), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step2(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "2 %d", i)
    await pause()
    var v0 = mix(i, 2), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step3(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "3 %d", i)
    await pause()
    var v0 = mix(i, 3), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step4(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "4 %d", i)
    await pause()
    var v0 = mix(i, 4), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step5(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "5 %d", i)
    await pause()
    var v0 = mix(i, 5), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step6(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "6 %d", i)
    await pause()
    var v0 = mix(i, 6), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step7(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "7 %d", i)
    await pause()
    var v0 = mix(i, 7), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
@MainActor @inline(never) func step8(_ b: Box, _ i: Int) async -> Int {
    let s = NSString(format: "8 %d", i)
    await pause()
    var v0 = mix(i, 8), v1 = mix(v0, 2), v2 = mix(v1, 3), v3 = mix(v2, 4), v4 = mix(v3, 5)
    var v5 = mix(v4, 6), v6 = mix(v5, 7), v7 = mix(v6, 8), v8 = mix(v7, 9), v9 = mix(v8, 10)
    var big = make(v0 &+ v9)
    big.c = mix(big.c, v1); big.h = mix(big.h, v2)
    let r = fold(big) &+ fold(make(v3))
    v0 = mix(v0, r); v1 = mix(v1, v0); v2 = mix(v2, v1); v3 = mix(v3, v2); v4 = mix(v4, v3)
    v5 = mix(v5, v4); v6 = mix(v6, v5); v7 = mix(v7, v6); v8 = mix(v8, v7); v9 = mix(v9, v8)
    await Task.yield()
    b.n &+= s.length
    return v0 &+ v1 &+ v2 &+ v3 &+ v4 &+ v5 &+ v6 &+ v7 &+ v8 &+ v9 &+ r
}
let box = MainActor.assumeIsolated { Box() }
Task { @MainActor in
    var t = 0
    for i in 0..<200 {
        t &+= await step1(box, i)
        t &+= await step2(box, i)
        t &+= await step3(box, i)
        t &+= await step4(box, i)
        t &+= await step5(box, i)
        t &+= await step6(box, i)
        t &+= await step7(box, i)
        t &+= await step8(box, i)
    }
    print("done", box.n, t)
    exit(0)
}
dispatchMain()
