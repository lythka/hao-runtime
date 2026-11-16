// Guest calls of native Swift functions typed NSTextAlignment (tests/guest/swifttextalign.sh,
// library tests/guest/swifttextalign_lib.swift). NSText.h: Center and Right are 1 and 2 on arm64,
// 2 and 1 on x86_64. Each line is what the native side received or what the guest received back.
import AppKit
import STALib

// the guest's own reading of a value (arm64 numbering), not the library's
@inline(never) func gname(_ a: NSTextAlignment) -> String {
    switch a {
    case .left: return "left"
    case .center: return "center"
    case .right: return "right"
    case .justified: return "justified"
    case .natural: return "natural"
    @unknown default: return "?\(a.rawValue)"
    }
}

let all: [NSTextAlignment] = [.left, .center, .right, .justified, .natural]
let f = Field()
for a in all {
    f.alignment = a
    print("setter \(gname(a)): native \(f.seen), getter \(gname(f.alignment))")
}
let font = NSFont.systemFont(ofSize: 13)
for a in all {
    print("param \(gname(a)): \(placeholder(text: "Get", font: font, textAlignment: a, lineSpacing: 2))")
    print("unlabeled \(gname(a)): \(withAlignment(a))")
}
for k in 0..<5 { print("result \(k): \(gname(preferred(k)))") }
// left alone: a parameter after a type whose registers are not known; its raw value passes unchanged
print("after unknown: center raw \(afterUnknown(Opaque(1), textAlignment: .center))")
