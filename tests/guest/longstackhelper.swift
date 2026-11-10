import Foundation

public enum Support { case off, on }
public struct Sharing {
    public let a: Bool, b: Bool
    public init(_ a: Bool, _ b: Bool) { self.a = a; self.b = b }
}
public struct Triple {
    public let a: Bool, b: Bool, c: Bool
    public init(_ a: Bool, _ b: Bool, _ c: Bool) { self.a = a; self.b = b; self.c = c }
}
public struct Empty { public init() {} }
public struct LongInfo {
    let listType: Support?, name: String, color: NSObject, count: Int?, sharing: Sharing?
    let create: Bool, toggle: Bool, add: Bool, clear: Bool, save: Bool, support: Support
    let printIt: Bool, shown: Bool, defaultButton: Bool, completed: Int, deleted: Bool
    let autosave: String?, custom: Bool, hashtags: Sharing?, template: Sharing?
    let origin: Bool, preview: Bool, grocery: Bool, categorize: Bool, recent: Bool, ios: Triple
    @inline(never)
    public init(listType: Support?, name: String, color: NSObject, count: Int?, sharing: Sharing?,
                create: Bool, toggle: Bool, add: Bool, clear: Bool, save: Bool, support: Support,
                printIt: Bool, shown: Bool, defaultButton: Bool, completed: Int, deleted: Bool,
                autosave: String?, custom: Bool, hashtags: Sharing?, template: Sharing?,
                origin: Bool, preview: Bool, grocery: Bool, categorize: Bool, recent: Bool,
                ios: Triple, macos: Empty) {
        self.listType = listType; self.name = name; self.color = color; self.count = count; self.sharing = sharing
        self.create = create; self.toggle = toggle; self.add = add; self.clear = clear; self.save = save; self.support = support
        self.printIt = printIt; self.shown = shown; self.defaultButton = defaultButton; self.completed = completed; self.deleted = deleted
        self.autosave = autosave; self.custom = custom; self.hashtags = hashtags; self.template = template
        self.origin = origin; self.preview = preview; self.grocery = grocery; self.categorize = categorize; self.recent = recent; self.ios = ios
    }
    public func describe() -> String {
        let bs = [create,toggle,add,clear,save,printIt,shown,defaultButton,deleted,custom,origin,preview,grocery,categorize,recent,ios.a,ios.b,ios.c]
        let mask = bs.enumerated().reduce(0) { $0 | ($1.element ? 1 << $1.offset : 0) }
        return "\(listType.map { $0 == .on ? 1 : 0 } ?? -1)|\(name)|\(color.description)|\(count ?? -1)|\(sharing.map { ($0.a ? 1 : 0) + ($0.b ? 2 : 0) } ?? -1)|\(support == .on ? 1 : 0)|\(mask)|\(completed)|\(autosave ?? "nil")|\(hashtags.map { ($0.a ? 1 : 0) + ($0.b ? 2 : 0) } ?? -1)|\(template.map { ($0.a ? 1 : 0) + ($0.b ? 2 : 0) } ?? -1)"
    }
}

// All stack arguments are word aligned; the generic16-word copy still truncates.
@inline(never)
public func wideWords(_ a0: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int, _ a7: Int, _ a8: Int, _ a9: Int, _ a10: Int, _ a11: Int, _ a12: Int, _ a13: Int, _ a14: Int, _ a15: Int, _ a16: Int, _ a17: Int, _ a18: Int, _ a19: Int, _ a20: Int, _ a21: Int, _ a22: Int, _ a23: Int, _ a24: Int, _ a25: Int, _ a26: Int) -> Int {
    var r = 0
    r = r &+ a0 &* 1
    r = r &+ a1 &* 3
    r = r &+ a2 &* 5
    r = r &+ a3 &* 7
    r = r &+ a4 &* 9
    r = r &+ a5 &* 11
    r = r &+ a6 &* 13
    r = r &+ a7 &* 15
    r = r &+ a8 &* 17
    r = r &+ a9 &* 19
    r = r &+ a10 &* 21
    r = r &+ a11 &* 23
    r = r &+ a12 &* 25
    r = r &+ a13 &* 27
    r = r &+ a14 &* 29
    r = r &+ a15 &* 31
    r = r &+ a16 &* 33
    r = r &+ a17 &* 35
    r = r &+ a18 &* 37
    r = r &+ a19 &* 39
    r = r &+ a20 &* 41
    r = r &+ a21 &* 43
    r = r &+ a22 &* 45
    r = r &+ a23 &* 47
    r = r &+ a24 &* 49
    r = r &+ a25 &* 51
    r = r &+ a26 &* 53
    return r
}

// Upper admitted kind-K bound:32 stack words afterx7.
@inline(never)
public func widestWords(_ a0: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int, _ a7: Int, _ a8: Int, _ a9: Int, _ a10: Int, _ a11: Int, _ a12: Int, _ a13: Int, _ a14: Int, _ a15: Int, _ a16: Int, _ a17: Int, _ a18: Int, _ a19: Int, _ a20: Int, _ a21: Int, _ a22: Int, _ a23: Int, _ a24: Int, _ a25: Int, _ a26: Int, _ a27: Int, _ a28: Int, _ a29: Int, _ a30: Int, _ a31: Int, _ a32: Int, _ a33: Int, _ a34: Int, _ a35: Int, _ a36: Int, _ a37: Int, _ a38: Int, _ a39: Int) -> Int {
    var r = 0
    r = r &+ a0 &* 1
    r = r &+ a1 &* 3
    r = r &+ a2 &* 5
    r = r &+ a3 &* 7
    r = r &+ a4 &* 9
    r = r &+ a5 &* 11
    r = r &+ a6 &* 13
    r = r &+ a7 &* 15
    r = r &+ a8 &* 17
    r = r &+ a9 &* 19
    r = r &+ a10 &* 21
    r = r &+ a11 &* 23
    r = r &+ a12 &* 25
    r = r &+ a13 &* 27
    r = r &+ a14 &* 29
    r = r &+ a15 &* 31
    r = r &+ a16 &* 33
    r = r &+ a17 &* 35
    r = r &+ a18 &* 37
    r = r &+ a19 &* 39
    r = r &+ a20 &* 41
    r = r &+ a21 &* 43
    r = r &+ a22 &* 45
    r = r &+ a23 &* 47
    r = r &+ a24 &* 49
    r = r &+ a25 &* 51
    r = r &+ a26 &* 53
    r = r &+ a27 &* 55
    r = r &+ a28 &* 57
    r = r &+ a29 &* 59
    r = r &+ a30 &* 61
    r = r &+ a31 &* 63
    r = r &+ a32 &* 65
    r = r &+ a33 &* 67
    r = r &+ a34 &* 69
    r = r &+ a35 &* 71
    r = r &+ a36 &* 73
    r = r &+ a37 &* 75
    r = r &+ a38 &* 77
    r = r &+ a39 &* 79
    return r
}
