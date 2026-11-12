# Tests

```sh
make test          # quick gate: unit, oracle, aot, bridge/async/dyld/mulhigh, forktest, jitlocktest, check_x86
make test-guest    # every other script that runs unattended here (tests/check.sh)
make check         # the gate for runtime changes: test, oracle-asan, test-guest
make oracle-asan   # oracle checks with the translator under ASan + UBSan
```

`tests/check.sh` runs the entries of `tests/check.list` in parallel (`J`
jobs, `make test-guest` passes `CHECK_J`, default 8), each in its own process
group with a timeout and a private `mktemp -d` output directory passed as
`OUT`. It prints one line per entry:

| Line | Meaning |
|---|---|
| `PASS` | exit 0, no `SKIP` line |
| `FAIL` | non-zero exit, or the entry's timeout |
| `SKIP` | the script printed `SKIP <reason>` lines (exit 0, 2 or 77); each reason is matched against `tests/expected-skips.txt`, and one that is not listed is shown as `UNEXPECTED` and fails the run |
| `XFAIL` | an entry marked `xfail` in `tests/check.list` failed, as recorded below |
| `XPASS` | an `xfail` entry passed: fails the run until the mark is removed |

The last line counts each kind. The exit status is 1 on any `FAIL`,
`XPASS` or unexpected skip. `KEEP=1` keeps the logs (`<dir>/<name>/log`);
they are kept anyway when the run fails. `EXPECTED_SKIPS=file` adds
host-specific expected skips. `tests/check.sh NAME...` runs some entries.

Without `OUT`, every script now works in a new `mktemp -d -t hao-NAME`
directory instead of a fixed `$TMPDIR/hao-NAME`; callers that pass `OUT` are
unchanged.

## What you need

Everything in `make test` and `make check` is self-contained apart from the
host: an Apple silicon Mac with Rosetta (the arm64 references run natively,
hao's x86_64 code under Rosetta), Xcode's clang and swiftc with arm64e,
Python 3.14 and `timeout` in `PATH`. No test needs root or the network, and
none launches a GUI application, app extension or XPC service.

Some cases use inputs that are not in this repository. Without them those
cases print a `SKIP` line that `tests/expected-skips.txt` accepts, so the run
still passes:

| Input | Used by | What to supply |
|---|---|---|
| `HAO_FS` | `guest/cli` (Apple-signed `ls`, trusted cdhash list), `tools` (MenuBarAgent bundle) | the path of a mounted macOS 27 System volume (read-only is enough) |
| `KC` | `tools` stamp cases | a BootKernelExtensions.kc of the target kernel |
| `PROFILE` | `host/sigpolicy` profile cases | a sandbox profile to check |

## Inventory

Measured on an Apple silicon development host (macOS 27.2, 16 cores) with
`HAO_FS` set. Times are wall-clock seconds; `test-guest` times are under
`J=8` parallel load.

`make test` (173 s in all; each target alone):

| Target | Result | Time | Needs |
|---|---|---|---|
| `unit` | PASS | 3 | (now also `tests/aot/jit_vector.c`, below) |
| `oracle` | PASS | 153 | |
| `aot` | PASS | 6 | |
| `bridgetest` | PASS | <1 | |
| `async-contract-test` | PASS | 4 | python3.14 |
| `legacy-dyld-test` | PASS | 1 | python3.14 |
| `indexed-mulhigh-test` | PASS | <1 | python3.14 |
| `forktest` (added to `make test`) | PASS | 7 | |
| `jitlocktest` (added to `make test`) | PASS | 37 | `/usr/bin/awk` copies |
| `tools/check_x86.sh` | PASS | 1 | |

`make test-guest` (about 150 s; 77 entries: 74 PASS, 1 XFAIL, 2 expected SKIP with `HAO_FS` set, 3 without it):

| Entry | Expected | Time | Needs / skip reason |
|---|---|---|---|
| guest/aggspi6 | PASS | 3 | |
| guest/atoken | PASS | 1 | |
| guest/bitfields | PASS | 2 | |
| guest/blocks2 | PASS | 2 | |
| guest/bridgefix | PASS | 2 | |
| guest/bridgeunit | PASS | 3 | |
| guest/callback-routes | PASS | 2 | |
| guest/callbacks | PASS | 10 | |
| guest/cfnull | PASS | 1 | |
| guest/chkstk | PASS | 1 | |
| guest/cli | PASS | 3 | two cases need `HAO_FS` (expected SKIP without it); `sandbox-exec` |
| guest/crypto | PASS | 1 | |
| guest/cuvalist | PASS | 2 | private CoreUtils on the host (`VSNPrintF`, `NSErrorV`, ...) |
| guest/cxxabi | PASS | 10 | |
| guest/datacall | PASS | 1 | |
| guest/dsc | PASS | 3 | the host's arm64e shared cache |
| guest/dscswift | PASS | 10 | the host's arm64e shared cache |
| guest/dylibs | PASS | 4 | |
| guest/eh | PASS | 19 | |
| guest/fenv | PASS | 1 | |
| guest/fma32 | PASS | 2 | |
| guest/footprint | PASS | 59 | `footprint(1)` |
| guest/fork | PASS | 45 | |
| guest/fpscalar | PASS | 55 | |
| guest/fullcache | PASS | 6 | |
| guest/gss | PASS | 1 | |
| guest/heapprobe | PASS | 1 | |
| guest/hfasret | PASS | 2 | |
| guest/identity | PASS | 5 | |
| guest/indirect-x8 | PASS | 1 | |
| guest/keypath | PASS | 2 | |
| guest/libval | PASS | 16 | |
| guest/msgfnptr | PASS | 4 | |
| guest/narrowret | PASS | 4 | |
| guest/nativesig | PASS | 25 | |
| guest/nccache | PASS | 3 | |
| guest/nojit | PASS | 5 | |
| guest/objcimp | PASS | 4 | |
| guest/objcva | PASS | 3 | |
| guest/optwitness | PASS | 1 | guest calls of native Optional, tuple and array witnesses on nested Optionals, also beside guest multi-payload enums (tag byte with several empty cases, spare-bit tags decoded from the image's layout record), and `Data??` whose nils are translated between the architectures' spare-bit encodings; see below |
| guest/oslog | PASS | 79 | `log show`; was a failure that depended on the checkout's path, see below. Under heavy parallel load one `oslogswift` mode can miss its records; it passes alone |
| guest/signals | PASS | 36 | |
| guest/sret-forward | PASS | 3 | |
| guest/stackargs | PASS | 2 | |
| guest/stackguard | PASS | 1 | |
| guest/svarchdiff | PASS | 1 | the walker on guest values holding Swift.Float16 (declared differently by architecture, frozen): refused; its Float16 refusals are checked only where this host's libswiftCore is the build `src/archdiff_gen.inc` describes |
| guest/mkgeometry | PASS | 2 | MapKit geometry functions passing or returning 32-byte HFAs (MKMapRect, MKCoordinateRegion) by value, including two without a header: native arm64 results under JIT, `--interp`, helpers-only and AOT; the six rows `src/aggspi_gen.inc` added for them fail without them |
| guest/svconformance | PASS | 2 | |
| guest/swift-object-callbacks | PASS | 5 | |
| guest/swiftlate | PASS | 4 | |
| guest/swiftls | PASS | 5 | |
| guest/swiftmeta | PASS | 8 | |
| guest/swiftpop | PASS | 4 | |
| guest/swiftthunks | PASS | 6 | |
| guest/threads | PASS | 2 | |
| guest/uresume | PASS | 26 | |
| guest/varsniff | PASS | 2 | |
| guest/xinhab | XFAIL | 2 | see known failures |
| guest/xpac | PASS | 1 | |
| guest/run-async | PASS | 9 | |
| guest/keypath-identity | PASS | 2 | |
| guest/late-method-signatures | PASS | 10 | |
| guest/native-bundle | PASS | 5 | loads a plug-in `.bundle` into a command-line guest |
| guest/objc-class-order | PASS | 6 | |
| guest/objcvascope | PASS | 2 | |
| host/runtime | PASS | 2 | |
| host/chain | PASS | 6 | |
| host/commpage | PASS | 1 | |
| host/sigpolicy | SKIP (expected) | 1 | the profile cases need `PROFILE`; the hao cases x86_64 hardware |
| host/near-map | PASS | 1 | |
| host/dscselhoist | PASS | 1 | |
| host/swiftvalue | PASS | 1 | |
| host/swiftlayout | PASS | 4 | builds one Swift program for arm64 and x86_64, runs both natively (the arm64 one needs Apple silicon) and loads SwiftUI, Network and AppIntents in both, without making a view, to resolve some of their enum types; the x86_64 one also dlopens ComputationalGraph. Section N walks nils of standard types (pointer words, single inhabitants, tag bytes, Data translated). Its native-type table checks (section E) apply where this host's images are the build `src/mpenum_gen.inc` and `src/archdiff_gen.inc` describe; elsewhere it prints a note and skips those labels |
| objc | PASS | 5 | |
| tools | SKIP (expected) | 25 | runs bundle, stubdeps, platform, identity and aotpayload; stamp cases need `KC` (a kernel collection); MenuBarAgent case needs `HAO_FS` |
| tools/release | PASS | 1 | python3.14 |
| translate | PASS | 3 | |
| fuzz-smoke | PASS | 100 | 20 s per target, random seed |

Not entries of `tests/check.list`:

| Script | Why |
|---|---|
| `tests/aot/run.sh` | `make aot` (in `make test`) |
| `tests/guest/legacy-fixups/run.py`, `tests/guest/indexed-mulhigh/run.py`, `tests/host/async_package.py` | in `make test` |
| `tests/host/jitlock.sh` | `make jitlocktest` (in `make test`) |
| `tests/tools/{bundle,stubdeps,platform,identity,aotpayload}.sh` | run by `tests/tools/run.sh` |
| `tests/fuzz/run.sh`, `tests/fuzz/fuzz_hao_fat.py` | `make fuzz-smoke` |
| `tests/fuzz/make_seeds.sh`, `tests/guest/bridgeunit_gen.py`, `tests/aot/patch.py` | generators and helpers, not tests |
| `tests/guest/bench.c`, `tests/guest/vbench.c`, `tests/guest/vbench2.c` | timing programs for `hao --stats` and wall-time comparisons (`vbench`: single-precision vector arithmetic and LD1/ST1; `vbench2`: TBL/TBX, EXT, MUL/MLA/MLS, FRINT*, 64-bit compares, min/max, ABS), not tests |

`make unit` runs `tests/aot/jit_vector.c`: 552,518 cases of the
translated vector floating-point, FRINT* (vector and scalar), integer
compare/min/max/multiply, TBL/TBX/EXT, shift/narrow/widen and LD1/ST1/LD1R
encodings (with neighbouring encodings that stay with the helper), each
through the translator and the interpreter from one random state under one
of the 16 FPCR RMode/DN/FZ settings, alone and with NZCV live across it.
Registers, NZCV, the full FPSR (IEEE flags included), FPCR and memory must
be equal; a store that faults straddling into an inaccessible page may
differ only in the bytes before the page end that the interpreter's
`memcpy` wrote (see docs/ISSUES.md). FMLA/FMLS .2S/.4S, three-same and
by element, include directed near-ties, all four indices, high Vm registers,
register aliases, all 16 FPCR settings, full FPSR comparisons and maximum
96-instruction blocks (including the emitter's scratch capacity).
FP16 .4H/.8H adds all eight H:L:M indices with the restricted Vm bank,
normal/subnormal/overflow boundaries, a late inexact pair after earlier
admitted pairs, FZ16/AHP helper routing, and maximum-length blocks.
The unit build redirects the descriptor helper through a counting wrapper
and asserts that ordinary exact cases stay inline and a known inexact
binary64 sum declines. This checks admission even when its lost low bits do
not affect the final half result; production helper routing is unchanged.
`aot/fmla-mutations` first requires an unmutated control to pass, then builds
private copies with FP32 odd/negation/index logic or FP16 rounding increment,
subnormal shift, H:L:M decode, exactness admission or product negation broken.
Each must produce a result/state or admission mismatch. The exactness
mutation is detected by helper routing, not by an assumed result difference. `guest/fma32` and `guest/jitinsns` compare
native arm64 with JIT, interpreter, helpers-only and AOT, including these
forms. `guest/fmla-bench` checks native result equality and reports ns/FMLA
for three-same and by-element .4S/.8H, JIT/AOT; timings are informational,
with no machine-dependent performance threshold.
`--stats` reports executed interpreter-helper calls as `helper_calls`,
separately from `helper` (translated helper sites). Dynamic counting is
enabled only with `--stats`; use separate runs for uninstrumented timings.

No script was left out for launching a GUI application, bundle, extension
or XPC service: `tests/tools/bundle.sh` only signs and inspects its `.app`
copies, `cli.sh` runs a bundle-shaped command-line tool by path, and
`native-bundle` loads a plug-in bundle with `CFBundle` in a command-line
process.

## Known failures

`guest/xinhab` is marked `xfail`:

- `guest/xinhab`: its header calls it an open class. Enum cases stored in a
  pointer payload's extra inhabitants cross the boundary with the wrong
  index on every hao route (JIT, `--interp`, AOT):
  `guest wrote [Int]?? case 0, native read 2`,
  `guest wrote Three case 2, native read 0`,
  `native wrote Three case 2, guest read 2` where native arm64 reads 0, 2
  and 3 respectively. The two compilers encode the cases differently
  (inhabitant i of a pointer is the value i on arm64 and i << 1 on x86_64,
  whose bit 0 is the ObjC tagged-pointer bit) and each side tests for its
  own constants inline: arm64 `caseOfDouble` compares the word with 1,
  x86_64 with 2. The test passes the value's address as `UnsafeRawPointer`,
  so no signature, metadata or witness names the type where it crosses, and
  there is nothing for a bridge to convert by; `src/swiftvalue.c` converts
  only where a typed result is published. It stays open until it is decided
  where such types may be shared between guest and native code.

Fixed: `guest/oslog` failed for the Swift program only (every `oslogswift`
record `<compose failure [UUID]>` under JIT, `--interp` and AOT), and only
in some build directories. No switch was involved. `tools/hao-embed` gave a
stub an LC_UUID made from the program's arm slice LC_UUID alone, and logd
saves the format strings of a UUID once (`/var/db/uuidtext`) and resolves
every later record of that UUID in the saved copy. `oslogswift` builds to
the same arm slice in every checkout, so all its stubs shared one UUID,
while the stub's `__cstring` moves with the length of the `--host-test`
rpath (the build directory): at offset 0x930 in the checkout whose stub
logd saved first, 0x950 in `build/` and `build-ts/` of a worktree with a
longer path. The C program never failed because its arm slice carries
`$OUT` in a load command, so each run had a new UUID. The stub's LC_UUID
now also names the stub's sections (addresses and bytes), as a linker's
UUID does; the script checks that its two different stubs of a program
have two UUIDs. The same defect reached installed systems: a program
embedded again with another libhao build kept its UUID.

Fixed: `guest/uresume`, case `alarmnx`, could exit 132 (SIGILL) when its
SIGALRM handler made an NX callback during the first callback's
Objective-C enum initialization. The captured RIP was
`_os_once_gate_recursive_abort+0x17`: the handler's callback reentered the
same unfinished `pthread_once`, not a failed signal return. The enum
bridge now defers guest signals around the complete `pthread_once` call.
[Measured scope and remaining limitations](../docs/ISSUES.md) record the
bounded before/after counts and native ARM comparison.

Flaky: `guest/fork`'s host stress (`test_fork`, mode `all`) ended with
SIGSEGV in one gate run; 8 runs of the script right after and 8 on an
a00a428 build passed. The host has `test_fork` crash reports from 5 and 8
November as well. Those stacks are in JIT emission or bridge binding,
without the recursive enum initialization above; no shared cause has been
established (see [remaining issues](../docs/ISSUES.md)).

Flaky under host load: `make oracle`'s `atomic16` control case ("two
8-byte halves (must tear)") failed in one of three `make oracle` runs with 0
torn loads of 300000 while the host's load average was about 20 from other
jobs; it is a check that the race is exercised, not a hao result, and five
direct runs right after tore 1935 to 281425 times.

## Sanitizer check

`make oracle-asan` builds the interpreter, SIMD/FP code, x86 encoder,
`src/jit.c` and the oracle checker with `-fsanitize=address,undefined`
(objects in `build/asan/`), runs `ORACLE_ASAN_N` records per mode (default
20000, about 36 s) and fails on any report; the reports stay in
`build/asan/oracle-*.log`. It is part of `make check`.

It reports nothing now. AddressSanitizer never did. UndefinedBehaviorSanitizer
reported 22 locations in the SIMD interpreter (`src/a64_simd.c`) at
`ORACLE_ASAN_N=200000`; other seeds reached the same two shift helpers from
three more instruction groups. They were of two kinds:

- Shifts by 64, 128 or -64 in encodings AArch64 leaves unallocated because
  their wide elements would be 128 bits, which hao executed: size 3 of the
  three-different group (all but PMULL), of SHLL, SADDLP/UADDLP/SADALP/UADALP,
  XTN/SQXTUN/SQXTN/UQXTN and the integer reductions across lanes (also
  32-bit lanes without Q), SSHLL/USHLL with `immh = 1xxx`, and the copy
  group with `imm5 = x0000`. They are undefined instructions now, as on
  hardware; PMULL also rejects `U = 1` and the 16- and 32-bit sizes. The
  three-different group has no reserved encoding left that hao accepts, so
  its entry is gone from `known_lenient` in `tests/oracle/known.h`.
- Signed overflow on valid encodings whose result wraps: the widening
  add, subtract, absolute-difference, multiply and accumulate forms
  (three-different and by-element), the SDOT/UDOT accumulator, and a left
  shift of a negative value in SQSHL/UQSHL/SQSHLU by immediate. They use
  unsigned arithmetic now.

Measured after the change: `make oracle` (200000 records per mode) and
`make oracle-asan ORACLE_ASAN_N=200000` (twice, different seeds) match
native arm64 on every record with no sanitizer report.

`guest/state-bench` compares native arm64, JIT, interpreter, helpers-only
and AOT for table-driven BLR/RET calls (compact and 8-KiB-spaced targets),
BR tail dispatch, alternating return sites, a single return site, and
short arithmetic blocks surrounding an XPACI helper. It prints timing
without counters, then collects `--stats` separately. Default N=100000;
`N=100000000 ENGINES='jit aot'` makes longer measurement runs. The result
checksum must match native; timings have no pass/fail threshold.

`aot/round16` (also in `make unit`) compares the specialized finite
binary64-to-binary16 rounding with the unchanged general converter: all
31,744 finite positive half intervals, endpoints and adjacent double values,
midpoints and their adjacent doubles, both signs and all four rounding
modes, plus random finite binary64 values (2,577,664 cases). It checks full
FPSR with clear/preset sticky flags and that the converter itself leaves the
FP environment unchanged. This exhausts boundary classes, not all doubles
inside each interval. `guest/fma16` checks 42 vector fused forms, including
all indexed lanes and aliases, against native arm64 in JIT, interpreter,
helpers-only and AOT, under the 16 RMode/FZ/DN settings. FZ16/AHP behavior is
outside this native comparison: the existing interpreter does not model it.

The FP16 guest also faults after ninety inline operations in a 96-instruction
block and checks the recovered guest PC and live NZCV against native execution
in every engine. This exercises deterministic re-emission into the enlarged
256 KiB recovery buffer. The mutation manifest timeout is 600 seconds for
its control plus eight independent variants.

`guest/helper-stats` checks dynamic decoder-group statistics in JIT and AOT:
1000 each of untranslated RBIT, FABD and integer SQADD, plus
1000 subnormal FADD declines,
with 1000 admitted FADDs excluded; N=0 subtracts startup calls. Without
`--stats`, no group lines are printed.

`guest/coverage-bench` compares all 112 SIMD arrangements and indexed forms with native arm64 in
JIT, interpreter, helpers-only and AOT. Constant ordinary operands and sixteen
operations per loop make each instruction's helper cost measurable. Timing
runs omit statistics; separate runs report helper groups. Loop/reset overhead
is included. Development-host ratios are descriptive; Intel measurements decide
performance acceptance. `N` controls iterations (default 50000).

`aot/integer-mutations` builds private translator copies and verifies that
the complete-state vector test passes unchanged and catches reversed ZIP
half selection and an ADDV reduction at the wrong lane width.
`guest/jitinsns` covers all 42 allocated ZIP/UZP/TRN forms, the five ADDV
arrangements and all 24 signed/unsigned pairwise-long arrangements against
native arm64 in JIT, interpreter, helpers-only and AOT.

`aot/fp-coverage-mutations` runs a passing vector-test control and a private
translator with the FADDP pair ordering deliberately corrupted. The complete
state test must detect the mutation; source files and the built runtime remain
unchanged. `guest/jitinsns` also includes vector/scalar S/D FADDP and all four
single-precision min/max reductions, compared with native in every engine.

`aot/register-shift-mutations` runs the vector differential test first on an
unmodified translation, then with the right-shift count negation removed.
The control must pass and the mutation must report an architectural mismatch.
The vector test covers every signed low-byte count (-128..127) for USHL/SSHL
.2S/.4S/.2D, ignored upper count bits, aliases and live NZCV.

Vector division cases cover .2S/.4S, aliases, full FPSR, all 16 FPCR controls
and directed exact/subnormal results. The FP coverage mutation script also
suppresses FDIV result admission; a directed exact tiny result asserts the
required helper route even if its architectural bits happen to agree.

The vector differential and native instruction tests also cover indexed
scalar FMUL S/D and FMLA/FMLS S, every index, high source register numbers,
aliases, upper clearing, FP special values and directed fused rounding.
The full-state cases use all 16 RMode/FZ/DN settings and full FPSR;
ordinary inputs with inactive signaling NaNs must remain inline.

The vector unit checks all five FP32 comparisons with zero in .2S/.4S,
including signed zeros, aliases, full FPSR and all 16 FPCR settings, with
ordinary/tiny helper-route assertions. `jitinsns` now checks every FP form
under all 16 RMode/FZ/DN settings in each engine against native arm64.

The tail-coverage scalar tests add FMADD/FMSUB/FNMADD/FNMSUB S, FNMUL S,
FRECPS and FRSQRTS S/.2S/.4S, all FPCR rounding/FZ/DN settings, aliases,
fused near-ties, inactive lanes and full-state comparisons with live NZCV.
The FP coverage mutation script additionally corrupts the scalar fused
addend sign and omits the reciprocal-square-root step's exact factor of 1/2;
each must fail the complete-state test. The native instruction and ordinary
coverage benchmark include all eleven forms. AOT coverage prints four
decimal places to distinguish changes below one tenth of a percent.
The integer-tail cases in `aot/jit_vector` cover widening add/subtract/multiply,
accumulation, SHLL, saturating narrowing and QC, SQ(RD)MULH vector/by-element,
REV, every SRI/SLI shift and scalar ADDP, including aliases and live NZCV.
Scalar SIMD B/H memory checks include inaccessible pages and writeback.
`guest/jitinsns` adds 229 native comparisons (FPSR included), and
`guest/coverage-bench` adds 219 integer instruction loops. `aot/integer-mutations`
also suppresses QC and selects the wrong wide operand half; each independent
mutation must be caught after an unmodified control passes.

The conversion tail cases in `aot/jit_vector` cover explicit scalar-to-GPR
rounding, H/S/D SIMD integer conversions, scalar H/S/D format conversions, vector FCVTN/N2 and FCVTL/L2,
register aliases, unsupported and reserved encodings, all sixteen FPCR
settings, sticky FPSR, and helper routing for ordinary inputs. D-to-S
includes FZ input and output cases that previously lost IDC or added IXC.
`guest/jitinsns` compares those forms to native in all four engines;
`guest/coverage-bench` adds twenty-six ordinary conversion workloads.
`aot/fp-coverage-mutations` additionally replaces all conversion rounding
modes with truncation and requires the complete-state test to detect it.

`aot/structure-mutations` requires a passing unmodified vector test, then
checks that incorrect interleaving and publishing load destinations before
the final memory access each fail. The vector test covers all LD2/3/4 and
ST2/3/4 multiple-structure arrangements, immediate/register post-index,
wrapped register lists, SP, base/offset aliases, and every byte boundary in
a 64-byte transfer into an inaccessible page. `guest/jitinsns` checks all
126 arrangement/addressing combinations against native ARM in every engine;
its signal probes additionally check recovered PC, NZCV and writeback when
interleaved transfers fault partway, skip or resume after page repair.
`guest/coverage-bench` includes 42 interleaved memory forms for native
checksums, separate helper statistics and uninstrumented timings.

`aot/wide-fp-mutations` checks a passing control and then disables packed
double/sqrt result admission; complete-state and exact-tiny helper-route
checks must reject it. `jit_vector` covers FADD/FSUB/FMUL/FDIV .2D and
FSQRT .2S/.4S/.2D, every FPCR RMode/FZ/DN setting, aliases, zero signs,
NaNs and range boundaries. `jitinsns` compares all seven forms with native
ARM in each engine; `coverage-bench` adds per-form checksum/timing/statistics
cases with ordinary inputs.

Half arithmetic cases cover FMUL/FADD/FSUB .4H/.8H, every indexed FMUL lane,
scalar H arithmetic and scalar/vector FMIN/FMAX/FMINNM/FMAXNM. The tests use
all sixteen FPCR settings, aliases, special operands, signed zeros, exact
and inexact half rounding boundaries and ordinary-input helper assertions.
`guest/jitinsns` compares these with native across all engines, while
`guest/coverage-bench` adds 37 ordinary half forms. The FP coverage mutation
script changes half subtraction into addition; `MUTATIONS` can select a
subset for focused development runs, with the full set remaining the default.

The FP environment tail cases add FCCMP/FCCMPE S/D under every condition,
all input NZCV, ordinary and exceptional operands, and FPCR controls. FPCR/FPSR
moves include XZR, writable-mask checks, seeded x87 exceptions and comparison
of x87 rounding bits as well as full FPSR and live NZCV. `guest/jitinsns`
adds 66 native forms; `guest/coverage-bench` adds eight loops. The FP coverage
mutation script independently reverses FCCMP's condition and corrupts the
FPCR rounding mapping.

FPCR state cases additionally seed a pending unmasked x87 invalid exception
and rewrite unchanged FPCR in all four rounding modes, with NZCV live and
dead. The JIT must skip the waiting FLDCW as the interpreter does. Rosetta
preserves the exception flag but does not deliver this pending exception;
Intel execution remains necessary to validate fault delivery itself.

Tail coverage expands the full-state corpus and x87 checks. The vector
unit and each mutation executable have a 240-second bound. Mutation runs
set `HAO_VECTOR_FAIL_FAST`: after a witnessed state/admission mismatch they
exit unsuccessfully, while every passing control still runs the entire
corpus. This avoids spending the remainder of the corpus on a mutation
already proved detectable. Suite-level timeouts remain bounded.

`aot/estimate-mutations` checks a full passing vector-test control and detects
independently corrupted estimate-table rounding and exponent parity. The
complete-state cases include all table buckets, both fraction edges, aliases,
late-lane rollback and exceptional boundaries. `guest/jitinsns` compares all
S/D scalar/vector reciprocal estimates with native ARM over every table
index, exponent parity, sign and FPCR control; ten benchmark loops measure
the ordinary paths. AOT format 7 has unchanged header layout and rejects
version 6 caches because emitted code can use the new runtime table pointer.

`aot/aes-profile` masks AES out of a private CPUID wrapper and checks that
all four AES instructions (including aliases) use the helper with complete
architectural state preserved. It validates a matching synthetic cache,
rejects its AES-profile identity on the CPU without AES, and compares
translation bytes across normal, AOT and fault-recovery emission modes.
It never executes AES after forcing its feature bit. `aot/aes-mutations`
requires a passing control and detects an omitted ARM AddRoundKey. The
vector differential test covers all four AES forms and reserved size/opcode
neighbours; `jitinsns` adds native comparisons for separate/aliased sources
in each engine. The existing crypto guest also checks AES known answers.
The installer test permits only base/AES generator identities and removes
arbitrary altered identities; `tools/aotpayload` checks profile-aware payload
creation, installation and cache loading.
