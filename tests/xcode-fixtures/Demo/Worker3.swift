import Foundation
struct Worker3 {
 actor Counter { var sum = 0; func add(_ n:Int) { sum += n }; func read() -> Int { sum } }
 static func run() async -> Int {
  let counter=Counter()
  await withTaskGroup(of:Void.self) { g in for n in 1...32 { g.addTask { await counter.add(n) } } }
  return await counter.read()
 }
}
