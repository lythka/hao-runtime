#!/bin/sh
# ___chkstk_darwin under each of its names (tests/guest/chkstk.c): native arm64 against
# hao under JIT, --interp and AOT. hao traps the name programs import (libcompiler_rt's
# re-export ___chkstk_darwin); libsystem_pthread's ____chkstk_darwin and thread_chkstk_darwin
# (the definition and its alias, the names a shared-cache stub slot or dlsym yields) reach the
# trap through their address (src/bridge.c: a trap answers every name of its function). The
# gate checks that src does not list them, so these runs test that route.
# tests/guest/addrkeys.sh covers imports by other names (thread-chkstk, a renamed re-export).
#   OUT=dir HAO=/abs/build/hao tests/guest/chkstk.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-chkstk)}
HAO=${HAO:-$repo/build/hao}
case $HAO in /*) ;; *) echo "chkstk.sh: HAO must be an absolute path"; exit 2 ;; esac
rm -rf "$OUT"; mkdir -p "$OUT/aot" "$OUT/none"; chmod 755 "$OUT" "$OUT/aot"
fail=0
for a in arm64 arm64e; do
    clang -arch $a -O1 "$here/chkstk.c" -o "$OUT/chkstk.$a" || exit 2
    codesign -s - -f "$OUT/chkstk.$a" 2>/dev/null || exit 2
done
timeout -s KILL 20 "$OUT/chkstk.arm64" >"$OUT/native.out" 2>&1 || { echo "chkstk: native rc=$?"; exit 2; }
HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/chkstk.arm64e" >"$OUT/aotgen.log" 2>&1 || { echo "chkstk: hao --aot failed"; fail=1; }
for mode in jit interp aot; do
    flag=; [ $mode = interp ] && flag=--interp
    cache=$OUT/none; noaot=1; [ $mode = aot ] && { cache=$OUT/aot; noaot=; }
    env HAO_CACHE="$cache" ${noaot:+HAO_NO_AOT=1} HAO_VERBOSE=1 timeout -s KILL 30 "$HAO" $flag "$OUT/chkstk.arm64e" \
        >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    ok=1
    [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" || ok=0
    [ $mode != aot ] || grep -q "AOT cache .* loaded" "$OUT/$mode.err" || ok=0
    if [ $ok = 1 ]; then echo "PASS chkstk $mode: as native ($(grep -c returned "$OUT/$mode.out") names)"
    else echo "FAIL chkstk $mode: rc=$rc"; diff "$OUT/native.out" "$OUT/$mode.out" | head -5; fail=1; fi
done
# the gate: the trap table names the probe once (the import name); its definition's names reach
# the trap by address (src/bridge.c trap_at), so the runs above go through that route
if grep -n '"____chkstk_darwin"\|"_thread_chkstk_darwin"' "$repo"/src/*.c >"$OUT/gate.txt"; then
    echo "FAIL chkstk gate: src names a definition of the probe, so the runs do not test the address route"; cat "$OUT/gate.txt"; fail=1
else echo "PASS chkstk gate: src traps only ___chkstk_darwin; the other names reach it by address"; fi
[ $fail = 0 ] && echo "chkstk: PASS" || echo "chkstk: FAIL"
exit $fail
