#!/bin/sh
# Build tests/guest/fenv.c (<fenv.h>: arm64 FE_* values, fenv_t, default
# environments) for arm64e (guest) and arm64 (reference). Run arm64 natively
# and arm64e under hao with the JIT, with --interp and from an AOT cache;
# each hao run must print exactly what native arm64 prints. Exit status 0
# only if every run matches.
#   OUT=dir HAO=build/hao [T=60] tests/guest/fenv.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-fenv}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -fno-builtin -o "$OUT/fenv.$a" "$here/fenv.c"
    codesign -s - -f "$OUT/fenv.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot "$OUT/fenv.arm64e" >/dev/null 2>&1
set +e
timeout -s KILL "$T" "$OUT/fenv.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/fenv.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
[ $fail = 0 ] && echo "fenv: PASS" || echo "fenv: FAIL"
exit $fail
