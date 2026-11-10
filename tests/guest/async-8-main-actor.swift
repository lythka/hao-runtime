// async ladder 8: MainActor isolation and hops between main and global executors.
import Dispatch
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

@MainActor enum Log { static var items: [String] = [] }

@MainActor func record(_ s: String) {
    dispatchPrecondition(condition: .onQueue(.main))
    Log.items.append(s)
}

func background(_ i: Int) async -> Int {
    await record("bg\(i)")
    return i
}

@MainActor func run() async {
    record("main-start")
    var s = 0
    for i in 1...3 { s += await background(i) }
    let detached = await Task.detached { await background(10) }.value
    record("main-end \(s + detached)")
}

print("start")
await run()
print(await MainActor.run { Log.items.joined(separator: ",") })
print("done")
