#!/bin/sh
# Native ObjC method IMPs that guest code gets from the runtime and calls
# directly, and native blocks, with arguments or results the two ABIs pass
# differently (MenuBarAgent: without this the menu bar's layers get stack
# garbage for bounds through the captured -[CALayer setBounds:]). The method
# shapes are QuartzCore's, in a native helper class (objcimphelper.m), since
# QuartzCore may not be loaded on the development Mac. Each section in its own
# process, natively (arm64, twice) and under hao with the JIT, --interp and
# an AOT cache; stdout and exit status must equal native arm64's. Section blocksig:
# native blocks whose signature string crosses a page boundary or is long (where an
# image's __cstring puts a signature depends only on its layout).
#   OUT=dir HAO=/abs/build/hao [T=60] [SECTIONS="getimp ..."] [MODES="jit interp aot"] tests/guest/objcimp.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-objcimp)}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
SECTIONS=${SECTIONS:-getimp method methodfor replace gated nilfirst exchange block blocksig}
MODES=${MODES:-jit interp aot}
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -O1 -fobjc-arc -dynamiclib -framework Foundation \
    -install_name "$OUT/libobjcimphelper.dylib" -o "$OUT/libobjcimphelper.dylib" "$here/objcimphelper.m"
codesign -s - -f "$OUT/libobjcimphelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lobjcimphelper -o "$OUT/objcimp.$a" "$here/objcimp.m"
    codesign -s - -f "$OUT/objcimp.$a" 2>/dev/null
done
set +e
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/objcimp.arm64e" >"$OUT/aotgen.out" 2>&1
fail=0
verdict() { if [ "$1" = 137 ]; then echo TIMEOUT; elif [ "$1" -gt 128 ]; then echo "CRASH sig=$(($1 - 128))"; else echo "rc=$1"; fi; }
for s in $SECTIONS; do
    timeout -s KILL "$T" "$OUT/objcimp.arm64" "$s" >"$OUT/$s.native.out" 2>"$OUT/$s.native.err"
    nrc=$?
    timeout -s KILL "$T" "$OUT/objcimp.arm64" "$s" >"$OUT/$s.native2.out" 2>/dev/null
    if [ $nrc != 0 ] || ! cmp -s "$OUT/$s.native.out" "$OUT/$s.native2.out"; then
        echo "objcimp $s native: $(verdict $nrc) or not repeatable"; fail=1; continue
    fi
    line="objcimp $(printf '%-10s' "$s")"
    for mode in $MODES; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        esac
        { timeout -s KILL "$T" "$@" "$OUT/objcimp.arm64e" "$s" >"$OUT/$s.$mode.out" 2>"$OUT/$s.$mode.err"; } 2>/dev/null
        rc=$?
        if [ $rc = 0 ] && cmp -s "$OUT/$s.native.out" "$OUT/$s.$mode.out"; then r=SAME
        elif [ $rc = 0 ]; then r=DIFF
        else r=$(verdict $rc); fi
        [ "$r" = SAME ] || fail=1
        line="$line $mode=$r"
    done
    echo "$line"
done
[ $fail = 0 ] && echo "objcimp: PASS" || echo "objcimp: FAIL"
exit $fail
