#!/bin/sh
# A MEMORY-class aggregate by value in a native function that also fills all six SysV GP
# argument registers (mds _SIOpenIndex+0xd4a). tests/guest/aggspi6.c calls
# stand-ins with the 26A428 prototypes (aggspi6helper.c, a native x86_64/arm64/arm64e dylib that
# exports the real names: SIOpenIndex, SICreateNewIndex, ess_notify_tcc_modify,
# ess_notify_od_group_add, ess_notify_od_attribute_set, QLPushCustomRoundedRect,
# CGImageCreateCopyWithParameters, SIUserCtxCreate, NodeCreateSemanticNodeQuery, MRMediaRemoteRequestVideoThumbnails, SIPathFilterCreate) and the real CEAcquireUnmanagedContext. It is built for arm64e
# (guest) and arm64 (reference); arm64 runs natively (twice: the reference must be deterministic),
# arm64e under hao with the JIT, with --interp and from an AOT cache, and each hao run must print
# exactly what native prints.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/aggspi6.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-aggspi6)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
# libCoreEntitlements has no SDK stub: a small .tbd stands in (xnu EXTERNAL_HEADERS/CoreEntitlements)
printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-macos, arm64e-macos ]\ninstall-name: '/usr/lib/libCoreEntitlements.dylib'\nexports:\n  - targets: [ arm64-macos, arm64e-macos ]\n    symbols: [ _CEAcquireUnmanagedContext, _CEGetErrorString, _kCENoError, _kCEInvalidArgument ]\n...\n" \
    >"$OUT/CE.tbd"
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" \
    -install_name "$OUT/libaggspi6helper.dylib" -o "$OUT/libaggspi6helper.dylib" "$here/aggspi6helper.c"
codesign -s - -f "$OUT/libaggspi6helper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -I"$here" -L"$OUT" -laggspi6helper "$OUT/CE.tbd" -o "$OUT/aggspi6.$a" "$here/aggspi6.c"
    codesign -s - -f "$OUT/aggspi6.$a" 2>/dev/null
done
set +e
CASES="si-open si-create si-cancel si-ops ess-tcc ess-group-add ess-attr-set ql-rect img-copy ce si-userctx node-query mr-thumbs si-path-filter"
fail=0
# the reference, case by case, twice: it must be deterministic
for c in $CASES; do
    timeout -s KILL "$T" "$OUT/aggspi6.arm64" $c >"$OUT/$c.native.out" 2>"$OUT/$c.native.err"
    rc=$?
    timeout -s KILL "$T" "$OUT/aggspi6.arm64" $c >"$OUT/$c.native2.out" 2>/dev/null
    if [ $rc != 0 ]; then echo "aggspi6 $c native rc=$rc FAIL"; fail=1
    elif ! cmp -s "$OUT/$c.native.out" "$OUT/$c.native2.out"; then echo "aggspi6 $c native-repeat DIFF"; fail=1; fi
done
# A fresh private AOT cache (user-owned, not group/other-writable): generate it, then run from it.
export HAO_CACHE="$OUT/aot"
rm -rf "$HAO_CACHE"; mkdir -p "$HAO_CACHE"; chmod 700 "$HAO_CACHE"
timeout -s KILL "$T" "$HAO" --aot --cache "$HAO_CACHE" "$OUT/aggspi6.arm64e" >"$OUT/aotgen.out" 2>&1
ls "$HAO_CACHE"/*.aot >/dev/null 2>&1 || { echo "aggspi6 aot: no cache written"; fail=1; }
for c in $CASES; do
    line="aggspi6 $(printf '%-13s' $c)"
    for mode in jit interp aot; do
        o=$OUT/$c.$mode
        case $mode in
        jit)    HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" "$OUT/aggspi6.arm64e" $c >"$o.out" 2>"$o.err" ;;
        interp) HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" --interp "$OUT/aggspi6.arm64e" $c >"$o.out" 2>"$o.err" ;;
        aot)    timeout -s KILL "$T" "$HAO" "$OUT/aggspi6.arm64e" $c >"$o.out" 2>"$o.err" ;;
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
[ $fail = 0 ] && echo "aggspi6: PASS" || echo "aggspi6: FAIL"
exit $fail
