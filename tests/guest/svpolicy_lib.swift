// A struct built without reflection metadata (tests/guest/svpolicy.sh: -disable-reflection-metadata),
// holding a metatype's nested Optional: arm64's and x86_64's inhabitants of a metatype word differ.
public struct Unreflected {
    public var t: Any.Type??
    public init(t: Any.Type??) { self.t = t }
}
