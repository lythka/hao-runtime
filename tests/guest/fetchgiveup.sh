#!/bin/sh
# A fault hao gives up on whose instruction fetch is the fault (tests/guest/fetchgiveup.c): native qsort
# calls a comparator inside a PROT_NONE page. arm64 natively is the reference for the exit status and
# stdout. Each hao run (JIT, --interp, AOT, and JIT with HAO_SIGRETURN_USER=1: the user-space resume an
# Intel Mac takes, which a TEST_SWITCHES=1 build honours) must end with the same status and stdout, and its stderr must
# hold the give-up line for that signal at the target address: before the fix the give-up read the two
# bytes before the faulting rip (a sent signal follows a syscall instruction), faulted on that same page
# inside the handler and died there, with no line and a fault address 2 bytes off.
#   OUT=dir HAO=build/hao [T=60] tests/guest/fetchgiveup.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-fetchgiveup)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    clang -arch $a -O1 -o "$OUT/fetchgiveup.$a" "$here/fetchgiveup.c" 2>"$OUT/build.$a.err" ||
        { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/fetchgiveup.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/fetchgiveup.arm64e" >"$OUT/aot.gen" 2>&1 || echo "fetchgiveup: aot generation failed"
timeout -s KILL "$T" "$OUT/fetchgiveup.arm64" >"$OUT/native.out" 2>"$OUT/native.err"; nrc=$?
echo "native    rc=$nrc"
fail=0
case $nrc in 138|139) ;; *) echo "fetchgiveup: native run did not die of the fault"; fail=1 ;; esac
for mode in jit interp aot user; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    user) set -- env HAO_NO_AOT=1 HAO_SIGRETURN_USER=1 "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/fetchgiveup.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    tgt=$(sed -n 's/^target=//p' "$OUT/$mode.err" | head -1)
    line="signal $((nrc - 128)) (code [0-9]*, addr $tgt) given up"
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && [ -n "$tgt" ] && grep -q "$line" "$OUT/$mode.err"; then
        r=SAME
    else
        r=DIFF; fail=1
    fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -6; tail -3 "$OUT/$mode.err" | sed 's/^/    err: /'; }
done
[ $fail = 0 ] && echo "fetchgiveup: PASS" || echo "fetchgiveup: FAIL"
exit $fail
