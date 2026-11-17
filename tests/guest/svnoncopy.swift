// A noncopyable native type in the Swift value walk (tests/guest/svnoncopy.sh): a Mutex<Int> that an
// escaping closure of unspecialized generic code captures lives in a box swift_allocBox makes, whose
// element type the walk examines; "stream" does the same with a Mutex<AsyncStream<@Sendable () -> ()>
// .Continuation>. The types' value witness flags say IsNonCopyable; only a SAME row of the walk's facts
// admits them.
import Foundation
import Synchronization

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func counter<T: Sendable>(_ v: T, _ step: @escaping (inout T) -> Void) -> () -> T {
    let m = Mutex(v)
    return { m.withLock { step(&$0); return $0 } }
}

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        if CommandLine.arguments.dropFirst().first == "stream" {
            let (stream, c) = AsyncStream<@Sendable () -> ()>.makeStream()
            let f = counter(c) { _ in }
            let sent = f().yield({ })
            f().finish()
            print("stream", sent, f().yield({ })); _ = stream
            return
        }
        let f = counter(41) { $0 += 1 }
        print("mutex", f(), f(), f())
    }
}
