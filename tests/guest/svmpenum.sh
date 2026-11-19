#!/bin/sh
# A runtime-laid-out guest multi-payload enum with three empty cases at offset 16 of a guest struct, its
# Optional copied by native Optional witnesses, as layout-string code compiles it (tests/guest/svmpenum.swift):
# the walk reads its case from the runtime's tag byte and walks the payload, so the value is certified under
# the default policy and under HAO_SWIFT_VALUE_STRICT=1, equal native arm64 under JIT, --interp and AOT.
#   OUT=dir HAO=/abs/build-ts/hao [T=60] tests/guest/svmpenum.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svmpenum)}
HAO=${HAO:-$repo/build-ts/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -module-name svmpenum -target $a-apple-macos27.0 \
        -enable-experimental-feature LayoutStringValueWitnesses \
        -enable-experimental-feature LayoutStringValueWitnessesInstantiation \
        -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation \
        -o "$OUT/svmpenum.$a" "$here/svmpenum.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svmpenum.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svmpenum.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svmpenum: aot generation failed"
timeout -s KILL "$T" "$OUT/svmpenum.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
fail=0
[ $nrc = 0 ] || { echo "native rc=$nrc"; fail=1; }
for mode in jit interp aot strict; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    strict) set -- env HAO_NO_AOT=1 HAO_SWIFT_VALUE_STRICT=1 "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/svmpenum.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'Swift value' "$OUT/$mode.err"
    then r=SAME; else r=DIFF; fail=1; fi
    printf '%-7s rc=%s %s\n' $mode "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | head -8; grep 'Swift value' "$OUT/$mode.err" | head -4 | cut -c1-300; } | sed 's/^/    /'
done
[ $fail = 0 ] && echo "svmpenum: PASS" || echo "svmpenum: FAIL"
exit $fail
