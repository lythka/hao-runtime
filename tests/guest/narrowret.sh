#!/bin/sh
# Build tests/guest/narrowret.m (+ the native helper dylib narrowhelper.m) and
# tests/guest/narrowret.swift (Swift runtime entry points) for arm64e (guest)
# and arm64 (reference). Run arm64 natively and arm64e under
# hao with the JIT and with --interp; each hao run must print exactly what
# native arm64 prints. Exit status 0 only if every run matches.
#   OUT=dir HAO=build/hao [T=60] tests/guest/narrowret.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-narrowret)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" -framework Foundation \
    -install_name "$OUT/libnarrowhelper.dylib" -o "$OUT/libnarrowhelper.dylib" "$here/narrowhelper.m"
codesign -s - -f "$OUT/libnarrowhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -Wno-deprecated-declarations -I"$here" -framework Foundation -framework CoreServices -L"$OUT" -lnarrowhelper \
        -o "$OUT/narrowret.$a" "$here/narrowret.m"
    codesign -s - -f "$OUT/narrowret.$a" 2>/dev/null
    xcrun swiftc -target $a-apple-macos27.0 -O -o "$OUT/narrowswift.$a" "$here/narrowret.swift"
    codesign -s - -f "$OUT/narrowswift.$a" 2>/dev/null
done
set +e
fail=0
for p in narrowret narrowswift; do
    timeout -s KILL "$T" "$OUT/$p.arm64" >"$OUT/$p.native.out" 2>"$OUT/$p.native.err"
    rc=$?
    echo "$p native rc=$rc lines=$(wc -l <"$OUT/$p.native.out" | tr -d ' ')"
    [ $rc = 0 ] || fail=1
    for mode in jit interp; do
        flag=; [ $mode = interp ] && flag=--interp
        timeout -s KILL "$T" "$HAO" $flag "$OUT/$p.arm64e" >"$OUT/$p.$mode.out" 2>"$OUT/$p.$mode.err"
        rc=$?
        if [ $rc = 0 ] && cmp -s "$OUT/$p.native.out" "$OUT/$p.$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
        echo "$p $mode $(printf '%-6s' '') rc=$rc $r"
        [ $r = SAME ] || diff "$OUT/$p.native.out" "$OUT/$p.$mode.out" | sed 's/^/    /' | head -20
    done
done
[ $fail = 0 ] && echo "narrowret: PASS" || echo "narrowret: FAIL"
exit $fail
