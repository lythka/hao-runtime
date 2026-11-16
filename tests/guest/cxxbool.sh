#!/bin/sh
# C++ imports with ObjC BOOL in their signature from a native library: ObjC
# BOOL is bool on arm64 and signed char on x86_64, so the guest's import
# (..b..) is exported by the x86_64 library as ..a.. (runtime.c
# native_bool_spelling). Builds tests/guest/cxxboolhelper.mm universal (its
# ObjC class keeps it native under hao) and cxxbool.mm for arm64e (guest) and
# arm64 (reference), checks the helper's slices really differ, then runs
# arm64 natively and arm64e under hao with the JIT, --interp and an AOT cache.
# A run passes when stdout and the exit status match native arm64 and hao
# reports no unresolved import.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/cxxbool.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-cxxbool)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)   # the helper's install name is absolute
set -e
clang++ -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -std=c++17 -fobjc-arc -I"$here" -framework Foundation \
    -install_name "$OUT/libcxxboolhelper.dylib" -o "$OUT/libcxxboolhelper.dylib" "$here/cxxboolhelper.mm"
codesign -s - -f "$OUT/libcxxboolhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang++ -arch $a -O1 -std=c++17 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lcxxboolhelper \
        -o "$OUT/cxxbool.$a" "$here/cxxbool.mm"
    codesign -s - -f "$OUT/cxxbool.$a" 2>/dev/null
done
set +e
fail=0
# the premise: the arm64 name is not exported by the x86_64 slice, its signed char spelling is
sym=__Z11cb_set_textP6CBCellP8NSStringbb
nm -arch x86_64 -gU "$OUT/libcxxboolhelper.dylib" >"$OUT/x86.syms"
nm -arch arm64e -gU "$OUT/libcxxboolhelper.dylib" >"$OUT/arm.syms"
if grep -q " $sym\$" "$OUT/arm.syms" && ! grep -q " $sym\$" "$OUT/x86.syms" &&
    grep -q " __Z11cb_set_textP6CBCellP8NSStringaa\$" "$OUT/x86.syms" && grep -q " __Z8cb_mixedbai\$" "$OUT/x86.syms"; then
    echo "slices differ: $sym (arm64) is ..aa on x86_64"
else
    echo "slices: unexpected exports"; fail=1
fi
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL 120 "$HAO" --aot --cache "$OUT/aot" "$OUT/cxxbool.arm64e" >/dev/null 2>&1
timeout -s KILL "$T" "$OUT/cxxbool.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
nrc=$?
echo "native rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- "$HAO" ;;
    interp) set -- "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/cxxbool.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'unresolved' "$OUT/$mode.err"; then
        r=SAME
    else
        r=DIFF; fail=1
    fi
    echo "$mode rc=$rc $r"
    if [ $r = DIFF ]; then
        diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -20
        grep -m3 'unresolved\|signal\|abort' "$OUT/$mode.err" | sed 's/^/    /'
    fi
done
[ $fail = 0 ] && echo "cxxbool: PASS" || echo "cxxbool: FAIL"
exit $fail
