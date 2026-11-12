import Foundation
// A Data? after three words of other arguments (x3/x4), two Data? around an Int, a Data? result.
public func odSignature(_ a: Int, _ b: Int, _ c: [Int16]?, sig: Data?) -> Int {
    guard let s = sig else { return -1 }
    return s.count
}
public func odTwo(_ x: Data?, _ y: Int, _ z: Data?) -> Int {
    (x == nil ? 1 : 10 + x!.count) * 100 + (z == nil ? 1 : 10 + z!.count) + y * 10000
}
public func odMaybe(_ n: Int) -> Data? { n < 0 ? nil : Data(repeating: 7, count: n) }
