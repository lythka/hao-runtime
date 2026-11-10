#!/bin/sh
# Guest thread state must be freed when a thread exits, and kept in a fork
# child. Builds tests/guest/threads.m for arm64e (guest) and arm64
# (reference), runs arm64 natively and arm64e under hao (JIT with N=400,
# --interp with N=100), and checks each hao run:
#   counters  the counter line equals native arm64's
#   vm        virtual size grew by less than 2 MB per thread (each guest
#             stack is 8 MB, so one stack leaked per thread fails)
#   fork      the fork child read the parked threads' stack locals
#             (sum=406) and exited with status 0
#   stats     --stats reports `threads live=1 freed=N+4` (main thread live;
#             the N workers and the 4 parked threads freed)
# Prints PASS/FAIL per check; exit 0 if all pass, 1 otherwise.
#   OUT=dir HAO=build/hao [T=60] [TI=200] [N=400] [NI=100] tests/guest/threads.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-threads)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
TI=${TI:-200}
N=${N:-400}
NI=${NI:-100}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -fno-objc-arc -framework Foundation -o "$OUT/threads.$a" "$here/threads.m"
    codesign -s - -f "$OUT/threads.$a" 2>/dev/null
done
set +e
fail=0
check() {  # label name ok(0/1) detail
    if [ "$3" = 1 ]; then r=PASS; else r=FAIL; fail=1; fi
    printf '%-7s %-9s %s  %s\n' "$1" "$2" "$r" "$4"
}
run() {  # label n timeout cmd...
    l=$1; n=$2; t=$3; shift 3
    timeout -s KILL "$T" "$OUT/threads.arm64" "$n" >"$OUT/native.$l.out" 2>"$OUT/native.$l.err"
    timeout -s KILL "$t" "$@" "$OUT/threads.arm64e" "$n" >"$OUT/$l.out" 2>"$OUT/$l.err"
    rc=$?
    echo "$l rc=$rc"
    want=$(grep '^threads=' "$OUT/native.$l.out")
    got=$(grep '^threads=' "$OUT/$l.out")
    ok=0; [ -n "$want" ] && [ "$got" = "$want" ] && ok=1
    check "$l" counters $ok "${got:-(none)}"
    vm=$(sed -n 's/^vm_growth=\(-*[0-9]*\)$/\1/p' "$OUT/$l.out")
    ok=0; [ -n "$vm" ] && [ "$vm" -lt $((2 * n)) ] && ok=1
    check "$l" vm $ok "vm_growth=${vm:-?} MB (limit $((2 * n)); native $(sed -n 's/^vm_growth=//p' "$OUT/native.$l.out"))"
    fk=$(grep -E '^child' "$OUT/$l.out" | tr '\n' ' ')
    ok=0; grep -qx 'child: sum=406' "$OUT/$l.out" && grep -qx 'child status=0' "$OUT/$l.out" && ok=1
    check "$l" fork $ok "${fk:-(none)}"
    st=$(grep '^hao: threads ' "$OUT/$l.err" | sed 's/^hao: //')
    ok=0; [ "$st" = "threads live=1 freed=$((n + 4))" ] && ok=1
    check "$l" stats $ok "${st:-(no threads line)}"
}
run jit "$N" "$T" "$HAO" --stats
run interp "$NI" "$TI" "$HAO" --stats --interp
exit $fail
