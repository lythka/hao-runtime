// A class from a module built with library evolution: subclasses of it in
// another module have resilient ancestry, so their ObjC class is a Swift
// class stub (tests/guest/stubcat.sh).
import Foundation

open class StubBase: NSObject {
    public var base = 1
    public override init() { super.init() }
}
