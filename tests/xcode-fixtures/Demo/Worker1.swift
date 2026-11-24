import Foundation
protocol Scored { var score: Int { get } }
struct Worker1 {
 struct Item: Codable, Scored { let score: Int }
 static func total<C: Collection>(_ c: C) -> Int where C.Element: Scored { c.reduce(0) { $0 + $1.score } }
 static func compute(_ seed: Int) -> Int { total((0..<32).map { Item(score:$0+seed) }.sorted { $0.score < $1.score }) }
}
