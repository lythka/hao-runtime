#!/bin/sh
# Code cache, thunk areas and hao's locks (tests/host/jitlock.c against libhao):
#   aot       hao_aot_load of 40 copies of /usr/bin/awk (cache made by
#             hao --aot) while 4 threads translate (AOT_RUNS runs)
#   sigs      65536 signature thunks keep their signatures, one per (guest,
#             sig), none past that, with one warning
#   closure-* signature thunks map back to their guest at 20000 and 60000,
#             and a miss stays cheap
#   full      HAO_JIT_CACHE_MB=8: blocks past the limit run in the
#             interpreter (flags crossing JIT <-> interpreter), one warning
# (Guest signal handlers vs hao's locks: tests/guest/signals.sh.)
# Prints PASS/FAIL per check; exit 0 if all pass, 1 otherwise.
#   OUT=dir LIB=build [HAO=LIB/hao] [INC=include] [AOT_RUNS=10] tests/host/jitlock.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-jitlock)}
LIB=$(cd "${LIB:-$repo/build}" && pwd)
HAO=${HAO:-$LIB/hao}
INC=${INC:-$repo/include}
AOT_RUNS=${AOT_RUNS:-10}
mkdir -p "$OUT"
T=$OUT/test_jitlock
clang -arch x86_64 -mmacosx-version-min=27.0 -std=gnu11 -O1 -I"$INC" -o "$T" "$here/jitlock.c" \
    -L"$LIB" -lhao -Wl,-rpath,"$LIB" || { echo "build failed"; exit 1; }
codesign -s - -f "$T" 2>/dev/null
fail=0
check() {  # name ok(0/1) detail
    if [ "$2" = 1 ]; then r=PASS; else r=FAIL; fail=1; fi
    printf '%-12s %s  %s\n' "$1" "$r" "$3"
}
one() {  # name timeout args...   -> rc, ok, line; $OUT/name.out, $OUT/name.err
    nm=$1; to=$2; shift 2
    timeout -s KILL "$to" "$T" "$@" >"$OUT/$nm.out" 2>"$OUT/$nm.err"
    rc=$?
    line=$(tail -1 "$OUT/$nm.out")
    ok=0; [ $rc = 0 ] && echo "$line" | grep -q ' -> PASS$' && ok=1
}

G=/usr/bin/awk
rm -rf "$OUT/aotc"; mkdir -p "$OUT/aotc"; chmod 700 "$OUT/aotc"
timeout -s KILL 300 "$HAO" --aot --cache $OUT/aotc "$G" >"$OUT/aotgen.err" 2>&1
export HAO_CACHE=$OUT/aotc
pass=0; last=
for i in $(seq "$AOT_RUNS"); do
    one aot 120 aot 40000 "$G"
    [ $ok = 1 ] && pass=$((pass + 1))
    [ $ok = 1 ] || last="rc=$rc ${line:-(no summary)}"
done
unset HAO_CACHE
check aot $([ $pass = "$AOT_RUNS" ] && echo 1 || echo 0) "$pass/$AOT_RUNS runs pass${last:+; last failure: $last}"

one sigs 60 sigs
w=$(grep -c 'signature thunk area full' "$OUT/sigs.err")
[ "$w" = 1 ] || ok=0
check sigs $ok "rc=$rc warnings=$w ${line:-(no summary)}"

for n in 20000 60000; do
    one "closure-$n" 60 closure $n
    check "closure-$n" $ok "rc=$rc ${line:-(no summary)}"
done

export HAO_JIT_CACHE_MB=8
one full 120 full 20000
unset HAO_JIT_CACHE_MB
w=$(grep -c 'code cache full (8 MB' "$OUT/full.err")
[ "$w" = 1 ] || ok=0
check full $ok "rc=$rc warnings=$w ${line:-(no summary)}"

exit $fail
