// async ladder 9: async functions with stack arguments (more than the
// argument registers on either side: x0-x7 on arm64, 6 GPRs on x86_64),
// suspending in between, called directly, through closures, child tasks and
// a protocol witness, with mixed integer / floating-point arguments.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

func many(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
          _ g: Int, _ h: Int, _ i: Int, _ j: Int, _ k: Int, _ l: Int) async -> Int {
    await Task.yield()
    let s1 = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f
    await Task.yield()
    return s1 + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k + 12 * l
}

func mixed(_ a: Int, _ x: Double, _ b: Int, _ y: Double, _ c: Int, _ z: Double,
           _ d: Int, _ w: Double, _ e: Int, _ v: Double, _ f: Int, _ u: Double,
           _ g: Int, _ t: Double, _ h: Int, _ s: Double, _ i: Int, _ r: Double,
           _ j: Int, _ q: Double) async -> Double {
    try? await Task.sleep(nanoseconds: 1_000_000)
    let ints = Double(a + b + c + d + e + f + g + h + i + j)
    return ints + x + y + z + w + v + u + t + s + r + q
}

// generic + many arguments: metadata and witness tables take registers too
func gen<T: Numeric & Sendable>(_ a: T, _ b: T, _ c: T, _ d: T, _ e: T, _ f: T, _ g: T, _ h: T, _ i: T) async -> T {
    await Task.yield()
    return a + b + c + d + e + f + g + h + i
}

protocol Summer: Sendable {
    func sum(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int
}
struct S: Summer {
    let bias: Int
    func sum(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int, _ g: Int, _ h: Int, _ i: Int, _ j: Int) async -> Int {
        await Task.yield()
        return bias + a + b + c + d + e + f + g + h + i + j
    }
}
func viaWitness(_ s: any Summer) async -> Int { await s.sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) }

print("start")
print("many =", await many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12))
print("mixed =", await mixed(1, 0.5, 2, 0.25, 3, 0.125, 4, 1.5, 5, 2.5, 6, 3.5, 7, 4.5, 8, 5.5, 9, 6.5, 10, 7.5))
print("gen Int =", await gen(1, 2, 3, 4, 5, 6, 7, 8, 9))
print("gen Double =", await gen(0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5))
print("witness =", await viaWitness(S(bias: 100)))
let f: @Sendable (Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int, Int) async -> Int = many
print("closure =", await f(12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))
let total = await withTaskGroup(of: Int.self) { g in
    for n in 0..<8 { g.addTask { await many(n, n, n, n, n, n, n, n, n, n, n, n) } }
    var s = 0
    for await v in g { s += v }
    return s
}
print("group =", total)
let t = Task.detached { await many(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2) &+ (await gen(1, 1, 1, 1, 1, 1, 1, 1, 1)) }
print("detached =", await t.value)
print("done")
