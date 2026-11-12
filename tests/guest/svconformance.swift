// tests/guest/svconformance.sh: a guest conformance (static and generic) must not pass as native.
import Foundation
protocol R: AnyObject { associatedtype A; func read() -> A }
final class StaticR: R { func read() -> Int { 7 } }
final class GenericR<T: Equatable>: R { let x:T; init(_ x:T){self.x=x}; func read()->T{x} }
struct Wrap { var object:any R }
struct Value { var e:NSObject??; var wrapped:Wrap? }
typealias Walk = @convention(c) (UnsafeRawPointer, UnsafeRawPointer, UnsafeMutableRawPointer, UInt32, UnsafeMutablePointer<UnsafeRawPointer?>) -> Int32
typealias Image = @convention(c) (UInt64) -> UnsafeRawPointer?
let lib=dlopen(CommandLine.arguments[1],RTLD_NOW)!
let walk=unsafeBitCast(dlsym(lib,"hao_sv_collect_empty_patches_detail")!,to:Walk.self)
let image=unsafeBitCast(dlsym(lib,"hao_image_for_addr")!,to:Image.self)
let patches=UnsafeMutableRawPointer.allocate(byteCount:64*40,alignment:8)
for (name,obj):(String,any R) in [("static",StaticR()),("generic",GenericR(7))] {
 var w=Wrap(object:obj)
 withUnsafeBytes(of:&w) { b in
  let table=b.load(fromByteOffset:8,as:UInt64.self)
  let descriptor=UnsafeRawPointer(bitPattern:UInt(table))!.load(as:UInt64.self)
  print(name,"table guest",image(table) != nil,"descriptor guest",image(descriptor) != nil)
 }
 var v=Value(e:nil,wrapped:w); var unresolved:UnsafeRawPointer?=nil
 let metadata=unsafeBitCast(Value.self,to:UnsafeRawPointer.self)
 let n=withUnsafePointer(to:&v) { walk(metadata,UnsafeRawPointer($0),patches,64,&unresolved) }
 print(name,"walk",n)
}
