#!/bin/sh
# W^X: hao's executable memory (code cache, thunks, Swift islands) is never writable and executable at once
# while guest code runs (tests/guest/wx.c, wx.swift). Each program counts the regions of its process that are
# both (Rosetta's own memory aside); natively 0. Built for arm64 (native reference) and arm64e (under hao:
# JIT, --interp, AOT); same output.
#   OUT=dir HAO=build/hao [T=60] tests/guest/wx.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-wx)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64 arm64e; do
    clang -arch $a -O2 -o "$OUT/wxc.$a" "$here/wx.c" && codesign -s - -f "$OUT/wxc.$a" 2>/dev/null || exit 2
    clang -arch $a -mmacosx-version-min=27.0 -O2 -DWX_LIB -c -o "$OUT/wxlib.$a.o" "$here/wx.c" || exit 2
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -o "$OUT/wxswift.$a" "$here/wx.swift" "$OUT/wxlib.$a.o" \
        && codesign -s - -f "$OUT/wxswift.$a" 2>/dev/null || exit 2
done
fail=0
for p in wxc wxswift; do
    rm -rf "$OUT/aot.$p"; mkdir -m 700 "$OUT/aot.$p"
    timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot.$p" "$OUT/$p.arm64e" >"$OUT/$p.aot.gen" 2>&1 || echo "$p: aot generation failed"
    timeout -s KILL "$T" "$OUT/$p.arm64" >"$OUT/$p.native.out" 2>/dev/null; nrc=$?
    echo "$p native    rc=$nrc $(tail -1 "$OUT/$p.native.out")"
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot.$p" "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/$p.arm64e" >"$OUT/$p.$mode.out" 2>"$OUT/$p.$mode.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/$p.native.out" "$OUT/$p.$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
        printf '%-8s %-9s rc=%s %s %s\n' "$p" "$mode" "$rc" "$r" "$(tail -1 "$OUT/$p.$mode.out")"
        [ $r = SAME ] || grep -m3 'wx:\|host fault' "$OUT/$p.$mode.err" | cut -c1-160 | sed 's/^/    /'
    done
done
[ $fail = 0 ] && echo "wx: PASS" || echo "wx: FAIL"
exit $fail
