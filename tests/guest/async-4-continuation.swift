// async ladder 4: checked/unsafe continuations resumed from Dispatch (native threads).
import Dispatch
import Darwin
setvbuf(stdout, nil, _IOLBF, 0)  // line-buffered: partial output survives a crash

func viaQueue(_ x: Int) async -> Int {
    await withCheckedContinuation { (k: CheckedContinuation<Int, Never>) in
        DispatchQueue.global().asyncAfter(deadline: .now() + .milliseconds(10)) { k.resume(returning: x * 3) }
    }
}

func viaUnsafe(_ s: String) async -> String {
    await withUnsafeContinuation { k in
        DispatchQueue(label: "hao.test").async { k.resume(returning: s + "!") }
    }
}

print("start")
print("viaQueue(14) =", await viaQueue(14))
print("viaUnsafe =", await viaUnsafe("resumed"))
var acc = 0
for i in 0..<5 { acc += await viaQueue(i) }
print("acc =", acc)
print("done")
