#!/bin/sh
# Guest messages to native forwarding objects (tests/guest/xpcrect.m): an NSXPC remote object proxy over
# an anonymous listener in the same process, and an NSProtocolChecker in front of a guest object. No
# method implements them, so they reach _objc_msgForward, which native forwarding decodes by the
# receiver's method signature: structs of floats and of doubles, struct results. The
# program is arm64 natively and arm64e under hao (JIT, --interp, AOT); every line must equal native arm64.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/xpcrect.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-xpcrect)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun clang -fobjc-arc -O1 -target $a-apple-macos27.0 -framework Foundation -framework CoreGraphics \
        -o "$OUT/xpcrect.$a" "$here/xpcrect.m" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/xpcrect.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/xpcrect.arm64e" >"$OUT/aot.gen" 2>&1 || echo "xpcrect: aot generation failed"
timeout -s KILL "$T" "$OUT/xpcrect.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/xpcrect.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16
done
[ $fail = 0 ] && echo "xpcrect: PASS" || echo "xpcrect: FAIL"
exit $fail
