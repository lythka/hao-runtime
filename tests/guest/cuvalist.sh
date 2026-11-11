#!/bin/sh
# Build tests/guest/cuvalist.m for arm64e (guest) and arm64 (reference): guest calls of CoreUtils
# functions that take a va_list (src/interpose.c vlists). Run arm64 natively and arm64e under hao
# with the JIT, with --interp and from an AOT cache; stdout must match native. Exit status 0 only if every run matches.
#   OUT=dir HAO=build/hao [T=60] tests/guest/cuvalist.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-cuvalist)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
CU=/System/Library/PrivateFrameworks/CoreUtils.framework/Versions/A/CoreUtils
mkdir -p "$OUT"
set -e
# link-time stand-in for the private CoreUtils (the run binds the system's)
printf 'void VSNPrintF(void){}\nvoid VASPrintF(void){}\nvoid NSPrintV(void){}\nvoid NSErrorV(void){}\n' > "$OUT/cu.c"
clang -arch arm64 -arch arm64e -dynamiclib -install_name "$CU" -o "$OUT/libcoreutils.dylib" "$OUT/cu.c"
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -framework Foundation "$OUT/libcoreutils.dylib" -o "$OUT/cuvalist.$a" "$here/cuvalist.m"
    codesign -s - -f "$OUT/cuvalist.$a" 2>/dev/null
done
set +e
timeout -s KILL "$T" "$OUT/cuvalist.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
mkdir -p "$OUT/aot" && chmod 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/cuvalist.arm64e" >"$OUT/aotgen.log" 2>&1 || { echo "AOT generation failed"; fail=1; }
for mode in jit interp aot; do
    set -- "$HAO"
    [ $mode = interp ] && set -- "$HAO" --interp
    [ $mode = aot ] && set -- env HAO_CACHE="$OUT/aot" "$HAO" --stats
    timeout -s KILL "$T" "$@" "$OUT/cuvalist.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'; tail -3 "$OUT/$mode.err" | sed 's/^/    /'; }
done
grep -q 'aot_blocks=[1-9]' "$OUT/aot.err" || { echo "aot: no block came from the cache"; fail=1; }
[ $fail = 0 ] && echo "cuvalist: PASS" || echo "cuvalist: FAIL"
exit $fail
