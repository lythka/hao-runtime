#!/bin/sh
# Key path reads and writes through libswiftCore's key path entry points (tests/guest/keypath.swift):
# WritableKeyPath and ReferenceWritableKeyPath on structs and classes, concrete and generic roots,
# stored, computed and optional-chained components, modify and read coroutines, partial and any key
# paths, and a SwiftUI-shaped environment-writing modifier. Built -O for arm64e (guest) and arm64
# (reference); each run must print what native arm64 prints and exit 0, under the JIT, --interp
# and an AOT cache.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/keypath.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-keypath}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
fail=0
for a in arm64e arm64; do
    xcrun swiftc -O -target $a-apple-macos27.0 -o "$OUT/keypath.$a" "$here/keypath.swift" 2>"$OUT/build.$a.err" ||
        { cat "$OUT/build.$a.err"; echo "keypath: build failed"; exit 1; }
    codesign -s - -f "$OUT/keypath.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot "$OUT/keypath.arm64e" >"$OUT/aot.gen" 2>&1 || echo "keypath: aot generation failed"
timeout -s KILL "$T" "$OUT/keypath.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native    rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] && grep -q '^keypath: done' "$OUT/native.out" || fail=1
for mode in ${MODES:-jit interp aot}; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/keypath.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    r=SAME
    [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" || r=DIFF
    [ "$r" = SAME ] || fail=1
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ "$r" = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | head -10
        grep -v '^hao: \(blocks\|threads\|bridge\)\|^hao-\|: bridge: ' "$OUT/$mode.err" | head -6; } | sed 's/^/    /'
done
[ $fail = 0 ] && echo "keypath: PASS" || echo "keypath: FAIL"
exit $fail
