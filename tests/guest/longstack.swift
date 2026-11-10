import Foundation
import LongStackHelper
final class Color: NSObject { override var description: String { "Color(42)" } }
// A separate wide-only process proves that8-byte-aligned stack calls also need
// a contract when more than16 guest words are consumed.
if CommandLine.arguments.last == "wide" {
    for i in 0..<96 {
        print(wideWords(i &* 1 &+ 0, i &* 3 &+ 1, i &* 5 &+ 2, i &* 7 &+ 3, i &* 9 &+ 4, i &* 11 &+ 5, i &* 13 &+ 6, i &* 15 &+ 7, i &* 17 &+ 8, i &* 19 &+ 9, i &* 21 &+ 10, i &* 23 &+ 11, i &* 25 &+ 12, i &* 27 &+ 13, i &* 29 &+ 14, i &* 31 &+ 15, i &* 33 &+ 16, i &* 35 &+ 17, i &* 37 &+ 18, i &* 39 &+ 19, i &* 41 &+ 20, i &* 43 &+ 21, i &* 45 &+ 22, i &* 47 &+ 23, i &* 49 &+ 24, i &* 51 &+ 25, i &* 53 &+ 26))
        print(widestWords(i &* 3 &+ 1, i &* 5 &+ 2, i &* 7 &+ 3, i &* 9 &+ 4, i &* 11 &+ 5, i &* 13 &+ 6, i &* 15 &+ 7, i &* 17 &+ 8, i &* 19 &+ 9, i &* 21 &+ 10, i &* 23 &+ 11, i &* 25 &+ 12, i &* 27 &+ 13, i &* 29 &+ 14, i &* 31 &+ 15, i &* 33 &+ 16, i &* 35 &+ 17, i &* 37 &+ 18, i &* 39 &+ 19, i &* 41 &+ 20, i &* 43 &+ 21, i &* 45 &+ 22, i &* 47 &+ 23, i &* 49 &+ 24, i &* 51 &+ 25, i &* 53 &+ 26, i &* 55 &+ 27, i &* 57 &+ 28, i &* 59 &+ 29, i &* 61 &+ 30, i &* 63 &+ 31, i &* 65 &+ 32, i &* 67 &+ 33, i &* 69 &+ 34, i &* 71 &+ 35, i &* 73 &+ 36, i &* 75 &+ 37, i &* 77 &+ 38, i &* 79 &+ 39, i &* 81 &+ 40))
    }
    exit(0)
}
let color = Color()
// Single-bit witnesses separate all 18 Boolean positions; subsequent deterministic
// masks exercise interacting settings, enum states and short/long/nil Strings.
for i in 0..<96 {
    let mask: UInt32
    if i < 18 { mask = 1 << i }
    else if i == 18 { mask = 0 }
    else if i == 19 { mask = (1 << 18) - 1 }
    else { mask = UInt32(truncatingIfNeeded: UInt64(i) &* 2654435761) ^ 0x273519 }
    func b(_ n: Int) -> Bool { mask & (1 << n) != 0 }
    let info = LongInfo(listType: i % 3 == 0 ? nil : (b(0) ? .on : .off), name: i % 2 == 0 ? "Reminders" : String(repeating: "long name ", count: 8),
                        color: color, count: b(1) ? i : nil, sharing: b(2) ? Sharing(b(3), b(4)) : nil,
                        create: b(0), toggle: b(1), add: b(2), clear: b(3), save: b(4), support: b(5) ? .on : .off,
                        printIt: b(5), shown: b(6), defaultButton: b(7), completed: i * 0x12345, deleted: b(8),
                        autosave: b(3) ? (b(4) ? "short" : String(repeating: "long autosave ", count: 8)) : nil,
                        custom: b(9), hashtags: b(5) ? Sharing(b(0), b(1)) : nil, template: b(0) ? Sharing(b(1), b(2)) : nil,
                        origin: b(10), preview: b(11), grocery: b(12), categorize: b(13), recent: b(14),
                        ios: Triple(b(15), b(16), b(17)), macos: Empty())
    print("\(i):\(info.describe())")
}
print("alive:\(color.description)")
