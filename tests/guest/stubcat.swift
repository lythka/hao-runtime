// Methods of an @objc extension on a class with resilient ancestry: the
// compiler emits the class as an ObjC class stub (__objc_stublist) and the
// extension as a category on the stub in __objc_catlist2
// (tests/guest/stubcat.sh).
import Foundation
import stubcat_lib

@objc protocol TrackSettable {
    var hideSwitch: Bool { get set }
    func describe() -> String
}

final class Derived: StubBase {
    var stored = false
}

extension Derived: TrackSettable {
    @objc var hideSwitch: Bool {
        get { stored }
        set { stored = newValue }
    }
    @objc func describe() -> String { "Derived(stored: \(stored), base: \(base))" }
    @objc static func classSide() -> String { "class side" }
}

@main
struct Main {
    static func main() {
        let d = Derived()
        print("responds hideSwitch:", d.responds(to: #selector(getter: TrackSettable.hideSwitch)))
        print("responds setHideSwitch:", d.responds(to: #selector(setter: TrackSettable.hideSwitch)))
        print("conforms:", Derived.self.conforms(to: TrackSettable.self))
        let s: TrackSettable = d
        s.hideSwitch = true
        print("hideSwitch:", s.hideSwitch)
        print(s.describe())
        let o: NSObject = d
        o.setValue(false, forKey: "hideSwitch")
        print("via KVC:", o.value(forKey: "hideSwitch") as! Bool)
        print((Derived.self as AnyObject).perform(#selector(Derived.classSide)).takeUnretainedValue() as! String)
    }
}
