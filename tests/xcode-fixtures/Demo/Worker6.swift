import Foundation
struct Worker6 {
 struct Box<T:BinaryInteger> { let value:T; func mapped<U>(_ body:(T)->U)->U { body(value) } }
 static func compute(_ seed:Int)->Int { let values=(0..<32).map { Box(value:$0+seed) }; return values.lazy.filter { $0.value % 2 == 0 }.map { $0.mapped { $0 * $0 } }.reduce(0,+) }
}
