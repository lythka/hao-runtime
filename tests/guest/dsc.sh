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
# a malformed cache header is refused with the reason, not mapped or read: a mapping past the end of its
# file, and no mappings at all (nothing then holds the header hao reads)
for c in /System/Cryptexes/OS /System/Volumes/Preboot/Cryptexes/OS ""; do
    real=$c/System/Library/dyld/dyld_shared_cache_arm64e
    [ -r "$real" ] && break
done
for bad in pastend:'a mapping past the end of the file' nomaps:'no mapping holds the header'; do
    name=${bad%%:*}
    mkdir -p bad-$name/System/Library/dyld
    python3.14 -I -c '
import struct, sys
h = bytearray(open(sys.argv[1], "rb").read(16384))
for table, count, size in ((0x10, 0x14, 32), (0x138, 0x13c, 56)):
    off, n = struct.unpack_from("<II", h, table)
    if sys.argv[3] == "pastend" and off and n: struct.pack_into("<Q", h, off + 16, 1 << 40)
    if sys.argv[3] == "nomaps": struct.pack_into("<I", h, count, 0)
if sys.argv[3] == "nomaps":
    for count in (0x18c, 0x1c4, 0x1c): struct.pack_into("<I", h, count, 0)   # subcaches, images
open(sys.argv[2], "wb").write(h)' "$real" bad-$name/System/Library/dyld/dyld_shared_cache_arm64e $name
    HAO_GUEST_ROOT=$PWD/bad-$name timeout -s KILL "$T" "$HAO" ./dsc.arm64e >bad-$name.out 2>bad-$name.err
    if grep -q "${bad#*:}" bad-$name.err; then echo "bad cache ($name) refused"
    else echo "bad cache ($name) not refused"; tail -3 bad-$name.err; fail=1; fi
done
exit $fail
