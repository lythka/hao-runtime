#!/bin/sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-fmtpos)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -fobjc-arc -framework Foundation \
    -install_name "$OUT/libfmtposhelper.dylib" -o "$OUT/libfmtposhelper.dylib" "$here/fmtposhelper.m"
codesign -s - -f "$OUT/libfmtposhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -framework Foundation -L"$OUT" -lfmtposhelper -o "$OUT/fmtpos.$a" "$here/fmtpos.m"
    codesign -s - -f "$OUT/fmtpos.$a" 2>/dev/null
done
set +e
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/fmtpos.arm64e" >"$OUT/aot.gen" 2>&1 || echo "fmtpos: aot generation failed"
timeout -s KILL "$T" "$OUT/fmtpos.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/fmtpos.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16
done
[ $fail = 0 ] && echo "fmtpos: PASS" || echo "fmtpos: FAIL"
exit $fail
