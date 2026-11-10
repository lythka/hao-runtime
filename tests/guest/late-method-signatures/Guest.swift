import Foundation
import ResilientProbe
class GenericBase<T>: NSObject {}
final class LateRect: GenericBase<Int> {
    @objc func constrainFrameRect(_ rect: NSRect, toScreen screen: AnyObject?) -> NSRect { rect }
}
class GenericRect<T>: NSObject {
    @objc func constrainFrameRect(_ rect: NSRect, toScreen screen: AnyObject?) -> NSRect { rect }
}
final class InheritedRect: GenericRect<Int> {}
class GenericFrame<T>: HaoNativeFrame {
    var token: T
    init(_ token: T) { self.token = token; super.init() }
    override var frame: NSRect {
        var result = super.frame
        result.origin.x += 10
        return result
    }
}
final class InheritedFrame: GenericFrame<Int> {}
final class ResilientFrame: NativeResilientFrame {
    override var frame: NSRect {
        var result = super.frame
        result.origin.x += 10
        return result
    }
}
var failures: Int32 = 0
for object: NSObject in [LateRect(), GenericRect<Int>(), InheritedRect()] {
    failures |= Int32(hao_check_rect(object))
}
for object in [GenericFrame(42), InheritedFrame(43)] {
    failures |= Int32(hao_check_frame(object))
}
failures |= Int32(hao_check_frame(ResilientFrame()))
exit(failures)
