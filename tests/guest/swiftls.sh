#!/bin/bash
# Guest enum-tag functions that native libswiftCore's layout-string value witnesses call
# (without native entries, WindowManager takes hundreds of thousands of NX faults). tests/guest/swiftls.swift is built for arm64e (guest) and arm64
# (reference) with layout-string value witnesses, linked with the native helper tests/guest/swiftlsfill.c.
# Sections: static (SinglePayloadEnumFN / MultiPayloadEnumFN in non-generic types' layout strings), template
# (a generic type's one layout string, handed to swift_cvw_instantiateLayoutString), generic (strings the
# runtime builds for generic types from their fields' strings: the *Resolved ops), dict (a Dictionary keyed by
# a guest Hashable type), all; and fill: the whole program with SWIFTLS_FILL=1, whose helper reserves every free
# range from 4 to 64 GB before the program is initialized (no free range within 2 GB of the main image when
# its Swift metadata is walked, as happens in system daemons). Each run under JIT, --interp and AOT with HAO_NX_STATS=1
# must print what native arm64 prints, exit 0 and take 0 native->guest NX faults.
# HAO_BASE=/abs/hao (a hao from before the fix) must reproduce faults in template and fill
# with the same output (the negative control).
#   OUT=dir HAO=/abs/build/hao [HAO_BASE=/abs/old/hao] [MODES="jit interp aot"] tests/guest/swiftls.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-swiftls}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
MODES=${MODES:-jit interp aot}
SECTIONS="static template generic dict all"
LS="-Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation
    -enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation"
export SWIFT_DETERMINISTIC_HASHING=1        # the dictionary section's order, natively and under hao
mkdir -p "$OUT/g" "$OUT/arm"
set -e
clang -arch x86_64 -arch arm64e -mmacosx-version-min=27.0 -dynamiclib -O1 -install_name "$OUT/g/libswiftlsfill.dylib" \
    -o "$OUT/g/libswiftlsfill.dylib" "$here/swiftlsfill.c"
clang -arch arm64 -mmacosx-version-min=27.0 -dynamiclib -O1 -install_name "$OUT/arm/libswiftlsfill.dylib" \
    -o "$OUT/arm/libswiftlsfill.dylib" "$here/swiftlsfill.c"
# shellcheck disable=SC2086
swiftc -O $LS -target arm64e-apple-macos27.0 -o "$OUT/swiftls.arm64e" "$here/swiftls.swift" -L"$OUT/g" -lswiftlsfill
# shellcheck disable=SC2086
swiftc -O $LS -target arm64-apple-macos27.0 -o "$OUT/swiftls.arm64" "$here/swiftls.swift" -L"$OUT/arm" -lswiftlsfill
codesign -s - -f "$OUT/g/libswiftlsfill.dylib" "$OUT/arm/libswiftlsfill.dylib" "$OUT/swiftls.arm64e" "$OUT/swiftls.arm64" 2>/dev/null
set +e
for s in _swift_cvw_initWithCopy _swift_cvw_instantiateLayoutString _swift_cvw_initStructMetadataWithLayoutString \
         _swift_cvw_initEnumMetadataMultiPayloadWithLayoutString; do
    nm -u "$OUT/swiftls.arm64e" | grep -q "^$s\$" || { echo "swiftls: the build does not import $s"; exit 1; }
done
case " $MODES " in *" aot "*)
    rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
    HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/swiftls.arm64e" >"$OUT/aot.log" 2>&1 ||
        echo "aot: --aot failed: $(tail -1 "$OUT/aot.log")" ;;
esac

fail=0
nx_of() { sed -n 's/^hao: nx faults: \([0-9]*\) native->guest.*/\1/p' "$1" | tail -1; }
run() {   # NAME SECTION HAO_BIN MODE: OUT/NAME.out, .err
    local name=$1 s=$2 bin=$3 m=$4 arg=$2 fill=
    [ "$s" = all ] && arg=
    [ "$s" = fill ] && arg= && fill=1
    case $m in
    jit) set -- env HAO_NO_AOT=1 HAO_NX_STATS=1 "$bin" ;;
    interp) set -- env HAO_NO_AOT=1 HAO_NX_STATS=1 "$bin" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" HAO_NX_STATS=1 "$bin" ;;
    esac
    SWIFTLS_FILL=$fill timeout -s KILL "$T" "$@" "$OUT/swiftls.arm64e" $arg >"$OUT/$name.out" 2>"$OUT/$name.err"
}
for s in $SECTIONS; do
    arg=$s; [ $s = all ] && arg=
    timeout -s KILL "$T" "$OUT/swiftls.arm64" $arg >"$OUT/native.$s.out" 2>"$OUT/native.$s.err" || { echo "native $s failed"; fail=1; }
done
SWIFTLS_FILL=1 timeout -s KILL "$T" "$OUT/swiftls.arm64" >"$OUT/native.fill.out" 2>"$OUT/native.fill.err" || { echo "native fill failed"; fail=1; }
cmp -s "$OUT/native.all.out" "$OUT/native.fill.out" || { echo "native: fill changes the output"; fail=1; }
printf '%-9s' section; for m in $MODES; do printf ' %-12s' "$m (nx)"; done; echo
for s in $SECTIONS fill; do
    printf '%-9s' $s
    detail=
    for m in $MODES; do
        run $m.$s $s "$HAO" $m
        rc=$?
        nx=$(nx_of "$OUT/$m.$s.err"); [ -n "$nx" ] || nx=-
        if [ $rc = 0 ] && [ "$nx" = 0 ] && cmp -s "$OUT/native.$s.out" "$OUT/$m.$s.out" && ! grep -q '^objc\[' "$OUT/$m.$s.err"; then r=SAME
        else
            r=FAIL; fail=1
            detail="$detail$(printf '\n  %s %s rc=%s nx=%s\n' $m $s $rc $nx)$( { diff "$OUT/native.$s.out" "$OUT/$m.$s.out" | head -6
                grep '^objc\[\|^hao' "$OUT/$m.$s.err" | head -8; } | sed 's/^/    /')"
        fi
        printf ' %-12s' "$r ($nx)"
    done
    echo
    [ -z "$detail" ] || printf '%s\n' "$detail"
done
if [ -n "${HAO_BASE:-}" ]; then
    for s in template fill; do
        run base.$s $s "$HAO_BASE" jit
        rc=$?
        nx=$(nx_of "$OUT/base.$s.err")
        if [ $rc = 0 ] && cmp -s "$OUT/native.$s.out" "$OUT/base.$s.out" && [ "${nx:-0}" -gt 0 ]; then
            echo "baseline $s: SAME, $nx faults reproduced"
        else
            echo "baseline $s: rc=$rc nx=${nx:--}: the negative control did not reproduce the faults"; fail=1
        fi
    done
fi
[ $fail = 0 ] && echo "swiftls: PASS" || echo "swiftls: FAIL"
exit $fail
