#!/bin/sh
# os_log_set_client_type from guest code (src/interpose.c trap_os_log_client_type). The SPI ends the process
# (_os_crash) unless the process is single-threaded and libtrace has not started. A guest's main runs after
# the native initializers and hao's setup, so hao does not apply the request. tests/guest/logclient.c calls
# the SPI first thing in main, as gssd does, and links GSS.framework, which loads CoreFoundation.
# Two ways the process is no longer pristine:
#   trace    __CF_USER_TEXT_ENCODING unset: CoreFoundation's initializer logs, which starts libtrace;
#   threads  the variable names this user (libtrace stays idle), but HAO_NX_STATS=1 starts a hao thread.
# 1. Controls, the arm64 program run natively: unset, it ends with a trap before running (the case
#    reproduces); set, it runs (the SPI works in a pristine process).
# 2. Under hao (JIT, --interp and AOT for trace; JIT for threads): the program runs to the end, and -v
#    reports the request as not applied, once.
#   OUT=dir HAO=build/hao [T=30] tests/guest/logclient.sh     exit 1 on any failure
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-logclient)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
mkdir -p "$OUT"
fail=0
enc=__CF_USER_TEXT_ENCODING=0x$(printf %X "$(id -u)"):0x0:0x0
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$OUT/logclient.$a" "$here/logclient.c" -framework GSS && codesign -s - -f "$OUT/logclient.$a" 2>/dev/null || exit 2
done

( timeout -s KILL "$T" env -u __CF_USER_TEXT_ENCODING "$OUT/logclient.arm64" >"$OUT/native.trace" 2>&1; exit $? ) 2>/dev/null
rc=$?
if [ $rc -ge 128 ] && ! grep -q running "$OUT/native.trace"; then echo "PASS native trace: ends with signal $((rc - 128)) before running"
else echo "FAIL native trace: rc $rc (the case no longer reproduces natively)"; fail=1; fi
( timeout -s KILL "$T" env "$enc" "$OUT/logclient.arm64" >"$OUT/native.pristine" 2>&1; exit $? ) 2>/dev/null
rc=$?
if [ $rc = 0 ] && grep -q "^logclient: running$" "$OUT/native.pristine"; then echo "PASS native pristine: runs"
else echo "FAIL native pristine: rc $rc"; fail=1; fi

timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/logclient.arm64e" >"$OUT/aot.gen" 2>&1 || { echo "FAIL aot generation"; fail=1; }
for m in jit interp aot threads; do
    case $m in
    jit) set -- env -u __CF_USER_TEXT_ENCODING "$HAO" -v ;;
    interp) set -- env -u __CF_USER_TEXT_ENCODING "$HAO" -v --interp ;;
    aot) set -- env -u __CF_USER_TEXT_ENCODING HAO_CACHE="$OUT/aot" "$HAO" -v ;;
    threads) set -- env "$enc" HAO_NX_STATS=1 "$HAO" -v ;;
    esac
    ( timeout -s KILL "$T" "$@" "$OUT/logclient.arm64e" >"$OUT/$m.out" 2>"$OUT/$m.err"; exit $? ) 2>/dev/null
    rc=$?
    n=$(grep -c "os_log_set_client_type(1, 0) not applied" "$OUT/$m.err")
    if [ $rc = 0 ] && grep -q "^logclient: running$" "$OUT/$m.out" && [ "$n" = 1 ]; then echo "PASS $m: runs to the end, request reported once"
    else echo "FAIL $m: rc $rc, reported $n times"; grep -E "signal|client_type" "$OUT/$m.err" | head -3; fail=1; fi
done
exit $fail
