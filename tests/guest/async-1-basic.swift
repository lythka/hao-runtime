// async ladder 1: one async function awaited from top-level code (async main).
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash
func add(_ a: Int, _ b: Int) async -> Int {
    return a + b
}

func twice(_ x: Int) async -> Int {
    let y = await add(x, x)
    return y
}

print("start")
let r = await twice(21)
print("twice(21) =", r)
var sum = 0
for i in 1...5 { sum += await add(i, i * 10) }
print("sum =", sum)
print("done")
