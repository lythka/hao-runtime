#!/bin/sh
# A guest closure native SwiftUI calls (View.overlay's content) returns an Optional whose nil hao's Optional
# witness calls left in the guest's encoding (tests/guest/overlaynil.swift); the callback's return gives it
# back the native encoding. The program is arm64 natively and arm64e under hao (JIT, --interp, AOT); every
# line must equal native arm64.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/overlaynil.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-overlaynil)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -o "$OUT/overlaynil.$a" "$here/overlaynil.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/overlaynil.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/overlaynil.arm64e" >"$OUT/aot.gen" 2>&1 || echo "overlaynil: aot generation failed"
timeout -s KILL "$T" "$OUT/overlaynil.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/overlaynil.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16
done
[ $fail = 0 ] && echo "overlaynil: PASS" || echo "overlaynil: FAIL"
exit $fail
