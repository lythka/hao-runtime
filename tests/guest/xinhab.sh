#!/bin/sh
# Enum cases stored in the extra inhabitants of a pointer payload, written on one side of the guest/native
# boundary and read on the other (tests/guest/xinhab.swift, xinhab_lib.swift). x86_64 Swift encodes
# inhabitant i of an ObjC-compatible pointer as the value i << 1 (bit 0 is the tagged-pointer bit); arm64
# as i. Index 0 agrees, every later one differs: `[Int]??` nil-of-outer, the no-payload cases of an enum over
# `[Int]`. A ControlCenter crash is this class: guest SwiftUI code
# writes Optional<TupleContent<...>> nil as 1, native SwiftUI tests for 2.
# The helper is a universal Swift dylib (arm64 for the native reference, x86_64 for hao, arm64e to link); the program is
# arm64 natively and arm64e under hao (JIT, --interp, AOT). Each line must equal native arm64.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/xinhab.sh     exit 1 on any difference (open class: it fails)
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-xinhab)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in x86_64 arm64 arm64e; do
    xcrun swiftc -O -parse-as-library -emit-library -module-name xinhab_lib -emit-module -emit-module-path "$OUT/$a/xinhab_lib.swiftmodule" \
        -target $a-apple-macos27.0 -Xlinker -install_name -Xlinker "$OUT/libxinhab_lib.dylib" \
        -o "$OUT/$a/libxinhab_lib.dylib" "$here/xinhab_lib.swift" 2>"$OUT/build.lib.$a.err" ||
        { cat "$OUT/build.lib.$a.err"; exit 2; }
done
lipo -create "$OUT/x86_64/libxinhab_lib.dylib" "$OUT/arm64/libxinhab_lib.dylib" "$OUT/arm64e/libxinhab_lib.dylib" -output "$OUT/libxinhab_lib.dylib" || exit 2
codesign -s - -f "$OUT/libxinhab_lib.dylib" 2>/dev/null
for a in arm64e arm64; do
    xcrun swiftc -O -target $a-apple-macos27.0 -I "$OUT/$a" -L "$OUT" -lxinhab_lib -o "$OUT/xinhab.$a" "$here/xinhab.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/xinhab.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/xinhab.arm64e" >"$OUT/aot.gen" 2>&1 || echo "xinhab: aot generation failed"
timeout -s KILL "$T" "$OUT/xinhab.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/xinhab.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16
done
[ $fail = 0 ] && echo "xinhab: PASS" || echo "xinhab: FAIL"
exit $fail
