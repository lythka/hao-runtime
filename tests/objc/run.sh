#!/bin/sh
# Objective-C metadata registration: objcmeta.m (relative method lists,
# protocols, ivars, properties, categories, +load, a superclass from a
# library missing at run time) built for arm64 (reference, run natively) and
# arm64e. The arm64e builds run under hao (JIT and --interp), and the host
# driver objcreg registers them with the native runtime without running any
# guest code; every result is compared with native arm64. The library is
# linked weakly (what native dyld accepts when it is missing) and strongly
# (the shape of a daemon whose framework has no x86_64 slice): natively dyld
# halts the launch with "Library not loaded", and hao must too, with the
# same exit status and the same missing path (objcreg, which runs no dyld,
# still registers the strong build like the weak one). The behaviour section covers protocols
# with extended method types (NSXPCInterface), variadic selectors, long
# object lists and guest IMPs installed with class_addMethod and friends.
# objcproto.m checks protocols that inherit across images: a guest dylib
# (objcprotolib.m) registers them during its initialisation.
# "objcmeta assert|cassert" end in an uncaught NSAssert/NSCAssert exception,
# whose reason is compared. noswift.c checks that a program without Swift
# does not get the Swift runtime loaded.
#   [OUT=dir] [B=build] [T=60] tests/objc/run.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-objc)}
B=${B:-$repo/build}
case $B in /*) ;; *) B=$repo/$B ;; esac
HAO=${HAO:-$B/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
rm -rf "$OUT/absent"
mkdir -p "$OUT/absent"
clang -arch arm64 -arch arm64e -dynamiclib -O1 -framework Foundation -install_name "$OUT/absent/libomabsent.dylib" \
    -o "$OUT/absent/libomabsent.dylib" "$here/objcabsent.m"
for a in arm64 arm64e; do
    clang -arch $a -O1 -fno-objc-arc -I"$here" -framework Foundation -Wl,-weak_library,"$OUT/absent/libomabsent.dylib" \
        -o "$OUT/objcmeta.$a" "$here/objcmeta.m"
done
for a in arm64 arm64e; do clang -arch $a -O1 -o "$OUT/noswift.$a" "$here/noswift.c"; done
clang -arch arm64 -arch arm64e -dynamiclib -O1 -fno-objc-arc -framework Foundation \
    -install_name "$OUT/libomproto.dylib" -o "$OUT/libomproto.dylib" "$here/objcprotolib.m"
for a in arm64 arm64e; do
    clang -arch $a -O1 -fno-objc-arc -framework Foundation -o "$OUT/objcproto.$a" "$here/objcproto.m" \
        "$OUT/libomproto.dylib"
done
for a in arm64 arm64e; do
    clang -arch $a -O1 -fno-objc-arc -I"$here" -framework Foundation "$OUT/absent/libomabsent.dylib" \
        -o "$OUT/objcmeta-strong.$a" "$here/objcmeta.m"
done
rm -rf "$OUT/absent"
clang -arch x86_64 -O1 -g -I"$repo/include" -I"$here" -o "$OUT/objcreg" "$here/objcreg.c" -L"$B" -lhao \
    -Wl,-rpath,"$B" -lobjc
codesign -s - -f "$OUT/objcmeta.arm64" "$OUT/objcmeta.arm64e" "$OUT/objcmeta-strong.arm64" \
    "$OUT/objcmeta-strong.arm64e" "$OUT/objcreg" \
    "$OUT/noswift.arm64" "$OUT/noswift.arm64e" "$OUT/libomproto.dylib" "$OUT/objcproto.arm64" \
    "$OUT/objcproto.arm64e" 2>/dev/null
set +e
fail=0
timeout -s KILL "$T" "$OUT/objcmeta.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native rc=$rc"
[ $rc = 0 ] || fail=1
sed '/^--- behaviour/,$d' "$OUT/native.out" >"$OUT/native.dump"
cmpout() {  # label rc file reference
    if [ "$2" = 0 ] && cmp -s "$3" "$4"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-14s rc=%-3s %s\n' "$1" "$2" "$r"
}
v=objcmeta
timeout -s KILL "$T" "$HAO" "$OUT/$v.arm64e" >"$OUT/$v.jit.out" 2>"$OUT/$v.jit.err"
cmpout "$v jit" $? "$OUT/$v.jit.out" "$OUT/native.out"
timeout -s KILL "$T" "$HAO" --interp "$OUT/$v.arm64e" >"$OUT/$v.interp.out" 2>"$OUT/$v.interp.err"
cmpout "$v interp" $? "$OUT/$v.interp.out" "$OUT/native.out"
# The strong build: dyld's halt natively (rc and the missing library's line)
timeout -s KILL "$T" "$OUT/objcmeta-strong.arm64" >"$OUT/strong.native.out" 2>"$OUT/strong.native.err"
nrc=$?
grep -o 'Library not loaded: .*' "$OUT/strong.native.err" >"$OUT/strong.native.lnl"
for m in jit interp; do
    case $m in jit) set -- ;; interp) set -- --interp ;; esac
    timeout -s KILL "$T" "$HAO" "$@" "$OUT/objcmeta-strong.arm64e" >"$OUT/strong.$m.out" 2>"$OUT/strong.$m.err"
    hrc=$?
    grep -o 'Library not loaded: .*' "$OUT/strong.$m.err" >"$OUT/strong.$m.lnl"
    if [ $nrc != 0 ] && [ $hrc = $nrc ] && [ -s "$OUT/strong.native.lnl" ] && [ ! -s "$OUT/strong.$m.out" ] &&
        cmp -s "$OUT/strong.native.lnl" "$OUT/strong.$m.lnl"; then r=SAME
    else r=DIFF; fail=1; fi
    printf '%-14s rc=%-3s %s (native rc=%s)\n' "objcmeta-strong $m" "$hrc" "$r" "$nrc"
done
for v in objcmeta objcmeta-strong; do
    OBJC_DISABLE_TAG_OBFUSCATION=YES timeout -s KILL "$T" "$OUT/objcreg" --om "$OUT/$v.arm64e" \
        >"$OUT/$v.objcreg.out" 2>"$OUT/$v.objcreg.err"
    cmpout "$v objcreg" $? "$OUT/$v.objcreg.out" "$OUT/native.dump"
done
for m in assert cassert; do   # rc 134 (SIGABRT) in both; the reason line must match
    timeout -s KILL "$T" "$OUT/objcmeta.arm64" $m >/dev/null 2>"$OUT/$m.native.err"
    nrc=$?
    grep -o "reason: .*" "$OUT/$m.native.err" >"$OUT/$m.native.reason"
    timeout -s KILL "$T" "$HAO" "$OUT/objcmeta.arm64e" $m >/dev/null 2>"$OUT/$m.jit.err"
    hrc=$?
    grep -o "reason: .*" "$OUT/$m.jit.err" >"$OUT/$m.jit.reason"
    if [ $hrc = $nrc ] && [ -s "$OUT/$m.native.reason" ] && cmp -s "$OUT/$m.native.reason" "$OUT/$m.jit.reason"; then r=SAME
    else r=DIFF; fail=1; fi
    printf '%-14s rc=%-3s %s (native rc=%s)\n' "$m jit" "$hrc" "$r" "$nrc"
done
timeout -s KILL "$T" "$OUT/objcproto.arm64" >"$OUT/objcproto.native.out" 2>&1 || fail=1
timeout -s KILL "$T" "$HAO" "$OUT/objcproto.arm64e" >"$OUT/objcproto.jit.out" 2>"$OUT/objcproto.jit.err"
cmpout "objcproto jit" $? "$OUT/objcproto.jit.out" "$OUT/objcproto.native.out"
timeout -s KILL "$T" "$HAO" --interp "$OUT/objcproto.arm64e" >"$OUT/objcproto.interp.out" 2>"$OUT/objcproto.interp.err"
cmpout "objcproto interp" $? "$OUT/objcproto.interp.out" "$OUT/objcproto.native.out"
timeout -s KILL "$T" "$OUT/noswift.arm64" >"$OUT/noswift.native.out" 2>&1
timeout -s KILL "$T" "$HAO" "$OUT/noswift.arm64e" >"$OUT/noswift.jit.out" 2>"$OUT/noswift.jit.err"
cmpout "noswift jit" $? "$OUT/noswift.jit.out" "$OUT/noswift.native.out"
if [ $fail = 0 ]; then echo PASS; else echo "FAIL (outputs in $OUT)"; fi
exit $fail
