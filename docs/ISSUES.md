# Runtime issues and measured scope

## `Optional<AnyObject>` in a published Swift value

On the 26B131 image Notification Center crash-looped 20 times in one session,
each time with `Swift value boundary refused: result publication has
unresolved active representation (... type=Swift.Optional<Swift.AnyObject>)`.
A SwiftUI view value returned by guest code (`View.body`, an associated
result) holds an enum whose empty cases sit in a pointer's extra inhabitants.
hao therefore walks the whole value, and it reached
`NSNotificationCenter.Publisher.object`, which is `Optional<AnyObject>`.
`common_leaf_value` admitted an `Optional` of a Swift class, of an imported
ObjC class, or nil of a one-witness class existential, but not `AnyObject`.

Native Swift programs on arm64 and x86_64 give the same `AnyObject`:
existential kind 0x303, flags 0, no protocols, the `$sBOWV`
(Builtin.UnknownObject) witnesses (size 8, flags 0x10007, xi 0x7fffffff).
Its first `Optional` has size 8, flags 0x210007 and nil 0 on both. hao now
admits exactly that first `Optional`, with the value rule of imported ObjC
references: nil, or a word the witnesses' own tag function calls a live
value, tagged pointers included. `Optional<Optional<AnyObject>>` stays
refused, because its second nil is an extra inhabitant that is 1 on arm64
and 2 on x86_64.

`tests/host/swiftvalue.sh` walks native x86_64 values with an `NSObject??`
field (walked, with extra inhabitants to patch) next to an
`Optional<AnyObject>` holding nil, an NSObject, a Swift object or a tagged
NSNumber. Before the change every case was refused at
`Optional<AnyObject>`, the Notification Center refusal. After it they are
walked with the patches of an `Optional<NSObject>` control.

The widget extensions on the same image refused two more first `Optional`s
in values crossing the boundary:

- `Optional<StocksWidget.ValueChangeTrend>` (StocksWidget). This is an enum
  with no payload, so the Optional is one byte: a case is 0..n-1 and nil is n,
  measured identical on both architectures. The byte rule needed 9 bytes or
  more. Now the first Optional of a payload-free enum of up to 255 cases is
  admitted when its byte is at most n.
- `Optional<Foundation.URL>` (NewsTag, NewsToday2). URL is a struct whose only
  field is `any _URLProtocol`, a class-bound existential with one witness
  table: 16 bytes, measured identical on both architectures, nil is a zero
  object word. Only nil of a bare such existential had been admitted, and
  only with flags exactly 0x210007, while URL's Optional also has
  0x2000000 (addressable for dependencies), which changes no layout. Now
  that wrapper is admitted with a live value too. The object word must pass
  the existential's own tag function and the witness table must lie in no
  guest image, since a guest conformance's table is arm64 code. A bare
  existential stays nil-only, because a weak field reports the same type and
  its nonzero storage is not a strong pointer.

`tests/host/swiftvalue.sh` covers both (nil and live values beside the
`NSObject??` field): refused before, walked after. Its URLs all have native
witness tables, so the refusal of a guest witness table is not exercised
there. Still open: WeatherWidget and WeatherIntents crash inside native
value-witness calls (`swift_retain` in `swift_cvw_initWithCopy`, an
`OpaqueExistentialBox` destroy at an address ending in fff9).

## `%a` slots of variadics whose format family is assumed

On a 26B131 image, sharingd crashed with SIGSEGV in CoreUtils'
`PrintFCoreVAList` whenever the Bluetooth LE private address rotated. It
logs `LogPrintF(cat, func, 30, "BLE address changed: %.6a\n", bytes)`.
`LogPrintF` has no SDK format attribute, so `src/varsniff.c` typed its slots
by assuming the last fixed argument is a printf format. printf's `%a` takes
a double, so the pointer went to xmm0. CoreUtils reads `%a` as a pointer to
an address and took r8, which held 0.

Native arm64 CoreUtils on the development host (27.2, 26B123), read through
`VSNPrintF` with marker slots, gives these rules. `%a` takes one pointer with
any precision or flag: `.4` is IPv4, `.6` Ethernet, `.8` 8 bytes, `.16`
IPv6, `##` a sockaddr, and a bare `%a` prints an error. `%A`, `%D`, `%O` and
the `L` and `q` modifiers are unknown and take no argument. `v` is a
NumVersion conversion, not a modifier. `%m` takes one integer, `%H` takes a
pointer and two integers, and `%N` takes nothing. `%e %f %g %E %F %G` take
doubles.

When the family is only assumed, a `%a`/`%A` slot without `L` now goes by its
value. An x86_64 user pointer is below 2^47, which read as a double is zero or
a positive subnormal, and no other double is. Those values go to a GPR and
everything else to an XMM register. Zero is both NULL and 0.0. It counts as an
address when the conversion has a `#` flag or a precision, the forms CoreUtils
needs, and as a double otherwise. Formats with an SDK attribute are typed as
before. `tests/guest/varsniff.sh` now checks printf `%a` (zero, precision,
`#`, `%A`) and CoreUtils `SNPrintF` (`%.6a`, `%.4a`, `%##a`, a NULL `%.6a`
followed by integer, string and double slots) against native arm64. Before
the fix the CoreUtils case crashed in `PrintFCoreVAList` under the JIT and
`--interp`.

Remaining limitations:

- printf `%#a` or `%.Na` of exactly 0.0, or `%a` of a positive subnormal,
  in an unattributed variadic followed by slots of the other class: those
  later slots read one register off.
- CoreUtils conversions outside printf's set (`%m`, `%H`, `%N`, `%{...}`,
  `%v`) still make the typer give up, and every slot then goes to a GPR. A
  real `%f` in such a format prints a wrong value but cannot crash. CoreUtils'
  `%D`, `%O` and `%q` take no argument but are typed as one slot.

## Signal callback reentry during Objective-C enum initialization

The intermittent `tests/guest/uresume.sh` `alarmnx` SIGILL was a runtime
signal-reentrancy defect. Every untyped native-to-guest callback calls
`hao_objc_enum_args`, which lazily registers selectors with `pthread_once`.
A SIGALRM arriving during that first initialization ran a guest handler
that made another native-to-guest callback. That callback entered the same
unfinished `pthread_once` on the same thread.

A JIT diagnostic run with `HAO_SIGRETURN_KERNEL=1` on the Apple Silicon
development host captured RIP `0x7ff80af03d4b`, resolving to
`libsystem_platform.dylib`'s `_os_once_gate_recursive_abort+0x17`, with
runtime depth 2, signal depth 2, signal nesting 1 and critical depth 0.
The stack contained both `hao_objc_enum_args` calls and the interrupted
outer `enum_init`. This was not the `ud2` after a failed `__sigreturn`.
The guest PC was the callback target (file VM address `0x1000015e0` in
that build), not an instruction fetched from an incorrectly located JIT
block. Temporary diagnostic instrumentation was removed after capture.

`hao_objc_enum_args` now surrounds the complete `pthread_once` call with
hao's existing critical-section protocol. An arriving asynchronous guest
signal is deferred until `pthread_once` has published completion, then
redelivered by `hao_crit_leave`. Guarding only the initializer body would
leave a reentry window before `pthread_once` publishes completion. The
change applies to every caller of the enum bridge, independent of the
program, execution engine and signal-return route.

On 10 November 2026, unmodified `1d230dc` with `TEST_SWITCHES=1` failed
51 of 1,200 fresh processes, in two batches of 100 per route. Every captured
failure had the recursive-once callback stack, including the interpreter
failure. With the fix, all six routes passed 500/500, or 0 failures in
3,000 fresh processes. The native ARM64 reference passed 100/100 fresh
processes.

| Engine | User resume, before | Kernel resume, before | User resume, after | Kernel resume, after |
|---|---:|---:|---:|---:|
| JIT | 20/200 | 19/200 | 0/500 | 0/500 |
| Interpreter | 0/200 | 1/200 | 0/500 | 0/500 |
| AOT | 4/200 | 7/200 | 0/500 | 0/500 |

Additional unmodified-runtime controls used JIT with the same fixture:

| Control | User resume failures | Kernel resume failures |
|---|---:|---:|
| `HAO_NO_CHAIN=1` | 1/100 | 1/100 |
| `HAO_JIT_HELPERS_ONLY=1` | 0/100 | 0/100 |
| Both `39d42d1` and `09a3420` reverted in a scratch build | 0/100 | 0/100 |

The no-chain failures had the same recursive-once stack. Zero failures in
these smaller controls are timing observations, not evidence that the
controls fix the defect: the second unmodified JIT batch also passed
100/100 on each route after the first batch failed 39/200. The captured
once recursion and the interpreter failure identify a cause independent
of either optimization.

Each AOT build uses its own newly generated cache; the fixed build's
`--stats` check reported `aot_blocks=256 aot_rejected=0`. JIT and
interpreter runs use `HAO_NO_AOT=1`. User and kernel routes set
`HAO_SIGRETURN_USER=1` and `HAO_SIGRETURN_KERNEL=1`, respectively.
Each guest process has a 60-second kill timeout; stdout and exit status
are compared with the native ARM64 `alarmnx` reference. The tests retain
their 100-microsecond timer and require at least 5,000 checked callbacks,
50 alarms and zero callback errors. No retries or relaxed expectations
are used.

`make check` passed: `make test`, `oracle-asan`, and the `TEST_SWITCHES`
guest harness (64 passed, 0 failed, 1 known failure in `guest/xinhab`,
8 expected skips, 73 scripts total). The complete `guest/uresume` script
passed, including its state, mask, nesting, exception, thread and
alternate-stack cases and signal-return counters. The gate's host fork
stress also passed 300/300 without hangs or errors; it does not resolve
the historical fork issue below.

The host was macOS 27.2 (26B123) on ARM64, running x86_64 hao through
Rosetta. The captured recursive-once ordering is a runtime bug that also
applies on Intel; execution on an
Intel kernel remains necessary to confirm the fix there. Rosetta's
separate restriction on abandoning nested signal frames still applies,
as described in `src/signals.c`. This result does not establish that all
signal-handler paths are reentrant or change the concurrent fallback in
`hao_jit_locate`. Application and release-image acceptance remain separate.

## Intermittent host fork stress failure

`tests/guest/fork.sh`'s host `test_fork all` has historical SIGSEGV/SIGBUS
failures. Reports from November 2026 include JIT emission
(`x_mov_ld`/`exit_indirect`) and bridge binding (`type_at`) stacks.
`test_fork` does not install hao's guest signal handlers, and those stacks
do not show the recursive enum initialization above. No shared cause has
been established; the fork failure remains open.
