# Objective-C name availability before Swift class initialization

Foundation-only command-line regression. `EarlySwift` has a resilient stored
`AsyncThrowingStream<[OrderLateObjC], Error>.Continuation?` property. Its Swift
metadata initializer needs the later ordinary Objective-C class's registered
name while completing the field layout.

The same authored object files are linked twice: Swift classes first versus
ordinary Objective-C classes first. `OrderLateBase` is linked after its subclass
in both executables, also checking superclass-before-subclass registration.
Native ARM and x86 print identical output for both orders.

Run it:

```sh
HAO=/path/to/hao python3 tests/guest/objc-class-order/run.py
```

`OUT` selects the output folder. The test requires both orders to pass under
JIT, interpreter and AOT, and verifies that the AOT cache loads.
`EXPECT_FAILURE=1` checks a runtime without the fix instead: Swift-first runs
must fail before `main` while Objective-C-first runs pass. Every executable is
ad-hoc signed, every execution has a timeout, and no GUI frameworks are used.

A direct Objective-C subclass of a Swift class is not included: Swift's
generated header rejects source-level subclassing. The superclass control uses
ordinary compiler-generated Objective-C inheritance instead.
