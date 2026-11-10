import Foundation

@objc(OrderEarlySwift)
public final class EarlySwift: NSObject {
    var continuation: AsyncThrowingStream<[OrderLateObjC], Error>.Continuation?
    @objc public func check() -> Int { continuation == nil ? OrderLateObjC().number() : -1 }
}

