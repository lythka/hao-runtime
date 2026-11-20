// Native generic functions whose tuple result Swift returns as one indirect result per element, in the
// caller's storage: native code makes T? = nil there with its own witnesses. With T = C? (a class) or
// CGImage? the nil is C?? / CGImage??'s outer nil, its word 1 on arm64 and 2 on x86_64.
@inline(never) public func emptyPair<T, U>(_ u: U, _: T.Type) -> (T?, U) { (nil, u) }
@inline(never) public func fullPair<T, U>(_ t: T, _ u: U) -> (T?, U) { (t, u) }
@inline(never) public func emptyOne<T>(_: T.Type) -> T? { nil }
// A generic argument native code forwards into its result, and one it replaces in place.
@inline(never) public func same<T>(_ t: T) -> T { t }
@inline(never) public func replace<T>(_ a: inout T, _ b: T) -> T { let old = a; a = b; return old }
