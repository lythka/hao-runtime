// A guest framework finding its own bundle and resources (tests/guest/fwbundle.sh).
import Foundation
public class Plain: NSObject {}
public class Resil: JSONEncoder {}
public class Gen<T>: NSObject {}
public class PureSwift { public init() {} }
public func probe() -> [String] {
    func row(_ name: String, _ c: AnyClass) -> String {
        let b = Bundle(for: c)
        return "\(name): \((b.bundlePath as NSString).lastPathComponent) resource=\(b.url(forResource: "model", withExtension: "txt") != nil)"
    }
    let byId = Bundle(identifier: "test.EC")
    let all = Bundle.allFrameworks.contains { ($0.bundlePath as NSString).lastPathComponent == "EC.framework" }
    return ["identifier: \(byId.map { ($0.bundlePath as NSString).lastPathComponent } ?? "nil") resource=\(byId?.url(forResource: "model", withExtension: "txt") != nil)",
            "allFrameworks has EC: \(all)",
            row("Plain", Plain.self), row("Resil", Resil.self), row("Gen<Int>", Gen<Int>.self), row("PureSwift", PureSwift.self)]
}
