#!/bin/sh
# NSUnarchiver decoding with arm64's @encode(BOOL) "B" (tests/guest/unarchbool.m): archives holding
# 'c' and 'B', decoded as a value, an array, a struct, multiple values, a struct whose name contains a
# 'B', and without a size. arm64 Foundation accepts an archived 'c' where "B" is asked for; x86_64's
# throws NSArchiverArchiveInconsistency, so hao passes the guest's BOOL codes to NSUnarchiver as 'c'
# (src/objc.c, enum_methods kind 3). Built for arm64 (native reference) and arm64e (guest under the
# JIT, --interp and AOT); every run must print what native prints.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/unarchbool.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-unarchbool)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -Wno-deprecated-declarations -framework Foundation \
        -o "$OUT/unarchbool.$a" "$here/unarchbool.m" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/unarchbool.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/unarchbool.arm64e" >"$OUT/aot.gen" 2>&1 || echo "unarchbool: aot generation failed"
timeout -s KILL "$T" "$OUT/unarchbool.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
grep -q Inconsistency "$OUT/native.out" && { echo "native threw: the test no longer measures this"; fail=1; }
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/unarchbool.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16
done
[ $fail = 0 ] && echo "unarchbool: PASS" || echo "unarchbool: FAIL"
exit $fail
