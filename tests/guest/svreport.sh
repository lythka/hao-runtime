#!/bin/sh
# Compare the bytes on each side of a declined generic argument or result.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
HAO=${HAO:-$here/../../build/hao}
OUT=${OUT:-$(mktemp -d -t hao-svreport)}
T=${T:-60}
mkdir -p "$OUT"
d=$(cd "$OUT" && pwd)
export HAO_NATIVE_APP_LIBS=1
for arch in x86_64 arm64 arm64e; do
    xcrun swiftc -O -target "$arch-apple-macos27" -enable-library-evolution -emit-library -module-name ReportLib \
        -emit-module -emit-module-path "$d/mod-$arch/ReportLib.swiftmodule" -Xlinker -install_name \
        -Xlinker "$d/libReportLib.dylib" -o "$d/lib-$arch.dylib" "$here/svreport_lib.swift"
done
lipo -create "$d/lib-x86_64.dylib" "$d/lib-arm64.dylib" "$d/lib-arm64e.dylib" -output "$d/libReportLib.dylib"
codesign -f -s - "$d/libReportLib.dylib"
for arch in arm64 arm64e; do
    xcrun swiftc -O -target "$arch-apple-macos27" -Xfrontend -disable-reflection-metadata -I "$d/mod-$arch" -L"$d" -lReportLib \
        "$here/svreport.swift" -o "$d/svreport.$arch"
    codesign -f -s - "$d/svreport.$arch"
done
timeout -s KILL "$T" "$d/svreport.arm64" >"$d/native.out"
printf '%s\n' 'unchanged 40' 'unchanged 56' 'returned unchanged true' >"$d/expected.out"
diff -u "$d/expected.out" "$d/native.out"
cat "$d/native.out"
cache=$(mktemp -d "$d/aot.XXXXXX")
timeout -s KILL "$T" "$HAO" --aot --cache "$cache" "$d/svreport.arm64e" >"$d/aot.gen" 2>&1
fail=0
for mode in jit interp aot; do
    case "$mode" in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$cache" "$HAO" -v ;;
    esac
    if timeout -s KILL "$T" "$@" "$d/svreport.arm64e" >"$d/$mode.out" 2>"$d/$mode.err" &&
       grep -q 'passed as found (report-only): lent value has unresolved active representation' "$d/$mode.err" &&
       grep -q 'passed as found (report-only): value native code would misread' "$d/$mode.err" &&
       grep -q 'passed as found (report-only): native result has unresolved active representation' "$d/$mode.err" &&
       { [ "$mode" != aot ] || grep -q 'AOT cache .* loaded' "$d/$mode.err"; } &&
       diff -u "$d/native.out" "$d/$mode.out"; then
        echo "$mode PASS"
    else
        echo "$mode FAIL"
        fail=1
    fi
done
# A verbose launch identifies the whole walked type even on repeated declines.
for reason in 'lent value has unresolved active representation' 'value native code would misread' \
              'native result has unresolved active representation'; do
    if ! grep -Fq "skipped_patches=1 reason=$reason type=(" "$d/aot.err"; then
        echo "missing verbose decline for $reason"
        fail=1
    fi
done
exit "$fail"
