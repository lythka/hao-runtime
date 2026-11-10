#!/bin/sh
# Build tests/guest/stackargs.m (+ the native helper dylib stackhelper.m) for
# arm64e (guest) and arm64 (reference). Run arm64 natively and arm64e under
# hao with the JIT and with --interp; each hao run must print exactly what
# native arm64 prints. Exit status 0 only if every run matches.
#   OUT=dir HAO=build/hao [T=60] tests/guest/stackargs.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-stackargs}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" -framework Foundation \
    -install_name "$OUT/libstackhelper.dylib" -o "$OUT/libstackhelper.dylib" "$here/stackhelper.m"
codesign -s - -f "$OUT/libstackhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -Wno-deprecated-declarations -I"$here" -framework Foundation -L"$OUT" -lstackhelper \
        -o "$OUT/stackargs.$a" "$here/stackargs.m"
    codesign -s - -f "$OUT/stackargs.$a" 2>/dev/null
done
set +e
timeout -s KILL "$T" "$OUT/stackargs.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
for mode in jit interp; do
    flag=; [ $mode = interp ] && flag=--interp
    timeout -s KILL "$T" "$HAO" $flag "$OUT/stackargs.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
[ $fail = 0 ] && echo "stackargs: PASS" || echo "stackargs: FAIL"
exit $fail
