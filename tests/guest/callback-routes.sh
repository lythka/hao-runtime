#!/bin/sh
# Issue 3: SDK callback tables and blocks inside heap/task storage.
# Native arm64 output and zero NX faults required in JIT, interpreter and AOT.
# Optional HAO_BASE verifies the unfixed runtime produces the same output but
# faults through Security's allocator and NSInvocation respectively.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
HAO=${HAO:-$repo/build/hao}
OUT=${OUT:-$(mktemp -d -t hao-callback-routes)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for a in arm64 arm64e; do
    clang -arch "$a" -O1 -Wno-deprecated-declarations "$here/cssm-callbacks.c" -framework Security -o "$OUT/cssm.$a"
    clang -arch "$a" -O1 -fblocks "$here/heapblock.m" -framework Foundation -o "$OUT/heapblock.$a"
    codesign -s - -f "$OUT/cssm.$a" "$OUT/heapblock.$a" 2>/dev/null
done
mkdir -p "$OUT/aot"
for p in cssm heapblock; do
    timeout -s KILL 30 "$OUT/$p.arm64" >"$OUT/$p.native.out" 2>"$OUT/$p.native.err"
    timeout -s KILL 120 "$HAO" --aot --cache "$OUT/aot" "$OUT/$p.arm64e" >"$OUT/$p.aot-build.log" 2>&1
    for m in jit interp aot; do
        case $m in
        jit) set -- env HAO_NO_AOT=1 ;;
        interp) set -- env HAO_NO_AOT=1 ;;
        aot) set -- env HAO_CACHE="$OUT/aot" ;;
        esac
        set -- "$@" HAO_NX_STATS=1 "$HAO"
        [ "$m" != interp ] || set -- "$@" --interp
        timeout -s KILL 30 "$@" "$OUT/$p.arm64e" >"$OUT/$p.$m.out" 2>"$OUT/$p.$m.err"
        diff -u "$OUT/$p.native.out" "$OUT/$p.$m.out"
        grep -q '^hao: nx faults: 0 native->guest, 0 native->trap' "$OUT/$p.$m.err"
        echo "$p $m: SAME, 0 NX faults"
    done
    if [ -n "${HAO_BASE:-}" ]; then
        HAO_NO_AOT=1 HAO_NX_STATS=1 timeout -s KILL 30 "$HAO_BASE" "$OUT/$p.arm64e" >"$OUT/$p.before.out" 2>"$OUT/$p.before.err"
        diff -u "$OUT/$p.native.out" "$OUT/$p.before.out"
        grep -Eq '^hao: nx faults: [1-9][0-9]* native->guest' "$OUT/$p.before.err"
        echo "$p before: SAME, NX faults reproduced"
    fi
done
echo 'callback-routes: PASS'
