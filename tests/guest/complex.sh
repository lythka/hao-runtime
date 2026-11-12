#!/bin/sh
# Compare complex arithmetic results and full FPSR with native arm64.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-complex)}
HAO=${HAO:-$repo/build/hao}
T=${T:-180}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    clang -arch $a -march=armv8.5-a+fp16 -O1 -fno-builtin -o "$OUT/complex.$a" "$here/complex.c"
    codesign -s - -f "$OUT/complex.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/complex.arm64e" >/dev/null 2>&1
set +e
timeout -s KILL "$T" "$OUT/complex.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
for mode in jit interp helpers aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/complex.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
[ $fail = 0 ] && echo "complex: PASS" || echo "complex: FAIL"
exit $fail
