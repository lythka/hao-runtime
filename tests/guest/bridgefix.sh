#!/bin/sh
# Build tests/guest/bridgefix.m (+ the native helper dylib bridgehelper.c) for
# arm64e (guest) and arm64 (reference). Run arm64 natively and arm64e under
# hao with the JIT and with --interp; each hao run must print exactly what
# native arm64 prints. Exit status 0 only if every run matches.
#   OUT=dir HAO=build/hao [T=60] tests/guest/bridgefix.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-bridgefix)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" -framework CoreGraphics -lobjc \
    -install_name "$OUT/libbridgehelper.dylib" -o "$OUT/libbridgehelper.dylib" "$here/bridgehelper.c"
codesign -s - -f "$OUT/libbridgehelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fno-builtin -fobjc-arc -Wno-deprecated-declarations -I"$here" -framework Foundation -framework QuartzCore -lsandbox -L"$OUT" -lbridgehelper \
        -o "$OUT/bridgefix.$a" "$here/bridgefix.m"
    codesign -s - -f "$OUT/bridgefix.$a" 2>/dev/null
done
clang -arch arm64e -O1 -o "$OUT/ldtwins" "$here/ldtwins.c"
codesign -s - -f "$OUT/ldtwins" 2>/dev/null
set +e
fail=0
# src/libm.c binds each arm64 long double function to its double twin: the
# host's arm64e libsystem_m must export them as the same code (the strtold
# family is separate code there and is left out of this check)
pairs=$(sed -n '/^static const struct { const char \*name, \*twin; } ld_twins/,/^};/p' "$repo/src/libm.c" |
    grep -v strto | grep -v wcsto | grep -oE '\{"_[A-Za-z0-9_]+", "[A-Za-z0-9_]+"\}' |
    sed -E 's/\{"_([^"]+)", "([^"]+)"\}/\1:\2/')
timeout -s KILL "$T" "$OUT/ldtwins" $pairs || fail=1
timeout -s KILL "$T" "$OUT/bridgefix.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] || fail=1
for mode in jit interp; do
    flag=; [ $mode = interp ] && flag=--interp
    timeout -s KILL "$T" "$HAO" $flag "$OUT/bridgefix.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
[ $fail = 0 ] && echo "bridgefix: PASS" || echo "bridgefix: FAIL"
exit $fail
