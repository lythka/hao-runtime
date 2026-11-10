// Guest dylib for tests/guest/swiftlate.sh: a generic NSObject subclass, so a
// class in another image that specializes it has its superclass filled in by
// the Swift runtime (singleton metadata initialization). Such a class is not
// in its image's __objc_classlist; its categories wait for the runtime.
import Foundation

open class LateBase<T>: NSObject {
    public var payload: T?
    public override init() { super.init() }
    @objc open dynamic func baseName() -> String { "base" }
}
