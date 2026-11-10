# Tests

```sh
make test          # quick gate: unit, oracle, aot, bridge/async/dyld/mulhigh, forktest, jitlocktest, check_x86
make test-guest    # every other script that runs unattended here (tests/check.sh)
make check         # both: the gate for runtime changes
make oracle-asan   # oracle checks with the translator under ASan + UBSan (not in the gate: see below)
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

## Inventory

Measured at b5dfc26 plus this change on an Apple silicon development host
(macOS 27.2, 16 cores; x86_64 code runs under Rosetta, the arm64 references
natively), with the 27.0 System mounted at `/private/tmp/ipsw27-fs`. Times
are wall-clock seconds; `test-guest` times are under `J=8` parallel load.
No script needs root or the network, and none launches a GUI application,
app extension or XPC service. Every guest-running script needs Xcode's
clang/swiftc with arm64e, native arm64 execution and Rosetta.

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

`make test-guest` (159 s in all: 62 PASS, 2 XFAIL, 8 expected SKIP):

| Entry | At b5dfc26 | Now | Time | Needs / skip reason |
|---|---|---|---|---|
| guest/aggspi6 | PASS | PASS | 3 | |
| guest/atoken | PASS | PASS | 1 | |
| guest/bitfields | PASS | PASS | 2 | |
| guest/blocks2 | PASS | PASS | 2 | |
| guest/bridgefix | PASS | PASS | 2 | |
| guest/bridgeunit | PASS | PASS | 3 | |
| guest/callback-routes | PASS | PASS | 2 | |
| guest/callbacks | PASS | PASS | 10 | |
| guest/cfnull | PASS | PASS | 1 | |
| guest/chkstk | PASS | PASS | 1 | |
| guest/cli | PASS | PASS | 3 | 27.0 System at `HAO_FS` (two cases; otherwise an unexpected SKIP); `sandbox-exec` |
| guest/crypto | PASS | PASS | 1 | |
| guest/cxxabi | PASS | PASS | 10 | |
| guest/datacall | PASS | PASS | 1 | |
| guest/dsc | PASS | PASS | 3 | the host's arm64e shared cache |
| guest/dscswift | PASS | PASS | 10 | the host's arm64e shared cache |
| guest/dylibs | PASS | PASS | 4 | |
| guest/eh | PASS | PASS | 19 | |
| guest/fenv | PASS | PASS | 1 | |
| guest/footprint | PASS | PASS | 59 | `footprint(1)` |
| guest/fork | PASS | PASS | 45 | |
| guest/fullcache | PASS | PASS | 6 | |
| guest/gss | PASS | PASS | 1 | |
| guest/heapprobe | PASS | PASS | 1 | |
| guest/hfasret | PASS | PASS | 2 | |
| guest/identity | PASS | PASS | 5 | |
| guest/indirect-x8 | PASS | PASS | 1 | |
| guest/keypath | PASS | PASS | 2 | |
| guest/libval | PASS | PASS | 16 | |
| guest/msgfnptr | PASS | PASS | 4 | |
| guest/narrowret | PASS | PASS | 4 | |
| guest/nativesig | PASS | PASS | 25 | |
| guest/nojit | PASS | PASS | 5 | |
| guest/objcimp | PASS | PASS | 4 | |
| guest/objcva | PASS | PASS | 3 | |
| guest/oslog | FAIL | XFAIL | 79 | `log show`; see known failures |
| guest/signals | PASS | PASS | 36 | |
| guest/sret-forward | PASS | PASS | 3 | |
| guest/stackargs | PASS | PASS | 2 | |
| guest/stackguard | PASS | PASS | 1 | |
| guest/swift-object-callbacks | PASS | PASS | 5 | |
| guest/swiftlate | PASS | PASS | 4 | |
| guest/swiftls | PASS | PASS | 5 | |
| guest/swiftmeta | PASS | PASS | 8 | |
| guest/swiftpop | FAIL | SKIP (expected) | 4 | scan stage: `tests/host/swiftslots.c` is not in this tree; the runtime modes pass |
| guest/swiftthunks | PASS | PASS | 6 | |
| guest/threads | PASS | PASS | 2 | |
| guest/uresume | PASS | PASS | 26 | |
| guest/varsniff | PASS | PASS | 2 | |
| guest/xinhab | FAIL | XFAIL | 2 | see known failures |
| guest/xpac | PASS | PASS | 1 | |
| guest/run-async | PASS | SKIP (expected) | 9 | 15 async cases pass; the modelmanagerdump cases need `ORACLE265` (and `IPSW27`) |
| guest/keypath-identity | PASS | PASS | 2 | |
| guest/late-method-signatures | PASS | PASS | 10 | |
| guest/native-bundle | PASS | PASS | 5 | loads a plug-in `.bundle` into a command-line guest |
| guest/objc-class-order | PASS | PASS | 6 | |
| guest/sret-spill | FAIL | SKIP (expected) | 1 | `tools/codesig.py` under `SCANNER_ROOT` (not in this tree) |
| guest/objcvascope | PASS | PASS | 2 | |
| host/runtime | PASS | PASS | 2 | |
| host/chain | PASS | PASS | 6 | |
| host/commpage | PASS | PASS | 1 | |
| host/sigpolicy | PASS | SKIP (expected) | 1 | the profile cases need `PROFILE`; the hao cases x86_64 hardware |
| host/near-map | PASS | PASS | 1 | |
| host/dscselhoist | PASS | PASS | 1 | |
| objc | PASS | PASS | 5 | |
| tools | PASS | SKIP (expected) | 25 | runs bundle, stubdeps, platform, identity and aotpayload; stamp cases need `KC` (a kernel collection); MenuBarAgent case needs `HAO_FS` |
| tools/release | PASS | PASS | 1 | python3.14 |
| translate | PASS | PASS | 3 | |
| oss265 | FAIL | SKIP (expected) | 1 | `OSS265`: the 26.5 Libc sources, not in this tree |
| fidelity | FAIL (exit 2) | SKIP (expected) | 1 | `OBSERVE`/`DIFF` (`tools/fidelity/`), not in this tree |
| fuzz-smoke | PASS (`make fuzz-smoke`) | PASS | 100 | 20 s per target, random seed |
| corpus27_df | FAIL | SKIP (expected), run alone | 1 | `tools/run27.sh`, not in this tree; 27.0 System; writes to the Data volume |

"At b5dfc26" is the script as it was, run with a private `OUT`; scripts
that then exited 0 with a `SKIP` line or skipped a case without one counted
as PASS. Changes to the scripts themselves: `SKIP` lines where a case or the
whole script was left out silently or failed for a missing input
(`cli.sh` x3, `libval.sh`, `run-async.sh`, `sigpolicy.sh`, `swiftpop.sh`,
`sret-spill/run.py`, `oss265/run.sh`, `fidelity/run.sh`, `corpus27_df.sh`),
timeouts on `oss265/run.sh`'s guest runs, and an absolute `B` accepted by
`fuzz/run.sh`.

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

Both fail the same way at b5dfc26 and are marked `xfail`:

- `guest/xinhab`: its header calls it an open class. Enum cases stored in a
  pointer payload's extra inhabitants cross the boundary with the wrong
  index on every hao route (JIT, `--interp`, AOT):
  `guest wrote [Int]?? case 0, native read 2`,
  `guest wrote Three case 2, native read 0`,
  `native wrote Three case 2, guest read 2` where native arm64 reads 0, 2
  and 3 respectively.
- `guest/oslog` passes on the `TEST_SWITCHES` build the gate uses (8 of 8) but fails on the
  default build (5 of 8): there, as described next, the Swift program's records do not compose.
  The cause of the difference between the two builds is not known yet.
- `guest/oslog`: the C program's 24 records match on every route, but every
  `oslogswift` record (Logger, `os_log()`, `os_signpost()` from Swift) is
  `<compose failure [UUID]>` under JIT, `--interp` and AOT; the `oldstub`
  route passes.

Flaky under host load: `make oracle`'s `atomic16` control case ("two
8-byte halves (must tear)") failed in one of three `make oracle` runs with 0
torn loads of 300000 while the host's load average was about 20 from other
jobs; it is a check that the race is exercised, not a hao result, and five
direct runs right after tore 1935 to 281425 times.

## Known sanitizer findings

`make oracle-asan` builds the interpreter, SIMD/FP code, x86 encoder,
`src/jit.c` and the oracle checker with `-fsanitize=address,undefined`
(objects in `build/asan/`), runs `ORACLE_ASAN_N` records per mode (default
20000) and fails on any report; the reports stay in
`build/asan/oracle-*.log`. AddressSanitizer reported nothing.
UndefinedBehaviorSanitizer reports these locations in the SIMD interpreter (`simd` mode; dp, mem, block and the 16-byte atomicity
checks are clean). Every record still matches native arm64, so these are
undefined behaviour the current compiler happens to resolve as wrapping,
not observed mismatches. The target therefore fails and is not part of
`make check`.

Measured with `ORACLE_ASAN_N=200000` (5 min; the default 20000 takes 36 s
and found 13 of these 22 locations). UBSan reports each location once per
run.

| Location | Kind | Reached from |
|---|---|---|
| `src/a64_simd.c:21` (`sext`, two reports) | shift exponent -64 | `simd_three_diff` line 1333: `esget` with a 16-byte element (size 3) |
| `src/a64_simd.c:43` (`emask`) | shift exponent 128 for a 64-bit type | `simd_three_diff` line 1347: `emask(oes)` with size 3 |
| `src/a64_simd.c:938` (SHLL) | shift exponent 64 | `simd_two_misc`, size 3 |
| `src/a64_simd.c:1211` (SQSHL/UQSHL/SQSHLU immediate) | left shift of a negative value | `simd_shift_imm`, `__int128 v <<= lsh` |
| `src/a64_simd.c:1308` (ADDHN/SUBHN family) | shift exponent 64 | `simd_three_diff`, size 3 |
| `src/a64_simd.c:1338`, `1339`, `1340` (x3), `1341` (x2), `1342` (x2), `1343` (x2), `1344` | signed 64-bit overflow (add, sub, absolute difference, multiply, accumulate) | `simd_three_diff` element loop: `int64_t` arithmetic where AArch64 wraps |
| `src/a64_simd.c:1458`, `1459` (x2) | signed 64-bit overflow (multiply, accumulate) | `simd_indexed`, SMULL/UMULL/SMLAL/SMLSL by element: `x * y` of two 32-bit unsigned values in `int64_t`, `acc ± p` |
| `src/a64_simd.c:1538` | signed 32-bit overflow | `simd_dot`, SDOT/UDOT accumulator in `int32_t` |

The shifts by 64, 128 and -64 come from size-3 encodings of the
three-different and SHLL groups, which AArch64 leaves unallocated
(`tests/oracle/known.h` lists the three-different ones as encodings hao
accepts); the overflows are on valid encodings whose architectural result
wraps.
