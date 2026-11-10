// async ladder 3: actor isolation, hops from many child tasks.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash
actor Counter {
    private var value = 0
    func increment(by n: Int) -> Int { value += n; return value }
    func get() -> Int { value }
}

print("start")
let c = Counter()
await withTaskGroup(of: Void.self) { g in
    for i in 1...100 { g.addTask { _ = await c.increment(by: i) } }
}
print("counter =", await c.get())
print("done")
