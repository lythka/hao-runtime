// tests/guest/regvar.sh: the Swift objects the register-variant cases retain and release
// (regvar.c drives them through regvar_arm64.s). A final class, so its instances are native
// Swift heap objects (swift_retain / swift_release; libobjc's variants reach them through
// objc_retain's Swift path, as for any Swift object on Darwin).
var deinits = 0
final class Obj {
    let tag: Int
    init(_ t: Int) { tag = t }
    deinit { deinits += 1 }
}

@_cdecl("rv_new") public func rvNew(_ t: Int) -> UnsafeMutableRawPointer {
    Unmanaged.passRetained(Obj(t)).toOpaque()
}
@_cdecl("rv_rc") public func rvRc(_ p: UnsafeMutableRawPointer) -> Int {
    Int(_getRetainCount(Unmanaged<Obj>.fromOpaque(p).takeUnretainedValue()))
}
@_cdecl("rv_tag") public func rvTag(_ p: UnsafeMutableRawPointer) -> Int {
    Unmanaged<Obj>.fromOpaque(p).takeUnretainedValue().tag
}
@_cdecl("rv_retain") public func rvRetain(_ p: UnsafeMutableRawPointer) {
    _ = Unmanaged<Obj>.fromOpaque(p).retain()
}
@_cdecl("rv_release") public func rvRelease(_ p: UnsafeMutableRawPointer) {
    Unmanaged<Obj>.fromOpaque(p).release()
}
@_cdecl("rv_deinits") public func rvDeinits() -> Int { deinits }
