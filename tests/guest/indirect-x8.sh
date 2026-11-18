#!/bin/sh
# Native C calling guest function pointers that return a struct in memory
# (bridge audit: plain thunks mapped rax to x8), and native callers that set
# rax themselves from an image with and without Swift metadata (x8calls.S).
# Builds tests/guest/x8.c (+ the native helper dylibs x8helper.c, x8calls.S)
# for arm64e (guest) and arm64 (reference); the hao runs (JIT, --interp)
# must print what native prints.
#   OUT=dir HAO=build/hao [T=60] tests/guest/indirect-x8.sh
set -u
# This fixture exercises the guest/native bridge through its universal helper.
export HAO_NATIVE_APP_LIBS=1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-indirect-x8)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" \
    -install_name "$OUT/libx8helper.dylib" -o "$OUT/libx8helper.dylib" "$here/x8helper.c" "$here/x8calls.S"
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -DSWIFT_IMAGE \
    -install_name "$OUT/libx8swift.dylib" -o "$OUT/libx8swift.dylib" "$here/x8calls.S"
codesign -s - -f "$OUT/libx8helper.dylib" 2>/dev/null
codesign -s - -f "$OUT/libx8swift.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -I"$here" -L"$OUT" -lx8helper -lx8swift -o "$OUT/x8.$a" "$here/x8.c"
    codesign -s - -f "$OUT/x8.$a" 2>/dev/null
done
set +e
timeout -s KILL "$T" "$OUT/x8.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
for mode in jit interp; do
    flag=; [ $mode = interp ] && flag=--interp
    timeout -s KILL "$T" "$HAO" $flag "$OUT/x8.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'; head -3 "$OUT/$mode.err" | sed 's/^/    stderr: /'; }
done
[ $fail = 0 ] && echo "indirect-x8: PASS" || echo "indirect-x8: FAIL"
exit $fail
