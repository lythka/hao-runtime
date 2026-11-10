#!/bin/sh
# Guest C function pointers passed to native ObjC methods ('^?' parameters:
# -[NSArray sortedArrayUsingFunction:context:], the other slots in
# tests/guest/msgfnptrhelper.m, NSInvocation, NSPointerFunctions) must reach
# the native callee as thunks, and pointers the encoding does not type as code
# must keep their guest addresses (tests/guest/msgfnptr.m lists the sections).
# Build msgfnptr.m for arm64e (guest) and arm64 (reference) with the native
# helper (x86_64, arm64, arm64e); run every section alone natively twice (the
# reference must repeat), then under hao with the JIT, with --interp and from
# an AOT cache, each with HAO_NX_STATS=1. A run passes when its stdout and exit
# status equal native arm64's and it took no NX fault (native->guest 0).
# HAO_BASE=/abs/hao (a hao without the fix) is the negative control: each of
# its runs is listed with its fault count, and the control passes when some
# section faults under it.
#   OUT=dir HAO=/abs/build/hao [HAO_BASE=/abs/old/hao] [T=60] [SECTIONS="sorted ..."] [MODES="jit interp aot"] tests/guest/msgfnptr.sh
# Prints one line per section and mode (SAME, DIFF, CRASH sig=N, TIMEOUT; nx=N); exit 1 on any failure.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-msgfnptr)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
SECTIONS=${SECTIONS:-sorted sort slots super imp invoke ptrfn ident}
MODES=${MODES:-jit interp aot}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -fno-optimize-sibling-calls -fobjc-arc -I"$here" -framework Foundation \
    -install_name "$OUT/libmsgfnptrhelper.dylib" -o "$OUT/libmsgfnptrhelper.dylib" "$here/msgfnptrhelper.m"
codesign -s - -f "$OUT/libmsgfnptrhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lmsgfnptrhelper \
        -o "$OUT/msgfnptr.$a" "$here/msgfnptr.m"
    codesign -s - -f "$OUT/msgfnptr.$a" 2>/dev/null
done
set +e
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/msgfnptr.arm64e" >"$OUT/aotgen.out" 2>&1
fail=0
case " $MODES " in *" aot "*)
    if ls "$OUT/aot"/*.aot >/dev/null 2>&1; then :; else echo "msgfnptr aot: no cache written"; cat "$OUT/aotgen.out"; fail=1; fi ;;
esac
verdict() {  # rc: name of a non-zero status
    if [ "$1" = 137 ]; then echo TIMEOUT; elif [ "$1" -gt 128 ]; then echo "CRASH sig=$(($1 - 128))"; else echo "rc=$1"; fi
}
nxof() { sed -n 's/^hao: nx faults: \([0-9]*\) native->guest.*/\1/p' "$1" | tail -1; }
basefaults=0
for s in $SECTIONS; do
    timeout -s KILL "$T" "$OUT/msgfnptr.arm64" "$s" >"$OUT/$s.native.out" 2>"$OUT/$s.native.err"
    nrc=$?
    timeout -s KILL "$T" "$OUT/msgfnptr.arm64" "$s" >"$OUT/$s.native2.out" 2>/dev/null
    if [ $nrc != 0 ] || ! cmp -s "$OUT/$s.native.out" "$OUT/$s.native2.out"; then
        echo "msgfnptr $s native: $(verdict $nrc) or not repeatable"; fail=1; continue
    fi
    for mode in $MODES; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 HAO_NX_STATS=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 HAO_NX_STATS=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" HAO_NX_STATS=1 "$HAO" ;;
        esac
        { timeout -s KILL "$T" "$@" "$OUT/msgfnptr.arm64e" "$s" >"$OUT/$s.$mode.out" 2>"$OUT/$s.$mode.err"; } 2>/dev/null
        rc=$?
        nx=$(nxof "$OUT/$s.$mode.err")
        if [ $rc = 0 ] && cmp -s "$OUT/$s.native.out" "$OUT/$s.$mode.out"; then r=SAME
        elif [ $rc = 0 ] || [ $rc = $nrc ]; then r=DIFF
        else r=$(verdict $rc); fi
        ok=1
        [ "$r" = SAME ] || ok=0
        [ "${nx:-x}" = 0 ] || ok=0
        printf 'msgfnptr %-7s %-6s %s nx=%s\n' "$s" "$mode" "$r" "${nx:-?}"
        if [ $ok = 0 ]; then
            fail=1
            diff "$OUT/$s.native.out" "$OUT/$s.$mode.out" | cut -c1-160 | head -12 | sed 's/^/    /'
            grep -m2 'fault\|hao:' "$OUT/$s.$mode.err" | grep -v 'nx faults' | cut -c1-200 | sed 's/^/    /'
        fi
    done
    if [ -n "${HAO_BASE:-}" ]; then
        { timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_NX_STATS=1 "$HAO_BASE" "$OUT/msgfnptr.arm64e" "$s" \
            >"$OUT/$s.base.out" 2>"$OUT/$s.base.err"; } 2>/dev/null
        rc=$?
        nx=$(nxof "$OUT/$s.base.err")
        if [ $rc = 0 ] && cmp -s "$OUT/$s.native.out" "$OUT/$s.base.out"; then r=SAME; else r="DIFF/$(verdict $rc)"; fi
        printf 'msgfnptr %-7s base   %s nx=%s (control)\n' "$s" "$r" "${nx:-?}"
        [ "${nx:-0}" -gt 0 ] && basefaults=$((basefaults + 1))
    fi
done
if [ -n "${HAO_BASE:-}" ]; then
    if [ $basefaults = 0 ]; then echo "msgfnptr control: no section faults under HAO_BASE"; fail=1
    else echo "msgfnptr control: $basefaults sections fault under HAO_BASE"; fi
fi
[ $fail = 0 ] && echo "msgfnptr: PASS" || echo "msgfnptr: FAIL"
exit $fail
