#!/bin/bash
# 26.5 OSS test: 26.5 Libc sources built for arm64e (run under hao: JIT,
# interpreter, AOT) vs the same sources built natively for x86_64 and arm64.
set -e
cd "$(dirname "$0")/../.."
[ -n "${OSS265:-}" ] || { echo "SKIP oss265: OSS265 not set (the 26.5 Libc sources are not in this tree)"; exit 77; }
tests/oss265/build.sh >/dev/null
O=build/oss265
T=${T:-120}
timeout -s KILL $T $O/oss265.x86_64 > $O/x86_64.out
timeout -s KILL $T $O/oss265.arm64 > $O/arm64.out 2>/dev/null || true
# The AOT case uses its own cache directory, and the JIT case cannot pick up
# a cache left by an earlier run.
rm -rf $O/aot && mkdir -m 700 $O/aot
HAO_NO_AOT=1 timeout -s KILL $T build/hao $O/oss265.arm64e > $O/hao-jit.out
timeout -s KILL $T build/hao --interp $O/oss265.arm64e > $O/hao-interp.out
HAO_CACHE=$PWD/$O/aot timeout -s KILL $T build/hao --aot $O/oss265.arm64e 2>$O/aot-gen.err
HAO_CACHE=$PWD/$O/aot HAO_VERBOSE=1 timeout -s KILL $T build/hao $O/oss265.arm64e > $O/hao-aot.out 2>$O/hao-aot.err
fail=0
if grep -q "AOT cache .* loaded" $O/hao-aot.err; then echo "PASS hao-aot cache loaded"; else echo "FAIL hao-aot cache not loaded: $(grep -m1 AOT $O/hao-aot.err)"; fail=1; fi
for m in hao-jit hao-interp hao-aot; do
  for ref in x86_64 arm64; do
    if diff -q $O/$ref.out $O/$m.out >/dev/null; then echo "PASS $m == native $ref"; else echo "FAIL $m vs native $ref"; fail=1; fi
  done
done
exit $fail
