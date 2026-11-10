// async ladder 13: cancellation: Task.cancel, checkCancellation, cancellation
// handlers (run synchronously by the canceller), group cancelAll, cancelled
// sleeps, cancellation propagating to child tasks and async let.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

nonisolated(unsafe) var handlerRan = false   // set synchronously by cancel()

func spin(_ tag: String) async -> String {
    while !Task.isCancelled { await Task.yield() }
    return "\(tag) saw cancel"
}

print("start")
let t1 = Task { () -> String in
    do { try await Task.sleep(for: .seconds(30)); return "slept" }
    catch is CancellationError { return "sleep cancelled" }
    catch { return "other error" }
}
t1.cancel()
print(await t1.value)

let t2 = Task { () -> String in
    await withTaskCancellationHandler {
        await spin("t2")
    } onCancel: {
        handlerRan = true
    }
}
try? await Task.sleep(nanoseconds: 2_000_000)
t2.cancel()
print(await t2.value)

let t3 = Task { () throws -> Int in
    try Task.checkCancellation()
    await Task.yield()
    return 1
}
print("t3", (try? await t3.value) ?? -1)
let t4 = Task { () throws -> Int in
    while true { try Task.checkCancellation(); await Task.yield() }
}
t4.cancel()
switch await t4.result {
case .success: print("t4 finished")
case .failure(let e): print("t4", e is CancellationError ? "CancellationError" : "\(e)")
}

let finished = await withTaskGroup(of: String.self) { g in
    for i in 0..<4 { g.addTask { await spin("child\(i)") } }
    g.addTask { "early" }
    var out: [String] = []
    out.append(await g.next()!)
    g.cancelAll()
    for await s in g { out.append(s) }
    return out.sorted()
}
print("group", finished)

let parent = Task { () -> [String] in
    async let a = spin("let-a")
    async let b = spin("let-b")
    let inner = Task { await spin("unstructured") }   // not a child: not cancelled with the parent
    let r = await [a, b]
    inner.cancel()
    return r + [await inner.value]
}
try? await Task.sleep(nanoseconds: 2_000_000)
parent.cancel()
print("parent", await parent.value)

let already = Task { () -> Bool in
    withUnsafeCurrentTask { $0?.cancel() }
    return Task.isCancelled
}
print("self-cancel", await already.value)
print("handler ran", handlerRan)
print("done")
