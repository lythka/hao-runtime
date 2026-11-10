// async ladder 11: nested tasks: task groups inside task groups, throwing and
// discarding groups, async let inside group children, unstructured tasks
// awaited from children, recursion through tasks.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

func leaf(_ x: Int) async -> Int {
    await Task.yield()
    return x * x
}

func inner(_ base: Int) async -> Int {
    await withTaskGroup(of: Int.self) { g in
        for i in 1...4 { g.addTask { await leaf(base + i) } }
        return await g.reduce(0, +)
    }
}

func fib(_ n: Int) async -> Int {
    if n < 2 { return n }
    async let a = fib(n - 1)
    async let b = fib(n - 2)
    return await a + b
}

actor Counter {
    var n = 0
    func add(_ k: Int) { n += k }
}

print("start")
let outer = await withTaskGroup(of: Int.self) { g in
    for b in stride(from: 0, to: 40, by: 10) { g.addTask { await inner(b) } }
    var s = 0
    for await v in g { s += v }
    return s
}
print("nested groups =", outer)

let thrown: Int
do {
    thrown = try await withThrowingTaskGroup(of: Int.self) { g in
        for i in 1...5 {
            g.addTask {
                try await withThrowingTaskGroup(of: Int.self) { h in
                    h.addTask { await leaf(i) }
                    h.addTask { try await Task.sleep(nanoseconds: 200_000); return i }
                    return try await h.reduce(0, +)
                }
            }
        }
        return try await g.reduce(0, +)
    }
} catch { thrown = -1 }
print("throwing nested =", thrown)

let counter = Counter()
await withDiscardingTaskGroup { g in
    for i in 1...20 {
        g.addTask {
            async let x = leaf(i)
            await counter.add(await x)
        }
    }
}
print("discarding =", await counter.n)

let chained = await Task {
    let t1 = Task { await leaf(3) }
    let t2 = Task.detached { await inner(1) }
    return await t1.value + t2.value
}.value
print("tasks in tasks =", chained)
print("fib(12) via async let =", await fib(12))
print("done")
