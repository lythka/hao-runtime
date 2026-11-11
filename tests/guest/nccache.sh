#!/bin/sh
# The native-target cache (src/runtime.c run_loop) against native arm64: a
# native IMP called bare by five threads while untyped (nil receivers), then
# typed by method_getImplementation; every thread's later calls must get the
# BOOL result the x86_64 IMP leaves with junk above al extended. Also snprintf
# through dlsym repeatedly (an interposer's trap; nccache.m). Native arm64,
# then hao JIT, --interp and AOT: the same output.
#   OUT=dir HAO=build/hao [T=60] tests/guest/nccache.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-nccache)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT/aot"
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -fobjc-arc -install_name "$OUT/libnch.dylib" -o "$OUT/libnch.dylib" \
    "$here/nccachehelper.m" -framework Foundation && codesign -s - -f "$OUT/libnch.dylib" 2>/dev/null || exit 2
for a in arm64 arm64e; do
    clang -arch $a -O1 -fobjc-arc -L"$OUT" -lnch -framework Foundation -o "$OUT/nccache.$a" "$here/nccache.m" &&
        codesign -s - -f "$OUT/nccache.$a" 2>/dev/null || exit 2
done
fail=0
timeout -s KILL "$T" "$OUT/nccache.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
nrc=$?
grep -q "0 of 1000 results wrong" "$OUT/native.out" || { echo "native reference failed (rc $nrc)"; exit 1; }
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/nccache.arm64e" >"$OUT/aotgen.out" 2>&1 || fail=1
for mode in jit interp aot; do
    flag=; [ $mode = interp ] && flag=--interp
    cache=$OUT/none; [ $mode = aot ] && cache=$OUT/aot
    HAO_CACHE=$cache HAO_VERBOSE=1 timeout -s KILL "$T" "$HAO" $flag "$OUT/nccache.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    if [ $mode = aot ] && ! grep -q "AOT cache .* loaded" "$OUT/$mode.err"; then r="$r (AOT cache not loaded)"; fail=1; fi
    echo "$mode rc=$rc $r"
    [ "$r" = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -20
done
exit $fail
