// Key path reads and writes through libswiftCore's key path entry points (swift_getAtKeyPath,
// swift_setAtWritableKeyPath, swift_setAtReferenceWritableKeyPath, swift_modifyAtWritableKeyPath,
// swift_modifyAtReferenceWritableKeyPath, swift_readAtKeyPath, swift_getAtPartialKeyPath,
// swift_getAtAnyKeyPath): stored, computed (guest accessors), class and optional-chained components,
// concrete and generic roots, small and large values, and an environment-writing modifier shaped as
// SwiftUI's _EnvironmentKeyWritingModifier<V> ({keyPath, value}, applied generically).
// Built -O for arm64e (guest) and arm64 (reference) by tests/guest/keypath.sh.

struct Big: Equatable { var a = 1, b = 2, c = 3, d = 4, e = 5 }
enum Small: UInt8 { case zero, one, two }

struct Env {
    var flag = false
    var scale = 1.0
    var small = Small.zero
    var name = "env"
    var big = Big()
    var inner = Inner()
    private var backing = 7
    var computed: Int { get { backing * 2 } set { backing = newValue / 2 } }
    var opt: Inner? = Inner()
}

struct Inner: Equatable { var x = 10; var s = "in" }

final class Box {
    var n = 3
    var s = "box"
    var big = Big()
    var env = Env()
    private var back = 5
    var comp: Int { get { back + 100 } set { back = newValue - 100 } }
    weak var peer: Box?
}

class Base { var v = 1 }
final class Derived: Base { var w = 2 }

@inline(never) func set<R, V>(_ r: inout R, _ kp: WritableKeyPath<R, V>, _ v: V) { r[keyPath: kp] = v }
@inline(never) func get<R, V>(_ r: R, _ kp: KeyPath<R, V>) -> V { r[keyPath: kp] }
@inline(never) func refset<R, V>(_ r: R, _ kp: ReferenceWritableKeyPath<R, V>, _ v: V) { r[keyPath: kp] = v }
@inline(never) func modify<R>(_ r: inout R, _ kp: WritableKeyPath<R, Int>) { r[keyPath: kp] += 41 }
@inline(never) func refmodify<R>(_ r: R, _ kp: ReferenceWritableKeyPath<R, Int>) { r[keyPath: kp] += 41 }
@inline(never) func modifyString<R>(_ r: inout R, _ kp: WritableKeyPath<R, String>) { r[keyPath: kp].append("+m") }
@inline(never) func partial<R>(_ r: R, _ kp: PartialKeyPath<R>) -> Any { r[keyPath: kp] }
@inline(never) func any(_ r: Any, _ kp: AnyKeyPath) -> Any? { r[keyPath: kp] }
@inline(never) func count<R>(_ r: R, _ kp: KeyPath<R, String>) -> Int { r[keyPath: kp].count }   // read coroutine

// SwiftUI's _EnvironmentKeyWritingModifier<Value> and its rule: environment[keyPath: modifier.keyPath] = modifier.value
struct Writer<V> { var keyPath: WritableKeyPath<Env, V>; var value: V }
@inline(never) func apply<V>(_ m: Writer<V>, _ e: inout Env) { e[keyPath: m.keyPath] = m.value }
@inline(never) func makeWriter<V>(_ kp: WritableKeyPath<Env, V>, _ v: V) -> Writer<V> { Writer(keyPath: kp, value: v) }
@inline(never) func opaque<V>(_ v: V) -> Any { v }

@inline(never) func dyn<T>(_ t: T) -> T { t }   // key paths built at run time, not folded by the optimizer

func show(_ s: String, _ v: Any) { print(s, v) }

var e = Env()
set(&e, dyn(\Env.flag), true)
set(&e, dyn(\Env.scale), 2.5)
set(&e, dyn(\Env.small), .two)
set(&e, dyn(\Env.name), "written")
set(&e, dyn(\Env.big), Big(a: 9, b: 8, c: 7, d: 6, e: 5))
set(&e, dyn(\Env.inner.x), 77)
set(&e, dyn(\Env.computed), 90)
set(&e, dyn(\Env.opt), nil)
show("set:", [e.flag, e.scale, e.small, e.name, e.big.a, e.big.e, e.inner.x, e.computed, e.opt == nil] as [Any])
show("get:", [get(e, dyn(\Env.flag)), get(e, dyn(\Env.scale)), get(e, dyn(\Env.small)), get(e, dyn(\Env.name)),
               get(e, dyn(\Env.big)).c, get(e, dyn(\Env.computed)), get(e, dyn(\Env.inner.s))] as [Any])
var e2 = Env()
set(&e2, dyn(\Env.opt!.x), 55)
show("optchain:", [e2.opt?.x ?? -1, get(e2, dyn(\Env.opt?.x)) ?? -1] as [Any])
modify(&e2, dyn(\Env.inner.x)); modify(&e2, dyn(\Env.computed)); modifyString(&e2, dyn(\Env.name))
show("modify:", [e2.inner.x, e2.computed, e2.name] as [Any])
show("read:", count(e2, dyn(\Env.name)))

let b = Box(), b2 = Box()
refset(b, dyn(\Box.n), 33)
refset(b, dyn(\Box.s), "boxed")
refset(b, dyn(\Box.big), Big(a: 0, b: 0, c: 0, d: 0, e: 42))
refset(b, dyn(\Box.comp), 250)
refset(b, dyn(\Box.env.flag), true)
refset(b, dyn(\Box.env.name), "deep")
refset(b, dyn(\Box.peer), b2)
refset(b, dyn(\Box.peer!.n), 66)
refmodify(b, dyn(\Box.n)); refmodify(b, dyn(\Box.comp)); refmodify(b, dyn(\Box.env.inner.x))
show("ref:", [b.n, b.s, b.big.e, b.comp, b.env.flag, b.env.name, b.env.inner.x, b2.n, get(b, dyn(\Box.peer?.s)) ?? "-"] as [Any])
var bv = b
set(&bv, dyn(\Box.n), 1000)      // a WritableKeyPath with a class root: a reference write through the inout entry point
show("refroot:", b.n)

let d = Derived()
refset(d, dyn(\Derived.v), 11); refset(d, dyn(\Derived.w), 22)
show("class hierarchy:", [d.v, d.w, get(d, dyn(\Base.v))] as [Any])

show("partial:", [partial(e, dyn(\Env.name)), partial(e, dyn(\Env.small)), partial(b, dyn(\Box.n))] as [Any])
show("any:", [any(e, dyn(\Env.flag)) ?? "nil", any(b, dyn(\Box.s)) ?? "nil", any(e, dyn(\Box.n)) ?? "nil"] as [Any])

// appended and identity key paths
let app = dyn(\Env.inner).appending(path: dyn(\Inner.x))
var e3 = Env()
set(&e3, app, 123)
set(&e3, dyn(\Env.self), e3)
show("appended:", [e3.inner.x, get(e3, app)] as [Any])

// the environment-writing modifier, stored in an existential and applied later
var mods: [Any] = [makeWriter(dyn(\Env.flag), true), makeWriter(dyn(\Env.small), Small.one),
                   makeWriter(dyn(\Env.scale), 3.0), makeWriter(dyn(\Env.name), "modified"),
                   makeWriter(dyn(\Env.big), Big(a: 5, b: 5, c: 5, d: 5, e: 5))]
mods.append(opaque(makeWriter(dyn(\Env.computed), 8)))
var e4 = Env()
for m in mods {
    if let w = m as? Writer<Bool> { apply(w, &e4) }
    if let w = m as? Writer<Small> { apply(w, &e4) }
    if let w = m as? Writer<Double> { apply(w, &e4) }
    if let w = m as? Writer<String> { apply(w, &e4) }
    if let w = m as? Writer<Big> { apply(w, &e4) }
    if let w = m as? Writer<Int> { apply(w, &e4) }
}
show("writers:", [e4.flag, e4.small, e4.scale, e4.name, e4.big.b, e4.computed] as [Any])

// equality and hashing of key paths made in different places
show("equal:", [dyn(\Env.flag) == \Env.flag, dyn(\Env.inner.x) == app, dyn(\Box.n).hashValue == (\Box.n).hashValue] as [Any])
print("keypath: done")
