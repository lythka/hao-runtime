#!/bin/sh
# A native call that faults on the guest's NULL argument ends the process as on arm64 (IOMFB_bics_daemon on Intel:
# CFDataGetLength(NULL), not a hao bug). cfnull.c built for arm64 (native reference) and arm64e (under hao: JIT,
# --interp, and without JIT memory, as IOMFB_bics_daemon's exec-time sandbox profile runs it): same stdout, same
# signal.
#   OUT=dir HAO=build/hao [T=30] tests/guest/cfnull.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-cfnull)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
mkdir -p "$OUT"
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$OUT/cfnull.$a" "$here/cfnull.c" -framework CoreFoundation -framework IOKit &&
        codesign -s - -f "$OUT/cfnull.$a" 2>/dev/null || exit 2
done
fail=0
for args in "" "--no-call"; do
    (cd /tmp && timeout -s KILL "$T" "$OUT/cfnull.arm64" $args >"$OUT/native.out" 2>/dev/null); nrc=$?
    for mode in jit interp nojit; do
        case $mode in
        jit) set -- "$HAO" ;;
        interp) set -- "$HAO" --interp ;;
        nojit) set -- env HAO_NO_JIT_MEMORY=1 "$HAO" ;;
        esac
        (cd /tmp && timeout -s KILL "$T" "$@" "$OUT/cfnull.arm64e" $args >"$OUT/$mode.out" 2>"$OUT/$mode.err"); rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then
            echo "SAME cfnull ${args:-call} $mode (rc $rc)"
        else
            echo "DIFF cfnull ${args:-call} $mode: native rc $nrc, hao rc $rc"; fail=1
            diff "$OUT/native.out" "$OUT/$mode.out" | head -5
        fi
    done
done
exit $fail
