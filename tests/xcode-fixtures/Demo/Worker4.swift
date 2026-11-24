import Foundation
struct Worker4 {
 static func compute(_ seed:Int) -> Int { let a=SIMD4<Int32>(1,2,3,4); let b=a &* SIMD4(repeating:Int32(seed)); return Int(b[0]+b[1]+b[2]+b[3]) }
}
