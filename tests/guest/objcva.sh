#!/bin/sh
# Native variadic ObjC methods reached through
# objc_msgSend, objc_msgSendSuper and objc_msgSendSuper2 whose selector no hao
# table names (tests/guest/objcva.m lists the members and their shapes; the
# defect's own is /usr/bin/log's -[OSLogTermDumper format:], `log show --style
# compact`). Build objcva.m for arm64e (guest) and arm64 (reference); run every
# section alone, natively twice (the reference must repeat), then under hao
# with the JIT, with --interp and from an AOT cache. A run passes when its
# stdout and exit status equal native arm64's.
#   OUT=dir HAO=/abs/build/hao [T=60] [SECTIONS="dumper ..."] [MODES="jit interp aot"] tests/guest/objcva.sh
# Prints one line per section and mode (SAME, DIFF, CRASH sig=N, TIMEOUT); exit 1 on any non-SAME.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-objcva)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
SECTIONS=${SECTIONS:-dumper dumpsub msgsuper dumpimp validated attributed cifilter mlerror msverror pql}
MODES=${MODES:-jit interp aot}
LS=/System/Library/PrivateFrameworks/LoggingSupport.framework/Versions/A/LoggingSupport
mkdir -p "$OUT"
# the guest subclasses the native OSLogTermDumper as log does: link LoggingSupport through a stand-in
printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-macos, arm64e-macos ]\ninstall-name: '%s'\nexports:\n  - targets: [ arm64-macos, arm64e-macos ]\n    objc-classes: [ OSLogTermDumper ]\n...\n" \
    "$LS" >"$OUT/LoggingSupport.tbd"
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -framework Foundation -framework CoreImage "$OUT/LoggingSupport.tbd" \
        -o "$OUT/objcva.$a" "$here/objcva.m"
    codesign -s - -f "$OUT/objcva.$a" 2>/dev/null
done
set +e
# pql's scratch databases (left behind when a run crashes) stay under OUT
mkdir -p "$OUT/tmp"; export TMPDIR="$OUT/tmp"
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL 120 "$HAO" --aot --cache "$OUT/aot" "$OUT/objcva.arm64e" >"$OUT/aotgen.out" 2>&1
fail=0
case " $MODES " in *" aot "*)
    if ls "$OUT/aot"/*.aot >/dev/null 2>&1; then :; else echo "objcva aot: no cache written"; cat "$OUT/aotgen.out"; fail=1; fi ;;
esac
verdict() {  # rc: name of a non-zero status
    if [ "$1" = 137 ]; then echo TIMEOUT; elif [ "$1" -gt 128 ]; then echo "CRASH sig=$(($1 - 128))"; else echo "rc=$1"; fi
}
for s in $SECTIONS; do
    timeout -s KILL "$T" "$OUT/objcva.arm64" "$s" >"$OUT/$s.native.out" 2>"$OUT/$s.native.err"
    nrc=$?
    timeout -s KILL "$T" "$OUT/objcva.arm64" "$s" >"$OUT/$s.native2.out" 2>/dev/null
    if [ $nrc != 0 ] || ! cmp -s "$OUT/$s.native.out" "$OUT/$s.native2.out"; then
        echo "objcva $s native: $(verdict $nrc) or not repeatable"; fail=1; continue
    fi
    for mode in $MODES; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        esac
        { timeout -s KILL "$T" "$@" "$OUT/objcva.arm64e" "$s" >"$OUT/$s.$mode.out" 2>"$OUT/$s.$mode.err"; } 2>/dev/null
        rc=$?
        if [ $rc = 0 ] && cmp -s "$OUT/$s.native.out" "$OUT/$s.$mode.out"; then r=SAME
        elif [ $rc = 0 ] || [ $rc = $nrc ]; then r=DIFF
        else r=$(verdict $rc); fi
        printf 'objcva %-10s %-6s %s\n' "$s" "$mode" "$r"
        if [ "$r" != SAME ]; then
            fail=1
            diff "$OUT/$s.native.out" "$OUT/$s.$mode.out" | cut -c1-160 | head -12 | sed 's/^/    /'
            grep -m2 'fault\|hao:' "$OUT/$s.$mode.err" | cut -c1-200 | sed 's/^/    /'
        fi
    done
done
[ $fail = 0 ] && echo "objcva: PASS" || echo "objcva: FAIL"
exit $fail
