// async ladder 12: errors thrown across suspension points: thrown after an
// await, through several async frames, out of child tasks and groups,
// rethrown by generic code, typed throws, defer ordering.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

enum Failure: Error, Equatable { case code(Int), message(String) }

func failAfterAwait(_ n: Int) async throws -> Int {
    await Task.yield()
    try await Task.sleep(nanoseconds: 100_000)
    if n.isMultiple(of: 2) { throw Failure.code(n) }
    return n
}

func deep(_ depth: Int, _ n: Int) async throws -> Int {
    if depth == 0 { return try await failAfterAwait(n) }
    defer { print("  unwind", depth) }
    let r = try await deep(depth - 1, n)
    await Task.yield()
    return r + depth
}

func typed(_ n: Int) async throws(Failure) -> Int {
    await Task.yield()
    if n > 2 { throw .message("too big \(n)") }
    return n
}

func rethrowing<T>(_ body: () async throws -> T) async rethrows -> T {
    await Task.yield()
    return try await body()
}

print("start")
for n in 1...4 {
    do { print("deep", n, try await deep(3, n)) }
    catch { print("deep", n, "threw", error) }
}
do { print("typed", try await typed(1)); print("typed", try await typed(5)) }
catch { print("typed threw", error) }
do { _ = try await rethrowing { try await failAfterAwait(4) } }
catch let e as Failure { print("rethrown", e) }
catch { print("other") }
print("rethrowing ok", try! await rethrowing { 99 })

let task = Task { () async throws -> Int in
    let a = try await failAfterAwait(3)
    return try await failAfterAwait(a + 3)
}
do { print("task value", try await task.value) }
catch { print("task threw", error) }

do {
    let s = try await withThrowingTaskGroup(of: Int.self) { g in
        for i in [1, 3, 5, 6, 7] { g.addTask { try await failAfterAwait(i) } }
        var s = 0
        for try await v in g { s += v }
        return s
    }
    print("group sum", s)
} catch { print("group threw", error) }

async let bad = failAfterAwait(8)
async let good = failAfterAwait(9)
do { print("async let", try await good, try await bad) }
catch { print("async let threw", error) }
print("done")
