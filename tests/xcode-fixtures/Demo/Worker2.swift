import Foundation
struct Worker2 {
 enum Event: Codable { case value(Int); case label(String); case pair(Int,String) }
 static func compute(_ seed: Int) -> Int {
  let data = try! JSONEncoder().encode([Event.value(seed), .label("hello"), .pair(7,"world")])
  return try! JSONDecoder().decode([Event].self,from:data).reduce(0) { sum,e in
   switch e { case .value(let n): return sum+n; case .label(let s): return sum+s.count; case .pair(let n,let s): return sum+n+s.count }
  }
 }
}
