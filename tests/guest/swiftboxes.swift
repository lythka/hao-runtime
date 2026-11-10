import Foundation
@inline(never) func numbers(_ limit: Int) -> AnySequence<Int> {
    AnySequence {
        var n = 0
        return AnyIterator<Int> { defer { n += 1 }; return n < limit ? n : nil }
    }
}
@inline(never) func iterate(_ seq: AnySequence<Int>) -> [Int] { Array(seq) }
print("sequence", iterate(numbers(5)))
var i = 0
let iterator = AnyIterator<String> { i += 1; return i <= 3 ? "n\(i)" : nil }
var values: [String] = []
while let value = iterator.next() { values.append(value) }
print("iterator", values)

struct Wide { let a, b, c, d, e: Int }
@inline(never) func wide(_ n: Int) -> AnySequence<Wide> {
    AnySequence {
        var i = 0
        return AnyIterator {
            defer { i += 1 }
            return i < n ? Wide(a: i, b: i + 1, c: i + 2, d: i + 3, e: i + 4) : nil
        }
    }
}
print("wide", wide(4).map { $0.a + $0.b + $0.c + $0.d + $0.e })
// Repeated destruction/reallocation exercises address reuse in the registry.
var total = 0
for _ in 0..<200 { total += iterate(numbers(5)).reduce(0, +) }
print("reuse", total)
