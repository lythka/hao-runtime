// async ladder 5: AsyncStream and a custom AsyncSequence.
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash
struct Countdown: AsyncSequence {
    typealias Element = Int
    let from: Int
    struct AsyncIterator: AsyncIteratorProtocol {
        var n: Int
        mutating func next() async -> Int? {
            if n < 0 { return nil }
            defer { n -= 1 }
            return n
        }
    }
    func makeAsyncIterator() -> AsyncIterator { AsyncIterator(n: from) }
}

print("start")
var seen: [Int] = []
for await v in Countdown(from: 5) { seen.append(v) }
print("countdown", seen)
let stream = AsyncStream<String> { cont in
    Task {
        for w in ["alpha", "beta", "gamma"] { cont.yield(w) }
        cont.finish()
    }
}
var words: [String] = []
for await w in stream { words.append(w) }
print("stream", words)
let mapped = Countdown(from: 3).map { $0 * 100 }
var m: [Int] = []
for await v in mapped { m.append(v) }
print("mapped", m)
print("done")
