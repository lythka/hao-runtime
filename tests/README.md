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
| `HAO_FS` | `guest/cli` (Apple-signed `ls`, trusted cdhash list), `tools` (MenuBarAgent bundle), `corpus27_df` | the path of a mounted macOS 27 System volume (read-only is enough) |
| `KC` | `tools` stamp cases | a BootKernelExtensions.kc of the target kernel |
| `PROFILE` | `host/sigpolicy` profile cases | a sandbox profile to check |

The remaining expected skips are maintainer tooling that is not distributed:
the offline scanners and oracle inputs (`tests/host/swiftslots.c`,
`SCANNER_ROOT`, `ORACLE265`/`IPSW27`, `OSS265`, `OBSERVE`/`DIFF`) and the
corpus runner `tools/run27.sh`. Their cases always skip in this tree; the
runtime paths they would add evidence for are covered by the other tests.

## Inventory

Measured on an Apple silicon development host (macOS 27.2, 16 cores) with
`HAO_FS` set. Times are wall-clock seconds; `test-guest` times are under
`J=8` parallel load.

`make test` (173 s in all; each target alone):

| Target | Result | Time | Needs |
|---|---|---|---|
| `unit` | PASS | 3 | |
| `oracle` | PASS | 153 | |
| `aot` | PASS | 6 | |
| `bridgetest` | PASS | <1 | |
| `async-contract-test` | PASS | 4 | python3.14 |
| `legacy-dyld-test` | PASS | 1 | python3.14 |
| `indexed-mulhigh-test` | PASS | <1 | python3.14 |
| `forktest` (added to `make test`) | PASS | 7 | |
| `jitlocktest` (added to `make test`) | PASS | 37 | `/usr/bin/awk` copies |
| `tools/check_x86.sh` | PASS | 1 | |

`make test-guest` (159 s in all, before guest/cuvalist: 63 PASS, 1 XFAIL, 8 expected SKIP):

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
| guest/footprint | PASS | 59 | `footprint(1)` |
| guest/fork | PASS | 45 | |
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
| guest/nojit | PASS | 5 | |
| guest/objcimp | PASS | 4 | |
| guest/objcva | PASS | 3 | |
| guest/optwitness | PASS | 1 | guest calls of native Optional, tuple and array witnesses on nested Optionals; see below |
| guest/oslog | PASS | 79 | `log show`; was a failure that depended on the checkout's path, see below |
| guest/signals | PASS | 36 | |
| guest/sret-forward | PASS | 3 | |
| guest/stackargs | PASS | 2 | |
| guest/stackguard | PASS | 1 | |
| guest/swift-object-callbacks | PASS | 5 | |
| guest/swiftlate | PASS | 4 | |
| guest/swiftls | PASS | 5 | |
| guest/swiftmeta | PASS | 8 | |
| guest/swiftpop | SKIP (expected) | 4 | scan stage: `tests/host/swiftslots.c` is not in this tree; the runtime modes pass |
| guest/swiftthunks | PASS | 6 | |
| guest/threads | PASS | 2 | |
| guest/uresume | PASS | 26 | |
| guest/varsniff | PASS | 2 | |
| guest/xinhab | XFAIL | 2 | see known failures |
| guest/xpac | PASS | 1 | |
| guest/run-async | SKIP (expected) | 9 | 15 async cases pass; the modelmanagerdump cases need `ORACLE265` (and `IPSW27`) |
| guest/keypath-identity | PASS | 2 | |
| guest/late-method-signatures | PASS | 10 | |
| guest/native-bundle | PASS | 5 | loads a plug-in `.bundle` into a command-line guest |
| guest/objc-class-order | PASS | 6 | |
| guest/sret-spill | SKIP (expected) | 1 | `tools/codesig.py` under `SCANNER_ROOT` (not in this tree) |
| guest/objcvascope | PASS | 2 | |
| host/runtime | PASS | 2 | |
| host/chain | PASS | 6 | |
| host/commpage | PASS | 1 | |
| host/sigpolicy | SKIP (expected) | 1 | the profile cases need `PROFILE`; the hao cases x86_64 hardware |
| host/near-map | PASS | 1 | |
| host/dscselhoist | PASS | 1 | |
| host/swiftvalue | PASS | 1 | |
| objc | PASS | 5 | |
| tools | SKIP (expected) | 25 | runs bundle, stubdeps, platform, identity and aotpayload; stamp cases need `KC` (a kernel collection); MenuBarAgent case needs `HAO_FS` |
| tools/release | PASS | 1 | python3.14 |
| translate | PASS | 3 | |
| oss265 | SKIP (expected) | 1 | `OSS265`: the 26.5 Libc sources, not in this tree |
| fidelity | SKIP (expected) | 1 | `OBSERVE`/`DIFF` (`tools/fidelity/`), not in this tree |
| fuzz-smoke | PASS | 100 | 20 s per target, random seed |
| corpus27_df | SKIP (expected), run alone | 1 | `tools/run27.sh`, not in this tree; 27.0 System; writes to the Data volume |

Not entries of `tests/check.list`:

| Script | Why |
|---|---|
| `tests/aot/run.sh` | `make aot` (in `make test`) |
| `tests/guest/legacy-fixups/run.py`, `tests/guest/indexed-mulhigh/run.py`, `tests/host/async_package.py` | in `make test` |
| `tests/host/jitlock.sh` | `make jitlocktest` (in `make test`) |
| `tests/tools/{bundle,stubdeps,platform,identity,aotpayload}.sh` | run by `tests/tools/run.sh` |
| `tests/fuzz/run.sh`, `tests/fuzz/fuzz_hao_fat.py` | `make fuzz-smoke` |
| `tests/fuzz/make_seeds.sh`, `tests/guest/bridgeunit_gen.py`, `tests/oss265/build.sh`, `tests/aot/patch.py` | generators and helpers, not tests |

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
