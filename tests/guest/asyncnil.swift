// Native generic functions whose result is G? (G a generic parameter of Self's type), with G a type whose own
// nil sits in a pointer word (`C?`, `(Int, C?)`): their nil is G's first extra inhabitant, 1 on arm64 and 2 on
// x86_64. Guest code specialized for G reads it inline (tests/guest/asyncnil.sh). Built -O (async iterators and
// group next() through their native entry points) and -Onone (also Dictionary/Set iterators). Every line must
// equal native arm64. Not here: an element whose own nil is two levels deep (AsyncStream<C??>, result C???),
// beyond the walk's bound of two consumed inhabitants (docs/ISSUES.md).
import Foundation

final class Node: NSObject { let n: Int; init(_ n: Int) { self.n = n } }
struct Failure: Error { let n: Int }

func show(_ b: Node?) -> String { b.map { "node\($0.n)" } ?? "nil" }
func show2(_ b: Node??) -> String {
    switch b { case .none: return "none"; case .some(.none): return "some(nil)"; case .some(.some(let x)): return "node\(x.n)" }
}
@inline(never) func seen(_ i: Int, _ n: Node?, _ log: inout [String]) { if n == nil { log.append("nil@\(i)") } }

// Finder's shape (26B131 Finder+0x793cfc): a throwing group of (offset, ObjC node?) filling an array by offset;
// the node is used again after the store, so it is retained.
@inline(never) func resolveNodes(_ count: Int) async throws -> String {
    try await withThrowingTaskGroup(of: (offset: Int, node: Node?).self) { g in
        for i in 0..<count { g.addTask { (offset: i, node: i % 3 == 1 ? nil : Node(i)) } }
        var nodes = [Node?](repeating: nil, count: count)
        var log: [String] = []
        for try await (offset, node) in g { nodes[offset] = node; seen(offset, node, &log) }
        return nodes.map(show).joined(separator: ",") + " " + log.sorted().joined(separator: ",")
    }
}

@inline(never) func groupNext() async throws -> String {
    try await withThrowingTaskGroup(of: NSObject?.self) { g in
        g.addTask { nil }
        g.addTask { NSObject() }
        var nils = 0, objects = 0
        while let r = try await g.next() { if r == nil { nils += 1 } else { objects += 1 } }
        return "nils \(nils) objects \(objects)"
    }
}

@inline(never) func taskGroup() async -> String {
    await withTaskGroup(of: Node?.self) { g in
        for i in 0..<4 { g.addTask { i % 2 == 0 ? Node(i) : nil } }
        var out: [String] = []
        for await r in g { out.append(show(r)) }
        return out.sorted().joined(separator: ",")
    }
}

@inline(never) func stream() async -> String {
    let s = AsyncStream<Node?> { c in c.yield(Node(1)); c.yield(nil); c.yield(Node(3)); c.finish() }
    var out: [String] = []
    for await r in s { out.append(show(r)) }
    return out.joined(separator: ",")
}

@inline(never) func throwingStream() async -> String {
    let s = AsyncThrowingStream<Node?, Error> { c in c.yield(nil); c.yield(Node(2)); c.finish(throwing: Failure(n: 7)) }
    var out: [String] = []
    do { for try await r in s { out.append(show(r)) } } catch let e as Failure { out.append("threw \(e.n)") } catch { out.append("threw ?") }
    return out.joined(separator: ",")
}

@inline(never) func childThrows() async -> String {
    var out: [String] = []
    do {
        try await withThrowingTaskGroup(of: (offset: Int, node: Node?).self) { g in
            g.addTask { throw Failure(n: 3) }
            for try await (offset, node) in g { out.append("\(offset) \(show(node))") }
        }
    } catch let e as Failure { out.append("threw \(e.n)") } catch { out.append("threw ?") }
    do {
        try await withThrowingTaskGroup(of: NSObject?.self) { g in
            g.addTask { throw Failure(n: 4) }
            while let r = try await g.next() { out.append(r == nil ? "nil" : "object") }
        }
    } catch let e as Failure { out.append("threw \(e.n)") } catch { out.append("threw ?") }
    return out.joined(separator: ",")
}

@inline(never) func controls() async -> String {
    let a = AsyncStream<Int?> { c in c.yield(nil); c.yield(2); c.finish() }
    let b = AsyncStream<String?> { c in c.yield("s"); c.yield(nil); c.finish() }
    var out: [String] = []
    for await r in a { out.append(r.map(String.init) ?? "nil") }
    for await r in b { out.append(r ?? "nil") }
    return out.joined(separator: ",")
}

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func drain<S: AsyncSequence>(_ s: S) async throws -> Int { var n = 0; for try await _ in s { n += 1 }; return n }

@inline(never) func generic() async -> String {
    let s = AsyncStream<Node?> { c in c.yield(nil); c.yield(Node(6)); c.finish() }
    return "drained \((try? await drain(s)) ?? -1)"
}

@inline(never) func taskValue() async -> String {
    let t = Task { () -> Node?? in .some(nil) }
    let u = Task { () -> Node?? in .none }
    return show2(await t.value) + "," + show2(await u.value)
}

@inline(never) func iterators() -> String {
    var out: [String] = []
    var vi = [2: Node(2), 1: nil].values.makeIterator()
    while let v = vi.next() { out.append(show(v)) }
    let s: Set<Node?> = [nil]
    var si = s.makeIterator()
    while let v = si.next() { out.append(show(v)) }
    return out.sorted().joined(separator: ",")
}

@main struct Main {
    static func main() async {
        print("finder " + ((try? await resolveNodes(5)) ?? "threw"))
        print("next " + ((try? await groupNext()) ?? "threw"))
        print("group " + (await taskGroup()))
        print("stream " + (await stream()))
        print("throwing " + (await throwingStream()))
        print("child " + (await childThrows()))
        print("controls " + (await controls()))
        print("generic " + (await generic()))
        print("value " + (await taskValue()))
        print("iterators " + iterators())
        print("done")
    }
}
