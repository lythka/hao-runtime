#!/bin/sh
# Swift storage whose element type holds a native Optional with a differing nil (Data?, Class??; in structs with
# layout-string value witnesses, one of 240 fields), destroyed last by native code (tests/guest/carrierdestroy.swift),
# must equal native arm64 under JIT, --interp and AOT with no refusal; before the fix the storage was refused when made.
# The program is arm64 natively and arm64e under hao.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/carrierdestroy.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-carrierdestroy)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
LSVW="-Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation
      -enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation"
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library $LSVW -target $a-apple-macos27.0 -o "$OUT/carrierdestroy.$a" "$here/carrierdestroy.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/carrierdestroy.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/carrierdestroy.arm64e" >"$OUT/aot.gen" 2>&1 || echo "carrierdestroy: aot generation failed"
timeout -s KILL "$T" "$OUT/carrierdestroy.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/carrierdestroy.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'Swift value boundary refused' "$OUT/$mode.err"
    then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16; grep -a 'Swift value' "$OUT/$mode.err" | head -4 | cut -c1-300; }
done
[ $fail = 0 ] && echo "carrierdestroy: PASS" || echo "carrierdestroy: FAIL"
exit $fail
