import Foundation
public struct Envelope<T: Codable & Sendable>: Codable, Sendable {
 public let value: T
 public init(_ value: T) { self.value = value }
 public func roundTrip() throws -> T { try JSONDecoder().decode(Self.self, from: JSONEncoder().encode(self)).value }
}
public enum FixtureError: Error { case negative }
public func checked(_ n: Int) throws -> Int { if n < 0 { throw FixtureError.negative }; return n * n }
public actor Counter {
 private var value = 0
 public init() {}
 public func add(_ n: Int) -> Int { value += n; return value }
}
public func parallelSum() async -> Int {
 await withTaskGroup(of: Int.self) { group in
  for i in 1...100 { group.addTask { i } }
  var sum = 0
  for await result in group { sum += result }
  return sum
 }
}
