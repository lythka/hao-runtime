// Native helper of tests/guest/swiftcallee.sh: a generic function whose closure argument returns T
// indirectly (x86_64 rax, arm64 x8). It imports nothing.
public func run<T>(_ f: () -> T) -> T { f() }
