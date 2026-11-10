// async ladder 7: Task.detached, Task.sleep (timer-driven resumption), cancellation.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash
print("start")
let t = Task.detached { () -> Int in
    try? await Task.sleep(nanoseconds: 20_000_000)
    return 42
}
print("detached =", await t.value)
let before = ContinuousClock.now
try await Task.sleep(for: .milliseconds(30))
let elapsed = ContinuousClock.now - before
print("slept >= 30ms:", elapsed >= .milliseconds(30))
let c = Task { () -> String in
    do { try await Task.sleep(for: .seconds(10)); return "not cancelled" }
    catch { return "cancelled: \(type(of: error))" }
}
c.cancel()
print(await c.value)
print("done")
