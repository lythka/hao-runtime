# Contributing to hao-runtime

Fix shared causes, not single programs: a change should follow from the ABI,
instruction or OS behaviour it implements, never from an application name,
UUID or instruction address. Compare every behaviour with native ARM execution
of the same code. Equal ABI carriers do not establish value, ownership,
callback or concurrency correctness.

## Build

```sh
make -j8                      # build/libhao.dylib, build/hao, build/hao-stub
make B=build/macos-26.0       # macOS 26 deployment target, separate directory
```

## Tests

Run `make test` for every runtime change. It runs:

- `unit`: instruction decoding, condition mapping, ABI layout, the Mach-O
  loader and interface-unit parser (under ASan/UBSan), growable maps and option
  parsing.
- `oracle`: randomized data-processing, SIMD, memory and block sequences run
  natively on ARM and through hao, compared state by state, plus 16-byte
  atomicity checks.
- `aot`: AOT cache trust rules, format checks and the cache policy unit test.
- `bridgetest`, `async-contract-test`, `legacy-dyld-test`,
  `indexed-mulhigh-test`: the native bridge table, async provider packaging,
  legacy dyld fixups and indexed multiply-high.
- `tools/check_x86.sh`: hao's x86 encoder against the system assembler.

Further targets:

| Command | Covers |
|---|---|
| `make forktest` | `fork` while other threads hold runtime locks |
| `make jitlocktest` | code cache and thunk areas under contention |
| `make fuzz`, `make fuzz-smoke` | parser fuzzing ([tests/fuzz/README.md](tests/fuzz/README.md)) |
| `OUT=dir tests/tools/run.sh` | embedding, identity, installation and AOT payload tools |
| `python3.14 -B -m unittest discover -s tests/tools -p test_release.py` | release assembly |
| `python3.14 tools/check_distribution.py` | every tracked file is in `distribution/runtime-files.txt` |

The scripts under `tests/guest/`, `tests/host/`, `tests/objc/` and
`tests/translate/` each describe their usage in their header. Most build an
ARM64 reference and an ARM64e guest, run the guest under JIT, interpreter and
AOT, and compare the output with the native run. Point `OUT` at a disposable
directory.

A passing runtime suite does not establish that an application or a system
image works; those are tested on the target system.

## Debugging

- `build/hao -v` (or `-vv`) logs loading, binding and the guest signature
  verdict; `--stats` prints translation statistics at exit; `--interp` runs
  without the JIT, which separates translator bugs from runtime bugs.
- `HAO_FLAGS` sets the same switches for a process hao starts indirectly
  (`v`, `V`, `i`, `s`, `u`). Environment switches are ignored in guarded and
  set-ID processes.
- `HAO_BRIDGE_CHECK=1` reports native-boundary mismatches, such as unbalanced
  native frames and results that differ from the signature. `HAO_NX_STATS=1`
  groups, by native caller and guest target, the native-to-guest calls that
  arrived by faulting on a non-executable guest page, and prints them at exit.
- On a target system, the boot arguments `hao_crash=1` and `hao_nx_stats=1`
  send the same diagnostics to the console and `/private/var/log/hao`.

## Conventions

- Ad-hoc sign every x86_64 executable you build (`codesign -s - BIN`);
  unsigned ones can hang in exec.
- Put a timeout on every guest run (`timeout -s KILL 60 ...`).
- Never launch GUI applications, app extensions or XPC services on the
  development host; they run on the target system.
- Never run APFS mutation tools (`newfs_apfs`, `apfs_*`) on the development host.
- Do not commit Apple binaries, credentials or files over 1 MB. Fixtures are
  built from source in this repository.
- Add every new file to `distribution/runtime-files.txt`.
- Commit messages say what changed and why, and end with a `Tested:` line
  describing the checks that actually ran.
