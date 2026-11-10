#!/bin/sh
# A guest call to an import whose native library defines it as data dies as natively, and hao's fault line names
# the image and section (airportd on MacBookPro9,2: OCLP's IO80211 wrapper exports apple80211_roam_profile_typetoStr as
# a pointer in __DATA,__data). datacall_lib.c is a universal dylib (arm64 for the native reference,
# x86_64 for hao), datacall.c the program: arm64 natively, arm64e under hao (JIT, --interp): same stdout, same
# status; under hao, the fault report has "native image: libdatacall.dylib (__DATA,__data); not code: a native data
# symbol was called".
#   OUT=dir HAO=build/hao [T=30] tests/guest/datacall.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-datacall)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
mkdir -p "$OUT"
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -install_name "$OUT/libdatacall.dylib" \
    -o "$OUT/libdatacall.dylib" "$here/datacall_lib.c" && codesign -s - -f "$OUT/libdatacall.dylib" 2>/dev/null || exit 2
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$OUT/datacall.$a" "$here/datacall.c" "$OUT/libdatacall.dylib" &&
        codesign -s - -f "$OUT/datacall.$a" 2>/dev/null || exit 2
done
fail=0
for args in "" "--no-call"; do
    tag=${args:+nocall}; tag=${tag:-call}
    (cd /tmp && timeout -s KILL "$T" "$OUT/datacall.arm64" $args >"$OUT/native.$tag.out" 2>/dev/null); nrc=$?
    for mode in jit interp; do
        case $mode in
        jit) set -- "$HAO" ;;
        interp) set -- "$HAO" --interp ;;
        esac
        (cd /tmp && timeout -s KILL "$T" "$@" "$OUT/datacall.arm64e" $args >"$OUT/$mode.$tag.out" 2>"$OUT/$mode.$tag.err"); rc=$?
        named=yes
        if [ -z "$args" ]; then
            grep -q "^  native image: libdatacall.dylib (__DATA,__data); not code: a native data symbol was called$" \
                "$OUT/$mode.$tag.err" || named=no
        fi
        if [ $rc = $nrc ] && cmp -s "$OUT/native.$tag.out" "$OUT/$mode.$tag.out" && [ $named = yes ]; then
            echo "SAME datacall ${args:-call} $mode (rc $rc)"
        else
            echo "DIFF datacall ${args:-call} $mode: native rc $nrc, hao rc $rc, fault line named: $named"; fail=1
            diff "$OUT/native.$tag.out" "$OUT/$mode.$tag.out" | head -5
            grep "host fault" "$OUT/$mode.$tag.err" | head -2
        fi
    done
done
exit $fail
