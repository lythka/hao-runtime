// async ladder 2: structured concurrency, child tasks on the global executor.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash
func square(_ x: Int) async -> Int { x * x }

print("start")
let total = await withTaskGroup(of: Int.self) { group in
    for i in 1...10 { group.addTask { await square(i) } }
    var s = 0
    for await v in group { s += v }
    return s
}
print("sum of squares 1...10 =", total)
async let a = square(7)
async let b = square(8)
let ab = await a + b
print("async let 49+64 =", ab)
print("done")
