import Foundation
struct Worker7 {
 enum Issue:Error { case negative }
 static func checked(_ n:Int)throws->Int { guard n>=0 else { throw Issue.negative }; return n*n }
 static func compute(_ seed:Int)->Int { let result=Result { try checked(seed) }; switch result { case .success(let n): return n; case .failure: return -1 } }
}
