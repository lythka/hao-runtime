// Categories (@objc extensions) on Swift classes that the Swift runtime
// realizes after image registration: classes with a generic superclass from
// a guest dylib (swiftlatelib.swift) or from this image, whose superclass
// word is 0 in the image, which are missing from __objc_classlist, and whose
// extensions are categories in __objc_catlist (the shape of 27.0 FindMy's
// FMReachabilityOperation, Shortcuts' window controllers, Reminders' cell
// views). Natively objc4 attaches the categories when the class is realized;
// hao used to call class_replaceMethod on the unrealized class at image
// registration and fault in prepareMethodLists before main.
//   swiftlate [instance|byname|local]   (no argument: all three)
import Foundation
import swiftlatelib

@objc protocol LateGreeter {
    func greet() -> String
    static func kind() -> String
}

final class LateDerived: LateBase<[String: Bool]> {
    var n = 7
    @objc func own() -> Int { n }
}

extension LateDerived: LateGreeter {
    @objc func greet() -> String { "hi \(n)" }
    @objc static func kind() -> String { "derived" }
    @objc var late: Int { n * 6 }
}

extension LateDerived {
    @objc func second(_ x: Int) -> Int { x + n }
    @objc override func baseName() -> String { "derived-" + super.baseName() }
}

// first touched by name from native code (NSClassFromString -> the Swift
// runtime's getClass hook -> the metadata accessor)
final class LateByName: LateBase<Int> {
    @objc func own() -> Int { 3 }
}
extension LateByName {
    @objc func byName() -> String { "by name" }
    @objc class func classSide() -> Int { 11 }
}

// a generic superclass from this image
class LocalBase<T>: NSObject {}
final class LocalDerived: LocalBase<Int> {}
extension LocalDerived: LateGreeter {
    @objc func greet() -> String { "local" }
    @objc static func kind() -> String { "local-kind" }
}

func methods(_ c: AnyClass) -> String {
    var n: UInt32 = 0
    guard let ms = class_copyMethodList(c, &n) else { return "-" }
    defer { free(ms) }
    return (0..<Int(n)).map { NSStringFromSelector(method_getName(ms[$0])) }.sorted().joined(separator: ",")
}

func report(_ c: AnyClass) {
    print("  class \(NSStringFromClass(c)) super=\(NSStringFromClass(class_getSuperclass(c)!))")
    print("  instance methods: \(methods(c))")
    print("  class methods: \(methods(object_getClass(c)!))")
    print("  conforms LateGreeter: \(class_conformsToProtocol(c, LateGreeter.self))")
}

func instance() {
    print("instance:")
    let d = LateDerived()
    let o = d as NSObject
    print("  responds greet: \(o.responds(to: #selector(LateGreeter.greet)))")
    print("  greet: \((o.perform(#selector(LateGreeter.greet))!.takeUnretainedValue() as! String))")
    print("  late: \(o.value(forKey: "late")!)")
    print("  baseName: \(d.baseName())  via msgSend: \(o.perform(#selector(LateBase<Int>.baseName))!.takeUnretainedValue() as! String)")
    let g = o as? LateGreeter
    print("  as LateGreeter: \(g?.greet() ?? "nil") kind=\(type(of: d).kind())")
    print("  second: \(d.second(35))")
    report(LateDerived.self)
}

func byName() {
    print("byname:")
    guard let c = NSClassFromString("swiftlate.LateByName") as? NSObject.Type else { print("  not found"); return }
    let o = c.init()
    print("  responds byName: \(o.responds(to: NSSelectorFromString("byName")))")
    print("  byName: \((o.perform(NSSelectorFromString("byName"))!.takeUnretainedValue() as! String))")
    print("  classSide responds: \((c as AnyObject).responds(to: NSSelectorFromString("classSide")))")
    report(c)
}

func local() {
    print("local:")
    let o = LocalDerived() as NSObject
    print("  greet: \((o.perform(#selector(LateGreeter.greet))!.takeUnretainedValue() as! String))")
    print("  kind: \(((LocalDerived.self as AnyObject).perform(NSSelectorFromString("kind"))!.takeUnretainedValue() as! String))")
    report(LocalDerived.self)
}

let arg = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "all"
if arg == "all" || arg == "instance" { instance() }
if arg == "all" || arg == "byname" { byName() }
if arg == "all" || arg == "local" { local() }
