#!/bin/sh
# A guest single-payload enum whose payload is a tuple with a resilient native field (its tag witnesses are
# the runtime's, so its empty cases are in the native encoding), in a guest struct copied by native Optional
# and Array witnesses (tests/guest/svtuplexi.swift): the walk reads the empty cases as the payload's native
# witnesses make them, so the value is certified under the default policy and HAO_SWIFT_VALUE_STRICT=1,
# with no Swift value line, equal native arm64 under JIT, --interp and AOT. Weather's WeatherData.
#   OUT=dir HAO=/abs/build-ts/hao [T=60] tests/guest/svtuplexi.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svtuplexi)}
HAO=${HAO:-$repo/build-ts/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -module-name svtuplexi -target $a-apple-macos27.0 \
        -enable-experimental-feature LayoutStringValueWitnesses \
        -enable-experimental-feature LayoutStringValueWitnessesInstantiation \
        -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation \
        -o "$OUT/svtuplexi.$a" "$here/svtuplexi.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svtuplexi.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svtuplexi.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svtuplexi: aot generation failed"
timeout -s KILL "$T" "$OUT/svtuplexi.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
fail=0
[ $nrc = 0 ] || { echo "native rc=$nrc"; fail=1; }
for mode in jit interp aot strict; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    strict) set -- env HAO_NO_AOT=1 HAO_SWIFT_VALUE_STRICT=1 "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/svtuplexi.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'Swift value' "$OUT/$mode.err"
    then r=SAME; else r=DIFF; fail=1; fi
    printf '%-7s rc=%s %s\n' $mode "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | head -8; grep 'Swift value' "$OUT/$mode.err" | head -4 | cut -c1-300; } | sed 's/^/    /'
done
[ $fail = 0 ] && echo "svtuplexi: PASS" || echo "svtuplexi: FAIL"
exit $fail
