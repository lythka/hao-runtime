#!/bin/sh
# Guest -> native -> guest recursion on a 512 KB thread (tests/guest/deepcallback.c): qsort's comparator sorts
# again, 250 levels deep. hao's native frames per level used to exhaust such a thread at 125 levels (WallpaperAgent
# on 26B131 at 134). Built for arm64 (native reference) and arm64e (under hao: JIT, --interp, AOT); same output.
#   OUT=dir HAO=build/hao [T=60] tests/guest/deepcallback.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-deepcallback)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64 arm64e; do
    clang -arch $a -O2 -o "$OUT/deepcallback.$a" "$here/deepcallback.c" && codesign -s - -f "$OUT/deepcallback.$a" 2>/dev/null || exit 2
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/deepcallback.arm64e" >"$OUT/aot.gen" 2>&1 || echo "deepcallback: aot generation failed"
timeout -s KILL "$T" "$OUT/deepcallback.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc $(cat "$OUT/native.out")"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/deepcallback.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { grep -m1 'host fault' "$OUT/$mode.err" | cut -c1-160 | sed 's/^/    /'; }
done
[ $fail = 0 ] && echo "deepcallback: PASS" || echo "deepcallback: FAIL"
exit $fail
