// Swift generic containers over guest types: the native Swift runtime and
// standard library call the guest's value witnesses, protocol witnesses,
// metadata destroy functions and closure destructors (src/fnslots.c).
// Build and run with tests/guest/callbacks.sh.
final class Tracker {
    static var live = 0
    let id: Int
    init(_ id: Int) { self.id = id; Tracker.live += 1 }
    deinit { Tracker.live -= 1 }
}

struct Item: Hashable, Comparable {
    var name: String
    var weight: Int
    var owner: Tracker?
    static func == (a: Item, b: Item) -> Bool { a.name.lowercased() == b.name.lowercased() && a.weight == b.weight }
    func hash(into h: inout Hasher) { h.combine(name.lowercased()); h.combine(weight) }
    static func < (a: Item, b: Item) -> Bool { a.weight != b.weight ? a.weight < b.weight : a.name < b.name }
}

enum Shape {
    case circle(Double, Tracker)
    case rect(w: Double, h: Double)
    case named(String, [Int])
    var area: Double {
        switch self {
        case let .circle(r, _): return 3.0 * r * r
        case let .rect(w, h): return w * h
        case let .named(_, v): return Double(v.reduce(0, +))
        }
    }
}

final class Box<T> {
    var value: T
    init(_ v: T) { value = v }
}

struct Pair<A, B> { var a: A; var b: B }

@inline(never) func run<T: Hashable & Comparable>(_ xs: [T]) -> (Int, T?, T?) {
    let s = Set(xs)
    let sorted = xs.sorted()
    return (s.count, sorted.first, sorted.last)
}

var items: [Item] = []
for k in 0..<300 {
    items.append(Item(name: k % 3 == 0 ? "Alpha\(k % 17)" : "alpha\(k % 17)", weight: (k * 37) % 23, owner: Tracker(k)))
}
let (distinct, lo, hi) = run(items)
print("items", items.count, "distinct", distinct, "lo", lo!.name, lo!.weight, "hi", hi!.name, hi!.weight)
var dict: [Item: Box<Pair<Int, String>>] = [:]
for (i, it) in items.enumerated() { dict[it, default: Box(Pair(a: 0, b: ""))].value.a += i }
print("dict", dict.count, dict.values.map { $0.value.a }.reduce(0, +))
var shapes: [Shape] = []
for k in 0..<120 {
    switch k % 3 {
    case 0: shapes.append(.circle(Double(k) / 10, Tracker(k)))
    case 1: shapes.append(.rect(w: Double(k), h: 0.5))
    default: shapes.append(.named("n\(k)", Array(0..<(k % 7))))
    }
}
print("shapes", shapes.count, Int((shapes.map(\.area).reduce(0.0, +) * 100).rounded()))
var actions: [() -> Int] = []
for k in 0..<50 {
    let t = Tracker(1000 + k)
    let label = "act\(k)"
    actions.append { t.id + label.count }
}
print("closures", actions.map { $0() }.reduce(0, +))
let opt: [Pair<Item?, [String: Int]>] = items.prefix(40).map { Pair(a: $0.weight % 2 == 0 ? $0 : nil, b: [$0.name: $0.weight]) }
print("optionals", opt.compactMap(\.a).count, opt.map { $0.b.count }.reduce(0, +))
let nested = Box([Box(items[0...9].map { $0.name }), Box(["x", "y"])])
print("nested", nested.value.map { $0.value.count })
items.removeAll()
shapes.removeAll()
actions.removeAll()
dict.removeAll()
print("live trackers", Tracker.live)
