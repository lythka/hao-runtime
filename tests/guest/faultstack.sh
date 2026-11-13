#!/bin/sh
# A fatal fault's report (runtime.c fault_diag) runs on a stack of its own. tests/guest/faultstack.c faults
# in native strlen at a chosen depth of nested native->guest callbacks on a 64 KB thread. Deep enough, the
# thread's stack has too little room left for the report: before the change, the process then ended
# (SIGBUS) with no report, as WallpaperAgent did on a Swift cooperative-pool worker.
# For each depth from 0 until the thread's stack overflows before the fault (the deepest runs leave the
# least stack), under JIT and --interp: the run must end with a signal and print a "host fault" report. A
# run that reached the fault must report it first (rip in strlen), unless even the kernel's signal frame
# for it did not fit: then the first report is that overflow, at _sigtramp+0 (only a handler on an
# alternate stack could see the strlen fault; hao's is not SA_ONSTACK, signals.c). Without the change,
# a depth where the frame fits but the report does not ends with no report at all.
#   OUT=dir HAO=build/hao [T=30] [MAXD=60] tests/guest/faultstack.sh     exit 1 on any failure
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-faultstack)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
MAXD=${MAXD:-60}
mkdir -p "$OUT"
clang -arch arm64e -O1 -o "$OUT/faultstack" "$here/faultstack.c" && codesign -s - -f "$OUT/faultstack" 2>/dev/null || exit 2
fail=0
for m in jit interp; do
    case $m in jit) set -- "$HAO" ;; interp) set -- "$HAO" --interp ;; esac
    d=0 faulted=0 tight=0 bad=0 edge=
    while [ $d -le "$MAXD" ]; do
        e=$OUT/$m.$d.err
        ( timeout -s KILL "$T" "$@" "$OUT/faultstack" 64 $d >/dev/null 2>"$e"; exit $? ) 2>/dev/null
        rc=$?
        first=$(grep -m1 "host fault" "$e")
        if [ $rc -lt 128 ] || [ -z "$first" ]; then
            echo "FAIL $m depth $d: rc $rc, no report"; bad=1
        elif grep -q "^faulting at depth" "$e"; then
            faulted=$((faulted + 1))
            case $first in
            *"[_platform_strlen+"*|*"[strlen+"*) ;;
            *"[_sigtramp+0]"*) tight=$((tight + 1)) ;;
            *) echo "FAIL $m depth $d: first report is not the fault: $first"; bad=1 ;;
            esac
        else
            edge=$d; break      # the stack overflowed before the fault: deeper runs only overflow sooner
        fi
        d=$((d + 1))
    done
    if [ -z "$edge" ]; then echo "FAIL $m: no overflow by depth $MAXD (the scan never reached a full stack)"; bad=1; fi
    if [ $bad = 0 ]; then echo "PASS $m: depths 0-$((edge - 1)) report ($faulted reached the fault, $tight of them with no room for its signal frame), depth $edge overflows and reports"; else fail=1; fi
done
exit $fail
