#!/bin/sh
# Guest images from the arm64e dyld shared cache (src/dsc.c): a program that
# calls libccan, libdpfu and libibverbs, libraries that exist only in the
# arm64e shared cache (no x86_64 build, no file on disk), and crobjc.m, which
# uses the Objective-C classes, category and protocol of libCoreRepairState
# (pre-optimised cache metadata: direct method lists, objc_msgSend stubs,
# selectors built in code, canonical protocols; src/objc.c). Built for arm64
# (run natively as the reference) and arm64e (run under hao: JIT and
# --interp). The link-time stand-ins (tests/guest/dsc/*_stub.c|m) carry the
# install names and are never installed.
# Needs the host's arm64e cache (or one under HAO_GUEST_ROOT).
#   OUT=dir HAO=build/hao [T=60] tests/guest/dsc.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=$here/dsc
OUT=${OUT:-$(mktemp -d -t hao-dsc)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
for a in arm64 arm64e; do
    stubs=
    for s in ccan:/usr/lib/rdma/libccan.dylib dpfu:/usr/lib/libdpfu.dylib ibverbs:/usr/lib/rdma/libibverbs.dylib; do
        n=${s%%:*}
        clang -arch $a -dynamiclib -install_name "${s#*:}" -o "$OUT/lib$n-stub.$a.dylib" "$src/${n}_stub.c"
        stubs="$stubs $OUT/lib$n-stub.$a.dylib"
    done
    clang -arch $a -O1 -o "$OUT/dsc.$a" "$src/ccan.c" $stubs
    clang -arch $a -dynamiclib -fobjc-arc -framework Foundation -install_name /usr/lib/libCoreRepairState.dylib \
        -o "$OUT/libcrstate-stub.$a.dylib" "$src/crstate_stub.m"
    clang -arch $a -O1 -fobjc-arc -framework Foundation -o "$OUT/crobjc.$a" "$src/crobjc.m" "$OUT/libcrstate-stub.$a.dylib"
    codesign -s - -f "$OUT/dsc.$a" "$OUT/crobjc.$a" 2>/dev/null
done
set +e
cd "$OUT"
fail=0
for t in dsc crobjc; do
    timeout -s KILL "$T" ./$t.arm64 >$t.native.out 2>$t.native.err
    rc=$?
    echo "$t native rc=$rc lines=$(wc -l <$t.native.out | tr -d ' ')"
    [ $rc = 0 ] || fail=1
    for mode in jit interp; do
        flag=; [ $mode = interp ] && flag=--interp
        timeout -s KILL "$T" "$HAO" -v $flag ./$t.arm64e >$t.$mode.out 2>$t.$mode.err
        rc=$?
        if [ $rc = 0 ] && cmp -s $t.native.out $t.$mode.out; then r=SAME; else r=DIFF; fail=1; fi
        echo "$t $mode rc=$rc $r | $(grep 'dsc bind' $t.$mode.err | sed 's/^hao: dsc bind //; s|/usr/lib/||' | tr '\n' ';')"
        [ $r = SAME ] || { diff $t.native.out $t.$mode.out | head -20; tail -5 $t.$mode.err; }
    done
done
exit $fail
