#!/bin/sh
# Upper ymm halves after a native call and on a native-to-guest callback.
# ymmupper_lib.c is a universal dylib (arm64 for the native reference,
# x86_64 for hao), ymmupper.c the program: arm64 natively, arm64e under hao
# (JIT, --interp), each with ROSETTA_ADVERTISE_AVX unset and set to 1 (under
# Rosetta, AVX state is enabled only with it). All runs must print what
# native arm64 prints: no dirty upper.
#   OUT=dir HAO=build/hao [T=30] tests/guest/ymmupper.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-ymmupper)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
mkdir -p "$OUT"
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -install_name "$OUT/libymmupper.dylib" \
    -o "$OUT/libymmupper.dylib" "$here/ymmupper_lib.c" && codesign -s - -f "$OUT/libymmupper.dylib" 2>/dev/null || exit 2
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$OUT/ymmupper.$a" "$here/ymmupper.c" "$OUT/libymmupper.dylib" &&
        codesign -s - -f "$OUT/ymmupper.$a" 2>/dev/null || exit 2
done
timeout -s KILL "$T" "$OUT/ymmupper.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
fail=0
[ $nrc = 0 ] || fail=1
for avx in unset 1; do
    for mode in jit interp; do
        case $mode in
        jit) set -- "$HAO" ;;
        interp) set -- "$HAO" --interp ;;
        esac
        if [ $avx = 1 ]; then set -- env ROSETTA_ADVERTISE_AVX=1 "$@"; else set -- env -u ROSETTA_ADVERTISE_AVX "$@"; fi
        timeout -s KILL "$T" "$@" "$OUT/ymmupper.arm64e" >"$OUT/$mode.$avx.out" 2>"$OUT/$mode.$avx.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.$avx.out"; then
            echo "SAME ymmupper $mode ROSETTA_ADVERTISE_AVX=$avx (rc $rc)"
        else
            echo "DIFF ymmupper $mode ROSETTA_ADVERTISE_AVX=$avx: native rc $nrc, hao rc $rc"; fail=1
            diff "$OUT/native.out" "$OUT/$mode.$avx.out" | head -5
        fi
    done
done
exit $fail
