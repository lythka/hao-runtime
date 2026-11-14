#!/bin/sh
# Calls of native code through pointers from dlsym (tests/guest/hostcode.c): hao's check that a
# first-call target is executable host memory reads the image's own segments (bridge.c
# image_exec_at), not the kernel's VM map. Built for arm64e (guest) and arm64 (reference).
#   call: JIT, --interp and AOT print what native prints; HAO_BRIDGE_STATS' "host-code checks=" line
#         shows the targets answered by image and 0 by the kernel
#   data: a pointer to native data is still refused ("not executable host memory"), in every engine
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/hostcode.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-hostcode)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -o "$OUT/hostcode.$a" "$here/hostcode.c"
    codesign -s - -f "$OUT/hostcode.$a" 2>/dev/null
done
set +e
fail=0
timeout -s KILL "$T" "$OUT/hostcode.arm64" >"$OUT/native.out" 2>/dev/null || { echo "hostcode native: FAIL"; fail=1; }
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/hostcode.arm64e" >"$OUT/aotgen.out" 2>&1
for mode in jit interp aot; do
    case $mode in
    jit)    set -- env HAO_NO_AOT=1 HAO_BRIDGE_STATS=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 HAO_BRIDGE_STATS=1 "$HAO" --interp ;;
    aot)    set -- env HAO_CACHE="$OUT/aot" HAO_BRIDGE_STATS=1 "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/hostcode.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    line=$(grep -o 'host-code checks=[0-9]* by image, [0-9]* by the kernel' "$OUT/$mode.err" | tail -1)
    img=$(echo "$line" | sed -n 's/host-code checks=\([0-9]*\) by image.*/\1/p')
    krn=$(echo "$line" | sed -n 's/.*, \([0-9]*\) by the kernel/\1/p')
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && [ "${img:-0}" -ge 3 ] && [ "${krn:-1}" = 0 ]; then r=PASS
    else r="FAIL (rc=$rc, $line)"; fail=1; fi
    echo "hostcode call $mode: $r"
    timeout -s KILL "$T" "$@" "$OUT/hostcode.arm64e" data >"$OUT/$mode.data.out" 2>"$OUT/$mode.data.err"
    rc=$?
    if [ $rc != 0 ] && grep -q 'not executable host memory' "$OUT/$mode.data.err"; then r=PASS
    else r="FAIL (rc=$rc, no refusal)"; fail=1; fi
    echo "hostcode data $mode: $r"
done
[ $fail = 0 ] && echo "hostcode: PASS" || echo "hostcode: FAIL"
exit $fail
