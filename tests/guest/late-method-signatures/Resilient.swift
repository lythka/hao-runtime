import Foundation

// Built as a native library with library evolution. A guest subclass cannot
// embed its completed metadata in its own image: the superclass layout is
// resolved at runtime and Swift allocates the subclass metadata separately.
open class NativeResilientFrame: NSObject {
    private let marker = 27
    @objc open var frame: NSRect {
        precondition(marker == 27)
        var result = NSRect()
        result.origin.x = 1.25
        result.origin.y = -2.5
        result.size.width = 300.75
        result.size.height = 40.125
        return result
    }
}
