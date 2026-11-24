// async ladder 2: structured concurrency, child tasks on the global executor.
import Darwin
import Dispatch
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
// A ready child resumes next() synchronously. Draining many in succession
// must keep the native stack bounded across those guest continuations.
@concurrent
func sumReadyChildren() async -> Int {
    await withTaskGroup(of: Int.self) { group in
        let ready = DispatchGroup()
        for i in 1...4096 {
            ready.enter()
            group.addTask {
                defer { ready.leave() }
                return i
            }
        }
        await withCheckedContinuation { continuation in
            ready.notify(queue: .global()) { continuation.resume() }
        }
        var sum = 0
        while let value = await group.next() { sum += value }
        return sum
    }
}
print("sum of ready children =", await sumReadyChildren())
print("done")
