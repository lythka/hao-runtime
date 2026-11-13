// Swift's __VaListBuilder under hao (tests/guest/valist.sh): withVaList and String(format:)
// build an arm64 va_list in code the compiler inlines from the stdlib.
import Foundation

@main
struct Main {
    static func main() {
        // withVaList into a native C function that takes a va_list
        var buf = [CChar](repeating: 0, count: 256)
        let n = withVaList([42, 3.5, -7, UInt8(200)]) { vsnprintf(&buf, buf.count, "%d %.2f %ld %u", $0) }
        print("vsnprintf:", n, String(cString: buf))
        // many words: the storage grows (allocStorage, moveInitialize, deallocStorage)
        let many: [CVarArg] = (0..<40).map { $0 * 3 }
        let fmt = Array(repeating: "%d", count: 40).joined(separator: ",")
        print("grown:", withVaList(many) { vsnprintf(&buf, buf.count, fmt, $0) }, String(cString: buf))
        // String(format:) with mixed arguments (Foundation, native, takes the va_list)
        print(String(format: "%@ %d %.3f %x %@", "obj" as NSString, 12, 2.0 / 3.0, 255, "swift"))
        print(String(format: "%5.1f|%-4d|%08.3f", 1.25, 9, -3.14159))
        // an empty list: va_list() points at the empty word
        print("empty:", withVaList([]) { vsnprintf(&buf, buf.count, "none", $0) }, String(cString: buf))
        // builders created and released many times: the destroyer frees storage and object
        var total = 0
        for i in 0..<2000 { total += Int(withVaList([i, i + 1]) { vsnprintf(&buf, buf.count, "%d-%d", $0) }) }
        print("loop:", total)
        // (getVaList is not inlinable: x86_64 libswiftCore builds its own va_list; a documented limit)
    }
}
