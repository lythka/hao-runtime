#!/bin/sh
# A published Swift value holding a class-bound existential whose conformance is the guest's
# (tests/guest/svconformance.swift): to a native protocol it must be refused by the walker, whether the
# witness table is in the guest image (a static conformance) or instantiated on the heap (a generic
# one): only a conformance whose descriptor lies in no guest image is native. To a guest protocol, which
# native code cannot name, it is admitted (src/swiftvalue/Recipe.c, live existentials).
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/svconformance.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svconformance)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
LIB=$(cd "$(dirname "$HAO")" && pwd)/libhao.dylib
xcrun swiftc -O -target arm64e-apple-macos27.0 -o "$OUT/svconformance" "$here/svconformance.swift" 2>"$OUT/build.err" ||
    { cat "$OUT/build.err"; exit 2; }
codesign -s - -f "$OUT/svconformance" 2>/dev/null
timeout -s KILL "$T" "$HAO" "$OUT/svconformance" "$LIB" >"$OUT/out" 2>"$OUT/err"; rc=$?
cat "$OUT/out"
fail=0
[ $rc = 0 ] || fail=1
grep -qx 'native static walk refused' "$OUT/out" || fail=1
grep -qx 'native generic walk refused' "$OUT/out" || fail=1
grep -qx 'static walk admitted' "$OUT/out" || fail=1
grep -qx 'generic walk admitted' "$OUT/out" || fail=1
grep -qx 'opaque small walk admitted' "$OUT/out" || fail=1
grep -qx 'opaque big walk admitted' "$OUT/out" || fail=1
grep -qx 'opaque native walk refused' "$OUT/out" || fail=1
[ $fail = 0 ] && echo "svconformance: PASS" || { tail -3 "$OUT/err"; echo "svconformance: FAIL"; }
exit $fail
