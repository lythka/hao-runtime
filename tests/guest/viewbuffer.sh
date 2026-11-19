#!/bin/sh
# A guest View's body, which SwiftUI evaluates with a value scope on its result, copies guest-defined values (with
# layout-string value witnesses: native code) through an existential, one boxed past the inline buffer and one
# inline (tests/guest/viewbuffer.swift); every line must equal native arm64 under JIT, --interp and AOT, with no
# refusal. The program is arm64 natively and arm64e under hao.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/viewbuffer.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-viewbuffer)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
LSVW="-Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation
      -enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation"
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library $LSVW -target $a-apple-macos27.0 -o "$OUT/viewbuffer.$a" "$here/viewbuffer.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/viewbuffer.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/viewbuffer.arm64e" >"$OUT/aot.gen" 2>&1 || echo "viewbuffer: aot generation failed"
timeout -s KILL "$T" "$OUT/viewbuffer.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/viewbuffer.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'Swift value boundary refused' "$OUT/$mode.err"
    then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16; grep -a 'Swift value' "$OUT/$mode.err" | head -4 | cut -c1-300; }
done
[ $fail = 0 ] && echo "viewbuffer: PASS" || echo "viewbuffer: FAIL"
exit $fail
