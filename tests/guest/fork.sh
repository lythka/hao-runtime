#!/bin/sh
# Fork safety: a fork child must not inherit hao's locks held by threads it
# does not have, and the parent must never wait on them inside fork().
#   host     tests/host/fork.c against libhao (built here next to $HAO):
#            fork while threads translate / make thunks / parse signatures;
#            every child must make a thunk, a signature thunk and a new
#            translation and exit 0 (modes all, jit, thunk, sig, and sig
#            with SIG_SLOW=200 for the signature-cache order check)
#   jit      tests/guest/fork.c under hao vs native arm64: atfork order,
#            fork / vfork+exec / raw svc fork while 4 threads translate;
#            the section lines must equal native's, and under hao every
#            vfork and raw-fork child must run the atfork child handlers
#            (natively none do: that is the documented difference)
#   interp   the same with --interp (fewer forks)
#   dyld     tests/guest/fork_dyld.c: fork while another thread runs new
#            guest code inside a dyld add-image callback; the parent must
#            not hang
# Prints PASS/FAIL per check; exit 0 if all pass, 1 otherwise.
#   OUT=dir HAO=build/hao [N=100] [NI=40] [FORKS=300] tests/guest/fork.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-fork}
HAO=${HAO:-$repo/build/hao}
N=${N:-100}
NI=${NI:-40}
FORKS=${FORKS:-300}
lib=$(cd "$(dirname "$HAO")" && pwd)
mkdir -p "$OUT"
set -e
clang -arch x86_64 -mmacosx-version-min=27.0 -O1 -I"$repo/include" -o "$OUT/test_fork" "$repo/tests/host/fork.c" \
    -L"$lib" -lhao -Wl,-rpath,"$lib"
codesign -s - -f "$OUT/test_fork" 2>/dev/null
for b in fork.arm64e fork.arm64 fork_dyld.arm64e; do
    clang -arch "${b##*.}" -O1 -Wno-deprecated-declarations -o "$OUT/$b" "$here/${b%.*}.c" &
done
wait
for b in fork.arm64e fork.arm64 fork_dyld.arm64e; do
    [ -x "$OUT/$b" ] || { echo "build of $b failed"; exit 1; }
    codesign -s - -f "$OUT/$b" 2>/dev/null
done
set +e
fail=0
check() {  # label name ok(0/1) detail
    if [ "$3" = 1 ]; then r=PASS; else r=FAIL; fail=1; fi
    printf '%-7s %-9s %s  %s\n' "$1" "$2" "$r" "$4"
}

host() {  # name forks mode [env]
    nm=$1; f=$2; m=$3; shift 3
    env "$@" timeout -s KILL 900 "$OUT/test_fork" "$f" "$m" >"$OUT/host.$nm.out" 2>&1
    rc=$?
    line=$(grep '^mode=' "$OUT/host.$nm.out")
    ok=0; [ $rc = 0 ] && grep -q "^mode=$m forks=$f ok=$f hung=0 bad=0 " "$OUT/host.$nm.out" && ok=1
    check host "$nm" $ok "rc=$rc ${line:-(no summary)}"
}
host all "$FORKS" all
host jit 40 jit
host thunk 40 thunk
host sig 40 sig
host sigslow "$FORKS" sig SIG_SLOW=200

guest() {  # label n cmd...
    l=$1; n=$2; shift 2
    timeout -s KILL 60 "$OUT/fork.arm64" "$n" >"$OUT/native.$l.out" 2>&1
    timeout -s KILL 900 "$@" "$OUT/fork.arm64e" "$n" >"$OUT/$l.out" 2>"$OUT/$l.err"
    rc=$?
    for s in 'atfork' 'fork under' 'vfork+exec' 'raw svc fork under'; do
        want=$(grep "^$s" "$OUT/native.$l.out")
        got=$(grep "^$s" "$OUT/$l.out")
        ok=0; [ -n "$want" ] && [ "$got" = "$want" ] && ok=1
        check "$l" "$(echo "$s" | cut -d' ' -f1)" $ok "${got:-(none)}"
    done
    for s in 'vfork children' 'raw svc fork children'; do
        got=$(grep "^$s" "$OUT/$l.out")
        nat=$(sed -n "s/^$s.*: //p" "$OUT/native.$l.out")
        ok=0; [ "${got##*: }" = "$n/$n" ] && [ "$nat" = "0/$n" ] && ok=1
        check "$l" "$(echo "$s" | cut -d' ' -f1)-hnd" $ok "${got:-(none)} (native $nat)"
    done
    ok=0; [ $rc = 0 ] && ok=1
    check "$l" exit $ok "rc=$rc"
}
guest jit "$N" "$HAO"
guest interp "$NI" "$HAO" --interp

timeout -s KILL 60 "$HAO" "$OUT/fork_dyld.arm64e" >"$OUT/dyld.out" 2>"$OUT/dyld.err"
rc=$?
ok=0; [ $rc = 0 ] && grep -qx 'dyld callback + fork: forks=20 parent ok' "$OUT/dyld.out" && ok=1
check dyld parent $ok "rc=$rc $(cat "$OUT/dyld.out")"
exit $fail
