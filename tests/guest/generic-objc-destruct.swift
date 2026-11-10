import Foundation

final class Target: NSObject { }
final class WeakBox<T>: NSObject {
    weak var target: AnyObject?
    let value: T
    init(_ target: AnyObject, _ value: T) { self.target = target; self.value = value }
    @objc func score() -> Double { target == nil ? -1.0 : 3.5 }
}
@inline(never) func exercise<T>(_ value: T) -> Int {
    let target = Target()
    let objects = NSMutableArray()
    for _ in 0..<3 { objects.add(WeakBox(target, value)) }
    let score = (objects[0] as! WeakBox<T>).score()
    objects.removeAllObjects() // Native objc_release invokes the generic .cxx_destruct.
    return score == 3.5 && objects.count == 0 ? 1 : 0
}
print("generic weak boxes", exercise(17), exercise("payload"))
