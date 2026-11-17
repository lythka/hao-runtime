#!/bin/sh
set -u
# This fixture exercises the guest/native bridge through its universal helper.
export HAO_NATIVE_APP_LIBS=1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swifttextalign)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT/aot"
chmod 755 "$OUT"
chmod 700 "$OUT/aot"
set -e
for t in x86_64 arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -emit-library -enable-library-evolution \
        -module-name STALib -emit-module -emit-module-path "$OUT/mod-$t/STALib.swiftmodule" \
        -Xlinker -install_name -Xlinker "$OUT/libSTALib.dylib" -o "$OUT/lib-$t.dylib" "$here/swifttextalign_lib.swift"
done
lipo -create "$OUT/lib-x86_64.dylib" "$OUT/lib-arm64.dylib" "$OUT/lib-arm64e.dylib" -output "$OUT/libSTALib.dylib"
codesign -s - -f "$OUT/libSTALib.dylib" 2>/dev/null
for t in arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -I "$OUT/mod-$t" -L"$OUT" -lSTALib -o "$OUT/swifttextalign.$t" "$here/swifttextalign.swift"
    codesign -s - -f "$OUT/swifttextalign.$t" 2>/dev/null
done
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/swifttextalign.arm64e" >/dev/null 2>&1
set +e
fail=0
timeout -s KILL "$T" "$OUT/swifttextalign.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] && [ "$(wc -l <"$OUT/native.out" | tr -d ' ')" = 21 ] || fail=1
for mode in jit interp helpers aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/swifttextalign.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
[ $fail = 0 ] && echo "swifttextalign: PASS" || echo "swifttextalign: FAIL"
exit $fail
