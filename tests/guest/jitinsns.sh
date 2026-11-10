#!/bin/sh
# Instructions the translator emits inline (atomics, bit fields, carry
# arithmetic, division, bit reversal, constant chains, integer SIMD): builds
# tests/guest/jitinsns.c for arm64e (guest) and arm64 (reference) and
# compares the output of hao's JIT, interpreter, helper-only JIT and AOT runs
# with the native one, line by line (one hash per instruction).
#   OUT=dir HAO=build/hao [T=60] tests/guest/jitinsns.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-jitinsns}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -o "$OUT/jitinsns.$a" "$here/jitinsns.c"
    codesign -s - -f "$OUT/jitinsns.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot "$OUT/jitinsns.arm64e" >/dev/null 2>&1
set +e
timeout -s KILL "$T" "$OUT/jitinsns.arm64" >"$OUT/native.out" 2>&1
nrc=$?
fail=0
for v in jit interp helpers aot; do
    case $v in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/jitinsns.arm64e" >"$OUT/$v.out" 2>"$OUT/$v.err"
    rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$v.out"; then r=PASS
    else r=FAIL; fail=1; diff "$OUT/native.out" "$OUT/$v.out" | head -20; fi
    printf '%s %-8s rc=%s (native rc=%s, %s lines)\n' "$r" "$v" "$rc" "$nrc" "$(wc -l <"$OUT/native.out" | tr -d ' ')"
done
exit $fail
