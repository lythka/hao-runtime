import AppKit
// NSTextAlignment through native Swift signatures (tests/guest/swifttextalign.sh): a property setter
// and getter, a parameter after a String? and an object, an unlabeled parameter, a result, and a
// parameter after a type whose register count is not known (left alone). Each reports the name of
// the alignment as this (native) side sees it, or the value it was given.
public func name(_ a: NSTextAlignment) -> String {
    switch a {
    case .left: return "left"
    case .center: return "center"
    case .right: return "right"
    case .justified: return "justified"
    case .natural: return "natural"
    @unknown default: return "?\(a.rawValue)"
    }
}

public final class Field {
    public init() {}
    private var stored: NSTextAlignment = .natural
    public var alignment: NSTextAlignment {
        @inline(never) get { stored }
        @inline(never) set { stored = newValue }
    }
    public var seen: String { name(stored) }
}

@inline(never)
public func placeholder(text: String?, font: NSFont, textAlignment: NSTextAlignment, lineSpacing: Int) -> String {
    "\(text ?? "nil") \(font.pointSize) \(name(textAlignment)) \(lineSpacing)"
}

@inline(never)
public func withAlignment(_ a: NSTextAlignment) -> String { name(a) }

@inline(never)
public func preferred(_ k: Int) -> NSTextAlignment { [.left, .center, .right, .justified, .natural][k] }

public struct Opaque { public var v: Int; public init(_ v: Int) { self.v = v } }
@inline(never)
public func afterUnknown(_ o: Opaque, textAlignment: NSTextAlignment) -> Int { textAlignment.rawValue }
