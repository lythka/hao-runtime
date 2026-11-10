// async ladder 10: indirect results (large structs returned from async
// functions: x8 on arm64, rax on x86_64 swiftcc), across suspension points,
// through generics, child tasks (task future result buffers) and groups.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

struct Big: Sendable, Equatable {
    var a, b, c, d, e, f, g, h: Int
    var name: String
    var x: Double
    func sum() -> Int { a + b + c + d + e + f + g + h }
}

func makeBig(_ n: Int) async -> Big {
    await Task.yield()
    var r = Big(a: n, b: n + 1, c: n + 2, d: n + 3, e: n + 4, f: n + 5, g: n + 6, h: n + 7, name: "big\(n)", x: Double(n) / 4)
    try? await Task.sleep(nanoseconds: 500_000)
    r.h += 100
    return r
}

func makeBigThrowing(_ n: Int) async throws -> Big {
    let b = await makeBig(n)
    if n < 0 { throw CancellationError() }
    return b
}

func generic<T: Sendable>(_ v: T) async -> (T, T, Int) {
    await Task.yield()
    return (v, v, 7)
}

// array-of-big result through a child task and a generic result type
func bigs(_ n: Int) async -> [Big] {
    var out: [Big] = []
    for i in 0..<n { out.append(await makeBig(i)) }
    return out
}

print("start")
let b1 = await makeBig(5)
print("b1", b1.name, b1.sum(), b1.x)
let b2 = try! await makeBigThrowing(9)
print("b2", b2.name, b2.sum(), b2.x)
do { _ = try await makeBigThrowing(-1); print("no throw") } catch { print("threw", error is CancellationError) }
let (g1, g2, k) = await generic(b1)
print("generic", g1 == b1, g2.name, k)
let t = Task { await makeBig(42) }
let tb = await t.value
print("task", tb.name, tb.sum())
let groupSum = await withTaskGroup(of: Big.self, returning: Int.self) { g in
    for i in 1...6 { g.addTask { await makeBig(i * 10) } }
    var s = 0
    for await b in g { s += b.sum() }
    return s
}
print("group", groupSum)
let arr = await Task.detached { await bigs(4) }.value
print("array", arr.map(\.name), arr.map { $0.sum() }.reduce(0, +))
async let l1 = makeBig(1)
async let l2 = makeBig(2)
let (r1, r2) = await (l1, l2)
print("async let", r1.name, r2.name, r1.sum() + r2.sum())
print("done")
