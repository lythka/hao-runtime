import Foundation
struct Worker0 {
 struct Record: Codable, Hashable { let value: Int; let label: String }
 static func compute(_ seed: Int) -> Int {
  let rows = (0..<64).map { Record(value: $0 + seed + 0, label: String($0)) }
  let data = try! JSONEncoder().encode(rows)
  let decoded = try! JSONDecoder().decode([Record].self, from: data)
  return decoded.filter { $0.value % 2 == 0 }.reduce(0) { $0 + $1.value }
 }
}
