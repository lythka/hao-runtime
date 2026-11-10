#!/bin/sh
# Guest dylib loading (tests/guest/dylibs/): arm64e libraries have no x86_64
# slice, so hao loads every one of them as a guest image. The same tree is
# built for arm64 and run natively under dyld as the reference.
# Covers @rpath (through the loading image's LC_RPATH), @loader_path,
# @executable_path, a dependency cycle, an upward dependency whose
# initializer calls into the not yet initialised library, whole-library and
# renamed re-exports (guest and native), weak imports, a missing weak
# library, a lazily loaded library (LC_LAZY_LOAD_DYLIB_INFO) with its own
# guest dependency, and initializer order and arguments.
#   OUT=dir HAO=build/hao [T=30] tests/guest/dylibs.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=$here/dylibs
OUT=${OUT:-$(mktemp -d -t hao-dylibs)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
set -e
for a in arm64 arm64e; do
    D=$OUT/$a
    rm -rf "$D"
    mkdir -p "$D/bin" "$D/lib" "$D/dlib" "$D/link"
    cc="clang -arch $a -O1 -I$src -w"
    lib() {  # out install-name source extra...
        o=$1; n=$2; s=$3; shift 3
        $cc -dynamiclib -install_name "$n" -o "$o" "$src/$s" "$@"
    }
    lib "$D/dlib/libd.dylib" @rpath/libd.dylib libd.c
    lib "$D/lib/libb.dylib" @loader_path/libb.dylib libb.c
    lib "$D/link/libb.dylib" @loader_path/libb.dylib libb.c -DWITH_OPTIONAL
    lib "$D/lib/liba.dylib" @rpath/liba.dylib liba.c -Wl,-rpath,@loader_path/../dlib "$D/lib/libb.dylib" "$D/dlib/libd.dylib"
    lib "$D/link/libc2.dylib" @rpath/libc2.dylib libc2.c -DSTAGE0
    lib "$D/lib/libc1.dylib" @rpath/libc1.dylib libc1.c "$D/link/libc2.dylib"
    lib "$D/lib/libc2.dylib" @rpath/libc2.dylib libc2.c "$D/lib/libc1.dylib"
    lib "$D/link/libup.dylib" @rpath/libup.dylib libup.c -DSTAGE0
    lib "$D/lib/libdown.dylib" @rpath/libdown.dylib libdown.c "$D/link/libup.dylib"
    lib "$D/lib/libup.dylib" @rpath/libup.dylib libup.c -Wl,-upward_library,"$D/lib/libdown.dylib"
    # main links the link/ copy (its libb has b_optional); lib/ has the real one
    for v in lib link; do
        lib "$D/$v/libre.dylib" @rpath/libre.dylib libre.c -Wl,-reexport_library,"$D/$v/libb.dylib" -Wl,-reexport-lz \
            -Wl,-alias,_b_value,_re_b_value
    done
    lib "$D/link/libgone.dylib" @rpath/libgone.dylib libgone.c
    lib "$D/lib/libe.dylib" @rpath/libe.dylib libe.c
    lib "$D/lib/liblz.dylib" @rpath/liblz.dylib liblz.c "$D/lib/libb.dylib" "$D/lib/libe.dylib"
    $cc -o "$D/bin/main" "$src/main.c" -Wl,-rpath,@executable_path/../lib "$D/lib/liba.dylib" "$D/lib/libc1.dylib" \
        "$D/lib/libdown.dylib" "$D/link/libre.dylib" -Wl,-weak_library,"$D/link/libgone.dylib" \
        -Wl,-lazy_library,"$D/lib/liblz.dylib"
    codesign -s - -f "$D"/lib/*.dylib "$D"/dlib/*.dylib "$D/bin/main" 2>/dev/null
done
set +e
cd "$OUT"
timeout -s KILL "$T" arm64/bin/main >native.out 2>native.err
echo "native rc=$?"
fail=0
# root: HAO_GUEST_ROOT applies to OS paths only, not to @loader_path or
# @executable_path expansions
for mode in jit interp root; do
    flag=; [ $mode = interp ] && flag=--interp
    root=; [ $mode = root ] && root=$OUT/no-such-root
    env ${root:+HAO_GUEST_ROOT=$root} timeout -s KILL "$T" "$HAO" $flag arm64e/bin/main >$mode.out 2>$mode.err
    rc=$?
    if [ $rc = 0 ] && cmp -s native.out $mode.out; then r=SAME; else r=DIFF; fail=1; fi
    echo "dylibs $mode rc=$rc $r"
    [ $r = SAME ] || diff native.out $mode.out | head -20
done
exit $fail
