#!/bin/sh
# An abort report says whether the process is a fork child and lists the thread's last native calls and
# traps, oldest first, with their lr: how guest code reached the failing call (runtime.c abort_nesting).
# tests/guest/forkabort.c forks; the child calls getpid, getppid and abort. Under JIT and --interp with
# HAO_CRASH_TRACE=1 the parent must see the child die of SIGABRT, and the child's report must name the
# fork, then the fork call, getpid, getppid and abort in that order. Before the change the report had
# neither the fork line nor the steps.
#   OUT=dir HAO=build/hao [T=60] tests/guest/forkabort.sh     exit 1 on any failure
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-forkabort)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
clang -arch arm64e -O2 -o "$OUT/forkabort" "$here/forkabort.c" && codesign -s - -f "$OUT/forkabort" 2>/dev/null || exit 2
fail=0
for m in jit interp; do
    case $m in jit) set -- "$HAO" ;; interp) set -- "$HAO" --interp ;; esac
    e=$OUT/$m.err
    HAO_CRASH_TRACE=1 timeout -s KILL "$T" "$@" "$OUT/forkabort" >"$OUT/$m.out" 2>"$e"
    rc=$?
    steps=$(grep "  step -" "$e" | sed 's/.* call \([^ ]*\) lr=.*/\1/' | tr '\n' ' ')
    if [ $rc != 0 ] || ! grep -q "^child signal 6$" "$OUT/$m.out"; then
        echo "FAIL $m: rc $rc, $(cat "$OUT/$m.out")"; fail=1
    elif ! grep -q "  fork child of pid [0-9]*, [0-9]* us after the fork" "$e"; then
        echo "FAIL $m: no fork child line"; fail=1
    elif ! echo "$steps" | grep -q "fork __getpid getppid abort $"; then
        echo "FAIL $m: steps '$steps'"; fail=1
    else
        echo "PASS $m: $steps"
    fi
done
exit $fail
