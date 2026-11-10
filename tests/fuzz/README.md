# Fuzzing

Fuzz targets for the parsers that read untrusted input. Each one gets a guest file, guest metadata or an image
file:

| Target | Code under test | Input |
|---|---|---|
| `fuzz_macho` | `src/macho.c`: slice choice, load commands, segment mapping, chained fixups, exports trie, function starts | a Mach-O file (odd-sized inputs are mapped only; even-sized ones also bind) |
| `fuzz_typeenc` | `src/abi.c` type-encoding parser (`parse_sig`, `parse_type`), under ASan and UBSan | an ObjC/block type encoding |
| `fuzz_codesig` | `src/guest_sig.c` embedded-signature parser (`sig_parse`: superblob index, CodeDirectories, hash ranking, sealed special slots), under ASan and UBSan | a code-signature superblob |
| `fuzz_ifunit` | `src/ifunit.c` interface-unit parser (`hao_ifunit_check`, `hao_ifunit_find`), under ASan and UBSan; each input is checked as is and again with its payload hash recomputed, so mutations reach the record rules | an interface unit (`tools/hao_ifunit.py`) |
| `fuzz_hao_fat.py` | the pure-Python packaging readers: `tools/hao_fat.py` (`slices`, `combine`, `replace_slice`, `load_commands`), hao-embed's Mach-O readers, `tools/hao_macho.py` (`Image`: load commands, exports trie, every import table; `file_slices`; `Cache`), `tools/hao_codesig.py`, `tools/hao_ifunit.py` (`macho_uuid_of`, `read`), and the load-command walks of `tools/hao-install.py` and `kext/stamp_identity.py`. Each must return or refuse with its one error type (`ValueError`; `Bad` for interface units) within 2 s | a fat or thin Mach-O, or an interface unit |

Apple's toolchain ships no libFuzzer runtime, so `driver.c` provides the loop. It is a small coverage-guided
mutation driver for `LLVMFuzzerTestOneInput` harnesses, built with `-fsanitize=address
-fsanitize-coverage=trace-pc-guard`, and it takes libFuzzer's main options. The Python target is a
random-mutation script with a per-input hang alarm.

```
make fuzz                                  # build/fuzz/fuzz_macho, fuzz_typeenc, fuzz_codesig, fuzz_ifunit
FUZZ_TIME=60 tests/fuzz/run.sh             # replay the corpus, then fuzz each target for 60 s
make fuzz-smoke                            # the same with FUZZ_TIME=20
build/fuzz/fuzz_macho -max_total_time=600 -artifact_prefix=out/ corpus-copy/   # one long run
build/fuzz/fuzz_macho crash-0123...        # reproduce one input
python3 -I tests/fuzz/fuzz_hao_fat.py --time 600 tests/fuzz/corpus/fat tests/fuzz/corpus/macho tests/fuzz/corpus/ifunit
python3 -I tests/fuzz/fuzz_hao_fat.py tests/fuzz/corpus/fat/* tests/fuzz/corpus/macho/* tests/fuzz/corpus/ifunit/*   # replay only
```

`corpus/` holds the seeds that `make_seeds.sh` builds with clang from `seeds/*.c` and with `tools/hao_ifunit.py`
(no Apple binaries), plus
`regress-*` inputs that once crashed or hung a target. A fuzz run writes new corpus entries into the directory it
is given, so point it at a copy (`run.sh` does). Under Rosetta, ASan symbolization is slow enough to trip the
timeout, so `run.sh` sets `ASAN_OPTIONS=symbolize=0`. Symbolize a reproduced crash with `atos`.
