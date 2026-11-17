#!/bin/sh
# Calls and callbacks hao cannot place fail closed (tests/guest/refuse.m, native side refusehelper.m):
#   mid    the guest result of a callback without a signature lands in hao's own area: the guest's stack
#          is intact under JIT and --interp (it was overwritten by an 8 KB result before)
#   big    a 96 KB result without a buffer: native prints its result; under hao the store faults in the guard
#          after hao's area (the process ends by a signal) instead of overwriting memory
#   call   an ObjC method with a 70000-byte argument: native prints its result; hao stops with
#          "bridge: REFUSED call" instead of passing it in registers
#   block  a guest block with a 70000-byte parameter called from native code: hao stops with
#          "bridge: REFUSED callback"; with HAO_NO_CLOSURES=1 (a test-switch build: no signature thunks) it is
#          refused all the same, at registration (nocl), never run through the plain register bridge
#   strip  tests/guest/swiftthunks.swift built with its local symbols stripped (a release build): Swift
#          closures handed to native Swift functions return their results in memory through rax; the
#          entries hao makes for them say so (the native callee's own Swift symbol): same output as native
#   OUT=dir HAO=build/hao [T=60] tests/guest/refuse.sh     exit 1 on any difference
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-refuse)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -fobjc-arc -I"$here" -framework Foundation \
    -install_name "$OUT/librefusehelper.dylib" -o "$OUT/librefusehelper.dylib" "$here/refusehelper.m" || exit 2
codesign -s - -f "$OUT/librefusehelper.dylib" 2>/dev/null
for a in arm64 arm64e; do
    clang -arch $a -O1 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lrefusehelper -o "$OUT/refuse.$a" "$here/refuse.m" \
        && codesign -s - -f "$OUT/refuse.$a" 2>/dev/null || exit 2
done
fail=0
for c in mid big call block; do
    timeout -s KILL "$T" "$OUT/refuse.arm64" $c >"$OUT/$c.native.out" 2>&1; nrc=$?
    modes="jit interp"; [ $c = block ] && modes="jit interp nocl"
    for mode in $modes; do
        flag=; [ $mode = interp ] && flag=--interp
        set -- env; [ $mode = nocl ] && set -- env HAO_NO_CLOSURES=1
        "$@" HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" $flag "$OUT/refuse.arm64e" $c >"$OUT/$c.$mode.out" 2>"$OUT/$c.$mode.err"; rc=$?
        case $c in
        mid) ok=0; [ $nrc = 0 ] && [ $rc = 0 ] && grep -q 'canary intact' "$OUT/$c.$mode.out" && ok=1 ;;
        big) ok=0; [ $nrc = 0 ] && [ $rc -ge 128 ] && [ $rc != 137 ] && ok=1 ;;
        call) ok=0; [ $nrc = 0 ] && [ $rc != 0 ] && grep -q 'bridge: REFUSED call: signature .*not marshalled' "$OUT/$c.$mode.err" && ok=1 ;;
        block) ok=0; [ $nrc = 0 ] && [ $rc != 0 ] && grep -q 'bridge: REFUSED callback' "$OUT/$c.$mode.err" && ok=1 ;;
        esac
        [ $ok = 1 ] && r=PASS || { r=FAIL; fail=1; }
        printf '%-6s %-7s native rc=%s hao rc=%s %s  %s\n' "$c" "$mode" "$nrc" "$rc" "$r" \
            "$(cat "$OUT/$c.$mode.out" | head -1) $(grep -m1 'REFUSED' "$OUT/$c.$mode.err" | cut -c1-110)"
    done
done
for a in arm64 arm64e; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -o "$OUT/strip.$a" "$here/swiftthunks.swift" 2>/dev/null &&
        strip -x "$OUT/strip.$a" && codesign -s - -f "$OUT/strip.$a" 2>/dev/null || exit 2
done
timeout -s KILL "$T" "$OUT/strip.arm64" >"$OUT/strip.native.out" 2>&1; nrc=$?
HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" "$OUT/strip.arm64e" >"$OUT/strip.jit.out" 2>"$OUT/strip.jit.err"; rc=$?
if [ $nrc = 0 ] && [ $rc = 0 ] && cmp -s "$OUT/strip.native.out" "$OUT/strip.jit.out"; then r=PASS; else r=FAIL; fail=1; fi
printf '%-6s %-7s native rc=%s hao rc=%s %s\n' strip jit "$nrc" "$rc" "$r"
[ $fail = 0 ] && echo "refuse: PASS" || echo "refuse: FAIL"
exit $fail
