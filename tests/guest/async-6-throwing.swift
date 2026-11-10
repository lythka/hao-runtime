// async ladder 6: errors thrown across await (swifterror + async context).
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash
enum E: Error { case bad(Int), worse(String) }

func check(_ x: Int) async throws -> Int {
    if x % 3 == 0 { throw E.bad(x) }
    return x * 2
}

func chain(_ x: Int) async throws -> Int {
    let a = try await check(x)
    let b = try await check(a + 1)
    return a + b
}

print("start")
for i in 1...6 {
    do { print(i, "ok", try await chain(i)) }
    catch E.bad(let n) { print(i, "bad", n) }
    catch { print(i, "other", error) }
}
do {
    try await withThrowingTaskGroup(of: Int.self) { g in
        g.addTask { try await check(1) }
        g.addTask { throw E.worse("child") }
        for try await _ in g {}
    }
    print("group: no error")
} catch { print("group error:", error) }
let r = await Task { () async throws -> Int in try await check(9) }.result
switch r { case .success(let v): print("task ok", v); case .failure(let e): print("task failed", e) }
print("done")
