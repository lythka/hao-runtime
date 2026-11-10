public final class Root {
    private var storage = 11
    public init() {}
    public var first: Int {
        get { storage }
        set { storage = newValue }
    }
    public var second: Int {
        get { storage }
        set { storage = newValue }
    }
}
@inline(never) func opaque<T>(_ value: T) -> T { value }
let first = opaque(\Root.first)
let second = opaque(\Root.second)
let value = Root()
value[keyPath: first] = 73
print("read", value[keyPath: first], value[keyPath: second])
print("equal", first == second)
print("hash", first.hashValue == second.hashValue)
