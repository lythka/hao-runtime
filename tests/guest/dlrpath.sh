#!/bin/sh
# Guest dlopen of @rpath, @executable_path and @loader_path names (tests/guest/dlrpath.c) in an app-bundle layout:
# under hao they resolve from the calling guest image, as dyld resolves them natively, not from hao's own
# directory. Built twice: arm64 (native reference) and arm64e (guest), each in its own Test.app; the universal
# libnat stays in the guest domain. JIT, --interp and AOT must print what native arm64 prints.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/dlrpath.sh
set -u
unset HAO_NATIVE_APP_LIBS
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-dlrpath)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    app=$OUT/$a/Test.app/Contents
    mkdir -p "$app/MacOS" "$app/Frameworks/sub"
    clang -arch $a -O1 -Wl,-rpath,@executable_path/../Frameworks -o "$app/MacOS/dlrpath" "$here/dlrpath.c"
    clang -arch $a -O1 -dynamiclib -DPLUG -install_name @rpath/libplug.dylib -o "$app/Frameworks/libplug.dylib" "$here/dlrpathlib.c"
    clang -arch $a -O1 -dynamiclib -DSUB -install_name @loader_path/sub/libsub.dylib -o "$app/Frameworks/sub/libsub.dylib" "$here/dlrpathlib.c"
    clang -arch x86_64 -arch arm64 -arch arm64e -O1 -dynamiclib -DNAT -install_name @rpath/libnat.dylib \
        -o "$app/Frameworks/libnat.dylib" "$here/dlrpathlib.c"
    for dir in A B; do
        value=1; [ "$dir" = B ] && value=2
        dest=$app/Frameworks/$dir
        mkdir -p "$dest"
        clang -arch "$a" -dynamiclib -DLEAF_VALUE=$value -install_name @rpath/leaf.dylib \
            "$here/dlrpathlib.c" -o "$dest/leaf.dylib"
        clang -arch "$a" -dynamiclib -DPARENT -install_name "@rpath/parent$dir.dylib" \
            -Wl,-rpath,@loader_path "$here/dlrpathlib.c" "$dest/leaf.dylib" -o "$dest/parent.dylib"
        codesign -s - -f "$dest/leaf.dylib" "$dest/parent.dylib" 2>/dev/null
    done
    codesign -s - -f "$app/Frameworks/libplug.dylib" "$app/Frameworks/sub/libsub.dylib" "$app/Frameworks/libnat.dylib" \
        "$app/MacOS/dlrpath" 2>/dev/null
done
set +e
fail=0
G=$OUT/arm64e/Test.app/Contents/MacOS/dlrpath
timeout -s KILL "$T" "$OUT/arm64/Test.app/Contents/MacOS/dlrpath" >"$OUT/native.out" 2>&1 || { echo "dlrpath native: FAIL"; fail=1; }
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$G" >"$OUT/aotgen.out" 2>&1
for mode in jit interp aot; do
    case $mode in
    jit)    set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot)    set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$G" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then echo "dlrpath $mode: PASS"
    else echo "dlrpath $mode: FAIL (rc=$rc)"; diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -8; fail=1; fi
    timeout -s KILL "$T" env HAO_NATIVE_APP_LIBS=1 "$@" "$G" >"$OUT/$mode.native-libs.out" 2>"$OUT/$mode.native-libs.err"
    if [ $? = 0 ] && grep -qx 'nat_value=86' "$OUT/$mode.native-libs.out"; then
        echo "dlrpath $mode native app libs: PASS"
    else echo "dlrpath $mode native app libs: FAIL"; fail=1; fi
done
[ $fail = 0 ] && echo "dlrpath: PASS" || echo "dlrpath: FAIL"
exit $fail
