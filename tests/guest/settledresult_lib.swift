// Native generic code that reads a guest conformance's associated-type getter (tests/guest/settledresult.sh).
public protocol Source { associatedtype Value; var value: Value { get } }
public func describe<S: Source>(_ s: S) -> String { "\(s.value)" }
public func pair<S: Source>(_ s: S) -> [S.Value] { [s.value, s.value] }
