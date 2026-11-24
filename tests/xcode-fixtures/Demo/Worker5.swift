import Foundation
struct Worker5 {
 static func compute(_ seed:Int) -> Int { let text="alpha 12 beta 34 gamma 56"; let r=try! NSRegularExpression(pattern:"[0-9]+"); return r.numberOfMatches(in:text,range:NSRange(text.startIndex...,in:text))+seed }
}
