# Runtime issues and measured scope

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
