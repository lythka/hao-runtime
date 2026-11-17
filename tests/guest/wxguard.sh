#!/bin/sh
# A guest cannot change the protection or mapping of hao's executable memory or of its own sealed
# constant data (tests/guest/wxguard.c). Natively there is no anonymous executable memory and the
# program may make its __DATA_CONST writable or map it a second time; under hao (JIT, --interp) every change of the code cache's
# page is refused and its protection stays, and __DATA_CONST stays read-only with no second view.
#   OUT=dir HAO=build/hao [T=60] tests/guest/wxguard.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-wxguard)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$OUT/wxguard.$a" "$here/wxguard.c" && codesign -s - -f "$OUT/wxguard.$a" 2>/dev/null || exit 2
done
fail=0
timeout -s KILL "$T" "$OUT/wxguard.arm64" >"$OUT/native.out" 2>&1; rc=$?
echo "native    rc=$rc $(tr '\n' ';' <"$OUT/native.out")"
[ $rc = 0 ] && grep -q 'memory: none' "$OUT/native.out" && grep -q 'writable: yes' "$OUT/native.out" &&
    grep -q 'remapped: yes' "$OUT/native.out" || fail=1
for mode in jit interp; do
    flag=; [ $mode = interp ] && flag=--interp
    HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" $flag "$OUT/wxguard.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    ok=0
    [ $rc = 0 ] && grep -q 'changes refused 9 of 9, protection kept yes' "$OUT/$mode.out" && grep -q 'writable: refused' "$OUT/$mode.out" &&
        grep -q 'remapped: refused' "$OUT/$mode.out" && ok=1
    [ $ok = 1 ] && r=PASS || { r=FAIL; fail=1; }
    printf '%-9s rc=%s %s %s\n' "$mode" "$rc" "$r" "$(tr '\n' ';' <"$OUT/$mode.out")"
done
[ $fail = 0 ] && echo "wxguard: PASS" || echo "wxguard: FAIL"
exit $fail
