#!/bin/sh
# An aggregate of three or four doubles RETURNED by a native function no header declares:
# an HFA in d0-d3 on arm64 (x8 untouched), MEMORY class on SysV (sret
# pointer in rdi, every argument one register up). Dock's
# SLSWindowIteratorGetBounds is one. tests/guest/hfasret.c calls stand-ins with the 26A428
# prototypes (hfasrethelper.c, a native x86_64/arm64/arm64e dylib exporting the real names:
# SLSWindowIteratorGetBounds, SLSWindowIteratorGetScreenRect, MKMapRectMakeWithRadialDistance,
# UIEdgeInsetsMakeWithEdges, QCVector_Make, QLContentRect, UIAccessibilityConvertFrameToScreenCoordinates,
# _MKMapRectThatFits (HFA arguments on the stack too);
# no SkyLight, MapKit, UIKit or QuickLook code is loaded), and compiler-rt's __mulsc3 (float _Complex: s0/s1
# on arm64, packed in xmm0 on SysV; the one import with a small float-HFA result). Built for arm64e (guest) and arm64
# (reference); arm64 runs natively (twice: the reference must be deterministic), arm64e under hao
# with the JIT, with --interp and from an AOT cache, and each hao run must print exactly what
# native prints (canaries around every result buffer included).
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/hfasret.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-hfasret}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
# no FMA contraction: the native slices must compute the same doubles (fmsub on arm64 only)
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -ffp-contract=off -I"$here" \
    -install_name "$OUT/libhfasrethelper.dylib" -o "$OUT/libhfasrethelper.dylib" "$here/hfasrethelper.c"
codesign -s - -f "$OUT/libhfasrethelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -I"$here" -L"$OUT" -lhfasrethelper -o "$OUT/hfasret.$a" "$here/hfasret.c"
    codesign -s - -f "$OUT/hfasret.$a" 2>/dev/null
done
set +e
CASES="sls radial edges vec3 qlrect axframe mkfits cmul georegion"
fail=0
# the reference, case by case, twice: it must be deterministic
for c in $CASES; do
    timeout -s KILL "$T" "$OUT/hfasret.arm64" $c >"$OUT/$c.native.out" 2>"$OUT/$c.native.err"
    rc=$?
    timeout -s KILL "$T" "$OUT/hfasret.arm64" $c >"$OUT/$c.native2.out" 2>/dev/null
    if [ $rc != 0 ]; then echo "hfasret $c native rc=$rc FAIL"; fail=1
    elif ! cmp -s "$OUT/$c.native.out" "$OUT/$c.native2.out"; then echo "hfasret $c native-repeat DIFF"; fail=1; fi
done
# A fresh private AOT cache (user-owned, not group/other-writable): generate it, then run from it.
export HAO_CACHE="$OUT/aot"
rm -rf "$HAO_CACHE"; mkdir -p "$HAO_CACHE"; chmod 700 "$HAO_CACHE"
timeout -s KILL "$T" "$HAO" --aot "$OUT/hfasret.arm64e" >"$OUT/aotgen.out" 2>&1
ls "$HAO_CACHE"/*.aot >/dev/null 2>&1 || { echo "hfasret aot: no cache written"; fail=1; }
for c in $CASES; do
    line="hfasret $(printf '%-13s' $c)"
    for mode in jit interp aot; do
        o=$OUT/$c.$mode
        case $mode in
        jit)    HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" "$OUT/hfasret.arm64e" $c >"$o.out" 2>"$o.err" ;;
        interp) HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" --interp "$OUT/hfasret.arm64e" $c >"$o.out" 2>"$o.err" ;;
        aot)    timeout -s KILL "$T" "$HAO" "$OUT/hfasret.arm64e" $c >"$o.out" 2>"$o.err" ;;
        esac
        rc=$?
        if [ $rc = 0 ] && cmp -s "$OUT/$c.native.out" "$o.out"; then r=SAME
        elif [ $rc -ge 128 ]; then r=CRASH; fail=1
        else r=DIFF; fail=1; fi
        line="$line $mode=$r"
        [ $r = SAME ] || { echo "$c" >>"$OUT/failed.$mode"; }
    done
    echo "$line"
    # show the first failing mode's diff (the three modes print the same bytes apart from addresses)
    for mode in jit interp aot; do
        o=$OUT/$c.$mode
        if [ -s "$OUT/failed.$mode" ] && grep -qx "$c" "$OUT/failed.$mode"; then
            diff "$OUT/$c.native.out" "$o.out" | sed 's/^/    /' | head -12
            grep -m2 "fault\|hao:" "$o.err" | sed 's/^/    /'
            break
        fi
    done
done
rm -f "$OUT"/failed.*
[ $fail = 0 ] && echo "hfasret: PASS" || echo "hfasret: FAIL"
exit $fail
