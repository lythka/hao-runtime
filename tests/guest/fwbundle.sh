#!/bin/sh
# A guest framework that finds its bundle by identifier, as a framework locating its own resources does
# (+[NSBundle bundleWithIdentifier:] then URLForResource:withExtension:), and by class (Bundle(for:) of a
# plain, a resilient-ancestry, a generic and a pure Swift class); and Bundle.allFrameworks
# (tests/guest/fwbundle_lib.swift, fwbundle.swift). A guest framework is not a dyld image, so CoreFoundation
# never made its bundle; hao creates it when it loads the framework (src/runtime.c register_framework_bundle).
# The framework is fat (arm64 for the native reference, arm64e for hao); the program is arm64 natively and
# arm64e under hao (JIT, --interp, AOT). Each line must equal native arm64.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/fwbundle.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-fwbundle)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
F=$OUT/Frameworks/EC.framework
rm -rf "$OUT/Frameworks"; mkdir -p "$F/Versions/A/Resources"
echo model >"$F/Versions/A/Resources/model.txt"
cat >"$F/Versions/A/Resources/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>test.EC</string><key>CFBundleExecutable</key><string>EC</string><key>CFBundlePackageType</key><string>FMWK</string></dict></plist>
PL
ln -sfn A "$F/Versions/Current"; ln -sfn Versions/Current/EC "$F/EC"; ln -sfn Versions/Current/Resources "$F/Resources"
for a in arm64 arm64e; do
    xcrun swiftc -O -parse-as-library -emit-library -module-name EC -emit-module -emit-module-path "$OUT/mod-$a/EC.swiftmodule" \
        -target $a-apple-macos27.0 -Xlinker -install_name -Xlinker @rpath/EC.framework/Versions/A/EC \
        -o "$OUT/EC.$a" "$here/fwbundle_lib.swift" 2>"$OUT/build.lib.$a.err" || { cat "$OUT/build.lib.$a.err"; exit 2; }
done
lipo -create "$OUT/EC.arm64" "$OUT/EC.arm64e" -output "$F/Versions/A/EC" || exit 2
codesign -s - -f "$F" 2>/dev/null
for a in arm64e arm64; do
    xcrun swiftc -O -target $a-apple-macos27.0 -I "$OUT/mod-$a" -Xlinker "$F/Versions/A/EC" \
        -Xlinker -rpath -Xlinker "$OUT/Frameworks" -o "$OUT/fwbundle.$a" "$here/fwbundle.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/fwbundle.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/fwbundle.arm64e" >"$OUT/aot.gen" 2>&1 || echo "fwbundle: aot generation failed"
timeout -s KILL "$T" "$OUT/fwbundle.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/fwbundle.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -12
done
[ $fail = 0 ] && echo "fwbundle: PASS" || echo "fwbundle: FAIL"
exit $fail
