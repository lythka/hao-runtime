#!/bin/sh
# FMLA .4S three-same/by-element timing and native checksum, JIT and AOT.
# OUT=dir HAO=build/hao [N=250000] [T=60] tests/guest/fmla_bench.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-fmla-bench)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
N=${N:-250000}
mkdir -p "$OUT"
for a in arm64 arm64e; do
    clang -arch "$a" -O1 -o "$OUT/bench.$a" "$here/fmla_bench.c"
    codesign -s - -f "$OUT/bench.$a" 2>/dev/null
done
timeout -s KILL "$T" "$OUT/bench.arm64" "$N" >"$OUT/native.out" 2>"$OUT/native.err"
mkdir -p "$OUT/aot"; chmod 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/bench.arm64e" >"$OUT/aot-build.out" 2>"$OUT/aot-build.err"
for mode in jit aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" --stats "$OUT/bench.arm64e" "$N" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    cmp "$OUT/native.out" "$OUT/$mode.out"
    echo "$mode"
    cat "$OUT/$mode.err"
done
