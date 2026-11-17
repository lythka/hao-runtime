#!/bin/sh
# Build tests/guest/signals.c (+ the native helper dylib) for arm64e (guest)
# and arm64 (reference). Each mode runs natively and under hao (JIT,
# --interp, with an AOT cache, and without JIT memory); stdout and the exit status must match
# the native run. A hang is killed after T seconds (rc 137).
# segvrecurse is the exception: natively a fault inside a SIGSEGV handler
# that has SIGSEGV blocked makes macOS re-fault forever (the native run is
# killed by the timeout, rc 137); hao takes the default action (rc 139).
# locate under --interp and nojit (interpreted on a TEST_SWITCHES build)
# compares only its first line: the interpreter updates the pc and the
# registers of one instruction at different times, so an asynchronous signal
# can show a register already written at the instruction's own pc, or the
# next pc with it not yet written (ISSUES.md). The JIT leg of the locate
# modes also runs with --stats: its counts of translated instructions must
# add up (native + helper = guest_insns), which locating must not change.
#   OUT=dir HAO=build/hao [T=30] [MODES="disp uctx ..."] tests/guest/signals.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-signals)}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
MODES=${MODES:-disp uctx prof jitlock locate locatefp native mlocked noalloc restart segvnative segvreset segvrecurse segvjmp}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 \
    -install_name "$OUT/libsighelper.dylib" -o "$OUT/libsighelper.dylib" "$here/sighelper.c"
codesign -s - -f "$OUT/libsighelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -L"$OUT" -lsighelper -o "$OUT/signals.$a" "$here/signals.c"
    codesign -s - -f "$OUT/signals.$a" 2>/dev/null
done
# AOT leg: a private cache directory (owner-only, as hao's trust rules want)
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/signals.arm64e" >/dev/null 2>&1
set +e
fail=0
for m in $MODES; do
    timeout -s KILL "$T" "$OUT/signals.arm64" $m >"$OUT/$m.native.out" 2>"$OUT/$m.native.err"
    nrc=$?
    for v in jit interp aot nojit; do
        case $v in
        jit) set -- env HAO_NO_AOT=1 "$HAO"; case $m in locate*) set -- "$@" --stats ;; esac ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        nojit) set -- env HAO_NO_AOT=1 HAO_NO_JIT_MEMORY=1 "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/signals.arm64e" $m >"$OUT/$m.$v.out" 2>"$OUT/$m.$v.err"
        rc=$?
        want=$nrc
        [ $m = segvrecurse ] && want=139
        if [ $m.$v = locate.interp ] || [ $m.$v = locate.nojit ]; then
            head -1 "$OUT/$m.native.out" >"$OUT/$m.native.cmp"; head -1 "$OUT/$m.$v.out" >"$OUT/$m.$v.cmp"
        else
            cp "$OUT/$m.native.out" "$OUT/$m.native.cmp"; cp "$OUT/$m.$v.out" "$OUT/$m.$v.cmp"
        fi
        if [ $rc = $want ] && cmp -s "$OUT/$m.native.cmp" "$OUT/$m.$v.cmp"; then r=SAME; else r=DIFF; fail=1; fi
        case $m.$v in locate*.jit)
            if ! sed -n 's/.* guest_insns=\([0-9]*\) native=\([0-9]*\) helper=\([0-9]*\) .*/\1 \2 \3/p' "$OUT/$m.$v.err" |
                    awk 'NF == 3 && $1 == $2 + $3 { ok = 1 } END { exit !ok }'; then
                r="DIFF (--stats: native + helper != guest_insns)"; fail=1
            fi ;;
        esac
        printf '%-12s %-6s native rc=%-3s hao rc=%-3s %s\n' "$m" "$v" "$nrc" "$rc" "$r"
    done
done
exit $fail
