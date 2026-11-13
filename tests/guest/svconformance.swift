// tests/guest/svconformance.sh: a guest conformance (static and generic) to a native protocol must not pass
// as native; one to a guest protocol, which native code cannot name, may.
import Foundation
protocol R: AnyObject { associatedtype A; func read() -> A }
final class StaticR: R { func read() -> Int { 7 } }
final class GenericR<T: Equatable>: R { let x:T; init(_ x:T){self.x=x}; func read()->T{x} }
final class StaticN: Identifiable { var id: Int { 7 } }
final class GenericN<T: Hashable>: Identifiable { let x:T; init(_ x:T){self.x=x}; var id: T { x } }
struct Wrap { var object:any R }
struct Value { var e:NSObject??; var wrapped:Wrap? }
struct WrapN { var object:any Identifiable & AnyObject }
struct ValueN { var e:NSObject??; var wrapped:WrapN? }
typealias Walk = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UInt32, UnsafeMutablePointer<UnsafeRawPointer?>) -> Int32
typealias Image = @convention(c) (UInt64) -> UnsafeRawPointer?
let lib=dlopen(CommandLine.arguments[1],RTLD_NOW)!
let walk=unsafeBitCast(dlsym(lib,"hao_sv_collect_empty_patches_detail")!,to:Walk.self)
let image=unsafeBitCast(dlsym(lib,"hao_image_for_addr")!,to:Image.self)
let patches=UnsafeMutableRawPointer.allocate(byteCount:64*40,alignment:8)
func table<W>(_ w: inout W) -> (Bool, Bool) {
 withUnsafeBytes(of:&w) { b in
  let table=b.load(fromByteOffset:8,as:UInt64.self)
  let descriptor=UnsafeRawPointer(bitPattern:UInt(table))!.load(as:UInt64.self)
  return (image(table) != nil, image(descriptor) != nil)
 }
}
func walked<V>(_ v: inout V) -> Int32 {
 var unresolved:UnsafeRawPointer?=nil
 let metadata=unsafeBitCast(V.self,to:UnsafeRawPointer.self)
 return withUnsafePointer(to:&v) { walk(metadata,UnsafeRawPointer($0),patches,64,&unresolved) }
}
// A guest protocol's existential: admitted with its guest conformances.
for (name,obj):(String,any R) in [("static",StaticR()),("generic",GenericR(7))] {
 var w=Wrap(object:obj)
 let (t,d)=table(&w)
 print(name,"table guest",t,"descriptor guest",d)
 var v=Value(e:nil,wrapped:w)
 print(name,"walk",walked(&v) < 0 ? "refused" : "admitted")
}
// A native protocol's existential: a guest conformance's table is arm64 code native code may call.
for (name,obj):(String,any Identifiable & AnyObject) in [("native static",StaticN()),("native generic",GenericN(7))] {
 var w=WrapN(object:obj)
 let (t,d)=table(&w)
 print(name,"table guest",t,"descriptor guest",d)
 var v=ValueN(e:nil,wrapped:w)
 print(name,"walk",walked(&v) < 0 ? "refused" : "admitted")
}
// Opaque (not class-bound) existentials, as ControlCenter's `any ControlCenterDisplayable`: the same rule.
protocol D { func name() -> String }
struct SmallD: D { func name() -> String { "small" } }
struct BigD: D, CustomStringConvertible { var a = [1, 2]; var b = "x"; var c = 3; func name() -> String { "big" }; var description: String { "big" } }
struct ValueD { var e:NSObject??; var d:any D; var f:C?? }
struct ValueS { var e:NSObject??; var d:any CustomStringConvertible }
final class C {}
for (name,obj):(String,any D) in [("opaque small",SmallD()),("opaque big",BigD())] {
 var v=ValueD(e:nil,d:obj,f:.some(nil))
 print(name,"walk",walked(&v) < 0 ? "refused" : "admitted")
}
var s=ValueS(e:nil,d:BigD())
print("opaque native","walk",walked(&s) < 0 ? "refused" : "admitted")
