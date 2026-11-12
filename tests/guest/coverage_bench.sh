#!/bin/sh
# Compare SIMD workload checksums with native; separate timing and stats runs.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-coverage-bench)}
HAO=${HAO:-$repo/build/hao}
N=${N:-50000}
T=${T:-120}
mkdir -p "$OUT/aot"; chmod 700 "$OUT/aot"
for arch in arm64 arm64e; do
    clang -arch "$arch" -O1 -o "$OUT/bench.$arch" "$here/coverage_bench.c"
    codesign -s - -f "$OUT/bench.$arch" 2>/dev/null
done
timeout -s KILL "$T" "$OUT/bench.arm64" "$N" >"$OUT/native.out" 2>"$OUT/native.err"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/bench.arm64e" >"$OUT/build.out" 2>"$OUT/build.err"
for mode in jit interp helpers aot; do
    case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/bench.arm64e" "$N" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    cmp "$OUT/native.out" "$OUT/$mode.out"
    echo "PASS $mode coverage benchmark checksum"
    case $mode in jit|aot)
        timeout -s KILL "$T" "$@" --stats "$OUT/bench.arm64e" "$N" >"$OUT/$mode.stats.out" 2>"$OUT/$mode.stats.err"
        cmp "$OUT/native.out" "$OUT/$mode.stats.out"
        grep '^hao-jit:' "$OUT/$mode.stats.err" ;;
    esac
done
