#!/bin/sh
# Guest structs with layout-string value witnesses whose layout swift_cvw_initStructMetadataWithLayoutString
# computes at run time from a frozen Data and resilient fields: hao gives them arm64's extra inhabitant count,
# which their compiled tag witnesses select a field by (tests/guest/layoutxi.swift). The program is arm64
# natively and arm64e under hao (JIT, --interp, AOT); every line must equal native arm64.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/layoutxi.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-layoutxi)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 \
        -enable-experimental-feature LayoutStringValueWitnesses \
        -enable-experimental-feature LayoutStringValueWitnessesInstantiation \
        -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation \
        -o "$OUT/layoutxi.$a" "$here/layoutxi.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/layoutxi.$a" 2>/dev/null
done
# the shape under test: the program's struct metadata is completed by the native layout-string initializer
dyld_info -fixups "$OUT/layoutxi.arm64e" 2>/dev/null | grep -q '_swift_cvw_initStructMetadataWithLayoutString' ||
    { echo "layoutxi: the arm64e build does not use layout-string value witnesses"; exit 2; }
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/layoutxi.arm64e" >"$OUT/aot.gen" 2>&1 || echo "layoutxi: aot generation failed"
timeout -s KILL "$T" "$OUT/layoutxi.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/layoutxi.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16
done
[ $fail = 0 ] && echo "layoutxi: PASS" || echo "layoutxi: FAIL"
exit $fail
